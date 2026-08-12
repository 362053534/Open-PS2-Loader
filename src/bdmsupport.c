#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/hdd.h"
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/sound.h"
#include "modules/iopcore/common/cdvd_config.h"

#include <usbhdfsd-common.h>

#include <ps2sdkapi.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioIoctl, fileXioDevctl

static int bdmModLoaded = 0;
static int iLinkModLoaded = 0;
static int mx4sioModLoaded = 0;
static int hddModLoaded = 0;
static s32 bdmLoadModuleLock;
int bdmDeviceModeStarted;

static item_list_t bdmDeviceList[MAX_BDM_DEVICES];
static int bdmDeviceListInitialized = 0;

// 判断BDM设备是否使用ART2文件夹
static int artUseBuckets_USB = 0;
static int artUseBuckets_ILINK = 0;
static int artUseBuckets_SDC = 0;
static int artUseBuckets_ATA = 0;

void bdmInitDevicesData();

// Identifies the partition that the specified file is stored on and generates a full path to it.
int bdmFindPartition(char *target, const char *name, int write)
{
    int i, fd;
    char path[256];

    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        if (gBDMPrefix[0] != '\0')
            sprintf(path, "mass%d:%s/%s", i, gBDMPrefix, name);
        else
            sprintf(path, "mass%d:%s", i, name);
        if (write)
            fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0666);
        else
            fd = open(path, O_RDONLY);

        if (fd >= 0) {
            if (gBDMPrefix[0] != '\0')
                sprintf(target, "mass%d:%s/", i, gBDMPrefix);
            else
                sprintf(target, "mass%d:", i);
            close(fd);
            return 1;
        }
    }

    // default to first partition (for themes, ...)
    if (gBDMPrefix[0] != '\0')
        sprintf(target, "mass0:%s/", gBDMPrefix);
    else
        sprintf(target, "mass0:");
    return 0;
}

static unsigned int BdmGeneration = 0;
// 启动保底：自动模式零设备时为1，任一槽挂上真设备后清0
static int bdmKeepPlaceholder = 0;
int bdmDefaultNeedsCorrection = 0;

static void bdmEventHandler(void *packet, void *opt)
{
    BdmGeneration++;
}

int bdmHasDeviceEvent(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return pDeviceData != NULL && pDeviceData->bdmDeviceTick != BdmGeneration;
}

int bdmGetDeviceType(int mode)
{
    bdm_device_data_t *pDeviceData;

    if (mode < BDM_MODE || mode > BDM_MODE4 || !bdmDeviceListInitialized)
        return BDM_TYPE_UNKNOWN;

    pDeviceData = (bdm_device_data_t *)bdmDeviceList[mode].priv;
    return pDeviceData != NULL ? pDeviceData->bdmDeviceType : BDM_TYPE_UNKNOWN;
}

void bdmRequestDeviceCheck(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (pDeviceData != NULL)
        pDeviceData->bdmDeviceTick = -1;
}

void bdmTryActivateStartupPlaceholder(void)
{
    int i;

    if (gBDMStartMode != START_MODE_AUTO)
        return;
    if (!gEnableUSB && !gEnableILK && !gEnableMX4SIO && !gEnableBdmHDD)
        return;
    if (!bdmDeviceListInitialized)
        return;

    // 任一槽已有挂载则不启用保底
    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        bdm_device_data_t *pDeviceData = (bdm_device_data_t *)bdmDeviceList[i].priv;
        if (pDeviceData && pDeviceData->bdmPrefix[0] != '\0')
            return;
    }

    bdmKeepPlaceholder = 1;
    if (bdmDeviceList[0].owner != NULL)
        ((opl_io_module_t *)bdmDeviceList[0].owner)->menuItem.visible = 1;
}

static void bdmLoadBlockDeviceModules(void)
{
    int modulesLoaded = 0;

    if (gEnableILK && !iLinkModLoaded) {
        // Load iLink Block Device drivers
        LOG("[ILINKMAN]:\n");
        sysLoadModuleBuffer(&iLinkman_irx, size_iLinkman_irx, 0, NULL);
        LOG("[IEEE1394_BD]:\n");
        sysLoadModuleBuffer(&IEEE1394_bd_irx, size_IEEE1394_bd_irx, 0, NULL);

        iLinkModLoaded = 1;
        modulesLoaded = 1;
    }

    if (gEnableMX4SIO && !mx4sioModLoaded) {
        // Load MX4SIO Block Device drivers
        LOG("[MX4SIO_BD]:\n");
        sysLoadModuleBuffer(&mx4sio_bd_irx, size_mx4sio_bd_irx, 0, NULL);

        mx4sioModLoaded = 1;
        modulesLoaded = 1;
    }

    if (gEnableBdmHDD && !hddModLoaded) {
        // Load dev9 and atad device drivers.
        LOG("bdmLoadBlockDeviceModules loading hdd drivers...\n");
        hddLoadModules();

        hddModLoaded = 1;
        modulesLoaded = 1;
    }

    // Give newly loaded block-device drivers time to initialize. Do not stall
    // periodic BDM refreshes once every optional driver is already loaded.
    if (modulesLoaded)
        usleep(100000);
}

