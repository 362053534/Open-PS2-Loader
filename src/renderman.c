/*
  Copyright 2010, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include <stdio.h>
#include <kernel.h>

#include "include/opl.h"
#include "include/renderman.h"
#include "include/ioman.h"
#include "include/debugdiag.h"

// Allocateable space in vram, as indicated in GsKit's code
#define __VRAM_SIZE 4194304

GSGLOBAL *gsGlobal;
s32 guiThreadID;

static int order;
static short int vmode = -1;
static u8 hires = 0;
// hires 的 gsKit pass 使用局部 scissor，无法在一个共享的 draw queue 中表达
// 每个 pass 不同的 Y 偏移。因此 Coverflow 在 hires 下改用图元级裁剪；普通
// 非 Coverflow 路径不启用此状态。
static u8 rmClipToDisplay = 0;
static u8 guiWakeupCount;
static int vsync_id = -1;

#define NUM_RM_VMODES 14
#define RM_VMODE_AUTO 0

// RM Vmode -> GS Vmode conversion table
struct rm_mode
{
    char mode;
    char hsync; // In KHz
    short int width;
    short int height;
    short int passes;
    short int VCK;
    short int interlace;
    short int field;
    short int aratio;
    short int PAR1; // Pixel Aspect Ratio 1 (For video modes with non-square pixels, like PAL/NTSC)
    short int PAR2; // Pixel Aspect Ratio 2 (For video modes with non-square pixels, like PAL/NTSC)
};

// clang-format off
static struct rm_mode rm_mode_table[NUM_RM_VMODES] = {
    // 24 bit color mode with black borders
    {-1,                 16,  640,   -1,  1, 4, GS_INTERLACED,    GS_FIELD, RM_ARATIO_4_3, -1, 15}, // AUTO
    {GS_MODE_PAL,        16,  640,  512,  1, 4, GS_INTERLACED,    GS_FIELD, RM_ARATIO_4_3, 16, 15}, // PAL@50Hz
    {GS_MODE_NTSC,       16,  640,  448,  1, 4, GS_INTERLACED,    GS_FIELD, RM_ARATIO_4_3, 14, 15}, // NTSC@60Hz
    {GS_MODE_DTV_480P,   31,  640,  448,  1, 2, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3, 14, 15}, // DTV480P@60Hz
    {GS_MODE_DTV_576P,   31,  640,  512,  1, 2, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3, 16, 15}, // DTV576P@50Hz
    {GS_MODE_VGA_640_60, 31,  640,  480,  1, 2, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3,  1,  1}, // VGA640x480@60Hz
    // 24 bit color mode full screen, multi-pass (2 passes, HIRES)
    {GS_MODE_PAL,        16,  704,  576,  2, 4, GS_INTERLACED,    GS_FIELD, RM_ARATIO_4_3, 12, 11}, // PAL@50Hz
    {GS_MODE_NTSC,       16,  704,  480,  2, 4, GS_INTERLACED,    GS_FIELD, RM_ARATIO_4_3, 10, 11}, // NTSC@60Hz
    {GS_MODE_DTV_480P,   31,  704,  480,  2, 2, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3, 10, 11}, // DTV480P@60Hz
    {GS_MODE_DTV_576P,   31,  704,  576,  2, 2, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3, 12, 11}, // DTV576P@50Hz
    // 16 bit color mode full screen, multi-pass (3 passes, HIRES)
    {GS_MODE_DTV_720P,   31, 1280,  720,  3, 1, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_16_9, 1,  1}, // HDTV720P@60Hz
    {GS_MODE_DTV_1080I,  31, 1920, 1080,  3, 1, GS_INTERLACED,    GS_FRAME, RM_ARATIO_16_9, 1,  1}, // HDTV1080I@60Hz
    // 24 bit color mode for older systems (non-interlaced, low resolution)
    {GS_MODE_PAL,        16,  640,  256,  1, 4, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3,  8, 15}, // PAL@50Hz
    {GS_MODE_NTSC,       16,  640,  224,  1, 4, GS_NONINTERLACED, GS_FRAME, RM_ARATIO_4_3,  7, 15}, // NTSC@60Hz
};
// clang-format on

// Display Aspect Ratio
static int iAspectWidth = 4;
static enum rm_aratio DAR = RM_ARATIO_4_3;

// Display dimensions after overscan compensation
static int iDisplayWidth;
static int iDisplayHeight;
static int iDisplayXOff;
static int iDisplayYOff;

// Transposition values including overscan compensation
static float fRenderXOff = 0.0f;
static float fRenderYOff = 0.0f;

const u64 gColWhite = GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x80);  // Alpha 0x80 -> solid white
const u64 gColBlack = GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x80);  // Alpha 0x80 -> solid black
const u64 gColDarker = GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x60); // Alpha 0x60 -> transparent overlay color
const u64 gColFocus = GS_SETREG_RGBA(0xFF, 0xFF, 0xFF, 0x50);  // Alpha 0x50 -> transparent overlay color

const u64 gDefaultCol = GS_SETREG_RGBA(0x80, 0x80, 0x80, 0x80); // Special color for texture multiplication
const u64 gDefaultAlpha = GS_SETREG_ALPHA(0, 1, 0, 1, 0);

// ---------------------------------------------------------------------------
// HIRES_PASS_DIAG：hires 中间横条花屏诊断（只可视化 + 只日志，不改渲染/缓存行为）。
// 1) rmEndFrame 的 hires 分支在 gsKit_hires_sync 之前，用帧缓冲坐标画出 pass 分界线
//    （洋红色，2 行高：分界线上方 pass 的最后一行 + 下方 pass 的第一行）。
// 2) 所有 gsKit_TexManager_bind 改走 rmTexBind()，hires 下记录每次绑定的纹理、显存
//    范围与是否上传；本帧一次上传覆盖了本帧更早绑定、且当时未上传的纹理时输出
//    [HIRES_ALIAS]，帧末有上传/重叠/溢出时输出 [HIRES_TEX]（LOG 仅调试构建有输出）。
// 诊断结束后把下面的 1 改为 0（或编译时 -DHIRES_PASS_DIAG=0）即可完全关闭。
#ifndef HIRES_PASS_DIAG
#define HIRES_PASS_DIAG 0
#endif

#if HIRES_PASS_DIAG
#define HIRES_DIAG_MAX_BINDS        128
#define HIRES_DIAG_MAX_ALIAS_LOGS   8

typedef struct
{
    GSTEXTURE *tex;
    u32 vram;     // tex->Vram（绑定后）
    u32 tsize;    // 纹理本体字节数（与 gsKit_TexManager_bind 相同算法）
    u32 vramClut; // tex->VramClut（绑定后，无 CLUT 时为 0）
    u32 csize;    // CLUT 字节数（无 CLUT 时为 0）
    u16 width;
    u16 height;
    u8 psm;
    u8 uploaded; // gsKit_TexManager_bind 返回值非 0：本次在队列里插入了上传
} hires_diag_bind_t;

static hires_diag_bind_t hiresDiagBinds[HIRES_DIAG_MAX_BINDS];
static int hiresDiagBindCount = 0;
static u32 hiresDiagBindDropped = 0;
static u32 hiresDiagUploads = 0;
static u32 hiresDiagUploadBytes = 0;
static u32 hiresDiagAliasCount = 0;
static u32 hiresDiagAliasLogged = 0;
static u32 hiresDiagFrame = 0;

static int hiresDiagRangesOverlap(u32 a0, u32 aSize, u32 b0, u32 bSize)
{
    if (aSize == 0 || bSize == 0)
        return 0;
    return (a0 < b0 + bSize) && (b0 < a0 + aSize);
}

// 早先的绑定 i 在其之前是否已在本帧、同一地址上传过同一纹理（若是，则 3 个 pass
// 回放时该纹理在被覆盖前总会先被重新上传，不构成跨 pass 别名）。
static int hiresDiagUploadedEarlier(int i)
{
    int j;
    for (j = 0; j < i; j++) {
        if (hiresDiagBinds[j].tex == hiresDiagBinds[i].tex && hiresDiagBinds[j].uploaded &&
            hiresDiagBinds[j].vram == hiresDiagBinds[i].vram)
            return 1;
    }
    return 0;
}

static void hiresDiagRecordBind(GSTEXTURE *tex, unsigned int uploaded)
{
    hires_diag_bind_t *cur;
    u32 csize = 0;
    int i;

    if (hiresDiagBindCount >= HIRES_DIAG_MAX_BINDS) {
        hiresDiagBindDropped++;
        return;
    }

    if (tex->Clut != NULL) {
        int cwidth = (tex->PSM == GS_PSM_T8) ? 16 : 8;
        int cheight = (tex->PSM == GS_PSM_T8) ? 16 : 2;
        csize = gsKit_texture_size(cwidth, cheight, tex->ClutPSM);
    }

    cur = &hiresDiagBinds[hiresDiagBindCount];
    cur->tex = tex;
    cur->vram = tex->Vram;
    cur->tsize = gsKit_texture_size(tex->Width, tex->Height, tex->PSM);
    cur->vramClut = (tex->Clut != NULL) ? tex->VramClut : 0;
    cur->csize = csize;
    cur->width = tex->Width;
    cur->height = tex->Height;
    cur->psm = tex->PSM;
    cur->uploaded = uploaded ? 1 : 0;

    if (cur->uploaded) {
        hiresDiagUploads++;
        hiresDiagUploadBytes += cur->tsize + cur->csize;

        // 新上传的范围是否覆盖了本帧更早、当时未上传就直接使用 VRAM 旧内容的纹理。
        for (i = 0; i < hiresDiagBindCount; i++) {
            hires_diag_bind_t *old = &hiresDiagBinds[i];
            if (old->tex == tex || old->uploaded)
                continue;
            if (!hiresDiagRangesOverlap(cur->vram, cur->tsize, old->vram, old->tsize) &&
                !hiresDiagRangesOverlap(cur->vram, cur->tsize, old->vramClut, old->csize) &&
                !hiresDiagRangesOverlap(cur->vramClut, cur->csize, old->vram, old->tsize) &&
                !hiresDiagRangesOverlap(cur->vramClut, cur->csize, old->vramClut, old->csize))
                continue;
            if (hiresDiagUploadedEarlier(i))
                continue;

            hiresDiagAliasCount++;
            if (hiresDiagAliasLogged < HIRES_DIAG_MAX_ALIAS_LOGS) {
                hiresDiagAliasLogged++;
                LOG("[HIRES_ALIAS] f=%u new#%d=%08x %ux%u psm=%u vram=%08x+%u clut=%08x+%u over old#%d=%08x %ux%u psm=%u vram=%08x+%u clut=%08x+%u\n",
                    (unsigned int)hiresDiagFrame,
                    hiresDiagBindCount, (unsigned int)tex, cur->width, cur->height, cur->psm,
                    (unsigned int)cur->vram, (unsigned int)cur->tsize, (unsigned int)cur->vramClut, (unsigned int)cur->csize,
                    i, (unsigned int)old->tex, old->width, old->height, old->psm,
                    (unsigned int)old->vram, (unsigned int)old->tsize, (unsigned int)old->vramClut, (unsigned int)old->csize);
            }
        }
    }

    hiresDiagBindCount++;
}

static void hiresDiagEndFrame(void)
{
    if (hiresDiagUploads || hiresDiagAliasCount || hiresDiagBindDropped) {
        LOG("[HIRES_TEX] f=%u binds=%d uploads=%u bytes=%u alias=%u alias_logged=%u dropped=%u\n",
            (unsigned int)hiresDiagFrame, hiresDiagBindCount, (unsigned int)hiresDiagUploads,
            (unsigned int)hiresDiagUploadBytes, (unsigned int)hiresDiagAliasCount,
            (unsigned int)hiresDiagAliasLogged, (unsigned int)hiresDiagBindDropped);
    }

    hiresDiagBindCount = 0;
    hiresDiagBindDropped = 0;
    hiresDiagUploads = 0;
    hiresDiagUploadBytes = 0;
    hiresDiagAliasCount = 0;
    hiresDiagAliasLogged = 0;
    hiresDiagFrame++;
}

// 按 gsKit_hires_init_screen 的同一算法计算 pass 高度并画分界线：
//   passCount 限制在 2..4；CT32/CT24 按 32 行对齐，其余（CT16S）按 64 行对齐；
//   passHeight = ceil(Height / passCount) 向上对齐。
// 720p：Height=720、3 pass → 256，分界在第 256/512 行；
// 1080i（FRAME 模式 Height 已减半为 540）：3 pass → 192，分界在缓冲第 192/384 行。
// 坐标直接用帧缓冲坐标（不加 fRender*Off 的 overscan/半像素偏移）；gsKit_prim_sprite
// 会自行加 gsGlobal->OffsetX/Y，而每个 pass 的 XYOFFSET = Offset + bufferline_begin*16，
// 所以这里的 y 就是帧缓冲行号。直接走 gsKit_prim_sprite，不经过 CPU 裁剪。
static void hiresDiagDrawPassSeams(void)
{
    const u64 seamColor = GS_SETREG_RGBAQ(0xFF, 0x00, 0xFF, 0x80, 0x00);
    int passCount, heightAlign, passHeight, pass;
    u8 prevAlphaEnable;

    if (vmode < 0)
        return;

    passCount = rm_mode_table[vmode].passes;
    if (passCount < 2)
        passCount = 2;
    if (passCount > 4)
        passCount = 4;

    heightAlign = ((gsGlobal->PSM == GS_PSM_CT32) || (gsGlobal->PSM == GS_PSM_CT24)) ? 32 : 64;
    passHeight = (gsGlobal->Height + (passCount - 1)) / passCount;
    passHeight = (passHeight + (heightAlign - 1)) & ~(heightAlign - 1);

    // 不透明绘制；只改 gsGlobal 软件状态（决定本图元 PRIM.ABE），用完恢复，
    // 不向队列写 ALPHA/TEST/SCISSOR 等寄存器。
    prevAlphaEnable = gsGlobal->PrimAlphaEnable;
    gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
    for (pass = 1; pass < passCount; pass++) {
        int y = pass * passHeight;
        if (y >= gsGlobal->Height)
            break;
        gsKit_prim_sprite(gsGlobal, 0.0f, (float)(y - 1), (float)gsGlobal->Width, (float)(y + 1), order, seamColor);
    }
    gsGlobal->PrimAlphaEnable = prevAlphaEnable;
}
#endif

// 所有 TexManager 绑定统一走这里；语义与直接调用 gsKit_TexManager_bind 完全相同。
static unsigned int rmTexBind(GSTEXTURE *tex)
{
#ifdef __DEBUG
    GUI_DIAG_STAGE(GUI_DIAG_TEX_BIND);
#endif
    unsigned int uploaded = gsKit_TexManager_bind(gsGlobal, tex);
#if HIRES_PASS_DIAG
    if (hires)
        hiresDiagRecordBind(tex, uploaded);
#endif
    return uploaded;
}

void rmInvalidateTexture(GSTEXTURE *txt)
{
    gsKit_TexManager_invalidate(gsGlobal, txt);
}

void rmUnloadTexture(GSTEXTURE *txt)
{
    gsKit_TexManager_free(gsGlobal, txt);
}

void rmStartFrame(void)
{
    if (hires == 0)
        gsKit_clear(gsGlobal, gColBlack);

    order = 0;
}

#ifdef __DEBUG
void rmDiagGetState(int *isHires, int *videoMode, int *width, int *height, int *activeBuffer)
{
    if (isHires)
        *isHires = hires;
    if (videoMode)
        *videoMode = vmode;
    if (width)
        *width = gsGlobal ? gsGlobal->Width : 0;
    if (height)
        *height = gsGlobal ? gsGlobal->Height : 0;
    if (activeBuffer)
        *activeBuffer = gsGlobal ? gsGlobal->ActiveBuffer : -1;
}
#endif

void rmEndFrame(void)
{
    if (hires) {
#if HIRES_PASS_DIAG
        // 最后一个入队的图元，画在本帧所有内容之上。
        hiresDiagDrawPassSeams();
#endif
#ifdef __DEBUG
        GUI_DIAG_STAGE(GUI_DIAG_HIRES_SYNC);
#endif
        gsKit_hires_sync(gsGlobal);
#ifdef __DEBUG
        GUI_DIAG_STAGE(GUI_DIAG_HIRES_FLIP);
#endif
        gsKit_hires_flip(gsGlobal);
    } else {
        gsKit_set_finish(gsGlobal);
#ifdef __DEBUG
        GUI_DIAG_STAGE(GUI_DIAG_QUEUE_EXEC);
#endif
        gsKit_queue_exec(gsGlobal);

        // Wait for draw ops to finish
#ifdef __DEBUG
        GUI_DIAG_STAGE(GUI_DIAG_GS_FINISH);
#endif
        gsKit_finish();

        if (!gsGlobal->FirstFrame) {
            SleepThread();
            guiWakeupCount = 0;

            if (gsGlobal->DoubleBuffering == GS_SETTING_ON) {
                GS_SET_DISPFB2(gsGlobal->ScreenBuffer[gsGlobal->ActiveBuffer & 1] / 8192,
                               gsGlobal->Width / 64, gsGlobal->PSM, 0, 0);

                gsGlobal->ActiveBuffer ^= 1;
            }
        }

        gsKit_setactive(gsGlobal);
    }

#ifdef __DEBUG
    GUI_DIAG_STAGE(GUI_DIAG_TEX_MANAGER_NEXT_FRAME);
#endif
    gsKit_TexManager_nextFrame(gsGlobal);
#if HIRES_PASS_DIAG
    hiresDiagEndFrame();
#endif
}

static int rmOnVSync(void)
{
    if (guiWakeupCount == 0) {
        guiWakeupCount = 1;
        iWakeupThread(guiThreadID);
    }

    ExitHandler();
    return 0;
}

void rmInit()
{
    short int mode = gsKit_check_rom();

    rm_mode_table[RM_VMODE_AUTO].mode = mode;
    rm_mode_table[RM_VMODE_AUTO].height = (mode == GS_MODE_PAL) ? 512 : 448;
    rm_mode_table[RM_VMODE_AUTO].PAR1 = (mode == GS_MODE_PAL) ? 16 : 14;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);

    // Initialize the DMAC
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    rmSetMode(1);

    order = 0;

    guiWakeupCount = 0;
    guiThreadID = GetThreadId();
}

int rmSetMode(int force)
{
    if (gVMode < RM_VMODE_AUTO || gVMode >= NUM_RM_VMODES)
        gVMode = RM_VMODE_AUTO;

    // we don't want to set the vmode without a reason...
    int changed = (vmode != gVMode || force);
    if (changed) {
        // Cleanup previous gsKit instance
        if (vmode >= 0)
            rmEnd();

        vmode = gVMode;
        hires = (rm_mode_table[vmode].passes > 1) ? 1 : 0;

        if (hires) {
            gsGlobal = gsKit_hires_init_global();
        } else {
            gsGlobal = gsKit_init_global();
            vsync_id = gsKit_add_vsync_handler(&rmOnVSync);
        }
        gsGlobal->Mode = rm_mode_table[vmode].mode;
        gsGlobal->Width = rm_mode_table[vmode].width;
        gsGlobal->Height = rm_mode_table[vmode].height;
        gsGlobal->Interlace = rm_mode_table[vmode].interlace;
        gsGlobal->Field = rm_mode_table[vmode].field;
        gsGlobal->PSM = GS_PSM_CT24;
        // Higher resolution use too much VRAM
        // so automatically switch back to 16bit color depth
        if ((gsGlobal->Width * gsGlobal->Height) > (704 * 576))
            gsGlobal->PSM = GS_PSM_CT16S;
        gsGlobal->PSMZ = GS_PSMZ_16S;
        gsGlobal->ZBuffering = GS_SETTING_OFF;
        gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
        gsGlobal->DoubleBuffering = GS_SETTING_ON;
        gsGlobal->Dithering = GS_SETTING_ON;

        // Do not draw pixels if they are fully transparent
        // gsGlobal->Test->ATE  = GS_SETTING_ON;
        gsGlobal->Test->ATST = 7; // NOTEQUAL to AREF passes
        gsGlobal->Test->AREF = 0x00;
        gsGlobal->Test->AFAIL = 0; // KEEP

        if ((gsGlobal->Interlace == GS_INTERLACED) && (gsGlobal->Field == GS_FRAME))
            gsGlobal->Height /= 2;

        // Coordinate space ranges from 0 to 4096 pixels
        // Center the buffer in the coordinate space
        gsGlobal->OffsetX = ((4096 - gsGlobal->Width) / 2) * 16;
        gsGlobal->OffsetY = ((4096 - gsGlobal->Height) / 2) * 16;

        if (hires) {
            gsKit_hires_init_screen(gsGlobal, rm_mode_table[vmode].passes);
        } else {
            gsKit_init_screen(gsGlobal);
            gsKit_mode_switch(gsGlobal, GS_ONESHOT);
        }

        gsKit_set_test(gsGlobal, GS_ZTEST_OFF);
        gsKit_set_primalpha(gsGlobal, gDefaultAlpha, 0);

        // reset the contents of the screen to avoid garbage being displayed
        if (hires) {
            gsKit_hires_sync(gsGlobal);
            gsKit_hires_flip(gsGlobal);
        } else {
            gsKit_clear(gsGlobal, gColBlack);
            gsKit_sync_flip(gsGlobal);
        }

        LOG("RENDERMAN New vmode: %d, %d x %d\n", vmode, gsGlobal->Width, gsGlobal->Height);
    }

    rmSetDisplayOffset(gXOff, gYOff);
    rmSetOverscan(gOverscan);
    rmSetAspectRatio((gWideScreen == 0) ? RM_ARATIO_4_3 : RM_ARATIO_16_9);

    return changed;
}

void rmGetScreenExtentsNative(int *w, int *h)
{
    *w = iDisplayWidth;
    *h = iDisplayHeight;
}

void rmGetScreenExtents(int *w, int *h)
{
    // Emulate 640x480 (square pixel VGA)
    *w = 640;
    *h = 480;
}

void rmEnd(void)
{
    if (hires) {
        gsKit_hires_deinit_global(gsGlobal);
    } else {
        gsKit_deinit_global(gsGlobal);
        gsKit_remove_vsync_handler(vsync_id);
    }

    vmode = -1;
}

#define X_SCALE(x) (((x)*iDisplayWidth) / 640)
#define Y_SCALE(y) (((y)*iDisplayHeight) / 480)
/** If txt is null, don't use DIM_UNDEF size */
static void rmSetupQuad(GSTEXTURE *txt, int x, int y, short aligned, int w, int h, short scaled, u64 color, rm_quad_t *q)
{
    if (w == DIM_UNDEF)
        w = txt->Width;
    if (h == DIM_UNDEF)
        h = txt->Height;

    // Legacy scaling
    x = X_SCALE(x);
    y = Y_SCALE(y);
    if (scaled & SCALING_RATIO)
        w = X_SCALE(w * iAspectWidth) >> 2;
    else
        w = X_SCALE(w);
    h = Y_SCALE(h);

    // Align LEFT/HCENTER/RIGHT
    if (aligned & ALIGN_HCENTER)
        q->ul.x = x - (w >> 1);
    else if (aligned & ALIGN_RIGHT)
        q->ul.x = x - w;
    else
        q->ul.x = x;
    q->br.x = q->ul.x + w;

    // Align TOP/VCENTER/BOTTOM
    if (aligned & ALIGN_VCENTER)
        q->ul.y = y - (h >> 1);
    else if (aligned & ALIGN_BOTTOM)
        q->ul.y = y - h;
    else
        q->ul.y = y;
    q->br.y = q->ul.y + h;

    q->color = color;
    if (txt) {
        q->txt = txt;
        q->ul.u = 0;
        q->ul.v = 0;
        q->br.u = txt->Width;
        q->br.v = txt->Height;
    }
}

