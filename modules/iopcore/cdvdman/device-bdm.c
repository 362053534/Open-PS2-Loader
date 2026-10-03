/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"

#include <bdm.h>
#include <bd_defrag.h>

#include "device.h"

#ifdef USE_BDM_ATA
#include "atad.h"
char lba_48bit = 0;
char atad_inited = 0;
#endif

extern struct cdvdman_settings_bdm cdvdman_settings;
#define INVALID_BD_GENERIC_SECTOR ((u64)-1)
static struct block_device *g_bd = NULL;
static u32 g_bd_sectors_per_sector = 4;
static int bdm_io_sema;
static u8 *g_bd_generic_sector_buffer_2 = NULL;
static u32 g_bd_generic_sector_buffer_size_2 = 0;
static u64 g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
static bd_defrag_cursor_t g_bd_defrag_cursor;
static bd_defrag_index_t g_bd_defrag_index;
static bd_fragment_t *g_frag_table = NULL;
static bd_defrag_checkpoint_t *g_bd_defrag_checkpoints = NULL;
static bd_fragment_t *g_bdm_fragment_pending = NULL;
static u32 g_bdm_fragment_pending_bytes = 0;
static u8 g_frag_table_owned = 0;
static int g_bdm_fragment_rpc_thread_id = -1;
static volatile u8 g_bdm_fragment_rpc_registered = 0;
static SifRpcDataQueue_t g_bdm_fragment_rpc_queue __attribute__((aligned(64)));
static SifRpcServerData_t g_bdm_fragment_rpc_server __attribute__((aligned(64)));
static u8 g_bdm_fragment_rpc_buffer[64] __attribute__((aligned(64)));
enum bdm_frag_table_state {
    BDM_FRAG_TABLE_EMPTY = 0,
    BDM_FRAG_TABLE_LOADING,
    BDM_FRAG_TABLE_READY,
    BDM_FRAG_TABLE_FAILED
};
static volatile u8 g_frag_table_state = BDM_FRAG_TABLE_EMPTY;
/* 大文件以64个碎片为步长，限制随机定位时的线性扫描长度。 */
#define BDM_DEFRAG_CHECKPOINT_STRIDE 64
/* 该边界覆盖APA HDL和单个PFS inode能够提供的完整区段表。 */
#define BDM_DEFRAG_DENSE_INDEX_LIMIT 114

/* 调试用：记录「当前正在读的请求」，卡住时日志里最后一条就是它。
 * 发布版不建看门狗线程、这些字段也不参与逻辑。 */
static volatile u32 g_bdm_cur_lsn;
static volatile u32 g_bdm_cur_sectors;
static volatile u32 g_bdm_cur_start_lo;
static volatile int g_bdm_cur_busy;
#ifdef __IOPCORE_DEBUG
static void bdm_watchdog_thread(void *arg);
#endif

static u32 bdm_get_checkpoint_stride(u32 fragcount)
{
    /* 小表为每个碎片建立检查点，避免随机读取仍从表头扫描。 */
    return fragcount < BDM_DEFRAG_DENSE_INDEX_LIMIT ? 1 : BDM_DEFRAG_CHECKPOINT_STRIDE;
}

static u32 bdm_get_checkpoint_count(u32 fragcount, u32 stride)
{
    u32 count = fragcount / stride;

    if ((fragcount % stride) != 0)
        count++;
    return count;
}

extern struct irx_export_table _exp_bdm;

#ifdef USE_BDM_ATA
extern struct irx_export_table _exp_atad;
#endif

/* 调试用：统计 BDM 设备的实际吞吐（USB / MX4SIO / HDD 都走这里）。
 * 用途：和日志里「游戏每秒要了多少扇区」对照——游戏要 1.7MB/s 而设备只给 1MB/s，
 * 还是设备给得动、游戏自己读法不对，一眼能分开。
 * 只在调试构建（make IOPCORE_DEBUG=1）里编译，发布版整个函数体为空，零开销。 */
