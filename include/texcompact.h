#ifndef __TEXCOMPACT_H
#define __TEXCOMPACT_H

/* 纯 C：CT16S 流式压缩核心，不依赖 libpng / gsKit。
 * 判定函数里的 colorType / interlaceType 与 libpng 的 PNG_COLOR_TYPE_* /
 * PNG_INTERLACE_NONE 数值相同，这样解码器可以直接传入 png_get_IHDR 的结果。 */

#define TEX_BACKGROUND_COMPACT_THRESHOLD (512 * 1024)
/* GS 纹理单边上限（TW/TH 最大 2^10），与 texSizeValidate 一致 */
#define TEX_MAX_TEX_DIM                  1024
#define TEX_COLOR5_MAX                   31

#define TEX_PNG_INTERLACE_NONE     0
#define TEX_PNG_COLOR_TYPE_RGB     2
#define TEX_PNG_COLOR_TYPE_PALETTE 3
#define TEX_PNG_COLOR_TYPE_RGBA    6

#ifdef __cplusplus
extern "C" {
#endif

/* src 为一行紧密排列的真彩像素；srcPixelBytes 为 3（RGB）或 4（RGBA，忽略 alpha）。 */
void texCompactRgbRowToCt16s(unsigned short *dst, const unsigned char *src, int width, int srcPixelBytes);

/* 固件在 textures.c 里实现成 gsKit_texture_size(w, h, GS_PSM_CT24)。
 * 放在核心外，宿主机测试才能不链接 gsKit，并用计数桩确认前置条件失败时不会算大小。 */
unsigned int texCt24TextureSize(int width, int height);

/* 非隔行、RGB 或丢弃 alpha 的 RGBA，且 CT24 大小超过 512K 时返回 1。
 * 大小在其余条件都成立后才通过 texCt24TextureSize 计算。
 * compactBackground 由解码器先判断，非背景图不进入这里。 */
int texShouldStreamCompactBackground(int width, int height, int colorType, int interlaceType, int dropAlpha);

#ifdef __cplusplus
}
#endif

#endif