// 将轴对齐纹理矩形裁剪到可见 display rect，并按裁剪比例同步修正 UV。
// 仅在 hires Coverflow 路径启用；普通主题仍完全走原来的 GS 提交路径。
static int rmClipTextureRect(float *x1, float *y1, float *u1, float *v1,
                             float *x2, float *y2, float *u2, float *v2)
{
    if (!rmClipToDisplay)
        return 1;

    float left = (float)iDisplayXOff;
    float top = (float)iDisplayYOff;
    float right = (float)(iDisplayXOff + iDisplayWidth);
    float bottom = (float)(iDisplayYOff + iDisplayHeight);
    float width = *x2 - *x1;
    float height = *y2 - *y1;

    if (width <= 0.0f || height <= 0.0f || *x2 <= left || *x1 >= right || *y2 <= top || *y1 >= bottom)
        return 0;

    if (*x1 < left) {
        float t = (left - *x1) / width;
        *u1 += (*u2 - *u1) * t;
        *x1 = left;
    }
    if (*x2 > right) {
        float t = (right - *x1) / (*x2 - *x1);
        *u2 = *u1 + (*u2 - *u1) * t;
        *x2 = right;
    }
    if (*y1 < top) {
        float t = (top - *y1) / height;
        *v1 += (*v2 - *v1) * t;
        *y1 = top;
    }
    if (*y2 > bottom) {
        float t = (bottom - *y1) / (*y2 - *y1);
        *v2 = *v1 + (*v2 - *v1) * t;
        *y2 = bottom;
    }

    return (*x2 > *x1 && *y2 > *y1);
}

