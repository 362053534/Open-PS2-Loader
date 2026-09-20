/*
  Copyright 2009-2010, Ifcaro, jimmikaelkael & Polo
  Copyright 2006-2008 Polo
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Some parts of the code are taken from HD Project by Polo
*/

#include "ee_core.h"
#include "asm.h"
#include "iopmgr.h"
#include "modmgr.h"
#include "util.h"
#include "patches.h"
#include "padhook.h"
#include "syshook.h"
#include "coreconfig.h"

#include <syscallnr.h>
#include <ee_regs.h>
#include <ps2_reg_defs.h>

int set_reg_hook;
int set_reg_disabled;
int iop_reboot_count = 0;

int padOpen_hooked = 0;
int disable_padOpen_hook = 1;

extern void *_end;

// Global data
u32 (*Old_SifSetDma)(SifDmaTransfer_t *sdd, s32 len);
int (*Old_SifSetReg)(u32 register_num, int register_value);
int (*Old_ExecPS2)(void *entry, void *gp, int num_args, char *args[]);
int (*Old_CreateThread)(ee_thread_t *thread_param);
void (*Old_Exit)(s32 exit_code);
void (*Old_SetOsdConfigParam)(ConfigParam *osdconfig);
void (*Old_GetOsdConfigParam)(ConfigParam *osdconfig);

#define HIGH_WIPE_TRAMP 0x1000u
#define HIGH_WIPE_KEEP  0x00084000u
#define HIGH_WIPE_ELF   0x00100000u

typedef struct
{
    u32 wipe0_end;
    u32 wipe1_start;
    u32 wipe1_end;
    u32 epc;
    u32 gp;
    u32 argc;
    char **argv;
    u32 stack;
    u32 exec_ptr;
} high_wipe_boot_t;

static void copy_bytes(void *dst, const void *src, unsigned int n)
{
    u8 *d = dst;
    const u8 *s = src;

    while (n--)
        *d++ = *s++;
}

static int elf_pt_load_range(u32 guess, u32 *base_out, u32 *end_out)
{
    const u8 *eh = (const u8 *)guess;
    const u8 *ph;
    u32 phoff, i, base, end;
    u16 phentsize, phnum;

    if (eh[0] != 0x7f || eh[1] != 'E' || eh[2] != 'L' || eh[3] != 'F')
        return -1;

    phoff = *(const u32 *)(eh + 28);
    phentsize = *(const u16 *)(eh + 42);
    phnum = *(const u16 *)(eh + 44);
    if (phentsize < 32 || phnum == 0 || phnum > 32)
        return -1;

    base = 0xffffffffu;
    end = 0;
    for (i = 0; i < phnum; i++) {
        u32 type, vaddr, memsz;

        ph = eh + phoff + i * phentsize;
        type = *(const u32 *)(ph + 0);
        if (type != 1)
            continue;
        vaddr = *(const u32 *)(ph + 8);
        memsz = *(const u32 *)(ph + 20);
        if (vaddr < base)
            base = vaddr;
        if (vaddr + memsz > end)
            end = vaddr + memsz;
    }

    if (base == 0xffffffffu || end <= base)
        return -1;

    *base_out = base;
    *end_out = (end + 63u) & ~63u;
    return 0;
}

/* 游戏已在内存中：跳到顶页清空隙再 Exec。成功不返回。 */
int highmem_wipe_exec(void *epc, void *gp, int argc, char **argv)
{
    u32 memSize = GetMemorySize();
    u8 *page;
    u32 codeSize;
    u32 elf_base = HIGH_WIPE_ELF;
    u32 elf_end = 0;
    high_wipe_boot_t *boot;
    char **nargv;
    char *nstr;
    void (*tramp)(void *);
    int i;

    if (Old_ExecPS2 == NULL)
        Old_ExecPS2 = GetSyscallHandler(__NR__ExecPS2);

    codeSize = (u32)((char *)&_HighWipeAndExec_end - (char *)HighWipeAndExec);
    if (memSize <= HIGH_WIPE_TRAMP || codeSize < 64 || codeSize > 0x2F0)
        return -1;

    page = (u8 *)(memSize - HIGH_WIPE_TRAMP);

    if ((u32)epc >= HIGH_WIPE_ELF)
        elf_base = HIGH_WIPE_ELF;
    else
        elf_base = (u32)epc & ~0xfffu;

    if (elf_pt_load_range(elf_base, &elf_base, &elf_end) != 0)
        return -1;

    if (elf_base < HIGH_WIPE_KEEP)
        elf_base = HIGH_WIPE_KEEP;
    if (elf_end > (u32)page)
        elf_end = (u32)page;

    copy_bytes(page, (const void *)HighWipeAndExec, codeSize);

    boot = (high_wipe_boot_t *)(page + 0x300);
    nargv = (char **)(page + 0x340);
    nstr = (char *)(page + 0x380);

    if (argc < 0)
        argc = 0;
    if (argc > 4)
        argc = 4;
    for (i = 0; i < argc; i++) {
        const char *s = (argv && argv[i]) ? argv[i] : "";
        int n = 0;

        while (s[n] && n < 127)
            n++;
        copy_bytes(nstr, s, (unsigned int)n + 1);
        nargv[i] = nstr;
        nstr += n + 1;
        if (nstr > (char *)page + 0x0E00)
            break;
    }

    boot->wipe0_end = elf_base;
    if (boot->wipe0_end < HIGH_WIPE_KEEP)
        boot->wipe0_end = HIGH_WIPE_KEEP;
    if (elf_end > elf_base && elf_end < (u32)page) {
        boot->wipe1_start = elf_end;
        boot->wipe1_end = (u32)page;
    } else {
        boot->wipe1_start = 0;
        boot->wipe1_end = 0;
    }
    boot->epc = (u32)epc;
    boot->gp = (u32)gp;
    boot->argc = (u32)argc;
    boot->argv = nargv;
    boot->stack = (u32)page + 0x0F00;
    boot->exec_ptr = (u32)Old_ExecPS2;

    FlushCache(0);
    FlushCache(2);

    tramp = (void (*)(void *))page;
    tramp(boot);
    return -1;
}

