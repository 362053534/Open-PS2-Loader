/*
  Copyright 2009-2010, jimmikaelkael
  Licenced under Academic Free License version 3.0
  Review Open PS2 Loader README & LICENSE files for further details.
*/

#include "internal.h"
#include "cdvdman_bdm_fragment_rpc.h"

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
static int bdm_io_sema;
static u8 *g_bd_generic_sector_buffer_2 = NULL;
static u32 g_bd_generic_sector_buffer_size_2 = 0;
static u64 g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
/* 内嵌静态表已取消，提交后的碎片表统一由本模块释放。 */
static bd_fragment_t *g_frag_table = NULL;
static bd_fragment_t *g_bdm_fragment_pending = NULL;
static u32 g_bdm_fragment_pending_bytes = 0;
static volatile u8 g_bdm_fragment_rpc_registered = 0;
static volatile u8 g_bdm_fragment_rpc_committed = 0;
static SifRpcServerData_t g_bdm_fragment_rpc_server;
/* 32 字节专用接收/回包缓冲，16 字节对齐。不与 NCMD/SCMD 的 1024 字节缓冲共用。 */
static u8 g_bdm_fragment_rpc_buffer[32] __attribute__((aligned(16)));
enum bdm_frag_table_state {
    BDM_FRAG_TABLE_EMPTY = 0,
    BDM_FRAG_TABLE_LOADING,
    BDM_FRAG_TABLE_READY,
    BDM_FRAG_TABLE_FAILED
};
static volatile u8 g_frag_table_state = BDM_FRAG_TABLE_EMPTY;

/* 使用本地读取器，避免为碎片定位链接整个 SDK bd_defrag 对象。
 * 游标和检查点由 bdm_io_sema 保护；本轮测试统一为每个碎片建立一个路标。
 */
typedef struct {
    u32 index;
    u64 offset;
} slim_cursor_t;

static slim_cursor_t g_slim_cursor;

/* 分开保存偏移与下标，共用一次分配，避免每个检查点的对齐填充。 */
static u64 *g_slim_cp_offsets;
static u32 *g_slim_cp_indices;
static u32 g_slim_cp_count;

static void slim_cursor_reset(void)
{
    g_slim_cursor.index = 0;
    g_slim_cursor.offset = 0;
}

static void slim_checkpoints_reset(void)
{
    if (g_slim_cp_offsets != NULL)
        FreeSysMemory(g_slim_cp_offsets);
    g_slim_cp_offsets = NULL;
    g_slim_cp_indices = NULL;
    g_slim_cp_count = 0;
}

static void slim_checkpoints_build(u32 fragcount, const bd_fragment_t *frags)
{
    u32 i;
    u64 off = 0;

    slim_checkpoints_reset();
    if (fragcount == 0 || frags == NULL)
        return;
    if (fragcount > 0xFFFFFFFFU / (sizeof(u64) + sizeof(u32)))
        return;

    g_slim_cp_offsets = AllocSysMemory(ALLOC_FIRST,
                                       fragcount * (sizeof(u64) + sizeof(u32)), NULL);
    /* 分配失败只退回线性定位，不让光盘初始化失败。 */
    if (g_slim_cp_offsets == NULL)
        return;
    g_slim_cp_indices = (u32 *)(g_slim_cp_offsets + fragcount);

    for (i = 0; i < fragcount; i++) {
        g_slim_cp_offsets[i] = off;
        g_slim_cp_indices[i] = i;
        off += frags[i].count;
    }
    g_slim_cp_count = fragcount;
}

static void slim_start_from_checkpoints(u64 sector, u32 *index, u64 *offset)
{
    u32 i;
    u32 best = 0;

    if (g_slim_cp_count == 0) {
        *index = 0;
        *offset = 0;
        return;
    }
    for (i = 1; i < g_slim_cp_count; i++) {
        if (g_slim_cp_offsets[i] <= sector)
            best = i;
        else
            break;
    }
    *index = g_slim_cp_indices[best];
    *offset = g_slim_cp_offsets[best];
}

static void slim_on_table_ready(u32 fragcount, const bd_fragment_t *frags)
{
    slim_cursor_reset();
    slim_checkpoints_build(fragcount, frags);
}