static int rmSubmitSpriteTexture(GSTEXTURE *txt, float x1, float y1, float u1, float v1,
                                 float x2, float y2, float u2, float v2, u64 color)
{
    if (!rmClipTextureRect(&x1, &y1, &u1, &v1, &x2, &y2, &u2, &v2))
        return 0;

    gsKit_prim_sprite_texture(gsGlobal, txt, x1, y1, u1, v1, x2, y2, u2, v2, order, color);
    return 1;
}

static int rmSubmitQuadTexture(GSTEXTURE *txt, float x1, float y1, float u1, float v1,
                               float x2, float y2, float u2, float v2, u64 color)
{
    if (!rmClipTextureRect(&x1, &y1, &u1, &v1, &x2, &y2, &u2, &v2))
        return 0;

    gsKit_prim_quad_texture(gsGlobal, txt,
                            x1, y1, u1, v1,
                            x2, y1, u2, v1,
                            x1, y2, u1, v2,
                            x2, y2, u2, v2,
                            order, color);
    return 1;
}

// 直接提交四个顶点的纹理 quad。Case 的内框可能不是严格的轴对齐矩形；
// Coverflow 主图必须使用和 overlay_* 四个内框顶点一一对应的四个顶点，
// 不能先平均成中心矩形，否则动画缩放时四角会相对 case 漂移。
static int rmSubmitQuadTextureCorners(GSTEXTURE *txt,
                                      float ulX, float ulY, float ulU, float ulV,
                                      float urX, float urY, float urU, float urV,
                                      float blX, float blY, float blU, float blV,
                                      float brX, float brY, float brU, float brV,
                                      u64 color)
{
    // 现有内置 Coverflow case 的四个内框顶点是轴对齐矩形；继续走原有
    // 图元级裁切路径，保持 hires overscan 裁切和 UV 修正完全不变。
    if (ulX == blX && urX == brX && ulY == urY && blY == brY)
        return rmSubmitQuadTexture(txt, ulX, ulY, ulU, ulV, urX, brY, urU, brV, color);

    if (rmClipToDisplay) {
        float minX = ulX, maxX = ulX;
        float minY = ulY, maxY = ulY;
        if (urX < minX) minX = urX;
        if (blX < minX) minX = blX;
        if (brX < minX) minX = brX;
        if (urX > maxX) maxX = urX;
        if (blX > maxX) maxX = blX;
        if (brX > maxX) maxX = brX;
        if (urY < minY) minY = urY;
        if (blY < minY) minY = blY;
        if (brY < minY) minY = brY;
        if (urY > maxY) maxY = urY;
        if (blY > maxY) maxY = blY;
        if (brY > maxY) maxY = brY;

        // 当前 Coverflow case 通常完全位于可见区域内。对完全在区域外的
        // 自定义四边形直接丢弃；部分越界仍由 GS 当前 pass 的 scissor 裁切。
        float left = (float)iDisplayXOff;
        float top = (float)iDisplayYOff;
        float right = (float)(iDisplayXOff + iDisplayWidth);
        float bottom = (float)(iDisplayYOff + iDisplayHeight);
        if (maxX <= left || minX >= right || maxY <= top || minY >= bottom)
            return 0;
    }

    gsKit_prim_quad_texture(gsGlobal, txt,
                            ulX, ulY, ulU, ulV,
                            urX, urY, urU, urV,
                            blX, blY, blU, blV,
                            brX, brY, brU, brV,
                            order, color);
    return 1;
}