/*----------------------------------------------------------------------------------------*/
/* This function is called when SifSetDma catches a reboot request.                       */
/*----------------------------------------------------------------------------------------*/
u32 New_SifSetDma(SifDmaTransfer_t *sdd, s32 len)
{
    // Hook padOpen function to install In Game Reset
    if (!(g_compat_mask & COMPAT_MODE_6) && padOpen_hooked == 0) {
        Install_IGR();
        padOpen_hooked = Install_PadOpen_Hook(0x00100000, 0x01ff0000, PADOPEN_HOOK);
    }

    struct _iop_reset_pkt *reset_pkt = (struct _iop_reset_pkt *)sdd->src;

    disable_padOpen_hook = 1;

    // does IOP reset
    New_Reset_Iop(reset_pkt->arg, reset_pkt->arglen);

    disable_padOpen_hook = 0;

    return 1;
}

// ------------------------------------------------------------------------
void sysLoadElf(char *filename, int argc, char **argv)
{
    USE_LOCAL_EECORE_CONFIG;
    int r;
    t_ExecData elf;

    SifInitRpc(0);

    DPRINTF("t_loadElf()\n");

    DPRINTF("t_loadElf: Resetting IOP...\n");

    set_reg_disabled = 0;
    New_Reset_Iop(NULL, 0);
    set_reg_disabled = 1;

    iop_reboot_count = 1;

    SifInitRpc(0);
    LoadFileInit();

    DPRINTF("t_loadElf: elf path = '%s'\n", filename);

    if (EnableDebug)
        DBGCOL(0x00FF00, SYSHOOK, "WipeUserMemory()");

    DPRINTF("t_loadElf: cleaning user memory...");

    // wipe user memory
    WipeUserMemory((void *)&_end, (void *)config->ModStorageStart);
    // The upper half (from ModStorageEnd to GetMemorySize()) is taken care of by LoadExecPS2().
    // WipeUserMemory((void *)ModStorageEnd, (void *)GetMemorySize());

    FlushCache(0);

    DPRINTF(" done\n");

    DPRINTF("t_loadElf: loading elf...");
    r = LoadElf(filename, &elf);

    if (!r) {
        DPRINTF(" done\n");

        DPRINTF("t_loadElf: trying to apply patches...\n");
        // applying needed patches
        apply_patches(filename);

        FlushCache(0);
        FlushCache(2);

        DPRINTF("t_loadElf: exiting services...\n");
        // exit services
        SifExitIopHeap();
        LoadFileExit();
        SifExitRpc();

        disable_padOpen_hook = 0;

        DPRINTF("t_loadElf: executing...\n");
        /* 游戏已读入后再跳板清高位空隙，避开 EELOAD / 读盘通道。 */
        if (highmem_wipe_exec((void *)elf.epc, (void *)elf.gp, argc, argv) != 0)
            CleanExecPS2((void *)elf.epc, (void *)elf.gp, argc, argv);
    }

    DPRINTF(" failed\n");

    // Error
    DBGCOL(0xFFFFFF, LOADELF, "sysLoadElf() error. hitting function end");
    SleepThread();
}

static void unpatchEELOADCopy(void)
{
    USE_LOCAL_EECORE_CONFIG;
    vu32 *p = (vu32 *)config->eeloadCopy;

    p[1] = 0x0240302D; /* daddu    a2, s2, zero */
    p[2] = 0x8FA50014; /* lw       a1, 0x0014(sp) */
    p[3] = 0x8C67000C; /* lw       a3, 0x000C(v1) */
}

static void unpatchInitUserMemory(void)
{
    USE_LOCAL_EECORE_CONFIG;
    vu16 *p = (vu16 *)config->initUserMemory;

    /*
     * Reset the start of user memory to 0x00082000, by changing the immediate value being loaded into $a0.
     *  lui  $a0, 0x0008
     *  jal  InitializeUserMemory
     *  ori  $a0, $a0, 0x2000
     */
    p[0] = 0x0008;
    p[4] = 0x2000;
}