static int slim_defrag_read(struct block_device *bd, u32 fragcount, const bd_fragment_t *frags,
                            u64 sector, void *buffer, u32 count)
{
    u64 sector_start = sector;
    u32 count_left = count;
    u32 i;
    u64 offset;

    if (bd == NULL || frags == NULL)
        return -1;
    if (count == 0)
        return 0;

#if SLIM_DEFRAG_CURSOR
    if (g_slim_cursor.index < fragcount && g_slim_cursor.offset <= sector_start) {
        i = g_slim_cursor.index;
        offset = g_slim_cursor.offset;
    } else
#endif
    {
        slim_start_from_checkpoints(sector_start, &i, &offset);
    }

    while (count_left > 0) {
        const bd_fragment_t *f = NULL;
        u32 count_read;

        for (; i < fragcount; i++) {
            f = &frags[i];
            if (offset <= sector_start && (offset + f->count) > sector_start)
                break;
            offset += f->count;
        }
        if (i == fragcount)
            return -1;

        count_read = count_left;
        if ((sector_start + count_read) > (offset + f->count))
            count_read = (u32)((offset + f->count) - sector_start);

        if (bd->read(bd, f->sector + (sector_start - offset), buffer, count_read) != (int)count_read)
            return -1;

#if SLIM_DEFRAG_CURSOR
        g_slim_cursor.index = i;
        g_slim_cursor.offset = offset;
#endif

        sector_start += count_read;
        count_left -= count_read;
        buffer = (u8 *)buffer + (count_read * bd->sectorSize);
        if (sector_start >= (offset + f->count)) {
            offset += f->count;
            i++;
#if SLIM_DEFRAG_CURSOR
            g_slim_cursor.index = i;
            g_slim_cursor.offset = offset;
#endif
        }
    }

    return (int)count;
}

extern struct irx_export_table _exp_bdm;

#ifdef USE_BDM_ATA
extern struct irx_export_table _exp_atad;
#endif

//
// BDM exported functions
//

void bdm_connect_bd(struct block_device *bd)
{
    DPRINTF("connecting device %s%dp%d\n", bd->name, bd->devNr, bd->parNr);

    if (g_bd == NULL && bd->devNr == cdvdman_settings.bdDeviceId) {
        DPRINTF("attaching to %s%dp%d\n", bd->name, bd->devNr, bd->parNr);
        g_bd = bd;
        slim_cursor_reset();
        g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
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
            slim_cursor_reset();
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
    /* COMMIT 成功后服务保持注册。再次调用只失败返回，不再分配，也不重新初始化光盘。 */
    if (g_bdm_fragment_rpc_committed)
        return buffer;
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
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
        cdvdman_settings.fragfile[0].frag_count = request->fragment_count;
        request->result = 0;
        g_bdm_fragment_rpc_committed = 1;
    }

    return buffer;
}

void cdvdman_register_bdm_fragment_rpc(SifRpcDataQueue_t *qd)
{
    if (qd == NULL || g_bdm_fragment_rpc_registered)
        return;

    /* 队列已由 cdvdfsv 完成 SetRpcQueue。不要在这里 InitRpc 或建线程。
     * 回包源是本模块 BSS 缓冲，在 ExecRequest 回包期间保持有效。 */
    sceSifRegisterRpc(&g_bdm_fragment_rpc_server, BDM_FRAGMENT_RPC_ID,
                      bdm_fragment_rpc_handler, g_bdm_fragment_rpc_buffer,
                      NULL, NULL, qd);
    g_bdm_fragment_rpc_registered = 1;
}

static int bdm_prepare_fragment_table(void)
{
    unsigned int i;
    u32 fragcount = cdvdman_settings.fragfile[0].frag_count;

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

    slim_on_table_ready(fragcount, &g_frag_table[cdvdman_settings.fragfile[0].frag_start]);

    /* 表、扇区单位和索引全部就绪后再允许光盘读取。 */
    g_frag_table_state = BDM_FRAG_TABLE_READY;
    return 0;
}

void DeviceInit(void)
{
    iop_sema_t smp;

    DPRINTF("%s\n", __func__);

    // Create semaphore, initially locked
    smp.initial = 0;
    smp.max = 1;
    smp.option = 0;
    smp.attr = SA_THPRI;
    bdm_io_sema = CreateSema(&smp);
    slim_cursor_reset();
    g_frag_table_state = BDM_FRAG_TABLE_EMPTY;
    g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;
    if (g_bdm_fragment_pending != NULL) {
        FreeSysMemory(g_bdm_fragment_pending);
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
    }

    RegisterLibraryEntries(&_exp_bdm);

#ifdef USE_BDM_ATA
    RegisterLibraryEntries(&_exp_atad);
    // Initialize ATA interface which will register the HDD as a block device.
    atad_start();
    atad_inited = 1;
#endif
    /* CDVDMAN 先于 CDVDFSV 启动。碎片表 SID 由随后的 cdvdfsv rpc0
     * 在 SetRpcQueue 之后调用 cdvdman_register_bdm_fragment_rpc 注册。
     * EE 的 BindRpc 会等到服务出现；此处不提前 sceCdInit。 */
}

