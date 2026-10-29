#include "include/opl.h"
#include "include/textures.h"
#include "include/util.h"
#include "include/ioman.h"
#include <png.h>
#include <libjpg_ps2_addons.h>
#include "texcompact_core.c" // 纯 C 核心并入本翻译单元，artpack 通过链接调用；测试直接编译此文件

/* 核心不包含 gsKit。这里与调用点原来传入的 gsKit_texture_size(w, h, GS_PSM_CT24) 保持一致，测试链接时替换这个符号。 */
unsigned int texCt24TextureSize(int width, int height)
{
    return (unsigned int)gsKit_texture_size(width, height, GS_PSM_CT24);
}
//#include <timer.h>
//#include "include/pad.h"

extern void *load0_png;
extern void *load1_png;
extern void *load2_png;
extern void *load3_png;
extern void *load4_png;
extern void *load5_png;
extern void *load6_png;
extern void *load7_png;
extern void *usb_png; // Leave BDM Icon as usb.png to maintain theme compat
extern void *usb_bd_png;
extern void *ilk_bd_png;
extern void *m4s_bd_png;
extern void *hdd_bd_png;
extern void *hdd_png;
extern void *eth_png;
extern void *app_png;
extern void *Index_0_png;
extern void *Index_1_png;
extern void *Index_2_png;
extern void *Index_3_png;
extern void *Index_4_png;
extern void *cf_index_0_png;
extern void *cf_index_1_png;
extern void *cf_index_2_png;
extern void *cf_index_3_png;
extern void *cf_index_4_png;

extern void *left_png;
extern void *right_png;
extern void *cross_png;
extern void *triangle_png;
extern void *circle_png;
extern void *square_png;
extern void *select_png;
extern void *start_png;
/* currently unused.
extern void *up_png;
extern void *down_png;
extern void *L1_png;
extern void *L2_png;
extern void *L3_png;
extern void *R1_png;
extern void *R2_png;
extern void *R3_png; */

extern void *settings_bg_png;
extern void *info_png;
extern void *cover_png;
//extern void *disc_png;
extern void *screen_png;

extern void *ELF_png;
extern void *HDL_png;
extern void *ISO_png;
extern void *ZSO_png;
extern void *UL_png;
extern void *APPS_png;
extern void *CD_png;
extern void *DVD_png;
extern void *Aspect_s_png;
extern void *Aspect_w_png;
extern void *Aspect_w1_png;
extern void *Aspect_w2_png;
extern void *Device_1_png;
extern void *Device_2_png;
extern void *Device_3_png;
extern void *Device_4_png;
extern void *Device_5_png;
extern void *Device_6_png;
extern void *Device_all_png;
extern void *Rating_0_png;
extern void *Rating_1_png;
extern void *Rating_2_png;
extern void *Rating_3_png;
extern void *Rating_4_png;
extern void *Rating_5_png;
extern void *Scan_240p_png;
extern void *Scan_240p1_png;
extern void *Scan_480i_png;
extern void *Scan_480p_png;
extern void *Scan_480p1_png;
extern void *Scan_480p2_png;
extern void *Scan_480p3_png;
extern void *Scan_480p4_png;
extern void *Scan_480p5_png;
extern void *Scan_576i_png;
extern void *Scan_576p_png;
extern void *Scan_720p_png;
extern void *Scan_1080i_png;
extern void *Scan_1080i2_png;
extern void *Scan_1080p_png;
extern void *Vmode_multi_png;
extern void *Vmode_ntsc_png;
extern void *Vmode_pal_png;

extern void *logo_png;
extern void *case_png;
extern void *apps_case_png;

// Coverflow(内置主题)专用内置贴图, 对齐 wOPL 的 CF 主题资源
extern void *plank_png;
extern void *cf_case_png;
// wOPL 风格设备图标(仅 Coverflow 内置主题使用)
extern void *cf_usb_png;
extern void *cf_usb_bd_png;
extern void *cf_ilk_bd_png;
extern void *cf_m4s_bd_png;
extern void *cf_hdd_bd_png;
extern void *cf_hdd_png;
extern void *cf_eth_png;
extern void *cf_app_png;

// Not related to screen size, just to limit at some point
static int maxSize = 720 * 512 * 4;

// 尝试添加open文件时的临界区
static s32 fileLockId;
static ee_sema_t fileLockSema;

//// 用来计算搜索图片的消耗时间
//static u64 beforeTime = 0;

typedef struct
{
    int id;
    char *name;
    void **texture;
} texture_t;

typedef struct
{
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
} png_clut_t;

typedef struct
{
    png_colorp palette;
    int numPalette;
    int numTrans;
    png_bytep trans;
} png_texture_t;

typedef struct
{
    u8 *data;
    size_t remaining;
} tex_read_buffer_t;

//static png_texture_t pngTexture;

void texInit(void)
{
    fileLockSema.init_count = 1;
    fileLockSema.max_count = 1;
    fileLockSema.option = 0;
    fileLockId = CreateSema(&fileLockSema);
}

