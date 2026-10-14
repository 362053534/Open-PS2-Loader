#ifndef __RENDERMAN_H
#define __RENDERMAN_H

/*
 * Rendering using renderman
 *
 * Resolution
 *   The resolution has been standardized to 640x480. This is the only resolution
 *   that was compatible with legacy themes and used square pixels. All other
 *   resolutions are scaled by renderman.
 *
 * Widescreen (anamorphic)
 *   Widescreen also uses the standard 640x480 resolution, but is scaled
 *   horizontally. Use rmWideScale to scale any x/width value.
 */

/// DTV 576 Progressive Scan (720x576). Not available in ROM v1.70 and earlier.
#define GS_MODE_DTV_576P 0x53

#define DIM_UNDEF -1

#define ALIGN_TOP     (0 << 0)
#define ALIGN_BOTTOM  (1 << 0)
#define ALIGN_VCENTER (2 << 0)
#define ALIGN_LEFT    (0 << 2)
#define ALIGN_RIGHT   (1 << 2)
#define ALIGN_HCENTER (2 << 2)
#define ALIGN_NONE    (ALIGN_TOP | ALIGN_LEFT)
#define ALIGN_CENTER  (ALIGN_VCENTER | ALIGN_HCENTER)

#define SCALING_NONE  0
#define SCALING_RATIO 1

/// GSKit CLUT base struct. This should've been in gsKit from the start :)
typedef struct
{
    u8 PSM;       ///< Pixel Storage Method (Color Format)
    u8 ClutPSM;   ///< CLUT Pixel Storage Method (Color Format)
    u32 *Clut;    ///< EE CLUT Memory Pointer
    u32 VramClut; ///< GS VRAM CLUT Memory Pointer
} GSCLUT;

typedef struct
{
    float x, y;
    float u, v;
} rm_tx_coord_t;

typedef struct
{
    rm_tx_coord_t ul;
    rm_tx_coord_t br;
    u64 color;
    GSTEXTURE *txt;
} rm_quad_t;

// Some convenience globals
extern const u64 gColWhite;
extern const u64 gColBlack;
extern const u64 gColDarker;
extern const u64 gColFocus;
extern const u64 gDefaultCol;
extern const u64 gDefaultAlpha;

enum rm_aratio {
    RM_ARATIO_4_3 = 0,
    RM_ARATIO_16_9,
};

/** Initializes the rendering manager */
void rmInit();

/** Sets a new screen mode */
int rmSetMode(int force);

void rmEnd(void);

/** Fills the parameters with the native screen width and height */
void rmGetScreenExtentsNative(int *w, int *h);

/** Fills the parameters with the virtual (640x480) screen width and height */
void rmGetScreenExtents(int *w, int *h);

/** Invalidate a texture so it will be re-transferred to VRAM the next time.
 * @param txt The texture to invalidate */
void rmInvalidateTexture(GSTEXTURE *txt);

/** Unload texture from texture manager, performance optimization */
void rmUnloadTexture(GSTEXTURE *txt);

void rmDrawQuad(rm_quad_t *q);

/** Queues a specified pixmap (tinted with colour) to be rendered on specified position */
void rmDrawPixmap(GSTEXTURE *txt, int x, int y, short aligned, int w, int h, short scaled, u64 color);
void rmDrawOverlayPixmap(GSTEXTURE *overlay, int x, int y, short aligned, int w, int h, short scaled, u64 color,
                         GSTEXTURE *inlay, int ulx, int uly, int urx, int ury, int blx, int bly, int brx, int bry);

/** 与 rmDrawPixmap() 相同，但会在图像下方额外绘制一段渐隐的镜像倒影。
 *  为 Coverflow 主题元素新增；主图仍复用 rmDrawPixmap() 绘制，
 *  因此常规的 pixmap 绘制路径保持原样不动。 */
void rmDrawPixmapReflect(GSTEXTURE *txt, int x, int y, short aligned, int w, int h, short scaled, u64 color);

