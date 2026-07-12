/*
 * Experimental GSM 4:3 correction compositor.
 *
 * GSM's DISPLAY adaptation can only select integral MAGH values. This code
 * reads the framebuffer selected by the game, draws it as a texture into an
 * OPL-reserved framebuffer, and uses a 14/15-wide destination rectangle.
 * A 640x448 native image therefore becomes 597 1/3 x 448, exactly 4:3 before
 * the forced NTSC scanout is applied.
 *
 * This is deliberately conservative: indexed/depth framebuffer formats,
 * unusual display dimensions, and source buffers overlapping the reserved
 * output range are left under normal GSM control. The PS2 GS has no safe
 * global VRAM allocation service after a game has started, so this remains an
 * opt-in experimental mode. A game using the reserved range as texture memory
 * can still be incompatible.
 */

#include <tamtypes.h>
#include <kernel.h>
#include <dma.h>
#include <draw.h>
#include <gs_privileged.h>
#include <gs_psm.h>

#include "aspectfix.h"
#include "gsm_api.h"

#define ASPECT_OUTPUT_WIDTH   640
#define ASPECT_OUTPUT_HEIGHT  448
/* GS frame/texture addresses are 32-bit words, not byte addresses. */
#define ASPECT_OUTPUT_ADDRESS 0x000B0000
#define ASPECT_OUTPUT_SIZE    (ASPECT_OUTPUT_WIDTH * 512)
#define ASPECT_PACKET_QWORDS  96
#define ASPECT_STACK_SIZE     4096

struct AspectGSRegs
{
    u64 pmode;
    u64 smode1;
    u64 smode2;
    u64 srfsh;
    u64 synch1;
    u64 synch2;
    u64 syncv;
    u64 dispfb1;
    u64 display1;
    u64 dispfb2;
    u64 display2;
} __attribute__((packed));

extern volatile struct AspectGSRegs GSMSourceGSRegs;
extern volatile struct AspectGSRegs GSMDestGSRegs;

static int sAspectRunning;
static int sAspectSema = -1;
static int sAspectVBlankHandler = -1;
static u8 sAspectStack[ASPECT_STACK_SIZE] __attribute__((aligned(16)));
static qword_t sAspectPacket[ASPECT_PACKET_QWORDS] __attribute__((aligned(64)));

static int AspectFrameWords(int psm, int pixels)
{
    switch (psm) {
        case GS_PSM_32:
        case GS_PSM_24:
            return pixels;
        case GS_PSM_16:
        case GS_PSM_16S:
            return (pixels + 1) >> 1;
        default:
            return 0;
    }
}