void texFinish(void)
{
    DeleteSema(fileLockId);
}

static texture_t internalDefault[TEXTURES_COUNT] = {
    {LOAD0_ICON, "load0", &load0_png},
    {LOAD1_ICON, "load1", &load1_png},
    {LOAD2_ICON, "load2", &load2_png},
    {LOAD3_ICON, "load3", &load3_png},
    {LOAD4_ICON, "load4", &load4_png},
    {LOAD5_ICON, "load5", &load5_png},
    {LOAD6_ICON, "load6", &load6_png},
    {LOAD7_ICON, "load7", &load7_png},
    {BDM_ICON, "usb", &usb_png},
    {USB_ICON, "usb_bd", &usb_bd_png},
    {ILINK_ICON, "ilk_bd", &ilk_bd_png},
    {MX4SIO_ICON, "m4s_bd", &m4s_bd_png},
    {HDD_BD_ICON, "hdd_bd", &hdd_bd_png},
    {HDD_ICON, "hdd", &hdd_png},
    {ETH_ICON, "eth", &eth_png},
    {APP_ICON, "app", &app_png},
    {INDEX_0, "Index_0", &Index_0_png},
    {INDEX_1, "Index_1", &Index_1_png},
    {INDEX_2, "Index_2", &Index_2_png},
    {INDEX_3, "Index_3", &Index_3_png},
    {INDEX_4, "Index_4", &Index_4_png},
    {CF_INDEX_0, "cf_index_0", &cf_index_0_png},
    {CF_INDEX_1, "cf_index_1", &cf_index_1_png},
    {CF_INDEX_2, "cf_index_2", &cf_index_2_png},
    {CF_INDEX_3, "cf_index_3", &cf_index_3_png},
    {CF_INDEX_4, "cf_index_4", &cf_index_4_png},
    {LEFT_ICON, "left", &left_png},
    {RIGHT_ICON, "right", &right_png},
    {CROSS_ICON, "cross", &cross_png},
    {TRIANGLE_ICON, "triangle", &triangle_png},
    {CIRCLE_ICON, "circle", &circle_png},
    {SQUARE_ICON, "square", &square_png},
    {SELECT_ICON, "select", &select_png},
    {START_ICON, "start", &start_png},
    /* currently unused.
    {UP_ICON, "up", &up_png},
    {DOWN_ICON, "down", &down_png},
    {L1_ICON, "L1", &L1_png},
    {L2_ICON, "L2", &L2_png},
    {L3_ICON, "L3", &L3_png},
    {R1_ICON, "R1", &R1_png},
    {R2_ICON, "R2", &R2_png},
    {R3_ICON, "R3", &R3_png}, */
    {SETTINGS_BG, "settings_bg", &settings_bg_png},
    {INFO_BG, "info", &info_png},
    {COVER_DEFAULT, "cover", &cover_png},
    /*{DISC_DEFAULT, "disc", &disc_png},*/
    {SCREEN_DEFAULT, "screen", &screen_png},
    {ELF_FORMAT, "ELF", &ELF_png},
    {HDL_FORMAT, "HDL", &HDL_png},
    {ISO_FORMAT, "ISO", &ISO_png},
    {ZSO_FORMAT, "ZSO", &ZSO_png},
    {UL_FORMAT, "UL", &UL_png},
    {APP_MEDIA, "APP", &APPS_png},
    {CD_MEDIA, "CD", &CD_png},
    {DVD_MEDIA, "DVD", &DVD_png},
    {ASPECT_STD, "Aspect_s", &Aspect_s_png},
    {ASPECT_WIDE, "Aspect_w", &Aspect_w_png},
    {ASPECT_WIDE1, "Aspect_w1", &Aspect_w1_png},
    {ASPECT_WIDE2, "Aspect_w2", &Aspect_w2_png},
    {DEVICE_1, "Device_1", &Device_1_png},
    {DEVICE_2, "Device_2", &Device_2_png},
    {DEVICE_3, "Device_3", &Device_3_png},
    {DEVICE_4, "Device_4", &Device_4_png},
    {DEVICE_5, "Device_5", &Device_5_png},
    {DEVICE_6, "Device_6", &Device_6_png},
    {DEVICE_ALL, "Device_all", &Device_all_png},
    {RATING_0, "Rating_0", &Rating_0_png},
    {RATING_1, "Rating_1", &Rating_1_png},
    {RATING_2, "Rating_2", &Rating_2_png},
    {RATING_3, "Rating_3", &Rating_3_png},
    {RATING_4, "Rating_4", &Rating_4_png},
    {RATING_5, "Rating_5", &Rating_5_png},
    {SCAN_240P, "Scan_240p", &Scan_240p_png},
    {SCAN_240P1, "Scan_240p1", &Scan_240p1_png},
    {SCAN_480I, "Scan_480i", &Scan_480i_png},
    {SCAN_480P, "Scan_480p", &Scan_480p_png},
    {SCAN_480P1, "Scan_480p1", &Scan_480p1_png},
    {SCAN_480P2, "Scan_480p2", &Scan_480p2_png},
    {SCAN_480P3, "Scan_480p3", &Scan_480p3_png},
    {SCAN_480P4, "Scan_480p4", &Scan_480p4_png},
    {SCAN_480P5, "Scan_480p5", &Scan_480p5_png},
    {SCAN_576I, "Scan_576i", &Scan_576i_png},
    {SCAN_576P, "Scan_576p", &Scan_576p_png},
    {SCAN_720P, "Scan_720p", &Scan_720p_png},
    {SCAN_1080I, "Scan_1080i", &Scan_1080i_png},
    {SCAN_1080I2, "Scan_1080i2", &Scan_1080i2_png},
    {SCAN_1080P, "Scan_1080p", &Scan_1080p_png},
    {VMODE_MULTI, "Vmode_multi", &Vmode_multi_png},
    {VMODE_NTSC, "Vmode_ntsc", &Vmode_ntsc_png},
    {VMODE_PAL, "Vmode_pal", &Vmode_pal_png},
    {LOGO_PICTURE, "logo", &logo_png},
    {CASE_OVERLAY, "case", &case_png},
    {APPS_CASE_OVERLAY, "apps_case", &apps_case_png},
    // Coverflow(内置主题)专用资源, 对齐 wOPL 的 CF 主题
    {PLANK_PICTURE, "plank", &plank_png},
    {CF_CASE_OVERLAY, "cf_case", &cf_case_png},
    // wOPL 风格设备图标(384x256, 带 BDM/SMB/APA/APPS 标签), 仅 Coverflow 主题
    {CF_DEV_BDM, "cf_usb", &cf_usb_png},
    {CF_DEV_USB, "cf_usb_bd", &cf_usb_bd_png},
    {CF_DEV_ILK, "cf_ilk_bd", &cf_ilk_bd_png},
    {CF_DEV_M4S, "cf_m4s_bd", &cf_m4s_bd_png},
    {CF_DEV_HDD_BD, "cf_hdd_bd", &cf_hdd_bd_png},
    {CF_DEV_HDD, "cf_hdd", &cf_hdd_png},
    {CF_DEV_ETH, "cf_eth", &cf_eth_png},
    {CF_DEV_APP, "cf_app", &cf_app_png},
};

