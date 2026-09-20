#include "include/opl.h"
#include "include/gui.h"
#include "include/fmcb_fingerprint.h"

#include <kernel.h>
#include <syscallnr.h>
#include <osd_config.h>
#include <stdio.h>

#define FP_EELOAD_ADDR 0x00082000u
#define FP_EELOAD_SIZE 0x00002000u
#define FP_C0000_ADDR  0x000C0000u
#define FP_K000_ADDR   0x80000000u
#define FP_K000_SIZE   0x00004000u
#define FP_K300_ADDR   0x80030000u
#define FP_K300_SIZE   0x00008000u
#define FP_HI02_ADDR   0x00200000u
#define FP_HI02_SIZE   0x00100000u
#define FP_HI10_ADDR   0x01000000u
#define FP_HI10_SIZE   0x00100000u
#define FP_SPAD_ADDR   0x70000000u
#define FP_SPAD_SIZE   0x00004000u
#define FP_GS_CSR      0x12001000u

typedef struct
{
    u32 eeload;
    u32 c0000;
    u32 k000;
    u32 k300;
    u32 hi02;
    u32 hi10;
    u32 hi1f;
    u32 spad;
    u32 gscsr;
    u32 execps2;
    u32 loadexec;
    u32 getosd;
    u32 setosd;
    u32 osdparam;
} fp_snap_t;

static fp_snap_t gFpPre;
static int gFpPreReady;

static u32 crc32_bytes(const void *ptr, u32 len)
{
    const u8 *data = (const u8 *)ptr;
    u32 crc = 0xFFFFFFFFu;
    u32 i;

    for (i = 0; i < len; i++) {
        u32 bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/* 内核段分块拷到用户缓冲再算，避免在内核态做复杂运算。 */
static u32 crc32_kernel(u32 addr, u32 len)
{
    static u8 copy[0x1000] __attribute__((aligned(64)));
    u32 crc = 0xFFFFFFFFu;
    u32 off;

    for (off = 0; off < len; off += sizeof(copy)) {
        u32 n = len - off;
        u32 i;
        vu8 *src;

        if (n > sizeof(copy))
            n = sizeof(copy);

        DI();
        ee_kmode_enter();
        src = (vu8 *)(addr + off);
        for (i = 0; i < n; i++)
            copy[i] = src[i];
        ee_kmode_exit();
        EI();

        {
            const u8 *data = copy;
            for (i = 0; i < n; i++) {
                u32 bit;
                crc ^= data[i];
                for (bit = 0; bit < 8; bit++) {
                    if (crc & 1)
                        crc = (crc >> 1) ^ 0xEDB88320u;
                    else
                        crc >>= 1;
                }
            }
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

static u32 syscall_u32(int nr)
{
    return (u32)GetSyscallHandler(nr);
}

static u32 crc_hi1f(void)
{
    u32 mem = GetMemorySize();
    u32 addr;

    if (mem < 0x00100000u)
        mem = 0x02000000u;
    addr = mem - 0x00100000u;
    return crc32_bytes((const void *)addr, 0x00100000u);
}

static void fp_take_snapshot(fp_snap_t *snap)
{
    u32 osdparam = 0;

    FlushCache(0);
    GetOsdConfigParam((ConfigParam *)&osdparam);

    snap->eeload = crc32_bytes((const void *)FP_EELOAD_ADDR, FP_EELOAD_SIZE);
    snap->c0000 = *(vu32 *)FP_C0000_ADDR;
    snap->k000 = crc32_kernel(FP_K000_ADDR, FP_K000_SIZE);
    snap->k300 = crc32_kernel(FP_K300_ADDR, FP_K300_SIZE);
    snap->hi02 = crc32_bytes((const void *)FP_HI02_ADDR, FP_HI02_SIZE);
    snap->hi10 = crc32_bytes((const void *)FP_HI10_ADDR, FP_HI10_SIZE);
    snap->hi1f = crc_hi1f();
    snap->spad = crc32_bytes((const void *)FP_SPAD_ADDR, FP_SPAD_SIZE);
    snap->gscsr = (u32) * (vu32 *)FP_GS_CSR;
    snap->execps2 = syscall_u32(__NR__ExecPS2);
    snap->loadexec = syscall_u32(__NR__LoadExecPS2);
    snap->getosd = syscall_u32(__NR_GetOsdConfigParam);
    snap->setosd = syscall_u32(__NR_SetOsdConfigParam);
    snap->osdparam = osdparam;
}

void fmcbCaptureFingerprint(void)
{
    fp_take_snapshot(&gFpPre);
    gFpPreReady = 1;
}

void fmcbShowFingerprint(void)
{
    fp_snap_t post;
    char text[1280];
    const fp_snap_t *pre = gFpPreReady ? &gFpPre : &post;

    fp_take_snapshot(&post);
    if (!gFpPreReady)
        pre = &post;

    snprintf(text, sizeof(text),
             "FMCB fp v2  PRE      POST\n"
             "EELOAD      %08X %08X\n"
             "C0000       %08X %08X\n"
             "K000        %08X %08X\n"
             "K300        %08X %08X\n"
             "HI02        %08X %08X\n"
             "HI10        %08X %08X\n"
             "HI1F        %08X %08X\n"
             "SPAD        %08X %08X\n"
             "GSCSR       %08X %08X\n"
             "ExecPS2     %08X %08X\n"
             "LoadExec    %08X %08X\n"
             "GetOsd      %08X %08X\n"
             "OsdParam    %08X %08X\n"
             "IGR: Exit Path = this ELF\n"
             "press confirm",
             pre->eeload, post.eeload,
             pre->c0000, post.c0000,
             pre->k000, post.k000,
             pre->k300, post.k300,
             pre->hi02, post.hi02,
             pre->hi10, post.hi10,
             pre->hi1f, post.hi1f,
             pre->spad, post.spad,
             pre->gscsr, post.gscsr,
             pre->execps2, post.execps2,
             pre->loadexec, post.loadexec,
             pre->getosd, post.getosd,
             pre->osdparam, post.osdparam);

    guiShowFingerprint(text);

    /* 游戏启动链上不能清高位（会把读盘通道清掉）。进菜单前只清顶 1MB。 */
    {
        u32 mem = GetMemorySize();

        if (mem > 0x00100000u) {
            memset((void *)(mem - 0x00100000u), 0, 0x00100000u);
            FlushCache(0);
        }
    }
}
