/*
  padhook.c Open PS2 Loader In Game Reset

  Copyright 2009-2010, Ifcaro, jimmikaelkael & Polo
  Copyright 2006-2008 Polo
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Reset SPU function taken from PS2SDK freesd.
  Copyright (c) 2004 TyRaNiD <tiraniddo@hotmail.com>
  Copyright (c) 2004,2007 Lukasz Bruun <mail@lukasz.dk>

  PadOpen Hooking function inspired from ps2rd.
  Hook scePadPortOpen/scePad2CreateSocket instead of scePadRead/scePad2Read
  Copyright (C) 2009 jimmikaelkael <jimmikaelkael@wanadoo.fr>
  Copyright (C) 2009 misfire <misfire@xploderfreax.de>
*/

#include <ee_regs.h>
#include <iopcontrol.h>
#include <sifrpc.h>
#include "asm.h"
#include "ee_core.h"
#include "iopmgr.h"
#include "modmgr.h"
#include "modules.h"
#include "util.h"
#include "padhook.h"
#include "padpatterns.h"
#include "syshook.h"
#include "tlb.h"
#include "gsm_api.h"
#ifdef IGS
#include "igs_api.h"
#endif
#include "cheat_api.h"
#include "cd_igr_rpc.h"
#include "coreconfig.h"
#define NEWLIB_PORT_AWARE
#include <fileXio.h>
#include <io_common.h>

/* scePadPortOpen & scePad2CreateSocket prototypes */
static int (*scePadPortOpen)(int port, int slot, void *addr);
static int (*scePad2CreateSocket)(pad2socketparam_t *SocketParam, void *addr);

/* Monitored pad data */
static paddata_t Pad_Data;

/* Monitored power button data */
static powerbuttondata_t Power_Button;

/* IGR Thread ID and interrupt handler */
static int IGR_Thread_ID = -1;
static int IGR_Intc_ID = -1;

/* IGR thread stack & stack size */
#define IGR_STACK_SIZE (4 * 1024)
static u8 IGR_Stack[IGR_STACK_SIZE] __attribute__((aligned(16)));

/* Extern symbol */
extern void *_gp;
extern void *_end;

static SifRpcClientData_t fileXioClient __attribute__((section(".ramD0")));
static u32 fileXioBuffer[(sizeof(struct fxio_mount_packet) + 3) / 4] __attribute__((section(".ramD0"), aligned(64)));

// IGR 只需要挂载 PFS，使用最小 RPC 避免引入完整 fileXio 缓冲区。
static int mountPfs(const char *partition)
{
    struct fxio_mount_packet *packet = (struct fxio_mount_packet *)fileXioBuffer;
    int ret, result;

    memset(&fileXioClient, 0, sizeof(fileXioClient));
    while ((ret = SifBindRpc(&fileXioClient, FILEXIO_IRX, 0)) >= 0 && !fileXioClient.server)
        nopdelay();
    if (ret < 0)
        return ret;

    memset(packet, 0, sizeof(*packet));
    strncpy(packet->blockdevice, partition, sizeof(packet->blockdevice) - 1);
    memcpy(packet->mountpoint, "pfs0:", sizeof("pfs0:"));
    packet->flags = FIO_MT_RDONLY;

    ret = SifCallRpc(&fileXioClient, FILEXIO_MOUNT, 0, packet, sizeof(*packet), &result, sizeof(result), NULL, NULL);
    return ret < 0 ? ret : result;
}

// 无有效退出路径时的“干净重启”，定义见下方。
static void IGR_CleanReboot(void);

// Load home ELF
static void t_loadElf(void)
{
    USE_LOCAL_EECORE_CONFIG;
    int ret;
    char *argv[2];
    t_ExecData elf;
    const char *loadPath = config->ExitPath;
    char pfsPath[CORE_EXIT_PATH_MAX_LEN];
    char partition[CORE_EXIT_PATH_MAX_LEN];

    if (EnableDebug)
        DBGCOL(0x80FF00, LOADELF, "t_loadElf() begins");

    // Init RPC & CMD
    SifInitRpc(0);

    if (EnableDebug)
        DBGCOL(0x000080, LOADELF, "Patch prefix check");

    // Apply Sbv patches
    sbv_patch_disable_prefix_check();

    if (EnableDebug)
        DBGCOL(0xFF8000, LOADELF, "loading SIO2 modules and USBD if found");

    // Load basic modules
    LoadModule("rom0:SIO2MAN", 0, NULL);
    LoadModule("rom0:MCMAN", 0, NULL);
    delay(1);

    if (config->ExitPath[1] == 'a') { // ie mass:
        ret = LoadModule("mc0:SYS-CONF/USBD.IRX", 0, NULL);
        if (ret >= 0)
            LoadModule("mc0:SYS-CONF/USBHDFSD.IRX", 0, NULL);
        else {
            LoadModule("mc1:SYS-CONF/USBD.IRX", 0, NULL);
            LoadModule("mc1:SYS-CONF/USBHDFSD.IRX", 0, NULL);
        }
        delay(5); // Wait for device to be detected.
    } else if ((_toupper(config->ExitPath[0]) == 'H' && _toupper(config->ExitPath[1]) == 'D' &&
                _toupper(config->ExitPath[2]) == 'D' && config->ExitPath[3] == '0' && config->ExitPath[4] == ':') ||
               (_toupper(config->ExitPath[0]) == 'P' && _toupper(config->ExitPath[1]) == 'F' &&
                _toupper(config->ExitPath[2]) == 'S' && config->ExitPath[3] == '0' && config->ExitPath[4] == ':')) {
        // HDD/PFS 路径需要先恢复 APA、PFS 文件系统模块。
        LoadModule("rom0:ATAD", 0, NULL);
        LoadModule("rom0:PS2HDD", 0, NULL);
        LoadModule("rom0:PFS", 0, NULL);

        if (_toupper(config->ExitPath[0]) == 'H' && _toupper(config->ExitPath[1]) == 'D' &&
            _toupper(config->ExitPath[2]) == 'D' && config->ExitPath[3] == '0' && config->ExitPath[4] == ':') {
            const char *path = config->ExitPath + 5;
            const char *separator;
            const char *filePath;
            int pfsLength;

            while (*path == '/' || *path == '\\')
                path++;
            separator = path;
            while (*separator && *separator != '/' && *separator != '\\' && *separator != ':')
                separator++;
            if (*separator) {
                int partitionLength = separator - path;

                memcpy(partition, "hdd0:", 5);
                memcpy(partition + 5, path, partitionLength);
                partition[5 + partitionLength] = '\0';
                memcpy(pfsPath, "pfs0:/", 6);
                pfsLength = 6;
                filePath = separator;
                if (*filePath == ':')
                    filePath++;
                while (*filePath == '/' || *filePath == '\\')
                    filePath++;
                while (*filePath && pfsLength < (int)sizeof(pfsPath) - 1) {
                    char c = *filePath++;
                    if (c == '\\')
                        c = '/';
                    if (c == '/' && pfsLength > 6 && pfsPath[pfsLength - 1] == '/')
                        continue;
                    pfsPath[pfsLength++] = c;
                }
                pfsPath[pfsLength] = '\0';

                // 先恢复挂载，再把用户填写的 hdd0: 路径转换成分区感知的加载参数。
                if (mountPfs(partition) == 0)
                    loadPath = pfsPath;
            }
        } else {
            const char *path = config->ExitPath + 5;
            int pfsLength;

            while (*path == '/' || *path == '\\')
                path++;
            memcpy(pfsPath, "pfs0:/", 6);
            pfsLength = 6;
            while (*path && pfs