int texLookupInternalTexId(const char *name)
{
    int i;
    int result = -1;

    for (i = 0; i < TEXTURES_COUNT; i++) {
        if (!strcmp(name, internalDefault[i].name)) {
            result = internalDefault[i].id;
            break;
        }
    }

    return result;
}

static int texSizeValidate(int width, int height, u8 psm)
{
    if (width > 1024 || height > 1024)
        return -1;

    if (gsKit_texture_size(width, height, (int)psm) > maxSize)
        return -1;

    return 0;
}

static void texPrepare(GSTEXTURE *texture)
{
    texture->Width = 0;                              // Must be set by loader
    texture->Height = 0;                             // Must be set by loader
    texture->PSM = GS_PSM_CT24;                      // Must be set by loader
    texture->ClutPSM = 0;                            // Default, can be set by loader
    texture->TBW = 0;                                // gsKit internal value
    texture->Mem = NULL;                             // Must be allocated by loader
    texture->Clut = NULL;                            // Default, can be set by loader
    texture->Vram = 0;                               // VRAM allocation handled by texture manager
    texture->VramClut = 0;                           // VRAM allocation handled by texture manager
    texture->Filter = GS_FILTER_LINEAR;              // Default
    //texture->ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    // Do not load the texture to VRAM directly, only load it to EE RAM
    texture->Delayed = 1;
}

// 大型真彩背景在 CT24/CT32 下会独占纹理池的大部分空间。保留原格式
// 会让 gsKit 在背景、plank、case 和 Coverflow 封面之间反复驱逐/重新上传。
// CT24 与带 alpha 的 CT32 背景都压缩；CT32 的 alpha 被忽略，全部写成不透明
// （原来半透明/透明处透出的只是帧清屏黑色）。失败时不破坏原始纹理，调用方仍可按原路径绘制。
// 8 位颜色转 5 位色不加抖动（抖动会让画面发糊），按最近级四舍五入：
// v5 = (v * 31 + 127) / 255，结果天然在 0~31。与解码阶段流式路径共用 to5 表。
void texCompactBackground(GSTEXTURE *texture)
{
    u16 *pixels16;
    u8 *pixels24;
    size_t pixelCount;
    size_t size;
    int bytesPerPixel;
    int y;

    if (!texture || !texture->Mem || (texture->PSM != GS_PSM_CT24 && texture->PSM != GS_PSM_CT32))
        return;
    if (gsKit_texture_size(texture->Width, texture->Height, texture->PSM) <= TEX_BACKGROUND_COMPACT_THRESHOLD)
        return;

    pixelCount = (size_t)texture->Width * texture->Height;
    size = pixelCount * sizeof(u16);
    pixels16 = memalign(128, size);
    if (!pixels16)
        return;

    // CT24 为 R,G,B；CT32 为 R,G,B,A（A 不读取）。
    bytesPerPixel = (texture->PSM == GS_PSM_CT32) ? 4 : 3;
    pixels24 = (u8 *)texture->Mem;
    for (y = 0; y < texture->Height; y++) {
        texCompactRgbRowToCt16s(pixels16 + (size_t)y * texture->Width,
                                pixels24 + (size_t)y * texture->Width * bytesPerPixel,
                                texture->Width, bytesPerPixel);
    }

    free(texture->Mem);
    // GSTEXTURE stores Mem as a u32 pointer even for 16-bit GS formats.
    texture->Mem = (u32 *)pixels16;
    texture->PSM = GS_PSM_CT16S;
}