// Coverflow 倒影使用一个带顶点 alpha 插值的纹理四边形，避免把同一张图
// 切成多条横带后在屏幕上形成明显的重复锯齿层。裁切上下边界时同步插值
// 顶/底 alpha，保证 hires 图元级裁切仍然不会改变渐隐曲线。
static int rmSubmitGoraudQuadTexture(GSTEXTURE *txt, float x1, float y1, float u1, float v1,
                                      float x2, float y2, float u2, float v2,
                                      u64 topColor, u64 bottomColor)
{
    float originalY1 = y1;
    float originalY2 = y2;

    if (!rmClipTextureRect(&x1, &y1, &u1, &v1, &x2, &y2, &u2, &v2))
        return 0;

    if (originalY2 > originalY1) {
        int topAlpha = (int)((topColor >> 24) & 0xFF);
        int bottomAlpha = (int)((bottomColor >> 24) & 0xFF);
        float topT = (y1 - originalY1) / (originalY2 - originalY1);
        float bottomT = (y2 - originalY1) / (originalY2 - originalY1);
        int clippedTopAlpha = (int)(topAlpha + (bottomAlpha - topAlpha) * topT + 0.5f);
        int clippedBottomAlpha = (int)(topAlpha + (bottomAlpha - topAlpha) * bottomT + 0.5f);

        topColor = GS_SETREG_RGBAQ(topColor & 0xFF, (topColor >> 8) & 0xFF,
                                   (topColor >> 16) & 0xFF, clippedTopAlpha, 0x00);
        bottomColor = GS_SETREG_RGBAQ(bottomColor & 0xFF, (bottomColor >> 8) & 0xFF,
                                      (bottomColor >> 16) & 0xFF, clippedBottomAlpha, 0x00);
    }

    gsKit_prim_quad_goraud_texture(gsGlobal, txt,
                                   x1, y1, u1, v1,
                                   x2, y1, u2, v1,
                                   x1, y2, u1, v2,
                                   x2, y2, u2, v2,
                                   order, topColor, topColor, bottomColor, bottomColor);
    return 1;
}

void rmDrawQuad(rm_quad_t *q)
{
    if ((q->txt->PSM == GS_PSM_CT32) || (q->txt->Clut && q->txt->ClutPSM == GS_PSM_CT32)) {
        gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
        gsKit_set_test(gsGlobal, GS_ATEST_ON);
    } else {
        gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;
        gsKit_set_test(gsGlobal, GS_ATEST_OFF);
    }

    rmTexBind(q->txt);
    if (rmSubmitSpriteTexture(q->txt,
                              q->ul.x + fRenderXOff, q->ul.y + fRenderYOff,
                              q->ul.u, q->ul.v,
                              q->br.x + fRenderXOff, q->br.y + fRenderYOff,
                              q->br.u, q->br.v, q->color))
        order++;
}

void rmDrawPixmap(GSTEXTURE *txt, int x, int y, short aligned, int w, int h, short scaled, u64 color)
{
    rm_quad_t quad;
    rmSetupQuad(txt, x, y, aligned, w, h, scaled, color, &quad);
    rmDrawQuad(&quad);
}