void DeviceDeinit(void)
{
    DPRINTF("%s\n", __func__);

    /* 服务留在 cdvdfsv 的永久队列上（COMMIT 后 fail-closed）。
     * IGR 会重启整个 IOP，cdvdman 与 cdvdfsv 一并卸载。不要再清理已删除的专用线程。 */
    slim_cursor_reset();
    slim_checkpoints_reset();
    g_frag_table_state = BDM_FRAG_TABLE_EMPTY;
    if (g_frag_table != NULL) {
        FreeSysMemory(g_frag_table);
        g_frag_table = NULL;
    }
    if (g_bdm_fragment_pending != NULL) {
        FreeSysMemory(g_bdm_fragment_pending);
        g_bdm_fragment_pending = NULL;
        g_bdm_fragment_pending_bytes = 0;
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

            if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                      &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                      file_sector, destination, block_count) != block_count) {
                u64 totalSectorCount = 0;
                unsigned int i;

                for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
                    totalSectorCount += g_frag_table[cdvdman_settings.fragfile[0].frag_start + i].count;

                // 仅将超出碎片表逻辑末尾的部分补零，范围内的真实读取错误仍然返回失败。
                if (file_sector >= totalSectorCount)
                    memset(destination, 0, block_count * sector_size);
                else if (block_count > totalSectorCount - file_sector) {
                    u32 validBlockCount = totalSectorCount - file_sector;

                    if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                              file_sector, destination, validBlockCount) != validBlockCount) {
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

            if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                      &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                      file_sector, destination, block_count) != block_count) {
                u64 totalSectorCount = 0;
                unsigned int i;

                for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
                    totalSectorCount += g_frag_table[cdvdman_settings.fragfile[0].frag_start + i].count;

                if (file_sector >= totalSectorCount)
                    memset(destination, 0, block_count * sector_size);
                else if (block_count > totalSectorCount - file_sector) {
                    u32 validBlockCount = totalSectorCount - file_sector;

                    if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                              file_sector, destination, validBlockCount) != validBlockCount) {
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
                if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count,
                                          &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                                          file_sector, g_bd_generic_sector_buffer_2, 1) != 1) {
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

int DeviceReadSectors(u32 lsn, void *buffer, unsigned int sectors)
{
    int rv = SCECdErNO;
    int isMX4SIO;

    // DPRINTF("%s(%u, 0x%p, %u)\n", __func__, (unsigned int)lsn, buffer, sectors);

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

    if (g_bd->sectorSize != 512) {
        SignalSema(bdm_io_sema);
        return DeviceReadSectorsGeneric_2(lsn, buffer, sectors);
    }

    isMX4SIO = g_bd->name[0] == 's' && g_bd->name[1] == 'd' && g_bd->name[2] == 'c' && g_bd->name[3] == '\0';
    u64 sector = ((u64)lsn) * 4;
    unsigned int sectorCount = sectors * 4;
    bd_fragment_t *frags = &g_frag_table[cdvdman_settings.fragfile[0].frag_start];
    if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count, frags, sector, buffer, sectorCount) != (int)sectorCount) {
        u64 totalSectorCount = 0;
        unsigned int i;

        for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
            totalSectorCount += frags[i].count;

        if (sector >= totalSectorCount)
            memset(buffer, 0, sectors * 2048);
        else if (sectorCount > totalSectorCount - sector) {
            unsigned int validSectorCount = totalSectorCount - sector;

            if (slim_defrag_read(g_bd, cdvdman_settings.fragfile[0].frag_count, frags, sector, buffer, validSectorCount) == (int)validSectorCount)
                memset((u8 *)buffer + validSectorCount * 512, 0, (sectorCount - validSectorCount) * 512);
            else
                rv = isMX4SIO ? SCECdErTRMOPN : SCECdErREAD;
        } else
            rv = isMX4SIO ? SCECdErTRMOPN : SCECdErREAD;
    }
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
