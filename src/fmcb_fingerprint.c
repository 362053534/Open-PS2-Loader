#include "include/opl.h"
#include "include/gui.h"
#include "include/fmcb_fingerprint.h"

#include <kernel.h>
#include <syscallnr.h>
#include <osd_config.h>
#include <stdio.h>

#define FP_EELOAD_ADDR  0x00082000u
#define FP_EELOAD_SIZE  0x00002000u
#define FP_C0000_ADDR   0x000C0000u
#define FP_K30000_ADDR  0x80030000u
#define FP_K30000_SIZE  0x00002000u
#define FP_SPAD_ADDR    0x70000000u
#define FP_SPAD_SIZE    0x00004000u

static u32 crc32_bytes(const void *ptr, u32 len)
{
    const u8 *data = (const u8 *)ptr;
    u32 crc = 0xFFFFFFFFu;
    u32 i, bit;

    for (i = 0; i < len; i++) {
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

/* 内核段只能在内核态读；拷到用户缓冲后再算校验，避免在内核态里跑复杂逻辑。 */
static u32 crc32_kernel(u32 addr, u32 len)
{
    static u8 copy[FP_K30000_SIZE] __attribute__((aligned(64)));
    u32 i;
    vu8 *src;
    u32 n = len;

    if (n > sizeof(copy))
        n = sizeof(copy);

    DI();
    ee_kmode_enter();
    src = (vu8 *)addr;
    for (i = 0; i < n; i++)
        copy[i] = src[i];
    ee_kmode_exit();
    EI();

    return crc32_bytes(copy, n);
}

static u32 syscall_u32(int nr)
{
    return (u32)GetSyscallHandler(nr);
}

void fmcbShowFingerprint(void)
{
    char text[768];
    u32 osdparam = 0;

    FlushCache(0);

    GetOsdConfigParam((ConfigParam *)&osdparam);

    snprintf(text, sizeof(text),
             "FMCB fingerprint v1\n"
             "photo this screen\n"
             "\n"
             "EELOAD %08X\n"
             "C0000  %08X\n"
             "K30000 %08X\n"
             "SPAD   %08X\n"
             "\n"
             "ExecPS2  %08X\n"
             "LoadExec %08X\n"
             "GetOsd   %08X\n"
             "SetOsd   %08X\n"
             "GetOsd2  %08X\n"
             "SetOsd2  %08X\n"
             "OsdParam %08X\n"
             "\n"
             "press confirm",
             crc32_bytes((const void *)FP_EELOAD_ADDR, FP_EELOAD_SIZE),
             *(vu32 *)FP_C0000_ADDR,
             crc32_kernel(FP_K30000_ADDR, FP_K30000_SIZE),
             crc32_bytes((const void *)FP_SPAD_ADDR, FP_SPAD_SIZE),
             syscall_u32(__NR__ExecPS2),
             syscall_u32(__NR__LoadExecPS2),
             syscall_u32(__NR_GetOsdConfigParam),
             syscall_u32(__NR_SetOsdConfigParam),
             syscall_u32(__NR_GetOsdConfigParam2),
             syscall_u32(__NR_SetOsdConfigParam2),
             osdparam);

    guiShowFingerprint(text);
}