void rmDrawOverlayPixmap(GSTEXTURE *overlay, int x, int y, short aligned, int w, int h, short scaled, u64 color,
                         GSTEXTURE *inlay, int ulx, int uly, int urx, int ury, int blx, int bly, int brx, int bry)
{
    rm_quad_t quad;
    rmSetupQuad(overlay, x, y, aligned, w, h, scaled, color, &quad);
    ulx = X_SCALE(ulx * iAspectWidth) >> 2;
    urx = X_SCALE(urx * iAspectWidth) >> 2;
    blx = X_SCALE(blx * iAspectWidth) >> 2;
    brx = X_SCALE(brx * iAspectWidth) >> 2;
    uly = Y_SCALE(uly);
    ury = Y_SCALE(ury);
    bly = Y_SCALE(bly);
    bry = Y_SCALE(bry);

    if ((inlay->PSM == GS_PSM_CT32) || (inlay->Clut && inlay->ClutPSM == GS_PSM_CT32))
        gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    else
        gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    rmTexBind(inlay);
    // 内嵌图(inlay，如 Coverflow 的封面主图)与外壳(overlay)共用同一个调制色 color。
    // 原先此处写死 gDefaultCol，导致压暗外壳时封面主图仍是满亮度、两者不一致。
    // 改用传入的 color 后：所有现有调用者传的都是 gDefaultCol（效果不变），
    // 只有 Coverflow 压暗路径传入压暗色，从而封面主图与外壳一起被压暗。
    gsKit_prim_quad_texture(gsGlobal, inlay,
                            quad.ul.x + ulx + fRenderXOff, quad.ul.y + uly + fRenderYOff,
                            0.0f, 0.0f,
                            quad.ul.x + urx + fRenderXOff, quad.ul.y + ury + fRenderYOff,
                            inlay->Width, 0.0f,
                            quad.ul.x + blx + fRenderXOff, quad.ul.y + bly + fRenderYOff,
                            0.0f, inlay->Height,
                            quad.ul.x + brx + fRenderXOff, quad.ul.y + bry + fRenderYOff,
                            inlay->Width, inlay->Height, order, color);
    order++;

    rmDrawQuad(&quad);
}

// 在由 (x,y,w,h,aligned,scaled) 描述的四边形正下方，绘制该纹理向下、
// 渐隐的镜像倒影。使用一个带顶点 alpha 插值的 quad，避免把纹理切成多条横带
// 后在屏幕上产生明显的阶梯、接缝和重叠感。
static void rmDrawReflectionRows(GSTEXTURE *txt, const rm_quad_t *quad, u64 color)
{
    float totalHeight = quad->br.y - quad->ul.y;
    float reflectionHeight = totalHeight / 4.0f;
    if (reflectionHeight <= 0.0f)
        return;

    float alphaStart = 0x20;
    float alphaEnd = 0x00;
    u64 reflectionTopColor = GS_SETREG_RGBAQ(color & 0xFF, (color >> 8) & 0xFF,
                                              (color >> 16) & 0xFF, alphaStart, 0x00);
    u64 reflectionBottomColor = GS_SETREG_RGBAQ(color & 0xFF, (color >> 8) & 0xFF,
                                                 (color >> 16) & 0xFF, alphaEnd, 0x00);

    // 倒影顶部取原图底部，倒影底部取原图上方四分之一处；交换 UV 方向
    // 才是真正的垂直镜像，不能把每一条横带按正向 UV 依次拼接。
    float texTop = quad->br.v;
    float texBottom = ((totalHeight - reflectionHeight) / totalHeight) * txt->Height;
    float screenTop = quad->br.y + fRenderYOff;
    float screenBottom = screenTop + reflectionHeight;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    rmTexBind(txt);
    if (rmSubmitGoraudQuadTexture(txt,
                                   quad->ul.x + fRenderXOff, screenTop,
                                   quad->ul.u, texTop,
                                   quad->br.x + fRenderXOff, screenBottom,
                                   quad->br.u, texBottom,
                                   reflectionTopColor, reflectionBottomColor))
        order++;
}

void rmDrawPixmapReflect(GSTEXTURE *txt, int x, int y, short aligned, int w, int h, short scaled, u64 color)
{
    // 主图仍走原封不动的公共绘制路径。
    rmDrawPixmap(txt, x, y, aligned, w, h, scaled, color);

    // 重新计算几何，再追加渐隐倒影。
    rm_quad_t quad;
    rmSetupQuad(txt, x, y, aligned, w, h, scaled, color, &quad);
    rmDrawReflectionRows(txt, &quad, color);
}

void rmDrawOverlayPixmapReflect(GSTEXTURE *overlay, int x, int y, short aligned, int w, int h, short scaled, u64 color,
                                GSTEXTURE *inlay, int ulx, int uly, int urx, int ury, int blx, int bly, int brx, int bry)
{
    // 主图（inlay + overlay）仍走原封不动的公共绘制路径。
    rmDrawOverlayPixmap(overlay, x, y, aligned, w, h, scaled, color, inlay, ulx, uly, urx, ury, blx, bly, brx, bry);

    // 重新计算几何（以及按宽高比缩放后的 overlay 四角偏移），以便在图像下方
    // 逐行绘制 inlay 与 overlay 的镜像。
    rm_quad_t quad;
    rmSetupQuad(overlay, x, y, aligned, w, h, scaled, color, &quad);

    ulx = X_SCALE(ulx * iAspectWidth) >> 2;
    urx = X_SCALE(urx * iAspectWidth) >> 2;
    blx = X_SCALE(blx * iAspectWidth) >> 2;
    brx = X_SCALE(brx * iAspectWidth) >> 2;
    uly = Y_SCALE(uly);
    ury = Y_SCALE(ury);
    bly = Y_SCALE(bly);
    bry = Y_SCALE(bry);

    float rowHeight = 1.0f;
    float totalHeight = quad.br.y - quad.ul.y;
    float alphaStart = 0x20;
    float alphaEnd = 0x00;

    if (totalHeight <= 0.0f)
        return;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    for (float row = 0; row < totalHeight; row += rowHeight) {
        float alpha;
        if (row < totalHeight / 4.0f)
            alpha = alphaStart - ((alphaStart - alphaEnd) * (row / (totalHeight / 4.0f)));
        else
            alpha = 0x00;

        u64 reflectionColor = GS_SETREG_RGBAQ((color >> 24) & 0xFF, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (u8)alpha, 0x00);

        float screenTop = quad.br.y + fRenderYOff + row;
        float screenBottom = quad.br.y + fRenderYOff + row + rowHeight;

        // Inlay（实际封面图）行。
        float texTop = ((totalHeight - row - rowHeight) / totalHeight) * inlay->Height;
        float texBottom = ((totalHeight - row) / totalHeight) * inlay->Height;

        rmTexBind(inlay);
        gsKit_prim_quad_texture(gsGlobal, inlay,
                                quad.ul.x + ulx + fRenderXOff, screenTop,
                                0.0f, texTop,
                                quad.ul.x + urx + fRenderXOff, screenTop,
                                inlay->Width, texTop,
                                quad.ul.x + blx + fRenderXOff, screenBottom,
                                0.0f, texBottom,
                                quad.ul.x + brx + fRenderXOff, screenBottom,
                                inlay->Width, texBottom,
                                order, reflectionColor);
        order++;

        // Overlay（盒装外壳边框）行。
        texTop = ((totalHeight - row - rowHeight) / totalHeight) * overlay->Height;
        texBottom = ((totalHeight - row) / totalHeight) * overlay->Height;

        rmTexBind(overlay);
        gsKit_prim_sprite_texture(gsGlobal, overlay,
                                  quad.ul.x + fRenderXOff, screenTop,
                                  quad.ul.u, texTop,
                                  quad.br.x + fRenderXOff, screenBottom,
                                  quad.br.u, texBottom,
                                  order, reflectionColor);
        order++;
    }
}

