#pragma once

#include <esp_heap_caps.h>  // ESP-IDF 特有的堆内存能力分配库（管理 SRAM 和 PSRAM）

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <vector>

// 用于检查当前硬件是否启用了 PSRAM (片外 SPI RAM)
bool TextGlyphStorageUsesPsram();

// ================= 自定义内存分配器 (C++ Allocator 模板) =================
// 专门用于让 std::vector 使用 ESP32 的特定内存区域
template <typename T>
class TextGlyphAllocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;     // 标记此分配器是无状态的

    TextGlyphAllocator() noexcept = default;

    template <typename U>
    TextGlyphAllocator(const TextGlyphAllocator<U>&) noexcept {}

    // 核心函数：分配内存
    T* allocate(size_t count) {
        if (count == 0) {
            return nullptr;
        }
        // 防止计算申请大小时发生整数溢出
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            std::abort();
        }
        // 默认分配策略：MALLOC_CAP_INTERNAL (内部高速 SRAM) | MALLOC_CAP_8BIT (支持字节对齐)
        uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        // 如果系统有 PSRAM，就把内存分配到 PSRAM 中，节省珍贵的内部 RAM
        if (TextGlyphStorageUsesPsram()) {
            caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        }
        // 使用 ESP-IDF 提供的 API 按指定的能力 (caps) 分配内存
        auto ptr = static_cast<T*>(heap_caps_malloc(count * sizeof(T), caps));
        // 如果分配失败，直接终止程序 (嵌入式系统中通常禁用 C++ 异常，所以直接 abort)
        if (ptr == nullptr) {
            std::abort();
        }
        return ptr;
    }

    // 核心函数：释放内存
    void deallocate(T* ptr, size_t) noexcept { heap_caps_free(ptr); }
};

// 运算符重载：判断两个分配器是否相等
template <typename T, typename U>
bool operator==(const TextGlyphAllocator<T>&, const TextGlyphAllocator<U>&) {
    return true;
}

template <typename T, typename U>
bool operator!=(const TextGlyphAllocator<T>&, const TextGlyphAllocator<U>&) {
    return false;
}

// ================= 字形数据结构 =================
// 定义一个类型别名，这是一个使用了上述自定义分配器的 std::vector
template <typename T>
using TextGlyphVector = std::vector<T, TextGlyphAllocator<T>>;

// 描述一个字符（字形）的完整结构
struct TextGlyph {
    uint32_t codepoint = 0; // Unicode 码位 (例如 'A' 或 '中' 对应的数字)
    uint32_t adv_w = 0;     // Advance width：绘制下一个字符前，光标需要水平移动的距离
    uint16_t box_w = 0;     // 字符包围盒的宽度（实际像素宽度）
    uint16_t box_h = 0;     // 字符包围盒的高度（实际像素高度）
    int16_t ofs_x = 0;      // X 轴偏移量（用于排版对齐）
    int16_t ofs_y = 0;      // Y 轴偏移量（用于排版对齐，处理基于基线的偏移）
    
    // 字符的像素位图数据。因为使用了 TextGlyphVector，
    // 这个数组的内存会自动分配到 PSRAM（如果有的话）。
    TextGlyphVector<uint8_t> bitmap; 
};