void bdmLoadModules(void)
{
    if (!bdmModLoaded) {
        bdmModLoaded = 1;
        LOG("BDMSUPPORT LoadModules\n");

        // Load Block Device Manager (BDM)
        LOG("[BDM]:\n");
        sysLoadModuleBuffer(&bdm_irx, size_bdm_irx, 0, NULL);

        // Load FATFS (mass:) driver
        LOG("[BDMFS_FATFS]:\n");
        sysLoadModuleBuffer(&bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL);

        // Load USB Block Device drivers
        LOG("[USBD]:\n");
        sysLoadModuleBuffer(&usbd_irx, size_usbd_irx, 0, NULL);
        LOG("[USBMASS_BD]:\n");
        sysLoadModuleBuffer(&usbmass_bd_irx, size_usbmass_bd_irx, 0, NULL);

        LOG("[BDMEVENT]:\n");
        sysLoadModuleBuffer(&bdmevent_irx, size_bdmevent_irx, 0, NULL);
        SifAddCmdHandler(0, &bdmEventHandler, NULL);

        LOG("BDMSUPPORT Modules loaded\n");
    }

    // Load Optional Block Device drivers
    bdmLoadBlockDeviceModules();
}

void bdmInit(item_list_t *itemList)
{
    LOG("BDMSUPPORT Init\n");

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->bdmModifiedCDPrev = 0;
    pDeviceData->bdmModifiedDVDPrev = 0;
    pDeviceData->bdmGameCount = 0;
    pDeviceData->bdmGames = NULL;
    pDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
    pDeviceData->massDeviceIndex = -1;
    pDeviceData->DeviceRemoved = 0;
    configGetInt(configGetByType(CONFIG_OPL), "usb_frames_delay", &itemList->delay);
    itemList->enabled = 1;
}

static int bdmNeedsUpdate(item_list_t *itemList)
{
    char path[256];
    int result = 0;
    struct stat st;

    // If we made it here then BDM device mode has been started.
    bdmDeviceModeStarted = 1;

    // If bdm mode is disabled bail out as we don't want to update the visibility state of the device pages.
    if (gBDMStartMode == START_MODE_DISABLED)
        return 0;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    bdmLoadBlockDeviceModules();

    // Check for forced refresh from deleting or renaming a game.
    if (pDeviceData->ForceRefresh != 0) {
        pDeviceData->ForceRefresh = 0;
        return 1;
    }

    //// If the device menu is visible double check the device type and if support for this device type is enabled. If the user switches device support
    //// to off for a bdm device we want to hide the menu even though the drivers are still loaded and the device is being detected by bdm.
    //opl_io_module_t *pOwner = (opl_io_module_t *)itemList->owner;
    //if (pOwner != NULL && pOwner->menuItem.visible == 1) {
    //    int deviceEnabled = 0;
    //    switch (pDeviceData->bdmDeviceType) {
    //        case BDM_TYPE_USB:
    //            //deviceEnabled = (gBDMStartMode != START_MODE_DISABLED);
    //            //deviceEnabled = (gEnableUSB || gEnableILK || gEnableMX4SIO || gEnableBdmHDD);
    //            deviceEnabled = gEnableUSB;
    //            break;
    //        case BDM_TYPE_ILINK:
    //            deviceEnabled = gEnableILK;
    //            break;
    //        case BDM_TYPE_SDC:
    //            deviceEnabled = gEnableMX4SIO;
    //            break;
    //        case BDM_TYPE_ATA:
    //            deviceEnabled = gEnableBdmHDD;
    //            break;
    //        default:
    //            deviceEnabled = 0;
    //            break;
    //    }

    //    // If the device page is visible but the device support is not enabled, hide the device page.
    //    if (deviceEnabled == 0)
    //        pOwner->menuItem.visible = 0;
    //}

    //// 加上mainScreenInitDone变量，让初始化阶段每一帧都检测bdm是否有更新，防止硬盘延迟启动造成的问题
    if ((pDeviceData->bdmULSizePrev != -2) && (pDeviceData->bdmDeviceTick == BdmGeneration) && mainScreenInitDone)
        return 0;
    //if ((pDeviceData->bdmULSizePrev != -2) && (pDeviceData->bdmDeviceTick == BdmGeneration))
        //return 0;
    pDeviceData->bdmDeviceTick = BdmGeneration;

    // Check if the device has been connected or removed.
    result = bdmUpdateDeviceData(itemList);
    if (bdmDefaultNeedsCorrection && gInitComplete && !mainScreenInitDone && itemList->owner &&
        pDeviceData->bdmPrefix[0] != '\0' && ((opl_io_module_t *)itemList->owner)->menuItem.visible) {
        struct gui_update_t *id = guiOpCreate(GUI_OP_SELECT_MENU);
        bdmDefaultNeedsCorrection = 0;
        id->menu.menu = &((opl_io_module_t *)itemList->owner)->menuItem;
        guiDeferUpdate(id);
    }
    if (result == 0)
        return 0;

    // If a device was added or removed play the appropriate UI sound.
    if (result == -1) {
        sfxPlay(SFX_BD_DISCONNECT);
        return result;
    }
    if (greetingAlpha <= 0)
        sfxPlay(SFX_BD_CONNECT);

    sprintf(path, "%sCD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedCDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedCDPrev = st.st_mtime;
        result = 1;
    }

    sprintf(path, "%sDVD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedDVDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedDVDPrev = st.st_mtime;
        result = 1;
    }

    if (!sbIsSameSize(pDeviceData->bdmPrefix, pDeviceData->bdmULSizePrev))
        result = 1;

    // update Themes
    if (!pDeviceData->ThemesLoaded) {
        sprintf(path, "%sTHM", pDeviceData->bdmPrefix);
        if (thmAddElements(path, "/", 1) > 0)
            pDeviceData->ThemesLoaded = 1;
    }

    // update Languages
    if (!pDeviceData->LanguagesLoaded) {
        sprintf(path, "%sLNG", pDeviceData->bdmPrefix);
        if (lngAddLanguages(path, "/", itemList->mode) > 0)
            pDeviceData->LanguagesLoaded = 1;
    }

    sbCreateFolders(pDeviceData->bdmPrefix, 1);
    return result;
}