// rmSetupQuad 的【浮点】版本：x/y/w/h 用 float，X_SCALE/Y_SCALE、宽屏压缩与对齐全程
// 浮点、不做任何整数取整。仅供下方 Coverflow 浮点旁路(rmDrawOverlayPixmap*Frac)调用，
// 使中心封面放大动画的宽/高连续变化，消除整数量化导致的宽高不同步形变蠕动（尤其宽屏）。
// 【重要】不改动、不影响整数版 rmSetupQuad 及所有非 Coverflow 绘制路径。
static void rmSetupQuadF(GSTEXTURE *txt, float x, float y, short aligned, float w, float h, short scaled, u64 color, rm_quad_t *q)
{
    if (txt) {
        if (w == (float)DIM_UNDEF)
            w = txt->Width;
        if (h == (float)DIM_UNDEF)
            h = txt->Height;
    }

    // Legacy scaling（浮点，不取整）
    x = (x * iDisplayWidth) / 640.0f;
    y = (y * iDisplayHeight) / 480.0f;
    if (scaled & SCALING_RATIO)
        w = (w * iDisplayWidth / 640.0f) * (float)iAspectWidth / 4.0f;
    else
        w = (w * iDisplayWidth) / 640.0f;
    h = (h * iDisplayHeight) / 480.0f;

    // Align LEFT/HCENTER/RIGHT
    if (aligned & ALIGN_HCENTER)
        q->ul.x = x - w / 2.0f;
    else if (aligned & ALIGN_RIGHT)
        q->ul.x = x - w;
    else
        q->ul.x = x;
    q->br.x = q->ul.x + w;

    // Align TOP/VCENTER/BOTTOM
    if (aligned & ALIGN_VCENTER)
        q->ul.y = y - h / 2.0f;
    else if (aligned & ALIGN_BOTTOM)
        q->ul.y = y - h;
    else
        q->ul.y = y;
    q->br.y = q->ul.y + h;

    q->color = color;
    if (txt) {
        q->txt = txt;
        q->ul.u = 0;
        q->ul.v = 0;
        q->br.u = txt->Width;
        q->br.v = txt->Height;
    }
}