static void bdmRateAccount(u32 bytes)
{
#ifdef __IOPCORE_DEBUG
    static u32 acc_bytes, window_start_lo;
    static const char *last_name;
    iop_sys_clock_t now;
    u32 elapsed_ms, kb_per_s;

    if (bytes == 0)
        return;

    GetSystemTime(&now);
    if (window_start_lo == 0 || last_name == NULL) {
        window_start_lo = now.lo;
        last_name = g_bd != NULL ? g_bd->name : "bdm";
        return;
    }

    acc_bytes += bytes;

    /* 36.864MHz，/37 得到微秒（和 cdvdfsv 里取时间的算法保持一致）。 */
    elapsed_ms = (now.lo - window_start_lo) / 37u / 1000u;
    if (elapsed_ms >= 1000u) {
        kb_per_s = (acc_bytes / elapsed_ms) * 1000u / 1024u;
        DPRINTF("%s rate: %u KB/s (%u KB in %u ms)\n", last_name, kb_per_s, acc_bytes >> 10, elapsed_ms);
        acc_bytes = 0;
        window_start_lo = now.lo;
    }
#else
    (void)bytes;
#endif
}

//
// BDM exported functions
//

void bdm_connect_bd(struct block_device *bd)
{
    DPRINTF("connecting device %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

    if (g_bd == NULL && bd->devNr == cdvdman_settings.bdDeviceId) {
        DPRINTF("attaching to %s%dp%d\n", bd->name, bd->devNr, bd->parNr);
        g_bd = bd;
        bd_defrag_index_reset(&g_bd_defrag_index);
        bd_defrag_cursor_reset(&g_bd_defrag_cursor);
        g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
        g_bd_sectors_per_sector = (2048 / bd->sectorSize);
        // Free usage of block device
        SignalSema(bdm_io_sema);
    }
}

void bdm_disconnect_bd(struct block_device *bd)
{
    DPRINTF("disconnecting device %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

    if (bd->devNr == cdvdman_settings.bdDeviceId) {
        DPRINTF("detatching from %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

        // Lock usage of block device
        WaitSema(bdm_io_sema);
        if (g_bd == bd) {
            g_bd = NULL;
            bd_defrag_cursor_reset(&g_bd_defrag_cursor);
            bd_defrag_index_reset(&g_bd_defrag_index);
            g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
        }
    }
}

//
// cdvdman "Device" functions
//

static void *bdm_fragment_rpc_handler(int function, void *buffer, int length)
{
    struct bdm_fragment_rpc *request = buffer;
    u32 required_bytes;

    if (buffer == NULL || length < (int)sizeof(*request))
        return buffer;

    request->result = -1;
    if (request->fragment_count == 0 ||
        request->fragment_count > 0xFFFFFFFFU / sizeof(bd_fragment_t))
        return buffer;

    required_bytes = request->fragment_count * sizeof(bd_fragment_t);
    if (request->fragment_bytes < required_bytes ||
        (request->fragment_bytes & 0xF) != 0)
        return buffer;

    if (function == BDM_FRAGMENT_RPC_PREPARE) {
        if (g_bdm_fragment_pending != NULL)
            FreeSysMemory(g_bdm_fragment_pending);
        g_bdm_fragment_pending = AllocSysMemory(ALLOC_FIRST, request->fragment_bytes, NULL);
        if (g_bdm_fragment_pending == NULL) {
            g_bdm_fragment_pending_bytes = 0;
            return buffer;
        }
        g_bdm_fragment_pending_bytes = request->fragment_bytes;
        request->iop_address = (u32)g_bdm_fragment_pending;
        request->fragment_bytes = g_bdm_fragment_pending_bytes;
        request->result = 0;
    } else if (function == BDM_FRAGMENT_RPC_COMMIT) {
        if (g_bdm_fragment_pending == NULL ||
            g_bdm_fragment_pending_bytes != request->fragment_bytes ||
            request->iop_address != (u32)g_bdm_fragment_pending)
            return buffer;

        g_frag_table = g_bdm_fragment_pending;
        g_frag_table_owned = 1;
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
        cdvdman_settings.fragfile[0].frag_count = request->fragment_count;
        request->result = 0;
    }

    return buffer;
}

static void bdm_fragment_rpc_thread(void *arg)
{
    (void)arg;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&g_bdm_fragment_rpc_queue, GetThreadId());
    sceSifRegisterRpc(&g_bdm_fragment_rpc_server, BDM_FRAGMENT_RPC_ID,
                      bdm_fragment_rpc_handler, g_bdm_fragment_rpc_buffer,
                      NULL, NULL, &g_bdm_fragment_rpc_queue);
    g_bdm_fragment_rpc_registered = 1;
    sceSifRpcLoop(&g_bdm_fragment_rpc_queue);
}

static int bdm_prepare_fragment_table(void)
{
    unsigned int i;
    u32 fragcount = cdvdman_settings.fragfile[0].frag_count;
    u32 stride = bdm_get_checkpoint_stride(fragcount);
    u32 checkpoint_count = bdm_get_checkpoint_count(fragcount, stride);

    if (g_bd == NULL)
        return -1;

    g_frag_table_state = BDM_FRAG_TABLE_LOADING;
    if (g_frag_table == NULL) {
        g_frag_table_state = BDM_FRAG_TABLE_FAILED;
        return -1;
    }

    if (cdvdman_settings.fragsAre512ByteSectors && g_bd->sectorSize != 512) {
        unsigned int sectors_per_pfs_sector = g_bd->sectorSize >> 9;

        while (sectors_per_pfs_sector > 1) {
            for (i = 0; i < fragcount; i++) {
                g_frag_table[i].sector >>= 1;
                g_frag_table[i].count >>= 1;
            }
            sectors_per_pfs_sector >>= 1;
        }
        cdvdman_settings.fragsAre512ByteSectors = 0;
    }

    bd_defrag_index_reset(&g_bd_defrag_index);
    if (g_bd_defrag_checkpoints != NULL &&
        bd_defrag_index_build(&g_bd_defrag_index,
                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                              fragcount,
                              stride,
                              g_bd_defrag_checkpoints,
                              checkpoint_count) < 0)
        DPRINTF("fragment index build failed; using linear lookup\n");
    bd_defrag_cursor_reset(&g_bd_defrag_cursor);

    /* 表、扇区单位和索引全部就绪后再允许光盘读取。 */
    g_frag_table_state = BDM_FRAG_TABLE_READY;
    return 0;
}

void DeviceInit(void)
{
    iop_sema_t smp;
    iop_thread_t thread;

    DPRINTF("%s\n", __func__);

    // Create semaphore, initially locked
    smp.initial = 0;
    smp.max = 1;
    smp.option = 0;
    smp.attr = SA_THPRI;
    bdm_io_sema = CreateSema(&smp);
    bd_defrag_cursor_reset(&g_bd_defrag_cursor);
    bd_defrag_index_reset(&g_bd_defrag_index);
    g_frag_table_state = BDM_FRAG_TABLE_EMPTY;
    g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
    if (g_bdm_fragment_pending != NULL) {
        FreeSysMemory(g_bdm_fragment_pending);
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
    }

    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = bdm_fragment_rpc_thread;
    thread.stacksize = 0x1000;
    thread.priority = 0x40;
    g_bdm_fragment_rpc_thread_id = CreateThread(&thread);
    if (g_bdm_fragment_rpc_thread_id >= 0)
        StartThread(g_bdm_fragment_rpc_thread_id, NULL);

#ifdef __IOPCORE_DEBUG
    /* 读看门狗，只在调试构建里建（发布版不建线程、不占栈）。 */
    g_bdm_cur_busy = 0;
    thread.option = 0;
    thread.thread = bdm_watchdog_thread;
    thread.stacksize = 0x400;
    thread.priority = 0x60;
    {
        int wd = CreateThread(&thread);
        if (wd >= 0)
            StartThread(wd, NULL);
    }
#endif

    RegisterLibraryEntries(&_exp_bdm);

#ifdef USE_BDM_ATA
    RegisterLibraryEntries(&_exp_atad);
    // Initialize ATA interface which will register the HDD as a block device.
    atad_start();
    atad_inited = 1;
#endif
}

void DeviceDeinit(void)
{
    DPRINTF("%s\n", __func__);

    /* IGR通常会重启整个IOP，但模块卸载时仍要显式撤销RPC和线程。 */
    if (g_bdm_fragment_rpc_registered) {
        sceSifRemoveRpc(&g_bdm_fragment_rpc_server, &g_bdm_fragment_rpc_queue);
        sceSifRemoveRpcQueue(&g_bdm_fragment_rpc_queue);
        g_bdm_fragment_rpc_registered = 0;
    }
    if (g_bdm_fragment_rpc_thread_id >= 0) {
        TerminateThread(g_bdm_fragment_rpc_thread_id);
        DeleteThread(g_bdm_fragment_rpc_thread_id);
        g_bdm_fragment_rpc_thread_id = -1;
    }

    bd_defrag_cursor_reset(&g_bd_defrag_cursor);
    bd_defrag_index_reset(&g_bd_defrag_index);
    g_frag_table_state = BDM_FRAG_TABLE_EMPTY;
    if (g_frag_table != NULL) {
        if (g_frag_table_owned)
            FreeSysMemory(g_frag_table);
        g_frag_table = NULL;
    }
    g_frag_table_owned = 0;
    if (g_bdm_fragment_pending != NULL) {
        FreeSysMemory(g_bdm_fragment_pending);
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
    }
    if (g_bd_defrag_checkpoints != NULL) {
        FreeSysMemory(g_bd_defrag_checkpoints);
        g_bd_defrag_checkpoints = NULL;
    }
    g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;

    if (g_bd_generic_sector_buffer_2 != NULL) {
        FreeSysMemory(g_bd_generic_sector_buffer_2);
        g_bd_generic_sector_buffer_2 = NULL;
        g_bd_generic_sector_buffer_size_2 = 0;
    }
}

int DeviceReady(void)
{
    // DPRINTF("%s\n", __func__);

    return (g_bd != NULL && g_frag_table_state == BDM_FRAG_TABLE_READY) ? SCECdComplete : SCECdNotReady;
}

void DeviceStop(void)
{
    DPRINTF("%s\n", __func__);

    if (g_bd != NULL)
        g_bd->stop(g_bd);
}

int DeviceFSInit(void)
{
    int result;

#ifdef USE_BDM_ATA
    lba_48bit = cdvdman_settings.hddIsLBA48;
    // TODO: there's more cdvdman init stuff after this in device-hdd.c...
#endif

    DPRINTF("Waiting for device...\n");
    WaitSema(bdm_io_sema);
    DPRINTF("Waiting for device...done!\n");

    /*
     * EE Core会在PS2LOGO运行前主动初始化CDVDMAN，避免首个光盘请求
     * 才开始跨处理器传输；游戏后续主动初始化时仍复用同一路径。
     */
    result = bdm_prepare_fragment_table();
    SignalSema(bdm_io_sema);

    if (result < 0)
        DPRINTF("fragment table transfer failed\n");
    return result;
}

void DeviceLock(void)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
}

void DeviceUnmount(void)
{
    DPRINTF("%s\n", __func__);
}

static int DeviceReadSectorsGeneric_2(u32 lsn, void *buffer, unsigned int sectors)
{
    u8 *destination;
    u32 sector_size;
    u32 iso_sectors_per_sector;
    u32 iso_sectors_remaining;
    u64 file_sector;

    if (g_bd == NULL)
        return SCECdErTRMOPN;

    WaitSema(bdm_io_sema);
    if (g_bd == NULL) {
        SignalSema(bdm_io_sema);
        return SCECdErTRMOPN;
    }
    if (g_frag_table_state != BDM_FRAG_TABLE_READY) {
        SignalSema(bdm_io_sema);
        return SCECdErREAD;
    }

    sector_size = g_bd->sectorSize;
    destination = buffer;

    if (g_bd_generic_sector_buffer_2 != NULL && g_bd_generic_sector_buffer_size_2 != sector_size) {
        FreeSysMemory(g_bd_generic_sector_buffer_2);
        g_bd_generic_sector_buffer_2 = NULL;
        g_bd_generic_sector_buffer_size_2 = 0;
        g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
    }

    if (sector_size == 1024 || sector_size == 2048) {
        u32 blocks_per_iso_sector = 2048 / sector_size;

        file_sector = (u64)lsn * blocks_per_iso_sector;
        iso_sectors_remaining = sectors;
        while (iso_sectors_remaining > 0) {
            u32 block_count;
            u32 sectors_to_read;

            sectors_to_read = iso_sectors_remaining;
            if (sectors_to_read > (0xffff / blocks_per_iso_sector))
                sectors_to_read = 0xffff / blocks_per_iso_sector;
            block_count = sectors_to_read * blocks_per_iso_sector;

            if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                      &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                      &g_bd_defrag_index, file_sector, destination, block_count, &g_bd_defrag_cursor) != block_count) {
                u64 totalSectorCount = 0;
                unsigned int i;

                for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
                    totalSectorCount += g_frag_table[cdvdman_settings.fragfile[0].frag_start + i].count;

                // 仅将超出碎片表逻辑末尾的部分补零，范围内的真实读取错误仍然返回失败。
                if (file_sector >= totalSectorCount)
                    memset(destination, 0, block_count * sector_size);
                else if (block_count > totalSectorCount - file_sector) {
                    u32 validBlockCount = totalSectorCount - file_sector;

                    if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                              &g_bd_defrag_index, file_sector, destination, validBlockCount, &g_bd_defrag_cursor) != validBlockCount) {
                        SignalSema(bdm_io_sema);
                        return SCECdErREAD;
                    }
                    memset(destination + validBlockCount * sector_size, 0, (block_count - validBlockCount) * sector_size);
                } else {
                    SignalSema(bdm_io_sema);
                    return SCECdErREAD;
                }
            }

            destination += sectors_to_read * 2048;
            file_sector += block_count;
            iso_sectors_remaining -= sectors_to_read;
        }

        SignalSema(bdm_io_sema);
        return SCECdErNO;
    } else if (sector_size == 4096) {
        file_sector = lsn / 2;
        iso_sectors_per_sector = 2;
    } else if (sector_size == 8192) {
        file_sector = lsn / 4;
        iso_sectors_per_sector = 4;
    } else {
        SignalSema(bdm_io_sema);
        return SCECdErREAD;
    }

    iso_sectors_remaining = sectors;
    if (mediaLsnCount && (mediaLsnCount % iso_sectors_per_sector)) {
        if (lsn >= mediaLsnCount) {
            memset(destination, 0, sectors * 2048);
            SignalSema(bdm_io_sema);
            return SCECdErNO;
        }

        if (iso_sectors_remaining > mediaLsnCount - lsn) {
            iso_sectors_remaining = mediaLsnCount - lsn;
            // 4K/8K物理扇区的最后一块可能包含逻辑介质末尾之外的填充数据，超出部分必须补零。
            memset(destination + iso_sectors_remaining * 2048, 0,
                   (sectors - iso_sectors_remaining) * 2048);
        }
    }
    while (iso_sectors_remaining > 0) {
        u32 iso_sector_offset = lsn % iso_sectors_per_sector;
        u32 sectors_to_read;

        if (iso_sector_offset == 0 && iso_sectors_remaining >= iso_sectors_per_sector) {
            u32 block_count = iso_sectors_remaining / iso_sectors_per_sector;

            if (block_count > 0xffff)
                block_count = 0xffff;

            if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                      &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                      &g_bd_defrag_index, file_sector, destination, block_count, &g_bd_defrag_cursor) != block_count) {
                u64 totalSectorCount = 0;
                unsigned int i;

                for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
                    totalSectorCount += g_frag_table[cdvdman_settings.fragfile[0].frag_start + i].count;

                if (file_sector >= totalSectorCount)
                    memset(destination, 0, block_count * sector_size);
                else if (block_count > totalSectorCount - file_sector) {
                    u32 validBlockCount = totalSectorCount - file_sector;

                    if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                              &g_bd_defrag_index, file_sector, destination, validBlockCount, &g_bd_defrag_cursor) != validBlockCount) {
                        SignalSema(bdm_io_sema);
                        return SCECdErREAD;
                    }
                    memset(destination + validBlockCount * sector_size, 0, (block_count - validBlockCount) * sector_size);
                } else {
                    SignalSema(bdm_io_sema);
                    return SCECdErREAD;
                }
            }

            sectors_to_read = block_count * iso_sectors_per_sector;
            destination += sectors_to_read * 2048;
            file_sector += block_count;
        } else {
            if (g_bd_generic_sector_buffer_size_2 != sector_size) {
                if (g_bd_generic_sector_buffer_2 != NULL)
                    FreeSysMemory(g_bd_generic_sector_buffer_2);

                g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
                g_bd_generic_sector_buffer_2 = AllocSysMemory(ALLOC_FIRST, sector_size, NULL);
                if (g_bd_generic_sector_buffer_2 == NULL) {
                    g_bd_generic_sector_buffer_size_2 = 0;
                    SignalSema(bdm_io_sema);
                    return SCECdErREAD;
                }

                g_bd_generic_sector_buffer_size_2 = sector_size;
            }

            sectors_to_read = iso_sectors_per_sector - iso_sector_offset;
            if (sectors_to_read > iso_sectors_remaining)
                sectors_to_read = iso_sectors_remaining;

            if (g_bd_generic_sector_buffer_sector_2 != file_sector) {
                g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
                if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                          &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                          &g_bd_defrag_index, file_sector, g_bd_generic_sector_buffer_2, 1, &g_bd_defrag_cursor) != 1) {
                    u64 totalSectorCount = 0;
                    unsigned int i;

                    for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
                        totalSectorCount += g_frag_table[cdvdman_settings.fragfile[0].frag_start + i].count;

                    if (file_sector >= totalSectorCount)
                        memset(destination, 0, sectors_to_read * 2048);
                    else {
                        SignalSema(bdm_io_sema);
                        return SCECdErREAD;
                    }
                } else {
                    g_bd_generic_sector_buffer_sector_2 = file_sector;
                }
            }

            if (g_bd_generic_sector_buffer_sector_2 == file_sector)
                memcpy(destination, g_bd_generic_sector_buffer_2 + iso_sector_offset * 2048, sectors_to_read * 2048);
            destination += sectors_to_read * 2048;
            file_sector++;
        }

        lsn += sectors_to_read;
        iso_sectors_remaining -= sectors_to_read;
    }
    SignalSema(bdm_io_sema);

    return SCECdErNO;
}

