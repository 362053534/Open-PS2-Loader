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
/* 大文件以64个碎片为步长，限制随机定位时的线性扫描长度。 */
#define BDM_DEFRAG_CHECKPOINT_STRIDE 64
/* 该边界覆盖APA HDL和单个PFS inode能够提供的完整区段表。 */
#define BDM_DEFRAG_DENSE_INDEX_LIMIT 114

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

static int bdm_load_fragment_table(void)
{
    u32 bytes = cdvdman_settings.frag_table_bytes;
    u32 address = cdvdman_settings.frag_table_ee_addr;
    u32 offset = 0;
    u32 required_bytes;

    if (bytes == 0 || address == 0 || cdvdman_settings.fragfile[0].frag_count == 0 ||
        cdvdman_settings.fragfile[0].frag_count > 0xFFFFFFFFU / sizeof(bd_fragment_t))
        return -1;
    required_bytes = cdvdman_settings.fragfile[0].frag_count * sizeof(bd_fragment_t);
    if (bytes < required_bytes || (bytes & 0xF) != 0)
        return -1;

    g_frag_table = AllocSysMemory(ALLOC_FIRST, bytes, NULL);
    if (g_frag_table == NULL)
        return -1;

    while (offset < bytes) {
        SifRpcReceiveData_t receive;
        u32 chunk = bytes - offset;

        if (chunk > 16384)
            chunk = 16384;

        /* IOP侧的sceSifSetDma只适用于IOP→EE，反向读取必须通过SIFCMD请求。 */
        if (sceSifGetOtherData(&receive, (void *)(address + offset),
                               (u8 *)g_frag_table + offset, chunk, 0) < 0) {
            FreeSysMemory(g_frag_table);
            g_frag_table = NULL;
            return -1;
        }
        offset += chunk;
    }

    {
        u32 fragcount = cdvdman_settings.fragfile[0].frag_count;
        u32 stride = bdm_get_checkpoint_stride(fragcount);
        u32 checkpoint_count = bdm_get_checkpoint_count(fragcount, stride);

        g_bd_defrag_checkpoints = AllocSysMemory(ALLOC_FIRST,
                                                  checkpoint_count * sizeof(bd_defrag_checkpoint_t), NULL);
        if (g_bd_defrag_checkpoints == NULL) {
            FreeSysMemory(g_frag_table);
            g_frag_table = NULL;
            return -1;
        }
    }

    return 0;
}

static int bdm_prepare_fragment_table(void)
{
    unsigned int i;
    u32 fragcount = cdvdman_settings.fragfile[0].frag_count;
    u32 stride = bdm_get_checkpoint_stride(fragcount);
    u32 checkpoint_count = bdm_get_checkpoint_count(fragcount, stride);

    if (g_bd == NULL)
        return -1;

    if (g_frag_table == NULL && bdm_load_fragment_table() < 0)
        return -1;

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
    if (bd_defrag_index_build(&g_bd_defrag_index,
                              &g_frag_table[cdvdman_settings.fragfile[0].frag_start],
                              fragcount,
                              stride,
                              g_bd_defrag_checkpoints,
                              checkpoint_count) < 0)
        DPRINTF("fragment index build failed; using linear lookup\n");
    bd_defrag_cursor_reset(&g_bd_defrag_cursor);

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
    bd_defrag_cursor_reset(&g_bd_defrag_cursor);
    bd_defrag_index_reset(&g_bd_defrag_index);
    g_bd_generic_sector_buffer_sector_2 = INVALID_BD_GENERIC_SECTOR;

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
    bd_defrag_cursor_reset(&g_bd_defrag_cursor);
    bd_defrag_index_reset(&g_bd_defrag_index);
    if (g_frag_table != NULL) {
        FreeSysMemory(g_frag_table);
        g_frag_table = NULL;
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

    return (g_bd == NULL) ? SCECdNotReady : SCECdComplete;
}

void DeviceStop(void)
{
    DPRINTF("%s\n", __func__);

    if (g_bd != NULL)
        g_bd->stop(g_bd);
}

void DeviceFSInit(void)
{
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
    if (bdm_prepare_fragment_table() < 0)
        DPRINTF("fragment table transfer failed\n");

    SignalSema(bdm_io_sema);
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
    if (g_frag_table == NULL)
        return SCECdErREAD;

    sector_size = g_bd->sectorSize;
    destination = buffer;

    WaitSema(bdm_io_sema);
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

int DeviceReadSectors(u32 lsn, void *buffer, unsigned int sectors)
{
    int rv = SCECdErNO;
    int isMX4SIO;

    // DPRINTF("%s(%u, 0x%p, %u)\n", __func__, (unsigned int)lsn, buffer, sectors);

    if (g_bd == NULL)
        return SCECdErTRMOPN;
    if (g_frag_table == NULL)
        return SCECdErREAD;

    if (g_bd->sectorSize != 512)
        return DeviceReadSectorsGeneric_2(lsn, buffer, sectors);

    isMX4SIO = g_bd->name[0] == 's' && g_bd->name[1] == 'd' && g_bd->name[2] == 'c' && g_bd->name[3] == '\0';
    WaitSema(bdm_io_sema);
    u64 sector = ((u64)lsn) * 4;
    unsigned int sectorCount = sectors * 4;
    bd_fragment_t *frags = &g_frag_table[cdvdman_settings.fragfile[0].frag_start];
    if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count, frags, &g_bd_defrag_index, sector, buffer, sectorCount, &g_bd_defrag_cursor) != (int)sectorCount) {
        u64 totalSectorCount = 0;
        unsigned int i;

        for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++)
            totalSectorCount += frags[i].count;

        if (sector >= totalSectorCount)
            memset(buffer, 0, sectors * 2048);
        else if (sectorCount > totalSectorCount - sector) {
            unsigned int validSectorCount = totalSectorCount - sector;

            if (bd_defrag_read_cached_indexed(g_bd, cdvdman_settings.fragfile[0].frag_count, frags, &g_bd_defrag_index, sector, buffer, validSectorCount, &g_bd_defrag_cursor) == (int)validSectorCount)
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