static int AspectCompose(const volatile struct AspectGSRegs *regs)
{
    const u64 dispfb = regs->dispfb1;
    const u64 display = regs->display1;
    const int fbp = (int)(dispfb & 0x1FF) << 11;
    const int fbw = (int)((dispfb >> 9) & 0x3F) << 6;
    const int psm = (int)((dispfb >> 15) & 0x1F);
    const int dbx = (int)((dispfb >> 32) & 0x7FF);
    const int dby = (int)((dispfb >> 43) & 0x7FF);
    const int magh = (int)((display >> 23) & 0x0F) + 1;
    const int magv = (int)((display >> 27) & 0x03) + 1;
    const int src_width = ((int)((display >> 32) & 0x0FFF) + 1) / magh;
    const int src_height = ((int)((display >> 44) & 0x07FF) + 1) / magv;
    const int source_words = AspectFrameWords(psm, fbw * (dby + src_height));
    const int source_end = fbp + source_words;
    const float corrected_width = (float)ASPECT_OUTPUT_WIDTH * (14.0f / 15.0f);
    const float corrected_x = ((float)ASPECT_OUTPUT_WIDTH - corrected_width) * 0.5f;
    framebuffer_t frame;
    zbuffer_t z;
    texbuffer_t texture;
    clutbuffer_t clut;
    lod_t lod;
    texrect_t rect;
    qword_t *q;

    if (fbw == 0 || source_words == 0 || src_width < 64 || src_height < 16 ||
        src_width > fbw || dbx + src_width > fbw || src_height > 512)
        return 0;

    if (fbp < ASPECT_OUTPUT_ADDRESS + ASPECT_OUTPUT_SIZE && source_end > ASPECT_OUTPUT_ADDRESS)
        return 0;

    frame.address = ASPECT_OUTPUT_ADDRESS;
    frame.width = ASPECT_OUTPUT_WIDTH;
    frame.height = 512;
    frame.psm = GS_PSM_32;
    frame.mask = 0;

    z.enable = DRAW_DISABLE;
    z.method = ZTEST_METHOD_ALLPASS;
    z.address = 0;
    z.zsm = GS_ZBUF_32;
    z.mask = 1;

    texture.address = fbp;
    texture.width = fbw;
    texture.psm = psm;
    texture.info.width = draw_log2(fbw);
    texture.info.height = draw_log2(dby + src_height);
    texture.info.components = TEXTURE_COMPONENTS_RGBA;
    texture.info.function = TEXTURE_FUNCTION_DECAL;

    clut.address = 0;
    clut.psm = 0;
    clut.storage_mode = CLUT_STORAGE_MODE1;
    clut.start = 0;
    clut.load_method = CLUT_NO_LOAD;

    lod.calculation = LOD_USE_K;
    lod.max_level = 0;
    lod.mag_filter = LOD_MAG_NEAREST;
    lod.min_filter = LOD_MIN_NEAREST;
    lod.mipmap_select = LOD_MIPMAP_REGISTER;
    lod.l = 0;
    lod.k = 0.0f;

    rect.v0.x = corrected_x;
    rect.v0.y = 0.0f;
    rect.v0.z = 0;
    rect.t0.u = (float)dbx;
    rect.t0.v = (float)dby;
    rect.v1.x = corrected_x + corrected_width;
    rect.v1.y = (float)ASPECT_OUTPUT_HEIGHT;
    rect.v1.z = 0;
    rect.t1.u = (float)(dbx + src_width);
    rect.t1.v = (float)(dby + src_height);
    rect.color.r = 0x80;
    rect.color.g = 0x80;
    rect.color.b = 0x80;
    rect.color.a = 0x80;
    rect.color.q = 1.0f;

    draw_disable_blending();
    q = sAspectPacket;
    q = draw_setup_environment(q, 1, &frame, &z);
    q = draw_texture_sampling(q, 1, &lod);
    q = draw_texturebuffer(q, 1, &texture, &clut);
    q = draw_clear(q, 1, 0.0f, 0.0f, ASPECT_OUTPUT_WIDTH, ASPECT_OUTPUT_HEIGHT, 0, 0, 0);
    q = draw_rect_textured(q, 1, &rect);
    q = draw_finish(q);

    if (q - sAspectPacket > ASPECT_PACKET_QWORDS)
        return 0;

    FlushCache(WRITEBACK_DCACHE);
    dma_channel_wait(DMA_CHANNEL_GIF, 0);
    dma_channel_send_normal(DMA_CHANNEL_GIF, sAspectPacket, q - sAspectPacket, 0, 0);
    draw_wait_finish();

    /* Bypass GSM's data breakpoint for this compositor's privileged writes. */
    Disable_GSBreakpoint();
    *GS_REG_DISPFB1 = GS_SET_DISPFB(ASPECT_OUTPUT_ADDRESS >> 11, ASPECT_OUTPUT_WIDTH >> 6, GS_PSM_32, 0, 0);
    *GS_REG_DISPFB2 = GS_SET_DISPFB(ASPECT_OUTPUT_ADDRESS >> 11, ASPECT_OUTPUT_WIDTH >> 6, GS_PSM_32, 0, 0);
    *GS_REG_DISPLAY1 = GSMDestGSRegs.display1;
    *GS_REG_DISPLAY2 = GSMDestGSRegs.display2;
    Enable_GSBreakpoint();

    return 1;
}

static void AspectThread(void *arg)
{
    (void)arg;

    while (sAspectRunning) {
        WaitSema(sAspectSema);
        if (sAspectRunning)
            AspectCompose(&GSMSourceGSRegs);
    }

    ExitThread();
}

static int AspectVBlankHandler(int cause)
{
    (void)cause;
    if (sAspectRunning)
        iSignalSema(sAspectSema);
    ExitHandler();
    return 0;
}

int AspectFixInit(void)
{
    ee_sema_t sema;
    ee_thread_t thread;
    int thread_id;

    if (sAspectRunning)
        return 0;

    sema.count = 0;
    sema.max_count = 1;
    sema.init_count = 0;
    sema.wait_threads = 0;
    sema.attr = 0;
    sema.option = 0;
    sAspectSema = CreateSema(&sema);
    if (sAspectSema < 0)
        return -1;

    thread.status = 0;
    thread.func = AspectThread;
    thread.stack = sAspectStack;
    thread.stack_size = sizeof(sAspectStack);
    thread.gp_reg = GetGP();
    thread.initial_priority = 0x30;
    thread.current_priority = 0;
    thread.attr = 0;
    thread.option = 0;
    thread_id = CreateThread(&thread);
    if (thread_id < 0) {
        DeleteSema(sAspectSema);
        sAspectSema = -1;
        return -1;
    }

    sAspectRunning = 1;
    StartThread(thread_id, NULL);

    DIntr();
    sAspectVBlankHandler = AddIntcHandler(INTC_VBLANK_S, AspectVBlankHandler, -1);
    if (sAspectVBlankHandler >= 0)
        EnableIntc(INTC_VBLANK_S);
    EIntr();

    if (sAspectVBlankHandler < 0) {
        sAspectRunning = 0;
        SignalSema(sAspectSema);
        return -1;
    }

    return 0;
}

void AspectFixShutdown(void)
{
    if (!sAspectRunning)
        return;

    DIntr();
    if (sAspectVBlankHandler >= 0)
        RemoveIntcHandler(INTC_VBLANK_S, sAspectVBlankHandler);
    sAspectVBlankHandler = -1;
    EIntr();

    sAspectRunning = 0;
    SignalSema(sAspectSema);
}