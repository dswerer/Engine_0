// 统一的 stb 实现 TU：避免各目标在 main.cpp 里重复定义造成符号冲突。
#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image/stb_image_write.h"
