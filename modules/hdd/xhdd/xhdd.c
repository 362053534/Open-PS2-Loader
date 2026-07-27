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
static unsigned char hashBuffer[16 * 1024] __attribute__((aligned(64)));

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

        case ATA_DEVCTL_HASH_SECTORS: {
            hddAtaHashSectors_t *request = (hddAtaHashSectors_t *)arg;
            u32 lba;
            u32 sectors;
            u32 bytes;
            u32 hash;

            if (arglen != sizeof(hddAtaHashSectors_t) || buflen < sizeof(u32) || request->bytes > request->sectors * 512)
                return -EINVAL;

            lba = request->lba;
            sectors = request->sectors;
            bytes = request->bytes;
            hash = request->hash;
            while (sectors > 0) {
                u32 sectorsToRead = sectors > 32 ? 32 : sectors;
                u32 bytesToHash = sectorsToRead * 512;

                if (sceAtaDmaTransfer(fd->unit, hashBuffer, lba, sectorsToRead, ATA_DIR_READ) != 0)
                    return -EIO;
                if (bytesToHash > bytes)
                    bytesToHash = bytes;
                for (u32 i = 0; i < bytesToHash; i++)
                    hash = (hash ^ hashBuffer[i]) * 16777619;

                lba += sectorsToRead;
                sectors -= sectorsToRead;
                bytes -= bytesToHash;
            }

            *(u32 *)buf = hash;
            return 0;
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
