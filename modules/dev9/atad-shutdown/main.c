#include <atahw.h>
#include <atad.h>
#include <dev9.h>
#include <loadcore.h>
#include <thbase.h>

#define MODNAME "atadshdn"
#define SHUTDOWN_TIMEOUT_MS 1000

IRX_ID(MODNAME, 1, 1);

static unsigned char ata_device_exists[2];

static int ata_wait_not_busy(ata_hwport_t *ata_hwport, int *elapsed)
{
    while (*elapsed < SHUTDOWN_TIMEOUT_MS) {
        if (!(ata_hwport->r_status & ATA_STAT_BUSY))
            return 1;

        DelayThread(1000);
        (*elapsed)++;
    }

    return 0;
}

static void ata_shutdown_with_timeout(void)
{
    USE_ATA_REGS;
    int device;
    int elapsed = 0;

    for (device = 0; device < 2 && elapsed < SHUTDOWN_TIMEOUT_MS; device++) {
        if (!ata_device_exists[device])
            continue;

        if (!ata_wait_not_busy(ata_hwport, &elapsed))
            break;

        ata_hwport->r_select = (device & 1) << 4;
        (void)ata_hwport->r_control;
        (void)ata_hwport->r_control;

        if (!ata_wait_not_busy(ata_hwport, &elapsed))
            break;

        ata_hwport->r_command = ATA_C_STANDBY_IMMEDIATE;
        if (!ata_wait_not_busy(ata_hwport, &elapsed))
            break;
    }
}

int _start(int argc, char *argv[])
{
    int device;

    (void)argc;
    (void)argv;

    for (device = 0; device < 2; device++) {
        ata_devinfo_t *devinfo = sceAtaInit(device);

        ata_device_exists[device] = devinfo != NULL && devinfo->exists;
    }

    /* ATAD uses callback slot 15. Replace it with a bounded shutdown handler. */
    Dev9RegisterPowerOffHandler(15, &ata_shutdown_with_timeout);
    return MODULE_RESIDENT_END;
}
