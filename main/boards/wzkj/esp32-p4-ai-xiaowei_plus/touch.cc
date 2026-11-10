#include "touch.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_lvgl_port.h>
#include <esp_log.h>

#include "config.h"

static const char* TAG = "LcdTouch";
static i2c_master_bus_handle_t s_camera_bus = nullptr;
extern "C" i2c_master_bus_handle_t xiaowei_camera_i2c_bus(void) { return s_camera_bus; }

// CST820B 的数据寄存器：从 0x02 起连读 6 字节
static constexpr uint8_t TOUCH_REG_POINT_INFO = 0x02;
// 7 字节 @400kHz 正常只要 ~200us，10ms 已经是 50 倍余量。
// 别调大：这个读是在 LVGL 任务里每帧调一次的，只有芯片真死了才会用满超时，
// 超时设成 50ms 的话芯片一挂就是每帧卡 50ms（肉眼可见的卡顿）。
static constexpr int TOUCH_READ_TIMEOUT_MS = 10;

// 读失败时沿用上一次状态的上限（ms）。窗口内 = 当作这一帧没采样（见 ReadCb）；
// 超过 = 认账"触摸挂了"，报抬手。300ms 足够盖住偶发抖动（采样本身 16ms 一次，
// 相当于连丢 ~19 帧），又短到芯片真掉线时不会让 UI 长时间卡在按下态。
static constexpr uint32_t TOUCH_HOLD_LAST_MS = 300;

// 读**成功**、但报"没按"时，也要先确认一下再认账（ms）。这是上面那条的反面：
// 读失败只是"没观测到"，而这里芯片是明确说了没按 —— 但 CST820B 在手指还按着的
// 时候偶尔就会给出一帧 pressed=0（接触面短暂变小、内部采样抖动、马达波形期间的
// 假采样）。这一帧在 LVGL 那边同样是一次**抬手**，而且比读失败更隐蔽：
//   • release 路径（indev/lv_indev.c:1525-1537）只看 scroll_obj 是否为空，
//     空就补一个 CLICKED —— 手指还停在列表上，页面却跳进了下一级；
//   • 下一帧又读到 PRESSED，那是一次全新按压：act_obj 重新搜索、gesture_sent
//     清零（:1326），手势探测器被重新武装，手指每再走 50px 就再翻一页。
// 所以按下之后至少要连续 TOUCH_RELEASE_DEBOUNCE_MS 都读到"没按"才报抬手；
// 窗口内沿用上一次的坐标报 PRESSED（报 PRESSED 却把坐标留成 (0,0) 会让 vect
// 一次跳一整屏，反而立刻触发一次手势）。32ms = 两个采样周期，够盖住单帧抖动，
// 又短到点按感觉不出延迟（真实抬手最多晚 32ms 上报）。
static constexpr uint32_t TOUCH_RELEASE_DEBOUNCE_MS = 32;

LcdTouch::~LcdTouch() {
    // LVGL 的 indev 要摘干净：LVGL 任务还在跑，留着回调指向一个正在析构的对象，
    // 下一次 lv_timer_handler 就会踩到已经释放的 this。
    if (indev_ != nullptr) {
        if (lvgl_port_lock(1000)) {
            lv_indev_delete(indev_);
            lvgl_port_unlock();
        } else {
            // 理论上到不了这里（LVGL 端口没起来的话 indev_ 本来就是空的）。
            // 真到了就别硬删 —— 拿不到锁说明 LVGL 任务还在跑，删了就是野回调。
            ESP_LOGW(TAG, "拿不到 LVGL 锁，LVGL 输入设备未注销（此时已不应再进入析构）");
        }
        indev_ = nullptr;
    }
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
        dev_ = nullptr;
    }
    if (bus_ != nullptr) {
        i2c_del_master_bus(bus_);
        bus_ = nullptr;
    }
}

