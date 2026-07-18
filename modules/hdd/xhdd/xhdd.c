#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <atad.h>
#include <iomanX.h>
#include <errno.h>

#include "opl-hdd-ioctl.h"
#include "xhdd.h"
#include "ata_identify.h"

#define MODNAME "xhdd"
IRX_ID(MODNAME, 1, 2);

static int isHDPro;

static IDENTIFY_DEVICE_DATA deviceIdentifyData;

static int xhddInit(iop_device_t *device)
{
    // Force atad to initialize the hdd devices.
    sceAtaInit(0);

    return 0;
}

static int xhddUnsupported(void)
{
    return -1;
}

static int xhddDevctl(iop_file_t *fd, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
    ata_devinfo_t *devinfo;

    if (fd->unit >= 2)
        return -ENXIO;

    switch (cmd) {
        case ATA_DEVCTL_IS_48BIT:
            return ((devinfo = sceAtaInit(fd->unit)) != NULL ? devinfo->lba48 : -1);

        case ATA_DEVCTL_SET_TRANSFER_MODE: {
            if (!isHDPro)
                return ata_device_set_transfer_mode(fd->unit, ((hddAtaSetMode_t *)arg)->type, ((hddAtaSetMode_t *)arg)->mode);
            else
                return hdproata_device_set_transfer_mode(fd->unit, ((hddAtaSetMode_t *)arg)->type, ((hddAtaSetMode_t *)arg)->mode);
        }

        case ATA_DEVCTL_READ_PARTITION_SECTOR: {
            // Make sure the length is a multiple of the device sector size.
            if (buflen % 512 != 0)
                return -EINVAL;

            return sceAtaDmaTransfer(fd->unit, buf, 0, buflen / 512, ATA_DIR_READ);
        }

        case ATA_DEVCTL_GET_HIGHEST_UDMA_MODE: {
            // Get the device info.
            int result = ata_device_identify(fd->unit, &deviceIdentifyData);
            if (result != 0)
                return result;

            // Check the highest UDMA mode supported.
            for (int i = 7; i >= 0; i--) {
                // Check if the current UDMA mode is supported.
                if ((deviceIdentifyData.UltraDMASupport & (1 << i)) != 0)
                    return i;
            }

            return -EINVAL;
        }

        case ATA_DEVCTL_GET_LOGICAL_SECTOR_SIZE: {
            u16 *identify_data = (u16 *)&deviceIdentifyData;
            u32 logical_sector_words;
            u16 physical_logical_sector_size;

            if (ata_device_identify(fd->unit, &deviceIdentifyData) != 0)
                return -EIO;

            physical_logical_sector_size = identify_data[106];
            if ((physical_logical_sector_size & 0xc000) != 0x4000 ||
                !(physical_logical_sector_size & 0x1000))
                return 512;

            logical_sector_words = identify_data[117] | ((u32)identify_data[118] << 16);
            if (logical_sector_words < 256 || logical_sector_words > (0xffffffff / 2))
                return 512;

            logical_sector_words *= 2;
            return (logical_sector_words < 512 || (logical_sector_words % 512) != 0) ? 512 : logical_sector_words;
        }
        default:
            return -EINVAL;
    }
}

static iop_device_ops_t xhdd_ops = {
    &xhddInit,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    (void *)&xhddUnsupported,
    &xhddDevctl,
};

static iop_device_t xhddDevice = {
    "xhdd",
    IOP_DT_BLOCK | IOP_DT_FSEXT,
    1,
    "XHDD",
    &xhdd_ops};

int _start(int argc, char *argv[])
{
    int i;

    isHDPro = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-hdpro"))
            isHDPro = 1;
    }

    return AddDrv(&xhddDevice) == 0 ? MODULE_RESIDENT_END : MODULE_NO_RESIDENT_END;
}