static int bdmUpdateGameList(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    // 检查设备是否就绪，第一次没有开启设备时返回-1
    if (pDeviceData != NULL) {
        if (pDeviceData->DeviceRemoved) {
            free(pDeviceData->bdmGames);
            pDeviceData->bdmGames = NULL;
            pDeviceData->bdmGameCount = 0;
            pDeviceData->bdmULSizePrev = -2;
            pDeviceData->bdmModifiedCDPrev = 0;
            pDeviceData->bdmModifiedDVDPrev = 0;
            pDeviceData->bdmPrefix[0] = '\0';
            pDeviceData->bdmDriver[0] = '\0';
            // 保留bdmDeviceType，拔出后仍按原类型与开关保留空页
            pDeviceData->massDeviceIndex = -1;
            pDeviceData->ThemesLoaded = 0;
            pDeviceData->LanguagesLoaded = 0;
            pDeviceData->DeviceRemoved = 0;
            return 0;
        }

        // 已拔出的空页：不扫列表
        if (pDeviceData->bdmPrefix[0] == '\0') {
            pDeviceData->bdmGameCount = 0;
            return 0;
        }

        int bdmDeviceOn = 0;
        if (pDeviceData->bdmDeviceType == BDM_TYPE_USB) {
            bdmDeviceOn = gEnableUSB;
        } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK) {
            bdmDeviceOn = gEnableILK;
        } else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC) {
            bdmDeviceOn = gEnableMX4SIO;
        } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA) {
            bdmDeviceOn = gEnableBdmHDD;
        } else { // 未初始化，或未知设备
            return 0;
        }
        // 已加载的设备，没开就返回0，开了就返回生成列表后的游戏数量（只有USB会走进已加载但没开的这个分支条件）
        if (!bdmDeviceOn) {
            pDeviceData->bdmGameCount = -1;
            return 0;
        } else {
            enum { BDMFS_NO_PATH = -5 };
            char isoPath[256];
            int rootDir, cdDir, dvdDir;

            rootDir = fileXioDopen(pDeviceData->bdmPrefix);
            if (rootDir < 0) {
                pDeviceData->bdmGameCount = -1;
                return 0;
            }

            if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC) {
                iox_dirent_t dirent;

                // MX4SIO根目录为空时，继续保持未完成状态，等待下一次扫描。
                if (fileXioDread(rootDir, &dirent) <= 0) {
                    fileXioDclose(rootDir);
                    pDeviceData->bdmGameCount = -1;
                    return 0;
                }
            }
            fileXioDclose(rootDir);

            snprintf(isoPath, sizeof(isoPath), "%sCD", pDeviceData->bdmPrefix);
            cdDir = fileXioDopen(isoPath);
            if (cdDir >= 0)
                fileXioDclose(cdDir);

            snprintf(isoPath, sizeof(isoPath), "%sDVD", pDeviceData->bdmPrefix);
            dvdDir = fileXioDopen(isoPath);
            if (dvdDir >= 0)
                fileXioDclose(dvdDir);

            // bdmfs_fatfs会将FatFs的FRESULT取负后原样返回。
            if ((cdDir < 0 && cdDir != BDMFS_NO_PATH) ||
                (dvdDir < 0 && dvdDir != BDMFS_NO_PATH)) {
                pDeviceData->bdmGameCount = -1;
                return 0;
            }

            int result = sbReadList(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, &pDeviceData->bdmULSizePrev, &pDeviceData->bdmGameCount);

            // 游戏列表生成完成后，再标记对应的BDM设备已完成初始化。
            if (pDeviceData->bdmDeviceType == BDM_TYPE_USB)
                usbFound = 1;
            else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
                ILKFound = 1;
            else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
                MX4SIOFound = 1;
            else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
                GptFound = 1;

            return result;
        }
    } else
        return 0;
}

static int bdmGetGameCount(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return pDeviceData->bdmGameCount;
}

static void *bdmGetGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return (void *)&pDeviceData->bdmGames[id];
}

static char *bdmGetGameName(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return pDeviceData->bdmGames[id].name;
}

static int bdmGetGameNameLength(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return ((pDeviceData->bdmGames[id].format != GAME_FORMAT_USBLD) ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1);
}

static char *bdmGetGameStartup(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return pDeviceData->bdmGames[id].startup;
}

