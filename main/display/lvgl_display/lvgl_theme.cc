#include "lvgl_theme.h"

LvglTheme::LvglTheme(const std::string& name) : Theme(name) {
}

// 解析颜色字符串为 lv_color_t
// 支持 #112233 格式
lv_color_t LvglTheme::ParseColor(const std::string& color) {
    if (color.find("#") == 0) {
        // Convert #112233 to lv_color_t
        uint8_t r = strtol(color.substr(1, 2).c_str(), nullptr, 16);
        uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);
        uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);
        return lv_color_make(r, g, b);
    }
    return lv_color_black();
}

// LVGL 主题管理器类
LvglThemeManager::LvglThemeManager() {
}

// 获取主题
// 如果主题不存在，返回 nullptr
LvglTheme* LvglThemeManager::GetTheme(const std::string& theme_name) {
    auto it = themes_.find(theme_name);
    if (it != themes_.end()) {
        return it->second;
    }
    return nullptr;
}

// 注册主题
// 如果主题已存在，会覆盖旧的主题
// 注册后，主题管理器会自动管理主题的内存
void LvglThemeManager::RegisterTheme(const std::string& theme_name, LvglTheme* theme) {
    themes_[theme_name] = theme;
}