/// JPG SUPPORT ///////////////////////////////////////////////////////////////////////////////////////
static int texJpgLoad(GSTEXTURE *texture, const char *filePath)
{
    texPrepare(texture);
    int result = ERR_BAD_FILE;

    // PNG loads already serialize their device access with fileLockId.  Keep
    // JPG discovery under the same lock so theme construction on the main
    // thread cannot race an active BDM/SMB art read in the IO worker.
    WaitSema(fileLockId);
    jpgData *jpg = jpgFromFilename(filePath, JPG_NORMAL);
    SignalSema(fileLockId);
    if (jpg) {
        texture->Width = jpg->width;
        texture->Height = jpg->height;
        texture->PSM = GS_PSM_CT24;
        texture->Mem = jpg->buffer;
        free(jpg);
        result = 0;
    }
    return result;
}

void texFree(GSTEXTURE *texture)
{
    if (texture) {
        if (texture->Mem) {
            //WaitSema(fileLockId);
            free(texture->Mem);
            texture->Mem = NULL;
            //SignalSema(fileLockId);
        }
        if (texture->Clut) {
            //WaitSema(fileLockId);
            free(texture->Clut);
            texture->Clut = NULL;
            //SignalSema(fileLockId);
        }
    }
}


static int texEnd(png_structp pngPtr, png_infop infoPtr, void *pFileBuffer, int status, png_texture_t *pngTexture, GSTEXTURE *texture)
{
    // All failure paths must release buffers allocated after texPrepare().
    // Successful loads pass NULL here because the cache takes ownership of
    // texture->Mem/Clut after this function returns.
    if (texture)
        texFree(texture);

    if (pFileBuffer)
        free(pFileBuffer);

    if (infoPtr || pngPtr)
        png_destroy_read_struct(&pngPtr, &infoPtr, (png_infopp)NULL);

    if (pngTexture)
        free(pngTexture);

    return status;
}

static void texReadMemFunction(png_structp pngPtr, png_bytep data, png_size_t length)
{
    tex_read_buffer_t *readBuffer = (tex_read_buffer_t *)png_get_io_ptr(pngPtr);

    if (!readBuffer || length > readBuffer->remaining) {
        // Do not let malformed/truncated files make libpng read past the
        // compressed file buffer. png_error() transfers control to the
        // setjmp cleanup path in texLoadAll()/texReadData().
        png_error(pngPtr, "PNG input buffer exhausted");
        return;
    }

    memcpy(data, readBuffer->data, length);
    readBuffer->data += length;
    readBuffer->remaining -= length;
}

static void texReadPixels4(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture)
{
    unsigned char *pixel = (unsigned char *)texture->Mem;
    png_clut_t *clut = (png_clut_t *)texture->Clut;
    int i;

    //WaitSema(fileLockId);
    memset(&clut[pngTexture->numPalette], 0, (16 - pngTexture->numPalette) * sizeof(clut[0]));

    for (i = 0; i < pngTexture->numPalette; i++) {
        clut[i].red = pngTexture->palette[i].red;
        clut[i].green = pngTexture->palette[i].green;
        clut[i].blue = pngTexture->palette[i].blue;
        clut[i].alpha = (i < pngTexture->numTrans) ? (pngTexture->trans[i] >> 1) : 0x80;
    }

    for (i = 0; i < texture->Height; i++)
        memcpy(&pixel[i * (texture->Width / 2)], rowPointers[i], texture->Width / 2);

    for (i = 0; i < size; i++)
        pixel[i] = (pixel[i] << 4) | (pixel[i] >> 4);
    //SignalSema(fileLockId);
}

// 填充 8 位调色板（含 GS CSM1 的块交换）；旧路径和直读路径共用
static void texFillClut8(GSTEXTURE *texture, png_texture_t *pngTexture)
{
    png_clut_t *clut = (png_clut_t *)texture->Clut;
    int i;

    memset(&clut[pngTexture->numPalette], 0, (256 - pngTexture->numPalette) * sizeof(clut[0]));

    for (i = 0; i < pngTexture->numPalette; i++) {
        clut[i].red = pngTexture->palette[i].red;
        clut[i].green = pngTexture->palette[i].green;
        clut[i].blue = pngTexture->palette[i].blue;
        clut[i].alpha = (i < pngTexture->numTrans) ? (pngTexture->trans[i] >> 1) : 0x80;
    }

    for (i = 0; i < pngTexture->numPalette; i++) {
        if ((i & 0x18) == 8) {
            png_clut_t tmp = clut[i];
            clut[i] = clut[i + 8];
            clut[i + 8] = tmp;
        }
    }
}