static void bdmDeleteGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    sbDelete(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

static void bdmRenameGame(item_list_t *itemList, int id, char *newName)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    sbRename(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id, newName);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, fd, iop_fd, index, compatmask = 0;
    int EnablePS2Logo = 0;
    int result;
    u64 startingLBA;
    unsigned int startCluster;
    char partname[256], filename[32];
    base_game_info_t *game;
    struct cdvdman_settings_bdm *settings;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;
    apa_sub_t parts[APA_MAXSUB + 1];
    pfs_blockinfo_t blocks[BDM_MAX_FRAGS];
    int hddPartCount = 0;

    bdm_device_data_t *pDeviceData = NULL;

    if (gAutoLaunchBDMGame == NULL) {
        pDeviceData = (bdm_device_data_t *)itemList->priv;
        game = &pDeviceData->bdmGames[id];
    } else {
        pDeviceData = gAutoLaunchDeviceData;
        game = gAutoLaunchBDMGame;
    }

    char vmc_name[32], vmc_path[256], have_error = 0;
    int vmc_id, size_mcemu_irx = 0;
    bdm_vmc_infos_t bdm_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&bdm_vmc_infos, 0, sizeof(bdm_vmc_infos_t));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0]) {
            have_error = 1;
            int vmcSizeInMb = sysCheckVMC(pDeviceData->bdmPrefix, "/", vmc_name, 0, &vmc_superblock);
            if (vmcSizeInMb > 0) {
                bdm_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                bdm_vmc_infos.flags |= 0x100;
                bdm_vmc_infos.specs.page_size = vmc_superblock.page_size;
                bdm_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                bdm_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                sprintf(vmc_path, "%sVMC/%s.bin", pDeviceData->bdmPrefix, vmc_name);

                fd = open(vmc_path, O_RDONLY);
                if (fd >= 0) {
                    iop_fd = ps2sdk_get_iop_fd(fd);
                    if (fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_LBA, NULL, 0, &startingLBA, sizeof(startingLBA)) == 0 && (startCluster = (unsigned int)fileXioIoctl(iop_fd, USBMASS_IOCTL_GET_CLUSTER, vmc_path)) != 0) {

                        // VMC only supports 32bit LBAs at the moment, so if the starting LBA + size of the VMC crosses the 32bit boundary
                        // just report the VMC as being fragmented to prevent file system corruption.
                        int vmcSectorCount = vmcSizeInMb * ((1024 * 1024) / 512); // size in MB * sectors per MB
                        if ((u32)startingLBA + (u32)vmcSectorCount > 0x100000000) {
                            LOG("BDMSUPPORT VMC bad LBA range\n");
                            have_error = 2;
                        }
                        // Check VMC cluster chain for fragmentation (write operation can cause damage to the filesystem).
                        else if (fileXioIoctl(iop_fd, USBMASS_IOCTL_CHECK_CHAIN, "") == 1) {
                            LOG("BDMSUPPORT Cluster Chain OK\n");
                            have_error = 0;
                            bdm_vmc_infos.active = 1;
                            bdm_vmc_infos.start_sector = (u32)startingLBA;
                            LOG("BDMSUPPORT VMC slot %d start: 0x%X\n", vmc_id, (u32)startingLBA);
                        } else {
                            LOG("BDMSUPPORT Cluster Chain NG\n");
                            have_error = 2;
                        }
                    }

                    close(fd);
                }
            }
        }

        if (gAutoLaunchBDMGame == NULL) {
            if (have_error) {
                char error[256];
                if (have_error == 2) // VMC file is fragmented
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTED_CONTINUE), vmc_name, (vmc_id + 1));
                else
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, (vmc_id + 1));
                if (!guiMsgBox(error, 1, NULL)) {
                    return;
                }
            }
        } else
            LOG("VMC error\n");

        for (i = 0; i < size_bdm_mcemu_irx; i++) {
            if (((u32 *)&bdm_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (bdm_vmc_infos.active)
                    size_mcemu_irx = size_bdm_mcemu_irx;
                memcpy(&((u32 *)&bdm_mcemu_irx)[i], &bdm_vmc_infos, sizeof(bdm_vmc_infos_t));
                break;
            }
        }
    }

    void *irx = NULL;
    int irx_size = 0;
    if (!strcmp(pDeviceData->bdmDriver, "ata") && strlen(pDeviceData->bdmDriver) == 3) {
        irx = &bdm_ata_cdvdman_irx;
        irx_size = size_bdm_ata_cdvdman_irx;
    } else {
        irx = &bdm_cdvdman_irx;
        irx_size = size_bdm_cdvdman_irx;
    }

    if (!strncmp(pDeviceData->bdmPrefix, "pfs", 3))
        hddPartCount = hddGetPartitionInfo(gOPLPart, parts);

    compatmask = sbPrepare(game, configSet, irx_size, irx, &index);
    settings = (struct cdvdman_settings_bdm *)((u8 *)irx + index);
    if (settings == NULL) {
        return;
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memset(&settings->frags[0], 0, sizeof(bd_fragment_t) * BDM_MAX_FRAGS);
#pragma GCC diagnostic pop
    u8 iTotalFragCount = 0;

    //
    // Add ISO as fragfile[0] to fragment list
    //
    struct cdvdman_fragfile *iso_frag = &settings->fragfile[0];
    iso_frag->frag_start = 0;
    iso_frag->frag_count = 0;
    for (i = 0; i < game->parts; i++) {
        // Open file
        sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", i);
        fd = open(partname, O_RDONLY);
        iop_fd = ps2sdk_get_iop_fd(fd);
        if (fd < 0) {
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
            return;
        }

        // Get fragment list
        int iFragCount;
        if (!strncmp(pDeviceData->bdmPrefix, "pfs", 3)) {
            iFragCount = hddGetFileBlockInfo(partname, parts, blocks, BDM_MAX_FRAGS - iTotalFragCount);
            if (iFragCount > 0) {
                int j;

                iFragCount--;
                for (j = 0; j < iFragCount; j++) {
                    if (blocks[j + 1].subpart >= hddPartCount) {
                        iFragCount = -1;
                        break;
                    }
                    settings->frags[iTotalFragCount + j].sector = parts[blocks[j + 1].subpart].start + ((u64)blocks[j + 1].number << 4);
                    settings->frags[iTotalFragCount + j].count = (u32)blocks[j + 1].count << 4;
                }
                settings->fragsAre512ByteSectors = 1;
            }
        } else
            iFragCount = fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_FRAGLIST, NULL, 0, (void *)&settings->frags[iTotalFragCount], sizeof(bd_fragment_t) * (BDM_MAX_FRAGS - iTotalFragCount));
        if ((!strncmp(pDeviceData->bdmPrefix, "pfs", 3) && iFragCount <= 0) || iFragCount > BDM_MAX_FRAGS) {
            // Too many fragments
            close(fd);
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FRAGMENTED), 0, NULL);
            return;
        }
        iso_frag->frag_count += iFragCount;
        iTotalFragCount += iFragCount;

        if ((gPS2Logo) && (i == 0))
            EnablePS2Logo = CheckPS2Logo(fd, 0);

        close(fd);
    }

    // Initialize layer 1 information.
    sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", 0);
    layer1_start = sbGetISO9660MaxLBA(partname);

    switch (game->format) {
        case GAME_FORMAT_USBLD:
            layer1_part = layer1_start / 0x80000;
            layer1_offset = layer1_start % 0x80000;
            sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", layer1_part);
            break;
        default: // Raw ISO9660 disc image; one part.
            layer1_part = 0;
            layer1_offset = layer1_start;
    }

    if (sbProbeISO9660(partname, game, layer1_offset) != 0) {
        layer1_start = 0;
        LOG("DVD detected.\n");
    } else {
        layer1_start -= 16;
        LOG("DVD-DL layer 1 @ part %u sector 0x%lx.\n", layer1_part, layer1_offset);
    }
    settings->common.layer1_start = layer1_start;

    // adjust ZSO cache
    settings->common.zso_cache = bdmCacheSize;

    if ((result = sbLoadCheats(pDeviceData->bdmPrefix, game->startup)) < 0) {
        if (gAutoLaunchBDMGame == NULL) {
            switch (result) {
                case -ENOENT:
                    guiWarning(_l(_STR_NO_CHEATS_FOUND), 10);
                    break;
                default:
                    guiWarning(_l(_STR_ERR_CHEATS_LOAD_FAILED), 10);
            }
        } else
            LOG("Cheats error\n");
    }

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    // deinit will free per device data.. copy driver name before free to compare for launch
    char bdmCurrentDriver[32];
    snprintf(bdmCurrentDriver, sizeof(bdmCurrentDriver), "%s", pDeviceData->bdmDriver);
    settings->bdDeviceId = pDeviceData->massDeviceIndex;

    if (!strcmp(bdmCurrentDriver, "ata") && strlen(bdmCurrentDriver) == 3) {
        // Get DMA settings for ATA mode.
        int dmaType = 0x40, dmaMode = 7;  // 默认为UDMA 4，与官方一致
        configGetInt(configSet, CONFIG_ITEM_DMASOURCE, &gDmaSource);
        if (gDmaSource == 0)
            configGetInt(configGetByType(CONFIG_GAME), CONFIG_ITEM_DMA, &dmaMode);
        else
            configGetInt(configSet, CONFIG_ITEM_DMA, &dmaMode);

        // Set DMA mode and spindown time.
        if (dmaMode < 3)
            dmaType = 0x20;
        else
            dmaMode -= 3;

        //// debug
        //char debugFileDir[64];
        //strcpy(debugFileDir, "mass0:debug-UDMA.txt");
        //FILE *debugFile = fopen(debugFileDir, "ab+");
        //if (debugFile != NULL) {
        //    fprintf(debugFile, "游戏以UDMA %d模式启动了！\r\n\r\n", dmaMode);
        //    fclose(debugFile);
        //}

        hddSetTransferMode(dmaType, dmaMode);
        // gHDDSpindown [0..20] -> spindown [0..240] -> seconds [0..1200]
        hddSetIdleTimeout(gHDDSpindown * 12);
        settings->hddIsLBA48 = pDeviceData->bdmHddIsLBA48;
    }

    if (gAutoLaunchBDMGame == NULL)
        deinit(NO_EXCEPTION, itemList->mode); // CAREFUL: deinit will call bdmCleanUp, so bdmGames/game will be freed
    else {
        miniDeinit(configSet);

        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;

        free(gAutoLaunchDeviceData);
        gAutoLaunchDeviceData = NULL;
    }

    LOG("bdm pre sysLaunchLoaderElf\n");
    if (!strcmp(bdmCurrentDriver, "usb")) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_USBD;
        sysLaunchLoaderElf(filename, "BDM_USB_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "sd") && strlen(bdmCurrentDriver) == 2) {
        settings->common.fakemodule_flags |= 0 /* TODO! fake ilinkman ? */;
        sysLaunchLoaderElf(filename, "BDM_ILK_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "sdc") && strlen(bdmCurrentDriver) == 3) {
        settings->common.fakemodule_flags |= 0;
        sysLaunchLoaderElf(filename, "BDM_M4S_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (!strcmp(bdmCurrentDriver, "ata") && strlen(bdmCurrentDriver) == 3) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_ATAD;
        sysLaunchLoaderElf(filename, "BDM_ATA_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    }
}

static config_set_t *bdmGetConfig(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return sbPopulateConfig(&pDeviceData->bdmGames[id], pDeviceData->bdmPrefix, "/");
}

static int bdmGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (!value || pDeviceData->bdmDeviceType == BDM_TYPE_UNKNOWN)
        return ERR_BAD_FILE;

    char path[256];
    if (isRelative) {
        // 根据BDM类型开启相应的分桶开关
        int artUseBuckets = 0;
        if (pDeviceData->bdmDeviceType == BDM_TYPE_USB)
            artUseBuckets = artUseBuckets_USB;
        else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
            artUseBuckets = artUseBuckets_ILINK;
        else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
            artUseBuckets = artUseBuckets_SDC;
        else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
            artUseBuckets = artUseBuckets_ATA;

        if (artUseBuckets) {
            int len = strlen(value);
            if (len >= 4 && (value[len - 1] == 'F' || value[len - 1] == 'f'))
                snprintf(path, sizeof(path), "%sART2/APPS/%s/%s_%s", pDeviceData->bdmPrefix, value, value, suffix);
            else
                snprintf(path, sizeof(path), "%sART2/GAMES/%s/%s_%s", pDeviceData->bdmPrefix, value, value, suffix);
        } else
            snprintf(path, sizeof(path), "%s%s/%s_%s", pDeviceData->bdmPrefix, folder, value, suffix);
    } else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);

    return texDiscoverLoad(resultTex, path, -1);
}

static int bdmGetTextId(item_list_t *itemList)
{
    int mode = _STR_BDM_GAMES;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (pDeviceData->bdmDeviceType == BDM_TYPE_USB)
        mode = _STR_USB_GAMES;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
        mode = _STR_ILINK_GAMES;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
        mode = _STR_MX4SIO_GAMES;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
        mode = _STR_HDD_GAMES;

    return mode;
}

static int bdmGetIconId(item_list_t *itemList)
{
    int mode = BDM_ICON;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (pDeviceData->bdmDeviceType == BDM_TYPE_USB && gEnableUSB)
        mode = USB_ICON;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
        mode = ILINK_ICON;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
        mode = MX4SIO_ICON;
    else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
        mode = HDD_BD_ICON;

    return mode;
}

// This may be called, even if bdmInit() was not.
static void bdmCleanUp(item_list_t *itemList, int exception)
{
    if (itemList->enabled) {
        LOG("BDMSUPPORT CleanUp\n");

        bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
        free(pDeviceData->bdmGames);
        free(pDeviceData);
        itemList->priv = NULL;

        //      if ((exception & UNMOUNT_EXCEPTION) == 0)
        //          ...
    }
}

// This may be called, even if bdmInit() was not.
static void bdmShutdown(item_list_t *itemList)
{
    char path[16];

    LOG("BDMSUPPORT Shutdown\n");

    // Format the device path.
    // Getting the device number is only relevant per module ie usb0 and mx40 will result in both being massDeviceIndex = 0 or mass0, use mode to determine instead.
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    snprintf(path, sizeof(path), "mass%d:", itemList->mode);

    // As required by some (typically 2.5") HDDs, issue the SCSI STOP UNIT command to avoid causing an emergency park.
    fileXioDevctl(path, USBMASS_DEVCTL_STOP_ALL, NULL, 0, NULL, 0);

    if (itemList->enabled) {
        LOG("BDMSUPPORT Shutdown free data\n");

        // Free device data.
        free(pDeviceData->bdmGames);
        free(pDeviceData);
        itemList->priv = NULL;
    }
}

static int bdmCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return sysCheckVMC(pDeviceData->bdmPrefix, "/", name, createSize, NULL);
}