esp_err_t LcdTouch::Init() {
    if (dev_ != nullptr) {
        return ESP_OK;  // 幂等
    }

    // 硬复位：CST820B 上电后必须走这一下，否则 I2C 上不应答
    gpio_config_t rst_cfg = {};
    rst_cfg.pin_bit_mask = 1ULL << TOUCH_RST_PIN;
    rst_cfg.mode = GPIO_MODE_OUTPUT;
    esp_err_t err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸复位脚配置失败: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = TOUCH_I2C_PORT;
    bus_cfg.sda_io_num = TOUCH_I2C_SDA_PIN;
    bus_cfg.scl_io_num = TOUCH_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    err = i2c_new_master_bus(&bus_cfg, &bus_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触摸 I2C 总线(I2C_NUM_%d, SDA=%d, SCL=%d)创建失败: %s", (int)TOUCH_I2C_PORT,
                 (int)TOUCH_I2C_SDA_PIN, (int)TOUCH_I2C_SCL_PIN, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = TOUCH_I2C_ADDRESS;
    dev_cfg.scl_speed_hz = 400000;
    err = i2c_master_bus_add_device(bus_, &dev_cfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST820B(0x%02X)挂载失败: %s", TOUCH_I2C_ADDRESS, esp_err_to_name(err));
        return err;
    }

    // 探一下有没有应答。i2c_master_bus_add_device 本身不探测，芯片没焊/没供电时
    // 这里也会"成功"，问题只会在后面每帧的读失败日志里慢慢显出来。
    // 只警告不失败：某些触摸芯片对裸地址探测不给 ACK（参考驱动就没探测过），
    // 真出这条警告也不代表芯片一定坏 —— 点屏试试就知道。
    if (i2c_master_probe(bus_, TOUCH_I2C_ADDRESS, 200) != ESP_OK) {
        ESP_LOGW(TAG, "CST820B(0x%02X) 对地址探测无应答，触摸可能不可用（若触摸实际正常，忽略这条）",
                 TOUCH_I2C_ADDRESS);
    }

    ESP_LOGI(TAG, "CST820B 就绪: 0x%02X @400kHz", TOUCH_I2C_ADDRESS);
    s_camera_bus = bus_;
    return ESP_OK;
}

bool LcdTouch::Read(uint16_t* x, uint16_t* y, bool* pressed) {
    if (dev_ == nullptr) {
        return false;
    }

    uint8_t reg = TOUCH_REG_POINT_INFO;
    uint8_t data[6] = {};
    esp_err_t err = i2c_master_transmit_receive(dev_, &reg, 1, data, sizeof(data), TOUCH_READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        // 这条线是 LVGL 任务每帧都会走的，失败日志只在状态翻转时打一条
        if (!reading_failed_) {
            reading_failed_ = true;
            ESP_LOGW(TAG, "读触摸数据失败: %s", esp_err_to_name(err));
        }
        return false;
    }
    if (reading_failed_) {
        reading_failed_ = false;
        ESP_LOGI(TAG, "触摸数据恢复正常");
    }

    *pressed = (data[0] & 0x0f) != 0;
    if (*pressed) {
        *x = ((data[1] & 0x0f) << 8) | data[2];
        *y = ((data[3] & 0x0f) << 8) | data[4];
    }
    return true;
}

esp_err_t LcdTouch::AttachToLvgl(lv_display_t* disp) {
    if (dev_ == nullptr) {
        return ESP_ERR_INVALID_STATE;  // 没 Init 就别挂
    }

    // 主工程的 esp_lvgl_port 没有 lvgl_port_add_indev，而 lvgl_port_add_touch 要
    // esp_lcd_touch_handle_t —— CST820B 没有对应的 esp_lcd_touch 驱动，所以这里直接
    // 用 LVGL 原生 indev。LVGL 任务此时已经在跑了（MipiLcdDisplay 里启的），
    // 动 indev 链表必须拿锁。
    // 这里用 1000ms 而不是 0：lvgl_port_lock(0) 内部是 portMAX_DELAY（永久等待），
    // 那样返回 false 的分支永远不会走到，写出来是自欺欺人。
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "拿 LVGL 锁失败，触摸未注册");
        return ESP_ERR_TIMEOUT;
    }

    if (disp == nullptr) {
        disp = lv_display_get_default();
    }
    if (disp == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "还没有 LVGL display，触摸注册不了（AttachToLvgl 要在 MipiLcdDisplay 之后调）");
        return ESP_ERR_INVALID_STATE;
    }

    indev_ = lv_indev_create();
    if (indev_ == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_indev_create 失败");
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_, ReadCb);
    lv_indev_set_display(indev_, disp);
    lv_indev_set_user_data(indev_, this);

    // ⚠ 这里**故意不**改 read_timer 的周期，让它保持 lv_indev_create() 给的默认值
    // LV_DEF_REFR_PERIOD（lv_indev.c:132；本工程 sdkconfig 是 16ms）。
    //
    // 曾经把它压到 5ms 想"让拖拽更跟手"，结果是把**滑动手势弄坏了**（要连续滑好几次
    // 才会生效）。原因在 lv_indev.c:1358 和 1787-1795：
    //
    //     vect = 本次读数 - 上次读数;                      // ← 每次**采样**的位移
    //     if (|vect| < gesture_min_velocity) gesture_sum = 0;   // 默认 3px
    //     gesture_sum += vect;
    //     if (|gesture_sum| > gesture_min_distance) 发 LV_EVENT_GESTURE;  // 默认 50px
    //
    // 那个 3px 的门限是**按单次采样**判的，而累计量一旦某次采样没跟上就被清零。于是
    // 采样周期一缩短，同样的手速分摊到每次采样上就变小，判定门限等于被抬高：
    //     16ms / 3px ≈ 187 px/s        （默认：正常滑动就够）
    //      5ms / 3px ≈ 600 px/s        （压到 5ms 后的实际门槛）
    // 正常速度的滑动每次采样只动 1~2px，gesture_sum 被反复清零 —— 越滑越不出手势。
    // （页面切换在本工程里**只**靠 LV_EVENT_GESTURE，没有跟着手指走的滚动动画，
    //   所以手势不出就等于"滑了没反应"，而不是"滑得慢"。）
    //
    // 真要动这个周期，必须同时按比例下调 gesture_min_velocity
    // （lv_indev_set_gesture_min_velocity），并且先确认触摸芯片自己的上报率 ——
    // 芯片若只有 60Hz，采样压到 5ms 也只是反复读同一个坐标，白付 I2C 和 CPU。
    // 这几点都没实测过之前，保持默认。

    lvgl_port_unlock();

    ESP_LOGI(TAG, "触摸已注册为 LVGL 指针输入（%dx%d，1:1 不缩放）", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return ESP_OK;
}

