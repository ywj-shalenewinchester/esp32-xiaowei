/*
 * xiaowei_ui_images.c —— 见 xiaowei_ui_images.h 的说明。
 *
 * 为什么读端是手写的，而不是用 esp_mmap_assets（emote 组件用的就是它）：
 *   1) 容器格式是**本仓库自己生成**的（scripts/build_default_assets.py 的
 *      pack_assets_simple()），格式定义就在本地，解析只有几十行；
 *   2) 手写才能做**逐张的尺寸校验**：把容器里的 size 和描述符的 data_size 比一遍，
 *      不对就只让那一张退化成黑图。库的接口只给"整包校验和"这一级粒度，
 *      真出错时表现为某张图花屏，比直接报错难查得多；
 *   3) zero 新增依赖：esp_partition_mmap 来自 spi_flash，本来就在 main 的
 *      PRIV_REQUIRES 里。
 */

#include "xiaowei_ui_images.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "lvgl/lvgl.h"

#include "assets/xiaowei_assets.h"

static const char *TAG = "xw_img";

/* ── 容器格式 ────────────────────────────────────────────────────────────────
 * 与 scripts/build_default_assets.py 的 pack_assets_simple() 对偶，
 * 也和 xiaozhi 自己 assets 分区用的是同一套（那边的读端是 main/assets.cc
 * 的 LvglStrategy::InitializePartition）。布局：
 *
 *   0x00  u32  文件数
 *   0x04  u32  校验和（0x0C 起、长 len 的逐字节累加 & 0xFFFF）
 *   0x08  u32  len = 表 + 负载 的总字节数
 *   0x0C  表：文件数 × 表项，表项 44 字节：
 *           char name[32] | u32 size | u32 offset | u16 width | u16 height
 *         其中 offset 是相对**负载区起点**的偏移。
 *   负载区：0x0C + 44*文件数 起。每个文件是 2 字节 0x5A5A（"ZZ" 魔数）+ 真数据；
 *          pack_assets_simple 会把每个文件的起始偏移补到 (offset+2)%4==0，
 *          **所以交出去的 data 指针是 4 字节对齐的**（466x466 的 RGB565 位图
 *          会被 LVGL 按 uint32 读，这条对齐不能省）。
 * ─────────────────────────────────────────────────────────────────────────── */

#define IMG_PARTITION_LABEL "uiimg"
#define IMG_NAME_LEN        32
#define IMG_TABLE_ENTRY_LEN (IMG_NAME_LEN + 4 + 4 + 2 + 2) /* 44 */
#define IMG_HEADER_LEN      12
#define IMG_ZZ_LEN          2
#define IMG_MAX_FILES       64 /* 表项数上限，纯防御：脏数据不至于让我们越界读 */

typedef struct {
    const char *name;      /* uiimg 容器里的文件名，也是查表的 key */
    lv_image_dsc_t *dsc;   /* 要填 .data 的那个描述符 */
} img_binding_t;

/* ⚠ 名字必须与 scripts/externalize_ui_images.py 里的 TARGETS 一致，
 *   改一边就要改另一边。 */
static const img_binding_t s_bindings[] = {
    { "wallpaper_img1.bin", &img_wallpaper_img1 },
    { "wallpaper_img2.bin", &img_wallpaper_img2 },
    { "wallpaper_img3.bin", &img_wallpaper_img3 },
};

#define IMG_BINDING_COUNT (sizeof(s_bindings) / sizeof(s_bindings[0]))

/* 绑不上时用的零填充缓冲。只分配一次，大小取所有图里最大的那个
 * max(data_size, h*stride) —— h*stride 才是 LVGL 真正会读到的字节数。 */
static void *s_black_buf;
static size_t s_black_buf_size;

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v)); /* 表项里的 u32 是对齐的，但不值得为此假设 */
    return v;
}

static uint32_t container_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum & 0xFFFF;
}

/* 把某个描述符退化成"黑图"。
 *
 * 为什么不干脆留 .data = NULL：LVGL 拿 dsc->data 当像素基址直接读，
 * NULL 就是空指针解引用。而且描述符的 w/h/stride/cf 还得和数据一致 ——
 * 只把 data 换掉、尺寸不改，只要缓冲够大就一定是安全的读。
 * 所以挑一块 >= 所有图尺寸的零填充缓冲，谁失败谁指过来。 */
static void use_black_fallback(lv_image_dsc_t *dsc)
{
    const size_t need = (size_t)dsc->header.h * dsc->header.stride;

    if (s_black_buf == NULL) {
        size_t max_need = 0;
        for (size_t i = 0; i < IMG_BINDING_COUNT; i++) {
            const lv_image_dsc_t *d = s_bindings[i].dsc;
            size_t n = d->data_size;
            size_t span = (size_t)d->header.h * d->header.stride;
            if (span > n) {
                n = span;
            }
            if (n > max_need) {
                max_need = n;
            }
        }
        max_need = (max_need + 0xFFF) & ~(size_t)0xFFF; /* 留一页余量 */
        s_black_buf = heap_caps_malloc(max_need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_black_buf == NULL) {
            s_black_buf = heap_caps_malloc(max_need, MALLOC_CAP_8BIT);
        }
        if (s_black_buf == NULL) {
            ESP_LOGE(TAG, "no memory for the %u-byte fallback buffer", (unsigned)max_need);
            return; /* 实在没有，只能保持 NULL 并已经打过 ERROR 了 */
        }
        memset(s_black_buf, 0, max_need);
        s_black_buf_size = max_need;
    }

    if (s_black_buf_size >= need) {
        dsc->data = s_black_buf;
    }
}