static char *bdmGetPrefix(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData->bdmPrefix;
}

static item_list_t bdmGameList = {
    BDM_MODE, 2, 0, 0, MENU_MIN_INACTIVE_FRAMES, BDM_MODE_UPDATE_DELAY, NULL, NULL, &bdmGetTextId, &bdmGetPrefix, &bdmInit, &bdmNeedsUpdate,
    &bdmUpdateGameList, &bdmGetGameCount, &bdmGetGame, &bdmGetGameName, &bdmGetGameNameLength, &bdmGetGameStartup, &bdmDeleteGame, &bdmRenameGame,
    &bdmLaunchGame, &bdmGetConfig, &bdmGetImage, &bdmCleanUp, &bdmShutdown, &bdmCheckVMC, &bdmGetIconId};

void bdmInitSemaphore()
{
    // Create a semaphore so only one thread can load IOP modules at a time.
    ee_sema_t semaphore;
    semaphore.init_count = 1;
    semaphore.max_count = 1;
    semaphore.option = 0;
    bdmLoadModuleLock = CreateSema(&semaphore);
}

void bdmInitDevicesData()
{
    // If the device list hasn't been initialized do it now.
    if (bdmDeviceListInitialized == 0) {
        bdmDeviceListInitialized = 1;

        for (int i = 0; i < MAX_BDM_DEVICES; i++) {
            // Setup the device list item.
            item_list_t *pDeviceSupport = &bdmDeviceList[i];
            memcpy(pDeviceSupport, &bdmGameList, sizeof(item_list_t));
            pDeviceSupport->mode = i;

            // Setup the per-device data.
            bdm_device_data_t *pDeviceData = (bdm_device_data_t *)malloc(sizeof(bdm_device_data_t));
            memset(pDeviceData, 0, sizeof(bdm_device_data_t));
            // memset后为0，会误判为BDM_TYPE_USB，需显式设为未知
            pDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
            pDeviceSupport->priv = pDeviceData;
        }
    }

    // Refresh the visibility of the menu.
    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        // Register the device structure into the UI.
        initSupport(&bdmDeviceList[i], i, 0);

        // If bdm support is set to auto then make the page invisible and reset the bdm tick counter, when a bdm device is mounted it will dynamically be made visible.
        // If bdm support is set to manual then only make the first page visible.
        if (bdmDeviceList[i].owner != NULL) {
            opl_io_module_t *pOwner = (opl_io_module_t *)bdmDeviceList[i].owner;

            if (gBDMStartMode == START_MODE_DISABLED) {
                pOwner->menuItem.visible = 0;
            } else if (gBDMStartMode == START_MODE_MANUAL) {
                // If BDM has already been started then make the page invisible and reset the bdm tick counter so visibility status is refreshed
                // according to device state.
                if (bdmDeviceModeStarted == 1) {
                    //pOwner->menuItem.visible = 0; // 已经在itemExecSelect里赋值
                    ((bdm_device_data_t *)bdmDeviceList[i].priv)->bdmDeviceTick = -1;
                } else {
                    if (i == 0) {
                        if (gEnableUSB || gEnableILK || gEnableMX4SIO || gEnableBdmHDD)
                            pOwner->menuItem.visible = 1;
                        else
                            pOwner->menuItem.visible = 0;
                    }
                    else {
                        pOwner->menuItem.visible = 0;
                    }
                }
            } else if (gBDMStartMode == START_MODE_AUTO) {
                //pOwner->menuItem.visible = 0; // initSupport里已经赋值
                ((bdm_device_data_t *)bdmDeviceList[i].priv)->bdmDeviceTick = -1;
            }
            LOG("bdmInitDevicesData: setting device %d %s\n", i, (pOwner->menuItem.visible != 0 ? "visible" : "invisible"));
        }
    }

    // applyConfig/initSupport会把UNKNOWN槽强制隐藏；主界面已就绪时需重新拉起保底页
    if (mainScreenInitDone)
        bdmTryActivateStartupPlaceholder();
}