// 像素对齐：把浮点屏幕坐标四舍五入到整数像素（对正负都取整，+/-0.5 避免负值截断错 1px）。
// 主图与倒影共用同一套取整，保证二者边界落在同一整数像素、接缝严丝合缝、不丢线。
static inline float rmPxSnap(float v)
{
    return (float)((int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
}

// Coverflow 专用的统一父子变换：case 是父级，inlay 是 case 内的 child。
//
// 这组数据只在 Coverflow 的浮点绘制路径中使用；普通整数版
// rmDrawOverlayPixmap()/rmDrawPixmap() 完全不经过这里，因此不会改变非 Coverflow 主题。
// 浮点布局、最终整数像素边界、主图和倒影都从同一份 transform 派生，避免同一帧
// 中重复 rmSetupQuadF / 重复取整造成 case、inlay、reflection 的边界不一致。
typedef struct
{
    // 已经完成最终像素取整的 case 四边形。坐标仍处于与普通 rmSetupQuad() 相同的
    // render offset 之前；提交时由 rmDrawQuad() 统一加 fRender*Off。
    rm_quad_t caseQuad;

    // 与 case 最终像素高度一致的整数尺寸：倒影从同一整数底边绘制。
    float caseHeight;

    // case/inlay 的整数屏幕坐标；提交到直接 quad primitive 时统一加 fRender*Off。
    // 这样它们与普通 ItemCover 使用同一半像素采样相位。
    float caseLeft;
    float caseRight;
    // 主图四个顶点分别从 overlay 的四个内框顶点变换而来；不再用中心点
    // 和平均宽高重建一个“近似矩形”。
    float inlayUlX;
    float inlayUlY;
    float inlayUrx;
    float inlayUry;
    float inlayBlx;
    float inlayBly;
    float inlayBrx;
    float inlayBry;

    // 倒影仍需要轴对齐的左右范围。对内框为矩形的现有 Coverflow case，
    // 这四个值与上面的四角完全一致。
    float inlayLeft;
    float inlayTop;
    float inlayRight;
    float inlayBottom;

    // 倒影从 case 的整数底边开始；横向范围与主图共用上面的结果。
    float reflectionBaseY;
} rm_cover_transform_t;

// Coverflow 专用：根据同一组父级参数构建 case、inlay 和 reflection 共用的变换。
// baseW/baseH 是 overlay 顶点的逻辑坐标系；游戏默认 140×200，APPS 使用自己的
// 140×140 基准。ov* 是内框在该父级坐标系中的局部位置。
static void rmBuildCoverTransform(GSTEXTURE *overlay, float x, float y, short aligned, float w, float h,
                                  short scaled, u64 color, int baseW, int baseH,
                                  int ovUlx, int ovUly, int ovUrx, int ovUry,
                                  int ovBlx, int ovBly, int ovBrx, int ovBry,
                                  rm_cover_transform_t *transform)
{
    rm_quad_t floatCase;
    rmSetupQuadF(overlay, x, y, aligned, w, h, scaled, color, &floatCase);

    float caseW = floatCase.br.x - floatCase.ul.x;
    float caseH = floatCase.br.y - floatCase.ul.y;
    float fbw = (baseW > 0) ? (float)baseW : 1.0f;
    float fbh = (baseH > 0) ? (float)baseH : 1.0f;

    // 先把父级 case 的浮点尺寸锁定为整数像素。不能把左右/上下边界分别取整，
    // 否则 Coverflow 横向移动时，case 的最终宽高会在 N/N+1/N 之间跳动，表现为
    // 封面在动画末尾“放大过头后又吸回”。
    float casePixelW = rmPxSnap(caseW);
    float casePixelH = rmPxSnap(caseH);

    // 父级位置仍按原有对齐方式取整，但远角统一由“近角 + 锁定尺寸”得到。
    // Coverflow 当前使用 ALIGN_HCENTER | ALIGN_BOTTOM，因此这里同时保持中心/底部锚点。
    float rUlX, rBrX, rUlY, rBrY;
    // Keep the transform in the pre-offset coordinate space. The case is submitted
    // through rmDrawQuad(), while the inlay and reflection are submitted directly;
    // each of those paths applies fRenderXOff/fRenderYOff exactly once.
    if (aligned & ALIGN_HCENTER) {
        float centerX = (floatCase.ul.x + floatCase.br.x) * 0.5f;
        rUlX = rmPxSnap(centerX - casePixelW * 0.5f);
        rBrX = rUlX + casePixelW;
    } else if (aligned & ALIGN_RIGHT) {
        rBrX = rmPxSnap(floatCase.br.x);
        rUlX = rBrX - casePixelW;
    } else {
        rUlX = rmPxSnap(floatCase.ul.x);
        rBrX = rUlX + casePixelW;
    }

    if (aligned & ALIGN_BOTTOM) {
        rBrY = rmPxSnap(floatCase.br.y);
        rUlY = rBrY - casePixelH;
    } else if (aligned & ALIGN_VCENTER) {
        float centerY = (floatCase.ul.y + floatCase.br.y) * 0.5f;
        rUlY = rmPxSnap(centerY - casePixelH * 0.5f);
        rBrY = rUlY + casePixelH;
    } else {
        rUlY = rmPxSnap(floatCase.ul.y);
        rBrY = rUlY + casePixelH;
    }

    // Case 只是外壳，不能用“已经取整的 casePixelH / baseH”反过来决定封面尺寸：
    // casePixelH 是物理像素，而 baseH 是 640×480 逻辑坐标，二者不能直接相除。
    // 之前的写法正是因此把 448p 的 214 逻辑高度压成了约 199 像素。
    //
    // 封面目标尺寸继续从【未取整的 caseW/caseH】和 overlay 内框比例得到；这与
    // drawCoverFlow 中由 inlayW/inlayH 反推 case 的方向相反，因而 Case 只适配封面。
    // casePixelW/H 只用于确定 Case 已经落地后的实际中心，不能用来缩放封面。
    float caseScaleX = caseW / fbw;
    float caseScaleY = caseH / fbh;
    float offL = ((float)ovUlx + (float)ovBlx) * 0.5f * caseScaleX;
    float offR = ((float)ovUrx + (float)ovBrx) * 0.5f * caseScaleX;
    float offT = ((float)ovUly + (float)ovUry) * 0.5f * caseScaleY;
    float offB = ((float)ovBly + (float)ovBry) * 0.5f * caseScaleY;
    float inlayWidth = rmPxSnap(offR - offL);
    float inlayHeight = rmPxSnap(offB - offT);

    // 以实际 Case 的中心放置“固定尺寸”的封面。这样 Case 外层即使因整数像素
    // 锁定与理想浮点尺寸相差不到一像素，也不会把封面本身压缩或拉伸。
    float innerCenterX = ((float)ovUlx + (float)ovUrx + (float)ovBlx + (float)ovBrx)
                       * 0.25f * (casePixelW / fbw);
    float innerCenterY = ((float)ovUly + (float)ovUry + (float)ovBly + (float)ovBry)
                       * 0.25f * (casePixelH / fbh);
    float inlayLeft = rmPxSnap(rUlX + innerCenterX - inlayWidth * 0.5f);
    float inlayTop = rmPxSnap(rUlY + innerCenterY - inlayHeight * 0.5f);
    float inlayRight = inlayLeft + inlayWidth;
    float inlayBottom = inlayTop + inlayHeight;

    // 非矩形内框仍使用 overlay 的四个原始顶点；现有内置 cf_case 是矩形，
    // 因而会走下面的固定尺寸矩形分支，四个顶点同时保持目标封面尺寸。
    int axisAlignedInner = (ovUlx == ovBlx && ovUrx == ovBrx &&
                            ovUly == ovUry && ovBly == ovBry);
    float inlayUlX, inlayUlY, inlayUrx, inlayUry;
    float inlayBlx, inlayBly, inlayBrx, inlayBry;
    if (axisAlignedInner) {
        inlayUlX = inlayBlx = inlayLeft;
        inlayUrx = inlayBrx = inlayRight;
        inlayUlY = inlayUry = inlayTop;
        inlayBly = inlayBry = inlayBottom;
    } else {
        inlayUlX = rUlX + rmPxSnap((float)ovUlx * caseScaleX);
        inlayUlY = rUlY + rmPxSnap((float)ovUly * caseScaleY);
        inlayUrx = rUlX + rmPxSnap((float)ovUrx * caseScaleX);
        inlayUry = rUlY + rmPxSnap((float)ovUry * caseScaleY);
        inlayBlx = rUlX + rmPxSnap((float)ovBlx * caseScaleX);
        inlayBly = rUlY + rmPxSnap((float)ovBly * caseScaleY);
        inlayBrx = rUlX + rmPxSnap((float)ovBrx * caseScaleX);
        inlayBry = rUlY + rmPxSnap((float)ovBry * caseScaleY);
    }

    transform->caseQuad = floatCase;
    // 不要把 fRender*Off 提前抵消：rmDrawQuad() 会像普通 ItemCover 一样统一加上它。
    transform->caseQuad.ul.x = rUlX;
    transform->caseQuad.ul.y = rUlY;
    transform->caseQuad.br.x = rBrX;
    transform->caseQuad.br.y = rBrY;
    transform->caseHeight = casePixelH;
    transform->caseLeft = rUlX;
    transform->caseRight = rBrX;
    transform->inlayUlX = inlayUlX;
    transform->inlayUlY = inlayUlY;
    transform->inlayUrx = inlayUrx;
    transform->inlayUry = inlayUry;
    transform->inlayBlx = inlayBlx;
    transform->inlayBly = inlayBly;
    transform->inlayBrx = inlayBrx;
    transform->inlayBry = inlayBry;
    transform->inlayLeft = inlayLeft;
    transform->inlayTop = inlayTop;
    transform->inlayRight = inlayRight;
    transform->inlayBottom = inlayBottom;
    transform->reflectionBaseY = rBrY;
}

// 使用已经构建好的父子变换绘制主图和 case。case 与 inlay 仍是两个 GS 纹理调用，
// 但二者不再各自重新计算位置/尺寸，而是严格共享 rm_cover_transform_t。
static void rmDrawCoverTransform(const rm_cover_transform_t *transform, GSTEXTURE *inlay, u64 color)
{
    if ((inlay->PSM == GS_PSM_CT32) || (inlay->Clut && inlay->ClutPSM == GS_PSM_CT32))
        gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    else
        gsGlobal->PrimAlphaEnable = GS_SETTING_OFF;

    rmTexBind(inlay);
    // 这是直接 quad 提交，不会经过 rmDrawQuad()；在这里补上与普通 sprite
    // 相同的 fRender*Off，避免套上 case 后主图又回到整数采样相位。
    // 四个屏幕顶点与 overlay 的四个内框顶点一一对应。
    if (rmSubmitQuadTextureCorners(inlay,
                                   transform->inlayUlX + fRenderXOff,
                                   transform->inlayUlY + fRenderYOff, 0.0f, 0.0f,
                                   transform->inlayUrx + fRenderXOff,
                                   transform->inlayUry + fRenderYOff, inlay->Width, 0.0f,
                                   transform->inlayBlx + fRenderXOff,
                                   transform->inlayBly + fRenderYOff, 0.0f, inlay->Height,
                                   transform->inlayBrx + fRenderXOff,
                                   transform->inlayBry + fRenderYOff, inlay->Width, inlay->Height,
                                   color))
        order++;

    // caseQuad 已经包含最终整数边界；rmDrawQuad 只负责提交，不再重新计算几何。
    // 用局部副本满足旧接口的非 const 参数，不改变 transform 本身。
    rm_quad_t caseQuad = transform->caseQuad;
    rmDrawQuad(&caseQuad);
}

// 使用同一份 transform 绘制主图下方的镜像倒影。主图和倒影共用 case 的整数底边、
// inlay 的左右边界以及 overlay 的左右边界；这里只重新生成每一行的纹理采样范围。
static void rmDrawCoverReflectionRows(const rm_cover_transform_t *transform, GSTEXTURE *overlay,
                                      GSTEXTURE *inlay, u64 color)
{
    // 只有 case 高度的前四分之一显示倒影。用单个 Gouraud 纹理四边形
    // 让 alpha 在硬件中连续插值，避免 4px 横带产生重复画面和锯齿层，
    // 同时把每张封面的倒影命令数从最多 32 个降为 2 个。
    float totalHeight = transform->caseHeight;
    float reflectionHeight = totalHeight / 4.0f;
    if (reflectionHeight <= 0.0f)
        return;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;

    u64 reflectionTopColor = GS_SETREG_RGBAQ(color & 0xFF, (color >> 8) & 0xFF,
                                              (color >> 16) & 0xFF, 0x20, 0x00);
    u64 reflectionBottomColor = GS_SETREG_RGBAQ(color & 0xFF, (color >> 8) & 0xFF,
                                                 (color >> 16) & 0xFF, 0x00, 0x00);
    // 倒影同样直接提交 quad，不能沿用 case 的整数最终坐标；补回统一的
    // render offset，使主图、case、倒影处于同一采样相位。
    float screenTop = transform->reflectionBaseY + fRenderYOff;
    float screenBottom = screenTop + reflectionHeight;
    // 倒影上下镜像：反射区域顶部取源图底部，向下逐渐取到更高的源图位置。
    float texTop = inlay->Height;
    float texBottom = ((totalHeight - reflectionHeight) / totalHeight) * inlay->Height;

    rmTexBind(inlay);
    if (rmSubmitGoraudQuadTexture(inlay,
                                  transform->inlayLeft + fRenderXOff, screenTop, 0.0f, texTop,
                                  transform->inlayRight + fRenderXOff, screenBottom, inlay->Width, texBottom,
                                  reflectionTopColor, reflectionBottomColor))
        order++;

    texTop = overlay->Height;
    texBottom = ((totalHeight - reflectionHeight) / totalHeight) * overlay->Height;
    rmTexBind(overlay);
    if (rmSubmitGoraudQuadTexture(overlay,
                                  transform->caseLeft + fRenderXOff, screenTop,
                                  transform->caseQuad.ul.u, texTop,
                                  transform->caseRight + fRenderXOff, screenBottom,
                                  transform->caseQuad.br.u, texBottom,
                                  reflectionTopColor, reflectionBottomColor))
        order++;
}

// Coverflow 专用浮点 overlay 绘制。普通整数版 rmDrawOverlayPixmap() 不经过此路径。
void rmDrawOverlayPixmapFrac(GSTEXTURE *overlay, float x, float y, short aligned, float w, float h, short scaled, u64 color,
                             GSTEXTURE *inlay, int baseW, int baseH,
                             int ovUlx, int ovUly, int ovUrx, int ovUry, int ovBlx, int ovBly, int ovBrx, int ovBry)
{
    rm_cover_transform_t transform;
    rmBuildCoverTransform(overlay, x, y, aligned, w, h, scaled, color, baseW, baseH,
                          ovUlx, ovUly, ovUrx, ovUry, ovBlx, ovBly, ovBrx, ovBry, &transform);
    rmDrawCoverTransform(&transform, inlay, color);
}

// Coverflow 专用浮点 overlay + reflection 绘制。主图、case、倒影全部复用同一份 transform。
void rmDrawOverlayPixmapReflectFrac(GSTEXTURE *overlay, float x, float y, short aligned, float w, float h, short scaled, u64 color,
                                    GSTEXTURE *inlay, int baseW, int baseH,
                                    int ovUlx, int ovUly, int ovUrx, int ovUry, int ovBlx, int ovBly, int ovBrx, int ovBry)
{
    rm_cover_transform_t transform;
    rmBuildCoverTransform(overlay, x, y, aligned, w, h, scaled, color, baseW, baseH,
                          ovUlx, ovUly, ovUrx, ovUry, ovBlx, ovBly, ovBrx, ovBry, &transform);
    rmDrawCoverTransform(&transform, inlay, color);
    rmDrawCoverReflectionRows(&transform, overlay, inlay, color);
}

void rmDrawRect(int x, int y, int w, int h, u64 color)
{
    float fx = X_SCALE(x) + fRenderXOff;
    float fy = Y_SCALE(y) + fRenderYOff;
    float fw = X_SCALE(w);
    float fh = Y_SCALE(h);

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_prim_sprite(gsGlobal, fx, fy, fx + fw, fy + fh, order, color);
    order++;
}

void rmDrawLine(int x1, int y1, int x2, int y2, u64 color)
{
    float fx1 = X_SCALE(x1) + fRenderXOff;
    float fy1 = Y_SCALE(y1) + fRenderYOff;
    float fx2 = X_SCALE(x2) + fRenderXOff;
    float fy2 = Y_SCALE(y2) + fRenderYOff;

    gsGlobal->PrimAlphaEnable = GS_SETTING_ON;
    gsKit_prim_line(gsGlobal, fx1, fy1, fx2, fy2, order, color);
    order++;
}

void rmSetDisplayOffset(int x, int y)
{
    gsKit_set_display_offset(gsGlobal, x * rm_mode_table[vmode].VCK, y);
}

void rmSetAspectRatio(enum rm_aratio dar)
{
    DAR = dar;

    switch (DAR) {
        case RM_ARATIO_4_3:
            iAspectWidth = 4; // width = width * 4 / 4
            break;
        case RM_ARATIO_16_9:
            iAspectWidth = 3; // width = width * 3 / 4
            break;
    };
}

int rmWideScale(int x)
{
    return (x * iAspectWidth) >> 2;
}

// rmWideScale 的【浮点】版本：横向宽屏压缩但不做 >>2 整数截断，结果随输入连续变化。
// 仅供 Coverflow 浮点旁路计算中心封面放大后的连续宽度，消除整数截断造成的形变蠕动。
// 4:3 下 iAspectWidth==4，返回原值。
float rmWideScaleF(float x)
{
    return x * (float)iAspectWidth / 4.0f;
}

// Get the pixel aspect ratio (how wide or narrow are the pixels?)
float rmGetPAR()
{
    float fPAR = (float)rm_mode_table[vmode].PAR2 / (float)rm_mode_table[vmode].PAR1;

    // In anamorphic mode the pixels are stretched to 16:9
    if ((DAR == RM_ARATIO_16_9) && (rm_mode_table[vmode].aratio == RM_ARATIO_4_3))
        fPAR *= 0.75f;

    // In interlaced frame mode, the pixel are (virtually) twice as high
    if ((gsGlobal->Interlace == GS_INTERLACED) && (gsGlobal->Field == GS_FRAME))
        fPAR *= 2.0f;

    return fPAR;
}

// Get interfaced frame mode
int rmGetInterlacedFrameMode()
{
    if ((gsGlobal->Interlace == GS_INTERLACED) && (gsGlobal->Field == GS_FRAME))
        return 1;

    return 0;
}

int rmScaleX(int x)
{
    return X_SCALE(x);
}

int rmScaleY(int y)
{
    return Y_SCALE(y);
}

int rmUnScaleX(int x)
{
    return (x * 640) / iDisplayWidth;
}

int rmUnScaleY(int y)
{
    return (y * 480) / iDisplayHeight;
}

void rmSetOverscan(int overscan)
{
    iDisplayXOff = (gsGlobal->Width * overscan) / (2 * 1000);
    iDisplayYOff = (gsGlobal->Height * overscan) / (2 * 1000);
    iDisplayWidth = gsGlobal->Width - (2 * iDisplayXOff);
    iDisplayHeight = gsGlobal->Height - (2 * iDisplayYOff);

    fRenderXOff = (float)iDisplayXOff - 0.5f;
    fRenderYOff = (float)iDisplayYOff - 0.5f;

    if (rmGetInterlacedFrameMode() == 1)
        fRenderYOff += 0.25f;
}

// 把 GS 裁剪框(scissor)收紧到【可见显示区域】= 过扫补偿后的矩形
// [iDisplayXOff, iDisplayXOff+iDisplayWidth) × [iDisplayYOff, iDisplayYOff+iDisplayHeight)。
// 之后排入的所有图元都会被 GS 硬件裁到此框内，超出部分不写入帧缓冲。
//
// hires 例外：gsKit 的多 pass 路径会为每个 pass 建立“局部” scissor（y=0 到
// 当前 pass 的行数），同时用 XYOFFSET 把该 pass 映射回完整屏幕。普通的
// GS_SETREG_SCISSOR() 使用的是 pass 局部坐标，不能直接套用这里的完整 1080i
// 显示坐标；否则每个 pass 都会从同一个 y0 开始裁剪，造成横向内容缺失和不同步。
// WOPL 不在 hires 的主题绘制队列中改写 scissor，因此这里保持 gsKit pass 的
// 原生设置；Coverflow 的轴对齐纹理则在提交前按同一个可见矩形裁剪。
void rmSetScissorDisplay(void)
{
    if (hires) {
        // A shared hires draw queue cannot carry a different Y scissor for each
        // pass. Enable exact rectangle clipping at the texture primitive level
        // instead; the pass queue keeps its own framebuffer-local scissor.
        rmClipToDisplay = 1;
        return;
    }

    rmClipToDisplay = 0;
    int x0 = iDisplayXOff;
    int y0 = iDisplayYOff;
    int x1 = iDisplayXOff + iDisplayWidth - 1;
    int y1 = iDisplayYOff + iDisplayHeight - 1;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    gsKit_set_scissor(gsGlobal, GS_SETREG_SCISSOR(x0, x1, y0, y1));
}

// 恢复默认 scissor（整个帧缓冲）。hires 时关闭上面的图元级裁剪，
// 但不能覆盖 gsKit 为当前 pass 写入的局部 scissor。
void rmResetScissor(void)
{
    rmClipToDisplay = 0;
    if (hires)
        return;
    gsKit_set_scissor(gsGlobal, GS_SCISSOR_RESET);
}

unsigned char rmGetHsync(void)
{
    return rm_mode_table[vmode].hsync;
}