/* USB 读失败时的重试次数（make USB_READ_RETRY=N，默认 0=不重试）。
 * 用途：区分「偶发读错误 → 游戏读到失败就不读了」和「设备层真的挂住」。
 * 若开重试后 OP 能过，说明是错误返回路径；若照样停住，说明卡在设备层内部。 */
#ifndef USB_READ_RETRY
#define USB_READ_RETRY 0
#endif

#ifdef __IOPCORE_DEBUG
/* 看门狗：读超过 5 秒还没回来就每秒打一行，用来判断是「卡住」还是「已返回错误」。 */
static void bdm_watchdog_thread(void *arg)
{
    iop_sys_clock_t now;
    u32 elapsed_ms;

    (void)arg;
    while (1) {
        DelayThread(1000000);
        if (g_bdm_cur_busy) {
            GetSystemTime(&now);
            elapsed_ms = (now.lo - g_bdm_cur_start_lo) / 37u / 1000u;
            if (elapsed_ms >= 5000u)
                DPRINTF("bdm read HUNG: lsn=%u sectors=%u (%u ms and counting)\n",
                        (unsigned int)g_bdm_cur_lsn, (unsigned int)g_bdm_cur_sectors, elapsed_ms);
        }
    }
}
#endif

/* 512 字节扇区设备的读（USB / MX4SIO / ATA 都走这条）。
 * 带可选重试，见 USB_READ_RETRY。 */
