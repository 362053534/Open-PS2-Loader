#include "texcompact.h"

/* 8 位转 5 位的最近级映射：texTo5[v] = (v * 31 + 127) / 255。
 * 预先算好写成常量表，避免懒初始化带来的多线程写入顺序隐患；
 * 表内容由脚本生成，测试会逐项与公式核对。 */
static const unsigned char texTo5[256] = {
     0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,
     2,  2,  2,  2,  2,  3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,
     4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  5,  5,  6,  6,
     6,  6,  6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  7,  7,  8,  8,
     8,  8,  8,  8,  8,  8,  9,  9,  9,  9,  9,  9,  9,  9,  9, 10,
    10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 12,
    12, 12, 12, 12, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15,
    16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19,
    19, 20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21,
    21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23,
    23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25,
    25, 25, 26, 26, 26, 26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27,
    27, 27, 27, 28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29,
    29, 29, 29, 30, 30, 30, 30, 30, 30, 30, 30, 31, 31, 31, 31, 31,
};

void texCompactRgbRowToCt16s(unsigned short *dst, const unsigned char *src, int width, int srcPixelBytes)
{
    int x;

    if (!dst || !src || width <= 0 || srcPixelBytes < 3)
        return;

    for (x = 0; x < width; x++) {
        int red = texTo5[src[0]];
        int green = texTo5[src[1]];
        int blue = texTo5[src[2]];

        /* CT16S：不透明 A1 B5 G5 R5。alpha 有意丢弃。 */
        dst[x] = (unsigned short)(0x8000 | (blue << 10) | (green << 5) | red);
        src += srcPixelBytes;
    }
}

int texShouldStreamCompactBackground(int width, int height, int colorType, int interlaceType, int dropAlpha)
{
    unsigned int ct24Bytes;

    /* 异常尺寸先挡掉：1024 与 texSizeValidate / ART_PACK_MAX_TEX_DIM 的 GS 单边上限一致，
     * 超出的图后面一定会被拒绝；提前返回也避免畸形 IHDR 让 gsKit 的大小计算溢出。 */
    if (width <= 0 || height <= 0 || width > TEX_MAX_TEX_DIM || height > TEX_MAX_TEX_DIM)
        return 0;
    if (interlaceType != TEX_PNG_INTERLACE_NONE)
        return 0;
    if (colorType != TEX_PNG_COLOR_TYPE_RGB && !(colorType == TEX_PNG_COLOR_TYPE_RGBA && dropAlpha))
        return 0;
    /* 大小放在最后：格式或隔行已经否决时不再调用 gsKit。 */
    ct24Bytes = texCt24TextureSize(width, height);
    if (ct24Bytes <= TEX_BACKGROUND_COMPACT_THRESHOLD)
        return 0;
    return 1;
}