static void texReadPixels8(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture)
{
    unsigned char *pixel = (unsigned char *)texture->Mem;
    int i;

    //WaitSema(fileLockId);
    texFillClut8(texture, pngTexture);

    for (i = 0; i < texture->Height; i++)
        memcpy(&pixel[i * texture->Width], rowPointers[i], texture->Width);
    //SignalSema(fileLockId);
}

static void texReadPixels24(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture)
{
    struct pixel3
    {
        unsigned char r, g, b;
    };
    struct pixel3 *Pixels = (struct pixel3 *)texture->Mem;

    int i, j, k = 0;
    //WaitSema(fileLockId);
    for (i = 0; i < texture->Height; i++) {
        for (j = 0; j < texture->Width; j++) {
            memcpy(&Pixels[k++], &rowPointers[i][4 * j], 3);
        }
    }
    //SignalSema(fileLockId);
}

static void texReadPixels32(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    };
    struct pixel *Pixels = (struct pixel *)texture->Mem;

    int i, j, k = 0;
    //WaitSema(fileLockId);
    for (i = 0; i < texture->Height; i++) {
        for (j = 0; j < texture->Width; j++) {
            memcpy(&Pixels[k], &rowPointers[i][4 * j], 3);
            Pixels[k++].a = rowPointers[i][4 * j + 3] >> 1;
        }
    }
    //SignalSema(fileLockId);
}


// 直读：CT24 和 T8 的 libpng 行数据布局与旧路径写入纹理内存的布局一致（像素紧密排列），
// 逐行直接读进 texture->Mem，省掉整图大小的行缓冲和第二遍拷贝
// 注意：gsKit 按每像素 4 字节计算 CT24 的 EE 内存大小，所以行数据总量可以小于分配大小
static int texReadDirect(GSTEXTURE *texture, png_structp pngPtr, png_infop infoPtr, png_texture_t *pngTexture)
{
    png_size_t rowBytes = png_get_rowbytes(pngPtr, infoPtr);
    size_t size = gsKit_texture_size_ee(texture->Width, texture->Height, texture->PSM);
    unsigned char *dst;
    int row;

    // 行数据总量不能超过分配的纹理内存，否则说明格式判断有误，直接失败
    // （CT24 的分配大小按每像素 4 字节，比紧密排列的 3 字节行数据大，不能要求严格相等）
    if (rowBytes == 0 || texture->Height == 0 || (size_t)rowBytes * (size_t)texture->Height > size) {
        LOG("TEXTURES PngReadDirect: Row layout mismatch\n");
        return -1;
    }

    texture->Mem = memalign(128, size);
    if (!texture->Mem) {
        LOG("TEXTURES PngReadDirect: Failed to allocate %u bytes\n", (unsigned int)size);
        return -1;
    }

    // 解码失败时由调用方统一释放 texture->Mem/Clut
    if (setjmp(png_jmpbuf(pngPtr))) {
        LOG("TEXTURES PngReadDirect: PNG decode failed\n");
        return -1;
    }

    dst = (unsigned char *)texture->Mem;
    for (row = 0; row < texture->Height; row++)
        png_read_row(pngPtr, dst + (size_t)row * rowBytes, NULL);
    png_read_end(pngPtr, NULL);

    if (texture->PSM == GS_PSM_T8)
        texFillClut8(texture, pngTexture);
    return 0;
}

// 流式 CT16S：只分配 w*h*2，用单行缓冲解码并立刻转换，避免 CT24 的整图中间缓冲
static int texReadDirectCt16s(GSTEXTURE *texture, png_structp pngPtr, png_infop infoPtr)
{
    png_size_t rowBytes = png_get_rowbytes(pngPtr, infoPtr);
    size_t size = (size_t)texture->Width * (size_t)texture->Height * sizeof(u16);
    png_bytep volatile rowBuf = NULL;
    u16 *dst;
    int row;

    if (rowBytes == 0 || texture->Width == 0 || texture->Height == 0 ||
        rowBytes < (png_size_t)texture->Width * 3) {
        LOG("TEXTURES PngReadDirectCt16s: Row layout mismatch\n");
        return -1;
    }

    texture->PSM = GS_PSM_CT16S;
    texture->Mem = memalign(128, size);
    if (!texture->Mem) {
        LOG("TEXTURES PngReadDirectCt16s: Failed to allocate %u bytes\n", (unsigned int)size);
        return -1;
    }

    rowBuf = malloc(rowBytes);
    if (!rowBuf) {
        LOG("TEXTURES PngReadDirectCt16s: Failed to allocate row buffer\n");
        return -1;
    }

    // 解码失败时由调用方统一释放 texture->Mem/Clut
    if (setjmp(png_jmpbuf(pngPtr))) {
        free(rowBuf);
        LOG("TEXTURES PngReadDirectCt16s: PNG decode failed\n");
        return -1;
    }

    dst = (u16 *)texture->Mem;
    for (row = 0; row < texture->Height; row++) {
        png_read_row(pngPtr, rowBuf, NULL);
        texCompactRgbRowToCt16s(dst + (size_t)row * texture->Width, rowBuf, texture->Width, 3);
    }
    png_read_end(pngPtr, NULL);
    free(rowBuf);
    return 0;
}

