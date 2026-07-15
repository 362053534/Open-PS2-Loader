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
        DPRINTF("attaching to %s%dp%d\n", bd->name, bd->devNr, bd->parNr);
        g_bd = bd;
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
    u64 byte_offset;
    u64 bytes_remaining;
    u8 *destination;
    u32 sector_size;

    if (g_bd == NULL)
        return SCECdErTRMOPN;

    sector_size = g_bd->sectorSize;
    if (g_bd_generic_sector_buffer_size_2 != sector_size) {
        if (g_bd_generic_sector_buffer_2 != NULL)
            FreeSysMemory(g_bd_generic_sector_buffer_2);

        g_bd_generic_sector_buffer_2 = AllocSysMemory(ALLOC_FIRST, sector_size, NULL);
        if (g_bd_generic_sector_buffer_2 == NULL) {
            g_bd_generic_sector_buffer_size_2 = 0;
            return SCECdErREAD;
        }

        g_bd_generic_sector_buffer_size_2 = sector_size;
    }

    byte_offset = ((u64)lsn) * 2048;
    bytes_remaining = ((u64)sectors) * 2048;
    destination = buffer;

    WaitSema(bdm_io_sema);
    while (bytes_remaining > 0) {
        u64 file_sector = byte_offset / sector_size;
        u32 sector_offset = byte_offset % sector_size;
        u32 bytes_to_copy;

        if (sector_offset == 0 && bytes_remaining >= sector_size) {
            u32 sector_count = bytes_remaining / sector_size;

            if (sector_count > 0xffff)
                sector_count = 0xffff;

            if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count,
                          &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start],
                          file_sector, destination, sector_count) != sector_count) {
                SignalSema(bdm_io_sema);
                return SCECdErREAD;
            }

            bytes_to_copy = sector_count * sector_size;
        } else {
            bytes_to_copy = sector_size - sector_offset;
            if (bytes_to_copy > bytes_remaining)
                bytes_to_copy = bytes_remaining;

            if (bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count,
                          &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start],
                          file_sector, g_bd_generic_sector_buffer_2, 1) != 1) {
                SignalSema(bdm_io_sema);
                return SCECdErREAD;
            }

            memcpy(destination, g_bd_generic_sector_buffer_2 + sector_offset, bytes_to_copy);
        }

        byte_offset += bytes_to_copy;
        bytes_remaining -= bytes_to_copy;
        destination += bytes_to_copy;
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
    bd_defrag(g_bd, cdvdman_settings.fragfile[0].frag_count, &cdvdman_settings.frags[cdvdman_settings.fragfile[0].frag_start], ((u64)lsn) * 4, buffer, sectors * 4); // 出错了也继续推进
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