void LcdTouch::ReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<LcdTouch*>(lv_indev_get_user_data(indev));
    if (self == nullptr) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint16_t x = 0;
    uint16_t y = 0;
    bool pressed = false;
    if (!self->Read(&x, &y, &pressed)) {
        /* 一次读失败 = **这一帧什么都没观测到**，它不等于"手指抬起来了"。
           这里一度是直接报 RELEASED（注释还写着"宁可丢一帧点击"），代价却远不止
           丢一帧 —— LVGL 的 release 路径（indev/lv_indev.c:1476-1487）会
           wait_until_release 清零、act_obj 置 NULL，手指明明还按着却已经被当成松开：

             • 按下的是按钮时，:1521-1531 会补一个 RELEASED，并且只要没进入滚动
               （scroll_obj == NULL）就再补一个 **CLICKED** —— 拖拽途中凭空点开
               下一级页面；
             • 下一帧又读到 PRESSED，那是一次**全新按压**：act_obj 重新搜索
               （:1263-1272）、gesture_sent 清零（:1326）、gesture_sum 归零，
               手势探测器被重新武装，于是手指每再走 50px（gesture_min_distance）
               就**再发一次 LV_EVENT_GESTURE**（:1797）。

           两者交替就是用户看到的："按着滑动屏幕一直闪""只左滑了一次却翻了两页"。
           又因为失败是偶发的（I2C 抖动；整屏重绘 22~41ms 期间总线/CPU 被挤），
           症状是"经常"而不是"每次"，且越重的页面越容易中招 —— 这正好对上
           "主菜单界面不闪了，但是二级三级菜单还是会闪"（主菜单的 item_event 有
           s_menu_dragged 兜住假 CLICKED，二级页的 item_event/group_item_event 没有）。

           正确做法：**把这一帧当作没采样**，沿用上一次的状态和坐标。只有连续失败
           超过 TOUCH_HOLD_LAST_MS 才认账（触摸芯片/总线真挂了要能自己爬出来，
           这是原来那句注释真正担心的"卡在按下态"）。 */
        if (self->last_state_ != LV_INDEV_STATE_PRESSED) {
            /* 本来就没按着，读失败和"没按"是一回事。 */
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        const uint32_t now = lv_tick_get();
        if (self->fail_since_ms_ == 0) self->fail_since_ms_ = now;
        if ((uint32_t)(now - self->fail_since_ms_) < TOUCH_HOLD_LAST_MS) {
            data->point.x = self->last_point_.x;
            data->point.y = self->last_point_.y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            /* 每个"卡住"的回合只可能走到这里一次：下面把 last_state_ 置成
               RELEASED 之后，直到某次读成功之前都会从上面那个分支直接 return。 */
            ESP_LOGW(TAG, "读触摸连续失败 %ums，按抬手处理", (unsigned)TOUCH_HOLD_LAST_MS);
            self->last_state_ = LV_INDEV_STATE_RELEASED;
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    self->fail_since_ms_ = 0;

    if (pressed) {
        self->release_since_ms_ = 0;
        self->last_point_.x = x;
        self->last_point_.y = y;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (self->last_state_ == LV_INDEV_STATE_PRESSED) {
        /* 上一帧还按着，这一帧芯片说没按 —— 先别急着认账，理由见
           TOUCH_RELEASE_DEBOUNCE_MS。 */
        const uint32_t now = lv_tick_get();
        if (self->release_since_ms_ == 0) self->release_since_ms_ = now;
        if ((uint32_t)(now - self->release_since_ms_) < TOUCH_RELEASE_DEBOUNCE_MS) {
            data->point.x = self->last_point_.x;
            data->point.y = self->last_point_.y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            /* 确认是真抬手了。清掉起点，之后每帧都直接走下面那条 else。 */
            self->release_since_ms_ = 0;
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    self->last_state_ = data->state;
}