static int texReadData(GSTEXTURE *texture, png_structp pngPtr, png_infop infoPtr,
                       void (*texPngReadPixels)(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture), png_texture_t *pngTexture, int directRead, int streamCompact)
{
    if (streamCompact)
        return texReadDirectCt16s(texture, pngPtr, infoPtr);

    if (directRead)
        return texReadDirect(texture, pngPtr, infoPtr, pngTexture);

    png_size_t rowBytes = png_get_rowbytes(pngPtr, infoPtr);
    size_t size = gsKit_texture_size_ee(texture->Width, texture->Height, texture->PSM);
    size_t rowsSize;
    png_bytep *rowPointers;
    png_bytep allRows;

    if (rowBytes == 0 || texture->Height == 0 || !texPngReadPixels) {
        LOG("TEXTURES PngReadData: Invalid PNG row data\n");
        return -1;
    }

    texture->Mem = memalign(128, size);

    if (!texture->Mem) {
        LOG("TEXTURES PngReadData: Failed to allocate %u bytes\n", (unsigned int)size);
        return -1;
    }

    rowPointers = calloc((size_t)texture->Height, sizeof(*rowPointers));
    if (!rowPointers) {
        LOG("TEXTURES PngReadData: Failed to allocate PNG row pointers\n");
        return -1;
    }

    rowsSize = (size_t)rowBytes * (size_t)texture->Height;
    if (rowsSize / (size_t)texture->Height != (size_t)rowBytes) {
        free(rowPointers);
        LOG("TEXTURES PngReadData: PNG row buffer size overflow\n");
        return -1;
    }

    allRows = malloc(rowsSize);
    if (!allRows) {
        free(rowPointers);
        LOG("TEXTURES PngReadData: Failed to allocate %u bytes for PNG rows\n", (unsigned int)rowsSize);
        return -1;
    }

    for (int row = 0; row < texture->Height; row++)
        rowPointers[row] = &allRows[(size_t)row * rowBytes];

    // libpng reports malformed/truncated data through longjmp. Catch it here
    // so the temporary row buffers are released before returning to texLoadAll.
    if (setjmp(png_jmpbuf(pngPtr))) {
        free(allRows);
        free(rowPointers);
        LOG("TEXTURES PngReadData: PNG decode failed\n");
        return -1;
    }

    png_read_image(pngPtr, rowPointers);
    texPngReadPixels(texture, rowPointers, size, pngTexture);
    png_read_end(pngPtr, NULL);

    free(allRows);
    free(rowPointers);
    return 0;
}

// 忽略 alpha 的 art 后缀（前缀匹配）：这些图都是不透明素材，不需要透明通道
// BG 同时覆盖图包里带序号的 BG_00；COV 同时覆盖 COV2；SCR 同时覆盖 SCR2 和图包里的 SCR_00/SCR_01
#define TEX_DROP_ALPHA_SUFFIX_BG  "BG"
#define TEX_DROP_ALPHA_SUFFIX_COV "COV"
#define TEX_DROP_ALPHA_SUFFIX_LAB "LAB"
#define TEX_DROP_ALPHA_SUFFIX_SCR "SCR"

static const char *const texDropAlphaSuffixes[] = {
    TEX_DROP_ALPHA_SUFFIX_BG,
    TEX_DROP_ALPHA_SUFFIX_COV,
    TEX_DROP_ALPHA_SUFFIX_LAB,
    TEX_DROP_ALPHA_SUFFIX_SCR,
};

#define TEX_DROP_ALPHA_SUFFIX_COUNT (sizeof(texDropAlphaSuffixes) / sizeof(texDropAlphaSuffixes[0]))

int texSuffixDropsAlpha(const char *suffix)
{
    size_t i;

    if (!suffix)
        return 0;
    for (i = 0; i < TEX_DROP_ALPHA_SUFFIX_COUNT; i++) {
        if (!strncmp(suffix, texDropAlphaSuffixes[i], strlen(texDropAlphaSuffixes[i])))
            return 1;
    }
    return 0;
}