/** 与 rmDrawOverlayPixmap() 相同，但会在（inlay + overlay）图像下方额外绘制
 *  一段渐隐的镜像倒影。为 Coverflow 主题元素新增；主图仍复用
 *  rmDrawOverlayPixmap() 绘制。 */
void rmDrawOverlayPixmapReflect(GSTEXTURE *overlay, int x, int y, short aligned, int w, int h, short scaled, u64 color,
                                GSTEXTURE *inlay, int ulx, int uly, int urx, int ury, int blx, int bly, int brx, int bry);

/** Coverflow 专用带外壳绘制（浮点旁路）：x/y/w/h 用 float，内部走浮点版 rmSetupQuadF，
 *  使中心封面放大动画连续变化、消除整数量化蠕动（尤其宽屏）。inlay 顶点以 case quad
 *  实际尺寸按浮点比例(顶点/baseW、顶点/baseH)定位，与 case 内框完全锁定、同步缩放；
 *  宽屏自动一致。ov* 为 overlay 顶点(元素坐标系 0..baseW × 0..baseH)。
 *  仅 Coverflow 主题调用，不影响非 Coverflow 绘制路径。 */
void rmDrawOverlayPixmapFrac(GSTEXTURE *overlay, float x, float y, short aligned, float w, float h, short scaled, u64 color,
                             GSTEXTURE *inlay, int baseW, int baseH,
                             int ovUlx, int ovUly, int ovUrx, int ovUry, int ovBlx, int ovBly, int ovBrx, int ovBry);

/** rmDrawOverlayPixmapFrac 的倒影版（同为浮点旁路）。 */
void rmDrawOverlayPixmapReflectFrac(GSTEXTURE *overlay, float x, float y, short aligned, float w, float h, short scaled, u64 color,
                                    GSTEXTURE *inlay, int baseW, int baseH,
                                    int ovUlx, int ovUly, int ovUrx, int ovUry, int ovBlx, int ovBly, int ovBrx, int ovBry);

/** Queues a opaque rectangle to be rendered */
void rmDrawRect(int x, int y, int w, int h, u64 color);

/** Queues a single color line to be rendered */
void rmDrawLine(int x1, int y1, int x2, int y2, u64 color);

/** Starts the frame - first to call every frame */
void rmStartFrame(void);

/** Ends the frame - last to call every frame */
void rmEndFrame(void);

/** Sets the display offset in units of pixels */
void rmSetDisplayOffset(int x, int y);

/** Set overscan in percentage/10 */
void rmSetOverscan(int overscan);

/** Sets the aspect ratio correction for the upcoming operations.
 * When set, it will treat all pixmap widths/heights (not positions) as scaled with the ratios provided */
void rmSetAspectRatio(enum rm_aratio dar);

/** Widescreen scaling */
int rmWideScale(int x);
/** rmWideScale 的浮点版：横向宽屏压缩、不做整数截断；供 Coverflow 浮点旁路使用。 */
float rmWideScaleF(float x);

/** 把 GS 裁剪框收紧到可见显示区域（过扫补偿后的矩形）；之后排入的图元超出部分被硬件裁掉。 */
void rmSetScissorDisplay(void);
/** 恢复默认裁剪框（整个帧缓冲）。与 rmSetScissorDisplay 成对使用。 */
void rmResetScissor(void);

/** Get Pixel Aspect Ratio of native resolution */
float rmGetPAR();

/** Get interfaced frame mode */
int rmGetInterlacedFrameMode();

/** Scale x from 640 to native resolution */
int rmScaleX(int x);

/** Scale y from 480 to native resolution */
int rmScaleY(int y);

/** Scale x from native to 640 resolution */
int rmUnScaleX(int x);

/** Scale y from native to 480 resolution */
int rmUnScaleY(int y);

// Returns H-sync frequency in KHz
unsigned char rmGetHsync(void);

#endif
