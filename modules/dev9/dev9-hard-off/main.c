#include <types.h>
#include <dev9regs.h>
#include <errno.h>
#include <iomanX.h>
#include <loadcore.h>
#include <thbase.h>

#include "dev9-hard-off.h"

#define MODNAME "opl9off"
#define DEVNAME "opl9"

IRX_ID(MODNAME, 1, 1);

static int hardoff_unsupported(void)
{
    return -EIO;
}

static int hardoff_init(iop_device_t *device)
{
    (void)device;
    return 0;
}

static int hardoff_devctl(iop_file_t *fd, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
    USE_DEV9_REGS;
    u16 dev9hw;

    (void)fd;
    (void)name;
    (void)arg;
    (void)arglen;
    (void)buf;
    (void)buflen;

    if (cmd != OPL_DEV9_HARD_OFF)
        return -EINVAL;

    /*
     * This is the power-register portion of Dev9CardStop(). Do not call the
     * registered shutdown callbacks here: one of those callbacks may be stuck
     * waiting for an ATA command while OPL is trying to launch a non-DEV9 game.
     */
    dev9hw = DEV9_REG(DEV9_R_REV) & 0xF0;
    if (dev9hw == 0x20) { /* PCMCIA */
        DEV9_REG(DEV9_R_POWER) = 0;
        DEV9_REG(DEV9_R_1474) = 0;
    } else if (dev9hw == 0x30) { /* Expansion Bay */
        DEV9_REG(DEV9_R_1466) = 1;
        DEV9_REG(DEV9_R_1464) = 0;
        DEV9_REG(DEV9_R_1460) = DEV9_REG(DEV9_R_1464);
        DEV9_REG(DEV9_R_POWER) &= ~4;
        DEV9_REG(DEV9_R_POWER) &= ~1;
    } else {
        return -ENXIO;
    }

    /* Match the settling delay used by Dev9CardStop(). */
    DelayThread(1000000);
    return 0;
}

static iop_device_ops_t hardoff_ops = {
    &hardoff_init,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    (void *)&hardoff_unsupported,
    &hardoff_devctl,
};

static iop_device_t hardoff_device = {
    DEVNAME,
    IOP_DT_FS | IOP_DT_FSEXT,
    1,
    "OPL DEV9 hard power-off",
    &hardoff_ops};

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    return AddDrv(&hardoff_device) == 0 ? MODULE_RESIDENT_END : MODULE_NO_RESIDENT_END;
}
