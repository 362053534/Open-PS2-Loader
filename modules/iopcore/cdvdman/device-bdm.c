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
static struct block_device *g_bd = NULL;
static u32 g_bd_sectors_per_sector = 4;
static int bdm_io_sema;
static u8 *g_bd_generic_sector_buffer_2 = NULL;
static u32 g_bd_generic_sector_buffer_size_2 = 0;

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
        unsigned int i;

        DPRINTF("attaching to %s%dp%d\n", bd->name, bd->devNr, bd->parNr);
        g_bd = bd;
        if (cdvdman_settings.fragsAre512ByteSectors && bd->sectorSize != 512) {
            unsigned int sectors_per_pfs_sector = bd->sectorSize >> 9;

            while (sectors_per_pfs_sector > 1) {
                for (i = 0; i < cdvdman_settings.fragfile[0].frag_count; i++) {
                    cdvdman_settings.frags[i].sector >>= 1;
                    cdvdman_settings.frags[i].count >>= 1;
                }
                sectors_per_pfs_sector >>= 1;
            }
            cdvdman_settings.fragsAre512ByteSectors = 0;
        }
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
        if (g_bd == bd)
            g_bd = NULL;
    }
}

//
// cdvdman "Device" functions
//

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

    sector_size = g_bd->sectorSize;
    destination = buffer;

    WaitSema(bdm_io_sema);
    if (g_bd_generic_sector_buffer_2 != NULL && g_bd_generic_sector_buffer_size_2 != sector_size) {
        FreeSysMemory(g_bd_generic_sector_buffer_2);
        g_bd_generic_sector_buffer_2 = NULL;
        g_bd_generic_sector_buffer_size_2 = 0;
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

            if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count,
                          &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start],
                          file_sector, destination, block_count) != block_count) {
                SignalSema(bdm_io_sema);
                return SCECdErREAD;
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
    while (iso_sectors_remaining > 0) {
        u32 iso_sector_offset = lsn % iso_sectors_per_sector;
        u32 sectors_to_read;

        if (iso_sector_offset == 0 && iso_sectors_remaining >= iso_sectors_per_sector) {
            u32 block_count = iso_sectors_remaining / iso_sectors_per_sector;

            if (block_count > 0xffff)
                block_count = 0xffff;

            if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count,
                          &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start],
                          file_sector, destination, block_count) != block_count) {
                SignalSema(bdm_io_sema);
                return SCECdErREAD;
            }

            sectors_to_read = block_count * iso_sectors_per_sector;
            destination += sectors_to_read * 2048;
            file_sector += block_count;
        } else {
            if (g_bd_generic_sector_buffer_size_2 != sector_size) {
                if (g_bd_generic_sector_buffer_2 != NULL)
                    FreeSysMemory(g_bd_generic_sector_buffer_2);

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

            if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count,
                          &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start],
                          file_sector, g_bd_generic_sector_buffer_2, 1) != 1) {
                SignalSema(bdm_io_sema);
                return SCECdErREAD;
            }

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

    // DPRINTF("%s(%u, 0x%p, %u)\n", __func__, (unsigned int)lsn, buffer, sectors);

    if (g_bd == NULL)
        return SCECdErTRMOPN;

    if (g_bd->sectorSize != 512)
        return DeviceReadSectorsGeneric_2(lsn, buffer, sectors);

    WaitSema(bdm_io_sema);
    //if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count, &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start], ((u64)lsn) * 4, buffer, sectors * 4) != (sectors * 4))
    //    rv = SCECdErREAD;
    //bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count, &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start], ((u64)lsn) * 4, buffer, sectors * 4); // 出错了也继续推进
    // 临时逐个读取512字节扇区，用于排查多扇区读取是否导致ZSO黑屏。
    for (unsigned int i = 0; i < sectors * 4; i++)
        bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count, &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start], ((u64)lsn) * 4 + i, (u8 *)buffer + (i * 512), 1);
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
    g_bd->write(g_bd, (u64)lba, buffer, nsectors);
    SignalSema(bdm_io_sema);
}