void bdmEnumerateDevices()
{
    LOG("bdmEnumerateDevices\n");
    
    bdmLoadModules();
    
    // Initialize the device list data if it hasn't been initialized yet.
    bdmInitDevicesData();

    // Because bdmLoadModules is called before the config file is loaded bdmLoadBlockDeviceModules will not have loaded any
    // optional bdm modules. Now that the config file has been loaded try loading any optional modules that weren't previously loaded.

    LOG("bdmEnumerateDevices done\n");
}

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData)
{
    // If atad is loaded then xhdd is also loaded, query the hdd to see if it supports LBA48 or not.
    pDeviceData->bdmHddIsLBA48 = fileXioDevctl("xhdd0:", ATA_DEVCTL_IS_48BIT, NULL, 0, NULL, 0);
    if (pDeviceData->bdmHddIsLBA48 < 0) {
        // Failed to query the LBA limit of the device, fail safe to LBA28.
        LOG("Mass device %d is backed by ATA but failed to get LBA limit %d\n", pDeviceData->massDeviceIndex, pDeviceData->bdmHddIsLBA48);
        pDeviceData->bdmHddIsLBA48 = 0;
    }

    //// Query the drive for the highest UDMA mode.
    //pDeviceData->ataHighestUDMAMode = fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, NULL, 0, NULL, 0);
    //if (pDeviceData->ataHighestUDMAMode < 0 || pDeviceData->ataHighestUDMAMode > 7) {
    //    // Failed to query highest UDMA mode supported.
    //    LOG("Mass device %d is backed by ATA but failed to get highest UDMA mode %d\n", pDeviceData->massDeviceIndex, pDeviceData->ataHighestUDMAMode);
    //    pDeviceData->ataHighestUDMAMode = 4;
    //}
    // else if (pDeviceData->ataHighestUDMAMode > 4) {
    //     // Set the UDMA mode to highest available.
    //     hddSetTransferMode(0x40, pDeviceData->ataHighestUDMAMode);
    // }

    // 根据全局DMA设置，来重设DMA传输模式，加快Art图片的读取速度
    int gDmaMode = -1; // 获取配置失败时，不重设传输模式
    configGetInt(configGetByType(CONFIG_GAME), CONFIG_ITEM_DMA, &gDmaMode);
    if (gDmaMode >= 3 && gDmaMode <= 10)
        hddSetTransferMode(0x40, gDmaMode - 3);
    else if(gDmaMode >= 0 && gDmaMode <= 2)
        hddSetTransferMode(0x20, gDmaMode);

    //// debug
    // char debugFileDir[64];
    // strcpy(debugFileDir, "mass0:debug-UDMA.txt");
    // FILE *debugFile = fopen(debugFileDir, "ab+");
    // if (debugFile != NULL) {
    //     fprintf(debugFile, "BDMHDD启动时：传输模式校准为UDMA %d\r\n", gDmaMode - 3);
    //     fclose(debugFile);
    // }
}