static int texLoadAll(GSTEXTURE *texture, const char *filePath, int texId, int dropAlpha, int compactBackground)
{
    texPrepare(texture);
    tex_read_buffer_t readBuffer;
    void *pFileBuffer = NULL;
    size_t fileBufferSize = 0;
    if (filePath) {
        WaitSema(fileLockId);
        int fd = open(filePath, O_RDONLY);
        if (fd < 0) {
            SignalSema(fileLockId);
            return ERR_BAD_FILE;
        }

        int fileSize = lseek(fd, 0, SEEK_END);
        if (fileSize <= 0) {
            close(fd);
            SignalSema(fileLockId);
            return ERR_BAD_FILE;
        }
        lseek(fd, 0, SEEK_SET);
        fileBufferSize = (size_t)fileSize;

        pFileBuffer = malloc(fileBufferSize);
        if (pFileBuffer == NULL) {
            close(fd);
            SignalSema(fileLockId);
            return ERR_BAD_FILE; // There's no out of memory error...
        }

        ssize_t bytesRead = read(fd, pFileBuffer, fileBufferSize);
        if (bytesRead != (ssize_t)fileBufferSize) {
            LOG("texLoadAll: failed to read file %s\n", filePath);
            free(pFileBuffer);
            close(fd);
            SignalSema(fileLockId);
            return ERR_BAD_FILE;
        }
        readBuffer.data = (u8 *)pFileBuffer;
        readBuffer.remaining = fileBufferSize;
        close(fd);
        SignalSema(fileLockId);
    } else {
        if (texId == -1 || !internalDefault[texId].texture)
            return ERR_BAD_FILE;

        // Built-in bin2c assets are trusted and do not expose their size in
        // this table; external files use the bounded path above.
        readBuffer.data = (u8 *)internalDefault[texId].texture;
        readBuffer.remaining = (size_t)-1;
    }

    png_structp pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, (png_voidp)NULL, NULL, NULL);
    if (!pngPtr)
        return texEnd(pngPtr, NULL, pFileBuffer, ERR_READ_STRUCT, NULL, texture);

    png_infop infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr)
        return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_INFO_STRUCT, NULL, texture);

    if (setjmp(png_jmpbuf(pngPtr))) {
        return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_SET_JMP, NULL, texture);
    }

    png_voidp readData = &readBuffer;
    png_rw_ptr readFunction = &texReadMemFunction;
    png_set_read_fn(pngPtr, readData, readFunction);

    unsigned int sigRead = 0;
    png_set_sig_bytes(pngPtr, sigRead);
    png_read_info(pngPtr, infoPtr);

    png_uint_32 pngWidth, pngHeight;
    int bitDepth, colorType, interlaceType;
    png_get_IHDR(pngPtr, infoPtr, &pngWidth, &pngHeight, &bitDepth, &colorType, &interlaceType, NULL, NULL);
    texture->Width = pngWidth;
    texture->Height = pngHeight;

    if (bitDepth == 16)
        png_set_strip_16(pngPtr);

    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA || bitDepth < 4) {
        png_set_expand(pngPtr);
        if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS))
            png_set_tRNS_to_alpha(pngPtr);
    }
    // 灰度图（含灰度加 alpha）转成 RGB（RGBA）后按真彩处理，否则颜色类型仍是灰度，会被当作不支持的格式而加载失败
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pngPtr);

    // 直读条件：非隔行扫描，且最终是 CT24（RGB，或忽略 alpha 的 RGBA）或 8 位调色板 T8
    // 隔行扫描图必须整图解码，其余格式（CT32、T4 等）继续走旧路径
    int directRgb = interlaceType == PNG_INTERLACE_NONE &&
                    (colorType == PNG_COLOR_TYPE_RGB || (colorType == PNG_COLOR_TYPE_RGB_ALPHA && dropAlpha));
    int directRead = directRgb || (interlaceType == PNG_INTERLACE_NONE && colorType == PNG_COLOR_TYPE_PALETTE && bitDepth == 8);
    /* compactBackground 只在 BG 且 Coverflow 时为真。为假时不进入判断，也不算 CT24 大小。 */
    int streamCompact = 0;
    if (compactBackground)
        streamCompact = texShouldStreamCompactBackground((int)pngWidth, (int)pngHeight, colorType, interlaceType, dropAlpha);

    if (directRgb) {
        // 行缓冲直接是每像素 3 字节，不补 filler；RGBA 在解码时就去掉 alpha
        if (colorType == PNG_COLOR_TYPE_RGB_ALPHA)
            png_set_strip_alpha(pngPtr);
    } else {
        png_set_filler(pngPtr, 0xff, PNG_FILLER_AFTER);
    }
    png_read_update_info(pngPtr, infoPtr);

    png_texture_t *pngTexture = calloc(1, sizeof(png_texture_t));
    void (*texPngReadPixels)(GSTEXTURE *texture, png_bytep *rowPointers, size_t size, png_texture_t *pngTexture);
    if (!pngTexture)
        return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_FILE, NULL, texture);

    switch (png_get_color_type(pngPtr, infoPtr)) {
        case PNG_COLOR_TYPE_RGB_ALPHA:
            if (dropAlpha) {
                // 行缓冲本来就是每像素 4 字节（RGB 也被补成 RGBX），按 CT24 只拷前 3 字节即可忽略 alpha
                texture->PSM = GS_PSM_CT24;
                texPngReadPixels = &texReadPixels24;
            } else {
                texture->PSM = GS_PSM_CT32;
                texPngReadPixels = &texReadPixels32;
            }
            break;
        case PNG_COLOR_TYPE_RGB:
            texture->PSM = GS_PSM_CT24;
            texPngReadPixels = &texReadPixels24;
            break;
        case PNG_COLOR_TYPE_PALETTE:
            pngTexture->palette = NULL;
            pngTexture->numPalette = 0;
            pngTexture->trans = NULL;
            pngTexture->numTrans = 0;

            if (!png_get_PLTE(pngPtr, infoPtr, &pngTexture->palette, &pngTexture->numPalette))
                return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_FILE, pngTexture, texture);
            png_get_tRNS(pngPtr, infoPtr, &pngTexture->trans, &pngTexture->numTrans, NULL);
            texture->ClutPSM = GS_PSM_CT32;

            if (bitDepth == 4) {
                if (pngTexture->numPalette < 1 || pngTexture->numPalette > 16)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_DEPTH, pngTexture, texture);

                texture->PSM = GS_PSM_T4;
                texture->Clut = memalign(128, gsKit_texture_size_ee(8, 2, GS_PSM_CT32));
                if (!texture->Clut)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_FILE, pngTexture, texture);
                memset(texture->Clut, 0, gsKit_texture_size_ee(8, 2, GS_PSM_CT32));

                texPngReadPixels = &texReadPixels4;
            } else if (bitDepth == 8) {
                if (pngTexture->numPalette < 1 || pngTexture->numPalette > 256)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_DEPTH, pngTexture, texture);

                texture->PSM = GS_PSM_T8;
                texture->Clut = memalign(128, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));
                if (!texture->Clut)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_FILE, pngTexture, texture);
                memset(texture->Clut, 0, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));

                texPngReadPixels = &texReadPixels8;
            } else
                return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_DEPTH, pngTexture, texture);
            break;
        default:
            return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_DEPTH, pngTexture, texture);
    }

    if (texSizeValidate(texture->Width, texture->Height, texture->PSM) < 0)
        return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_DIMENSION, pngTexture, texture);

    if (texReadData(texture, pngPtr, infoPtr, texPngReadPixels, pngTexture, directRead, streamCompact) < 0)
        return texEnd(pngPtr, infoPtr, pFileBuffer, ERR_BAD_FILE, pngTexture, texture);

    // Successful loads transfer ownership of texture->Mem/Clut to the caller.
    return texEnd(pngPtr, infoPtr, pFileBuffer, 0, pngTexture, NULL);
}