void sysExit(s32 exit_code)
{
    Remove_Kernel_Hooks();
    IGR_Exit(exit_code);
}

void hook_SetOsdConfigParam(ConfigParam *osdconfig)
{
    USE_LOCAL_EECORE_CONFIG;

    DPRINTF("%s: called\n", __func__);
    config->CustomOSDConfigParam.spdifMode = osdconfig->spdifMode;
    config->CustomOSDConfigParam.screenType = osdconfig->screenType;
    config->CustomOSDConfigParam.videoOutput = osdconfig->videoOutput;
    config->CustomOSDConfigParam.japLanguage = osdconfig->japLanguage;
    config->CustomOSDConfigParam.ps1drvConfig = osdconfig->ps1drvConfig;
    config->CustomOSDConfigParam.version = osdconfig->version;
    config->CustomOSDConfigParam.language = osdconfig->language;
    config->CustomOSDConfigParam.timezoneOffset = osdconfig->timezoneOffset;
}

void hook_GetOsdConfigParam(ConfigParam *osdconfig)
{
    USE_LOCAL_EECORE_CONFIG;

    DPRINTF("%s: called\n", __func__);
    osdconfig->spdifMode = config->CustomOSDConfigParam.spdifMode;
    osdconfig->screenType = config->CustomOSDConfigParam.screenType;
    osdconfig->videoOutput = config->CustomOSDConfigParam.videoOutput;
    osdconfig->japLanguage = config->CustomOSDConfigParam.japLanguage;
    osdconfig->ps1drvConfig = config->CustomOSDConfigParam.ps1drvConfig;
    osdconfig->version = config->CustomOSDConfigParam.version;
    osdconfig->language = config->CustomOSDConfigParam.language;
    osdconfig->timezoneOffset = config->CustomOSDConfigParam.timezoneOffset;
}

/*----------------------------------------------------------------------------------------*/
/* Replace SifSetDma, SifSetReg, LoadExecPS2 syscalls in kernel. (Game Loader)            */
/* Replace CreateThread and ExecPS2 syscalls in kernel. (In Game Reset)                   */
/*----------------------------------------------------------------------------------------*/
void Install_Kernel_Hooks(void)
{
    USE_LOCAL_EECORE_CONFIG;
    if (config->enforceLanguage) {
        Old_SetOsdConfigParam = GetSyscallHandler(__NR_SetOsdConfigParam);
        SetSyscall(__NR_SetOsdConfigParam, &hook_SetOsdConfigParam);
        Old_GetOsdConfigParam = GetSyscallHandler(__NR_GetOsdConfigParam);
        SetSyscall(__NR_GetOsdConfigParam, &hook_GetOsdConfigParam);
    }

    Old_SifSetDma = GetSyscallHandler(__NR_SifSetDma);
    SetSyscall(__NR_SifSetDma, &Hook_SifSetDma);

    Old_SifSetReg = GetSyscallHandler(__NR_SifSetReg);
    SetSyscall(__NR_SifSetReg, &Hook_SifSetReg);

    // If IGR is enabled hook ExecPS2 & CreateThread syscalls
    if (!(g_compat_mask & COMPAT_MODE_6)) {
        Old_CreateThread = GetSyscallHandler(__NR_CreateThread);
        SetSyscall(__NR_CreateThread, &Hook_CreateThread);

        Old_ExecPS2 = GetSyscallHandler(__NR__ExecPS2);
        SetSyscall(__NR__ExecPS2, &Hook_ExecPS2);
    }

    Old_Exit = GetSyscallHandler(__NR_KExit);
    SetSyscall(__NR_KExit, &Hook_Exit);
}

/*----------------------------------------------------------------------------------------------*/
/* Restore original SifSetDma, SifSetReg, LoadExecPS2, Exit syscalls in kernel. (Game loader)   */
/* Restore original CreateThread and ExecPS2 syscalls in kernel. (In Game Reset)                */
/*----------------------------------------------------------------------------------------------*/
void Remove_Kernel_Hooks(void)
{
    USE_LOCAL_EECORE_CONFIG;
    if (config->enforceLanguage) {
        SetSyscall(__NR_SetOsdConfigParam, Old_SetOsdConfigParam);
        SetSyscall(__NR_GetOsdConfigParam, Old_GetOsdConfigParam);
    }
    SetSyscall(__NR_SifSetDma, Old_SifSetDma);
    SetSyscall(__NR_SifSetReg, Old_SifSetReg);
    SetSyscall(__NR_KExit, Old_Exit);

    DI();
    ee_kmode_enter();

    unpatchEELOADCopy();
    unpatchInitUserMemory();

    ee_kmode_exit();
    EI();

    // If IGR is enabled unhook ExecPS2 & CreateThread syscalls
    if (!(g_compat_mask & COMPAT_MODE_6)) {
        SetSyscall(__NR_CreateThread, Old_CreateThread);
        SetSyscall(__NR__ExecPS2, Old_ExecPS2);
    }

    FlushCache(0);
    FlushCache(2);
}
