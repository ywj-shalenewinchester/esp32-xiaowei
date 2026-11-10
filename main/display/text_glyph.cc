#include "text_glyph.h"

// 通过检查具有 MALLOC_CAP_SPIRAM (PSRAM) 能力的内存总量是否大于 0，来判断硬件环境是否挂载了外部 PSRAM 芯片。
bool TextGlyphStorageUsesPsram() { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0; }