static int texLoad(GSTEXTURE *texture, const char *filePath, int dropAlpha, int compactBackground)
{
    return texLoadAll(texture, filePath, -1, dropAlpha, compactBackground);
}

int texLoadInternal(GSTEXTURE *texture, int texId)
{
    return texLoadAll(texture, NULL, texId, 0, 0);
}

int texDiscoverLoad(GSTEXTURE *texture, const char *path, int texId)
{
    return texDiscoverLoadEx(texture, path, texId, gEnableJpg, 0, 0);
}

// allowJpg 为 0 时只找 png（性能模式），为 1 时 png 找不到再找 jpg（兼容模式）
int texDiscoverLoadEx(GSTEXTURE *texture, const char *path, int texId, int allowJpg, int dropAlpha, int compactBackground)
{
    if (!path)
        return ERR_BAD_FILE;

    char filePath[256];

    LOG("texDiscoverLoad(%s)\n", path);
    //// debug
    // char debugFileDir[64];
    // strcpy(debugFileDir, "smb:debug-texload.txt");
    // FILE *debugFile = fopen(debugFileDir, "ab+");
    // if (debugFile != NULL) {
    //     fprintf(debugFile, "path:%s\r\n", path);
    //     fclose(debugFile);
    // }

    if (texId != -1)
        snprintf(filePath, sizeof(filePath), "%s%s.%s", path, internalDefault[texId].name, "png");
    else
        snprintf(filePath, sizeof(filePath), "%s.%s", path, "png");

    //beforeTime = GetTimerSystemTime(); // 开始搜索图片，记录时间
    if (texLoad(texture, filePath, dropAlpha, compactBackground) >= 0)
        return 0;
    else {
        if (allowJpg) {
            if (texId != -1)
                snprintf(filePath, sizeof(filePath), "%s%s.%s", path, internalDefault[texId].name, "jpg");
            else
                snprintf(filePath, sizeof(filePath), "%s.%s", path, "jpg");

            if (texJpgLoad(texture, filePath) >= 0)
                return 0;
            //else
            //    searchTexTime += (GetTimerSystemTime() - beforeTime) / CLOCKS_PER_MILISEC; // 记录搜索PNG和JPG图片的时间，避免出现光标连续跳2次的问题
        }
        //else
        //    searchTexTime += (GetTimerSystemTime() - beforeTime) / CLOCKS_PER_MILISEC; // 记录搜索PNG图片的时间，避免出现光标连续跳2次的问题
    }
    return ERR_BAD_FILE;
}