//static int bdmHddCheckDone = 0;
//static int bdmHddRetryCount = 0;
int bdmUpdateDeviceData(item_list_t *itemList)
{
    // If bdm mode is disabled bail out as we don't want to update the visibility state of the device pages.
    if (gBDMStartMode == START_MODE_DISABLED)
        return 0;

    // LOG("bdmUpdateDeviceData: %d\n", itemList->mode);

    // Get the per-device data and check if the menu item is currently visible.
    bdm_device_data_t *pDeviceData = itemList->priv;
    int visible = itemList->owner != NULL ? ((opl_io_module_t *)itemList->owner)->menuItem.visible : 0;

    // Format the device path and try to open the device.
    char path[16] = {0};
    sprintf(path, "mass%d:/", itemList->mode);
    int dir = fileXioDopen(path);
    // LOG("opendir %s -> %d\n", path, dir);

    // If we opened the device and the menu isn't visible (OR is visible but hasn't been initialized ex: manual device start) initialize device info.
    if (dir >= 0) {
        if (pDeviceData->bdmPrefix[0] == '\0') {
            if (gBDMPrefix[0] != '\0')
                snprintf(pDeviceData->bdmPrefix, sizeof(pDeviceData->bdmPrefix), "mass%d:%s/", itemList->mode, gBDMPrefix);
            else
                snprintf(pDeviceData->bdmPrefix, sizeof(pDeviceData->bdmPrefix), "mass%d:", itemList->mode);

            // Get the name of the underlying device driver that backs the fat fs.
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &pDeviceData->bdmDriver, sizeof(pDeviceData->bdmDriver) - 1);
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &pDeviceData->massDeviceIndex, sizeof(pDeviceData->massDeviceIndex));

            itemList->flags = 0;

            // Determine the bdm device type based on the underlying device driver.
            if (!strcmp(pDeviceData->bdmDriver, "usb"))
                pDeviceData->bdmDeviceType = BDM_TYPE_USB;
            else if (!strcmp(pDeviceData->bdmDriver, "sd") && strlen(pDeviceData->bdmDriver) == 2)
                pDeviceData->bdmDeviceType = BDM_TYPE_ILINK;
            else if (!strcmp(pDeviceData->bdmDriver, "sdc") && strlen(pDeviceData->bdmDriver) == 3)
                pDeviceData->bdmDeviceType = BDM_TYPE_SDC;
            else if (!strcmp(pDeviceData->bdmDriver, "ata") && strlen(pDeviceData->bdmDriver) == 3) {
                pDeviceData->bdmDeviceType = BDM_TYPE_ATA;
                itemList->flags = MODE_FLAG_COMPAT_DMA;
            } else
                pDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;

            // 根据BDM类型开启相应的分桶开关
            char art2Path[128];
            snprintf(art2Path, sizeof(art2Path), "%sART2", pDeviceData->bdmPrefix);
            if (pDeviceData->bdmDeviceType != BDM_TYPE_UNKNOWN) {
                DIR *art2Dir = opendir(art2Path);
                int artUseBuckets = art2Dir ? 1 : 0;

                if (art2Dir)
                    closedir(art2Dir);

                if (pDeviceData->bdmDeviceType == BDM_TYPE_USB)
                    artUseBuckets_USB = artUseBuckets;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
                    artUseBuckets_ILINK = artUseBuckets;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
                    artUseBuckets_SDC = artUseBuckets;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
                    artUseBuckets_ATA = artUseBuckets;
            }

            // If the device is backed by the ATA driver then get the supported LBA size for the drive.
            if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA) {
                bdmResolveLBA_UDMA(pDeviceData);
                LOG("Mass device: %d (%d LBA%d UDMA%d) %s -> %s\n", itemList->mode, pDeviceData->massDeviceIndex, (pDeviceData->bdmHddIsLBA48 == 1 ? 48 : 28), pDeviceData->ataHighestUDMAMode, pDeviceData->bdmPrefix, pDeviceData->bdmDriver);
            } else
                LOG("Mass device: %d (%d) %s -> %s\n", itemList->mode, pDeviceData->massDeviceIndex, pDeviceData->bdmPrefix, pDeviceData->bdmDriver);

            // Make the menu item visible.
            if (itemList->owner != NULL) {
                // 任一真设备挂上后结束启动保底；若BDM0仍为空则藏掉保底页
                if (bdmKeepPlaceholder) {
                    bdmKeepPlaceholder = 0;
                    if (itemList->mode != 0 && bdmDeviceList[0].owner != NULL) {
                        bdm_device_data_t *pSlot0 = (bdm_device_data_t *)bdmDeviceList[0].priv;
                        if (pSlot0 && pSlot0->bdmPrefix[0] == '\0' && pSlot0->bdmDeviceType == BDM_TYPE_UNKNOWN)
                            ((opl_io_module_t *)bdmDeviceList[0].owner)->menuItem.visible = 0;
                    }
                }
                // 设备初始化完成后，根据BDM设备开关，来决定visible的值
                if (pDeviceData->bdmDeviceType == BDM_TYPE_USB)
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableUSB;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableILK;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableMX4SIO;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableBdmHDD;
                else
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = 0; // 默认隐藏
            }
            // Close the device handle.
            fileXioDclose(dir);
            return 1;
        } else { // 如果已经初始化
            // 设备从关到开，才需要return1，否则不更新
            int result = 0;
            if (itemList->owner != NULL) {
                // 设备初始化后，开关从关闭到开启
                if (pDeviceData->bdmDeviceType == BDM_TYPE_USB) {
                    result = (visible && (pDeviceData->bdmGameCount == -1));
                } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK) {
                    result = (visible && (pDeviceData->bdmGameCount == -1));
                } else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC) {
                    result = (visible && (pDeviceData->bdmGameCount == -1));
                } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA) {
                    result = (visible && (pDeviceData->bdmGameCount == -1));
                } else {
                    result = 0;
                }
            }
            // Close the device handle.
            fileXioDclose(dir);
            return result;
        }
    } else if (dir < 0 && (visible == 1 || (bdmKeepPlaceholder && itemList->mode == 0))) {
        // 如果是真实设备被移除，才走移除路径，播放移除音效等
        if (pDeviceData->bdmPrefix[0] != '\0') {
            LOG("Mass device: %d (%d) disconnected\n", itemList->mode, pDeviceData->massDeviceIndex);
            pDeviceData->DeviceRemoved = 1;
            return -1;
        }
        // 无挂载时：未知空槽隐藏；曾识别过的按开关保留空页
        // 注：applyConfig后保底页可能已被设为不可见，故上面条件允许在visible==0时仍进入以恢复BDM0
        if (itemList->owner != NULL) {
            if (pDeviceData->bdmDeviceType == BDM_TYPE_UNKNOWN) {
                // 启动保底期间不隐藏空的BDM0
                if (bdmKeepPlaceholder && itemList->mode == 0) {
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = 1;
                } else {
                    LOG("bdmUpdateDeviceData: setting device %d invisible\n", itemList->mode);
                    ((opl_io_module_t *)itemList->owner)->menuItem.visible = 0;
                }
            } else if (pDeviceData->bdmDeviceType == BDM_TYPE_USB) {
                ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableUSB;
            } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK) {
                ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableILK;
            } else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC) {
                ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableMX4SIO;
            } else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA) {
                ((opl_io_module_t *)itemList->owner)->menuItem.visible = gEnableBdmHDD;
            } else {
                ((opl_io_module_t *)itemList->owner)->menuItem.visible = 0;
            }
        }
    }
    return 0;
}