static void bind_all_to_fallback(void)
{
    for (size_t i = 0; i < IMG_BINDING_COUNT; i++) {
        use_black_fallback(s_bindings[i].dsc);
    }
}

void xiaowei_ui_bind_external_assets(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, IMG_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition \"%s\" not found - check the partition table", IMG_PARTITION_LABEL);
        bind_all_to_fallback();
        return;
    }

    const void *root = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &root, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap of \"%s\" (%u KB) failed: %s", IMG_PARTITION_LABEL,
                 (unsigned)(part->size / 1024), esp_err_to_name(err));
        bind_all_to_fallback();
        return;
    }

    const uint8_t *base = (const uint8_t *)root;
    const uint32_t files = rd_u32(base + 0);
    const uint32_t stored_sum = rd_u32(base + 4);
    const uint32_t len = rd_u32(base + 8);

    if (files == 0 || files > IMG_MAX_FILES || len > part->size - IMG_HEADER_LEN) {
        ESP_LOGE(TAG, "bad \"%s\" header: %u files, len %u, partition %u B", IMG_PARTITION_LABEL,
                 (unsigned)files, (unsigned)len, (unsigned)part->size);
        goto fail;
    }

    if (container_checksum(base + IMG_HEADER_LEN, len) != stored_sum) {
        ESP_LOGE(TAG, "\"%s\" checksum mismatch - the partition is corrupt or partly flashed",
                 IMG_PARTITION_LABEL);
        goto fail;
    }

    {
        const uint8_t *table = base + IMG_HEADER_LEN;
        const uint8_t *payload = table + (size_t)IMG_TABLE_ENTRY_LEN * files;
        uint32_t bound = 0;

        for (size_t i = 0; i < IMG_BINDING_COUNT; i++) {
            const img_binding_t *b = &s_bindings[i];
            bool found = false;

            for (uint32_t j = 0; j < files; j++) {
                const uint8_t *entry = table + (size_t)IMG_TABLE_ENTRY_LEN * j;
                if (strncmp((const char *)entry, b->name, IMG_NAME_LEN) != 0) {
                    continue;
                }

                const uint32_t size = rd_u32(entry + IMG_NAME_LEN);
                const uint32_t offset = rd_u32(entry + IMG_NAME_LEN + 4);

                if (size != b->dsc->data_size) {
                    ESP_LOGE(TAG, "%s: container has %u B but the descriptor expects %u B",
                             b->name, (unsigned)size, (unsigned)b->dsc->data_size);
                    break;
                }
                /* 负载区必须在容器内，且容得下 ZZ 魔数 */
                if (offset > len - IMG_ZZ_LEN ||
                    (size_t)offset + IMG_ZZ_LEN + size > len) {
                    ESP_LOGE(TAG, "%s: offset %u + %u B runs past the payload (%u B)",
                             b->name, (unsigned)offset, (unsigned)size, (unsigned)len);
                    break;
                }

                const uint8_t *data = payload + offset;
                if (data[0] != 'Z' || data[1] != 'Z') {
                    ESP_LOGE(TAG, "%s: missing ZZ magic (got %02x %02x)", b->name,
                             (unsigned)data[0], (unsigned)data[1]);
                    break;
                }

                b->dsc->data = data + IMG_ZZ_LEN;
                bound++;
                found = true;
                break;
            }

            if (!found) {
                ESP_LOGE(TAG, "%s: not found in the \"%s\" container", b->name, IMG_PARTITION_LABEL);
                use_black_fallback(b->dsc);
            }
        }

        ESP_LOGI(TAG, "bound %u/%u external images from \"%s\" (%u KB)",
                 (unsigned)bound, (unsigned)IMG_BINDING_COUNT, IMG_PARTITION_LABEL,
                 (unsigned)(part->size / 1024));
        if (bound != IMG_BINDING_COUNT) {
            return; /* 已经逐张打过 ERROR，失败的走黑图兜底 */
        }

        /* 全部绑上之后不需要再 munmap：data 指针要在整个运行期有效。
         * handle 是有意不释放的（它是常驻映射，不是泄漏）。 */
        (void)handle;
        return;
    }

fail:
    /* 走到这里说明容器整体不可信，逐张退化成黑图。
     * 映射本身留着不释放 —— 黑图缓冲是另一块内存，与它无关。 */
    bind_all_to_fallback();
}