static int bdm_read_blocks(bd_fragment_t *frags, u64 sector, void *buffer, unsigned int sectorCount)
{
    int attempt = 0;

    for (;;) {
        if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count, frags,
                                          &g_bd_defrag_index, sector, buffer, sectorCount, &g_bd_defrag_cursor) == (int)sectorCount)
            return 1;

        attempt++;
        DPRINTF("bdm read attempt %d failed: sector=%u blocks=%u\n", attempt, (unsigned int)sector, sectorCount);
        if (attempt > (int)USB_READ_RETRY)
            return 0;
        DelayThread(50000);
    }
}

int DeviceReadSectors(u32 lsn, void *buffer, unsigned int sectors)
{
    int rv = SCECdErNO;
    int isMX4SIO;
    iop_sys_clock_t t0, t1;

    // DPRINTF("%s(%u, 0x%p, %u)\n", __func__, (unsigned int)lsn, buffer, sectors);

    if (g_bd == NULL)
        return SCECdErTRMOPN;

    g_bdm_cur_lsn = lsn;
    g_bdm_cur_sectors = sectors;
    GetSystemTime(&t0);
    g_bdm_cur_start_lo = t0.lo;
    g_bdm_cur_busy = 1;

    WaitSema(bdm_io_sema);
    if (g_bd == NULL) {
        g_bdm_cur_busy = 0;
        SignalSema(bdm_io_sema);
        return SCECdErTRMOPN;
    }
    if (g_frag_table_state != BDM_FRAG_TABLE_READY) {
        g_bdm_cur_busy = 0;
        SignalSema(bdm_io_sema);
        return SCECdErREAD;
    }

    if (g_bd->sectorSize != 512) {
        SignalSema(bdm_io_sema);
        rv = DeviceReadSectorsGeneric_2(lsn, buffer, sectors);
        if (rv == SCECdErNO)
            bdmRateAccount(sectors * 2048);
        else
            DPRINTF("bdm read failed: lsn=%u sectors=%u rv=%d\n", (unsigned int)lsn, sectors, rv);
        g_bdm_cur_busy = 0;
        return rv;
    }

    isMX4SIO = g_bd->name[0] == 's' && g_bd->name[1] == 'd' && g_bd->name[2] == 'c' && g_bd->name[3] == '\0';
    u64 sector = ((u64)lsn) * 4;
    unsigned int sectorCount = sectors * 4;
    bd_fragment_t *frags = &g_frag_table[cdvdman_settings.fragfile[0].frag_start];
    if (!bdm_read_blocks(frags, sector, buffer, sectorCount)) {
        u64 totalSectorCount = 0;
        unsigned int i;

        for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
            totalSectorCount += frags[i].count;

        if (sector >= totalSectorCount)
            memset(buffer, 0, sectors * 2048);
        else if (sectorCount > totalSectorCount - sector) {
            unsigned int validSectorCount = totalSectorCount - sector;

            if (bdm_read_blocks(frags, sector, buffer, validSectorCount))
                memset((u8 *)buffer + validSectorCount * 512, 0, (sectorCount - validSectorCount) * 512);
            else
                rv = isMX4SIO ? SCECdErTRMOPN : SCECdErREAD;
        } else
            rv = isMX4SIO ? SCECdErTRMOPN : SCECdErREAD;
    }
    GetSystemTime(&t1);
    if (rv == SCECdErNO)
        bdmRateAccount(sectors * 2048);
    else
        DPRINTF("bdm read failed: lsn=%u sectors=%u rv=%d\n", (unsigned int)lsn, sectors, rv);
    if ((t1.lo - t0.lo) / 37u / 1000u >= 200u)
        DPRINTF("bdm read slow: lsn=%u sectors=%u took %u ms\n", (unsigned int)lsn, sectors,
                (unsigned int)((t1.lo - t0.lo) / 37u / 1000u));
    g_bdm_cur_busy = 0;
    SignalSema(bdm_io_sema);

    return rv;
}

//
// oplutils exported function, used by MCEMU
//

void bdm_readSector(u32 lba, unsigned short int nsectors, unsigned char *buffer)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
    g_bd->read(g_bd, (u64)lba, buffer, nsectors);
    SignalSema(bdm_io_sema);
}

void bdm_writeSector(u32 lba, unsigned short int nsectors, const unsigned char *buffer)
{
    DPRINTF("%s\n", __func__);

    WaitSema(bdm_io_sema);
    g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
    g_bd->write(g_bd, (u64)lba, buffer, nsectors);
    SignalSema(bdm_io_sema);
}
