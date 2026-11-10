#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

/**
 * @brief NVS (非易失性存储) 键值对配置管理类
 * 
 * 基于 RAII 模式封装了 ESP-IDF 的 nvs_flash API。
 * 作用：用于跨重启/掉电持久化保存设备的配置参数。
 * 特性：对象创建时自动打开 NVS 命名空间，对象销毁时自动检测是否有修改并 commit（提交），然后安全关闭句柄。
 */
class Settings {
public:
    Settings(const std::string& ns, bool read_write = false);    // 构造函数，打开 NVS 命名空间
    ~Settings();    // 析构函数，自动提交 (若有修改) 并关闭 NVS 句柄

    // --- 数据读取接口 (若键不存在或发生错误，返回 default_value) ---
    std::string GetString(const std::string& key, const std::string& default_value = "");
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    bool GetBool(const std::string& key, bool default_value = false);

    // --- 数据写入接口 (只有 read_write = true 时才生效) ---
    void SetString(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int32_t value);
    void SetBool(const std::string& key, bool value);

    // --- 删除操作 ---
    void EraseKey(const std::string& key);  // 删除指定键
    void EraseAll();                        // 清空当前命名空间下的所有键值对

private:
    std::string ns_;               // 当前操作的命名空间
    nvs_handle_t nvs_handle_ = 0;  // 底层 NVS 句柄
    bool read_write_ = false;      // 是否拥有写权限
    bool dirty_ = false;           // 脏标记：记录数据是否被修改过，用于优化 commit
};

#endif
