#include "sys/fcntl.h"
#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/hddsupport.h"
#include "include/bdmsupport.h"
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "modules/iopcore/common/cdvd_config.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioFormat, fileXioMount, fileXioUmount, fileXioDevctl
#include <io_common.h>   // FIO_MT_RDWR

#include <hdd-ioctl.h>

#define OPL_HDD_MODE_PS2LOGO_OFFSET 0x17F8

#include "../modules/isofs/zso.h"

extern int probed_fd;
extern u32 probed_lba;
extern u8 IOBuffer[2048];

static unsigned char hddForceUpdate = 0;
static unsigned char hddHDProKitDetected = 0;
static unsigned char hddModulesLoadCount = 0;
static unsigned char hddSupportModulesLoaded = 0;
static unsigned char hddModulesLoading = 0;
static unsigned char hddConfigSource = 0;
static unsigned char hddConfigModulesRetained = 0;

static char *hddPrefix = "pfs0:";
static hdl_games_list_t hddGames;
static base_game_info_t *hddIsoGames;
static int hddIsoGameCount;
static int hddIsoFileSize;

typedef struct
{
    u32 checksum;
    u32 magic;
    char gamename[160];
    u8 compat_flags;
    u8 pad[3];
    char startup[60];
    u32 layer1_start;
    u32 discType;
    int num_partitions;
    struct
    {
        u32 part_offset;
        u32 data_start;
        u32 part_size;
    } part_specs[65];
} hdl_apa_header;

// forward declaration
static item_list_t hddGameList;

// 判断APA设备是否使用ART2文件夹
static int artUseBuckets_APA = 0;

static int hddLoadGameListCache(hdl_games_list_t *cache);
static int hddUpdateGameListCache(hdl_games_list_t *cache, hdl_games_list_t *game_list);

static int hddInitModules(void)
{
    int result, retryCount = 0;

    // 从HDD读取配置时已经启动了完整的HDD模块栈。
    // 后续自动或手动初始化HDD时，直接接管之前保留的生命周期引用，避免重复增加计数。
    if (hddConfigModulesRetained)
        hddConfigModulesRetained = 0;
    else
        hddLoadModules();

    if (!hddLoadModulesSuccess)
        return -1;

    // 如果驱动加载成功，就不断重试hddLoadSupportModules，直到超时2秒
    while ((result = hddLoadSupportModules())) {
        if (++retryCount >= 20)
            return result;
        usleep(100000);
    }

    // update Themes
    char path[256];
    sprintf(path, "%sTHM", gHDDPrefix);
    thmAddElements(path, "/", 1);

    sprintf(path, "%sLNG", gHDDPrefix);
    lngAddLanguages(path, "/", hddGameList.mode);

    sbCreateFolders(gHDDPrefix, 0);
    return 0;
}

// HD Pro Kit is mapping the 1st word in ROM0 seg as a main ATA controller,
// The pseudo ATA controller registers are accessed (input/ouput) by writing
// an id to the main ATA controller
#define HDPROreg_IO8   (*(volatile unsigned char *)0xBFC00000)
#define CDVDreg_STATUS (*(volatile unsigned char *)0xBF40200A)

static int hddCheckHDProKit(void)
{
    int ret = 0;

    DIntr();
    ee_kmode_enter();
    // HD Pro IO start commands sequence
    HDPROreg_IO8 = 0x72;
    CDVDreg_STATUS = 0;
    HDPROreg_IO8 = 0x34;
    CDVDreg_STATUS = 0;
    HDPROreg_IO8 = 0x61;
    CDVDreg_STATUS = 0;
    u32 res = HDPROreg_IO8;
    CDVDreg_STATUS = 0;

    // check result
    if ((res & 0xff) == 0xe7) {
        // HD Pro IO finish commands sequence
        HDPROreg_IO8 = 0xf3;
        CDVDreg_STATUS = 0;
        ret = 1;
    }
    ee_kmode_exit();
    EIntr();

    if (ret)
        LOG("HDDSUPPORT HD Pro Kit detected!\n");

    return ret;
}

// Taken from libhdd:
#define PFS_ZONE_SIZE 8192
#define PFS_FRAGMENT  0x00000000

static void hddCheckOPLFolder(const char *mountPoint)
{
    DIR *dir;
    char path[32];

    sprintf(path, "%sOPL", mountPoint);

    dir = opendir(path);
    if (dir == NULL)
        mkdir(path, 0777);
    else
        closedir(dir);
}

static void hddFindOPLPartition(void)
{
    static config_set_t *config;
    char name[64];
    int fd, ret = 0;

    fileXioUmount(hddPrefix);

    ret = fileXioMount("pfs0:", "hdd0:__common", FIO_MT_RDWR);
    if (ret == 0) {
        fd = open("pfs0:OPL/conf_hdd.cfg", O_RDONLY);
        if (fd >= 0) {
            config = configAlloc(0, NULL, "pfs0:OPL/conf_hdd.cfg");
            configRead(config);

            configGetStrCopy(config, "hdd_partition", name, sizeof(name));
            snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:%s", name);

            configFree(config);
            close(fd);

            return;
        }

        hddCheckOPLFolder(hddPrefix);

        fd = open("pfs0:OPL/conf_hdd.cfg", O_CREAT | O_TRUNC | O_WRONLY);
        if (fd >= 0) {
            config = configAlloc(0, NULL, "pfs0:OPL/conf_hdd.cfg");
            configRead(config);

            configSetStr(config, "hdd_partition", "+OPL");
            configWrite(config);

            configFree(config);
            close(fd);
        }
    }

    snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:+OPL");

    return;
}

static int hddCreateOPLPartition(const char *name)
{
    int formatArg[3] = {PFS_ZONE_SIZE, 0x2d66, PFS_FRAGMENT};
    int fd, result;
    char cmd[140];

    sprintf(cmd, "%s,,,128M,PFS", name);
    if ((fd = open(cmd, O_CREAT | O_TRUNC | O_WRONLY)) >= 0) {
        close(fd);
        result = fileXioFormat(hddPrefix, name, (const char *)&formatArg, sizeof(formatArg));
    } else {
        result = fd;
    }

    return result;
}

int hddLoadModulesSuccess = 0;
static void hddLoadModulesInternal(int bdmAsync)
{
    static char bdmAtadArg[] = "-bdm_async";
    int ret, xhddRet;

    LOG("HDDSUPPORT LoadModules %d\n", hddModulesLoadCount);

    if (hddModulesLoadCount == 0 || !hddLoadModulesSuccess) {
        if (hddModulesLoading) {
            hddModulesLoadCount++;
            return;
        }

        // Increment the load count as soon as possible to prevent thread scheduling from allowing another thread to
        // call into here and try to double load modules.
        if (hddModulesLoadCount == 0) {
            hddModulesLoadCount = 1;

            // DEV9 must be loaded, as HDD.IRX depends on it. Even if not required by the I/F (i.e. HDPro)
            sysInitDev9();
        }

        // 保留已加载的IRX和DEV9引用，使后续调用可以继续重试未完成的模块。
        hddModulesLoading = 1;

        // try to detect HD Pro Kit (not the connected HDD),
        // if detected it loads the specific ATAD module
        hddHDProKitDetected = hddCheckHDProKit();
        if (hddHDProKitDetected) {
            LOG("[ATAD_HDPRO]:\n");
            ret = sysLoadModuleBuffer(&hdpro_atad_irx, size_hdpro_atad_irx, 0, NULL);
            LOG("[XHDD]:\n");
            xhddRet = sysLoadModuleBuffer(&xhdd_irx, size_xhdd_irx, 6, "-hdpro");
        } else {
            LOG("[ATAD]:\n");
            ret = sysLoadModuleBuffer(&ps2atad_irx, size_ps2atad_irx,
                                      bdmAsync ? sizeof(bdmAtadArg) : 0,
                                      bdmAsync ? bdmAtadArg : NULL);
            LOG("[XHDD]:\n");
            xhddRet = sysLoadModuleBuffer(&xhdd_irx, size_xhdd_irx, 0, NULL);
        }

        if (ret < 0 || xhddRet < 0) {
            hddModulesLoading = 0;
            LOG("HDD: No HardDisk Drive detected.\n");
            setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_IF_NOT_DETECTED);
            return;
        }
        hddLoadModulesSuccess = 1;
        hddModulesLoading = 0;
        //usleep(500000); // 延迟0.5秒,加一点延迟,尤其在PS2上的HDD可能需要
    } else if (hddLoadModulesSuccess)
        hddModulesLoadCount++;

    LOG("HDDSUPPORT LoadModules done\n");
}

void hddLoadModules(void)
{
    hddLoadModulesInternal(0);
}

void hddLoadModulesBDM(void)
{
    hddLoadModulesInternal(1);
}

// Returns 1 for MBR/GPT, 0 for APA, and -1 if an error occured
int hddDetectNonSonyFileSystem()
{
    int result = -1;
    // Allocate memory for storing data for the first two sectors.
    u8 *pSectorData = (u8 *)malloc(512 * 2);
    if (pSectorData == NULL) {
        LOG("hddDetectNonSonyFileSystem: failed to allocate scratch memory\n");
        return -1;
    }

    // Trying to load the APA/PFS irx modules when a non-sony formatted HDD is connected (ie: MBR/GPT  w/ exFAT) runs
    // the risk of corrupting the HDD. To avoid that get the first two sectors and perform some sanity checks. If
    // we reasonably suspect the disk is not APA formatted bail out from loading the sony fs irx modules.
    result = fileXioDevctl("xhdd0:", ATA_DEVCTL_READ_PARTITION_SECTOR, NULL, 0, pSectorData, 512 * 2);
    if (result < 0) {
        LOG("hddDetectNonSonyFileSystem: failed to read data from hdd %d\n", result);
        free(pSectorData);
        return -1;
    }

    // Check for MBR signature.
    if (pSectorData[0x1FE] == 0x55 && pSectorData[0x1FF] == 0xAA) {
        // Found MBR partition type.
        LOG("hddDetectNonSonyFileSystem: found MBR partition data\n");
        result = 1;
    } else if (strncmp((const char *)&pSectorData[0x200], "EFI PART", 8) == 0) {
        // Found GPT partition type.
        LOG("hddDetectNonSonyFileSystem: found GPT partition data\n");
        result = 1;
    } else if (strncmp((const char *)&pSectorData[4], "APA", 3) == 0) {
        // Found APA partition type.
        LOG("hddDetectNonSonyFileSystem: found APA partition data\n");
        result = 0;
    } else {
        // Even though we didn't find evidence of non-APA partition data, if we load the APA irx module
        // it will write to the drive and potentially corrupt any data that might be there.
        LOG("hddDetectNonSonyFileSystem: partition data not recognized\n");
        result = -1;
    }

    // Cleanup and return.
    free(pSectorData);
    return result;
}

int hddLoadSupportModules(void)
{
    static char hddarg[] = "-o"
                           "\0"
                           "4"
                           "\0"
                           "-n"
                           "\0"
                           "20";
    static char pfsarg[] = "-m" // 最大挂载点数量
                           "\0"
                           "2" // 同时保留pfs0和pfs1
                           "\0"
                           "-o" // max open
                           "\0"
                           "6" // Default value: 2
                           "\0"
                           "-n" // Number of buffers
                           "\0"
                           "24"; // Default value: 8 | Max value: 127

    LOG("HDDSUPPORT LoadSupportModules\n");

    // Check if the drive contains MBR/GPT partition data before we load the APA/PFS modules. If the drive is not
    // APA then loading the APA irx modules can corrupt the drive as it will try to write APA partition data.
    if (hddDetectNonSonyFileSystem() != 0) {
        // Drive is MBR/GPT style, or unknown, bail out or risk corrupting the drive.
        LOG("HDDSUPPORT LoadSupportModules bailing out early...\n");
        return -1;
    }

    if (!hddSupportModulesLoaded) {
        LOG("[HDD]:\n");
        int ret = sysLoadModuleBuffer(&ps2hdd_irx, size_ps2hdd_irx, sizeof(hddarg), hddarg);
        if (ret < 0) {
            LOG("HDD: No HardDisk Drive detected.\n");
            setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_MODULE_HDD_FAILURE);
            return -1;
        }

        // Check if a HDD unit is connected
        if (hddCheck() < 0) {
            LOG("HDD: No HardDisk Drive detected.\n");
            setErrorMessageWithCode(_STR_HDD_NOT_CONNECTED_ERROR, ERROR_HDD_NOT_DETECTED);
            return -1;
        }

        LOG("[PS2FS]:\n");
        ret = sysLoadModuleBuffer(&ps2fs_irx, size_ps2fs_irx, sizeof(pfsarg), pfsarg);
        if (ret < 0) {
            LOG("HDD: HardDisk Drive not formatted (PFS).\n");
            setErrorMessageWithCode(_STR_HDD_NOT_FORMATTED_ERROR, ERROR_HDD_MODULE_PFS_FAILURE);
            return -1;
        }

        if (gOPLPart[0] == '\0')
            hddFindOPLPartition();

        fileXioUmount(hddPrefix);

        ret = fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR);
        if (ret == -ENOENT) {
            // Attempt to create the partition.
            if ((hddCreateOPLPartition(gOPLPart)) >= 0)
                ret = fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR);
        }

        if (ret < 0)
            return ret;

        hddSupportModulesLoaded = 1;
        LOG("HDDSUPPORT modules loaded\n");

        if (gOPLPart[5] != '+') {
            hddCheckOPLFolder(hddPrefix);
            gHDDPrefix = "pfs0:OPL/";
        }

        // 判断是否存在ART2，提升图片读取效率
        char art2Path[128];
        snprintf(art2Path, sizeof(art2Path), "%sART2", gHDDPrefix);
        DIR *art2Dir = opendir(art2Path);
        artUseBuckets_APA = art2Dir ? 1 : 0;
        if (art2Dir)
            closedir(art2Dir);

        // 根据全局DMA设置，来重设DMA传输模式，加快Art图片的读取速度
        int gDmaMode = -1; // 获取配置失败时，不重设传输模式
        configGetInt(configGetByType(CONFIG_GAME), CONFIG_ITEM_DMA, &gDmaMode);
        if (gDmaMode >= 3 && gDmaMode <= 10)
            hddSetTransferMode(0x40, gDmaMode - 3);
        else if (gDmaMode >= 0 && gDmaMode <= 2)
            hddSetTransferMode(0x20, gDmaMode);

        //// debug
        // char debugFileDir[64];
        // strcpy(debugFileDir, "mass0:debug-UDMA.txt");
        // FILE *debugFile = fopen(debugFileDir, "ab+");
        // if (debugFile != NULL) {
        //     fprintf(debugFile, "APAHDD启动时：传输模式校准为UDMA %d\r\n", gDmaMode - 3);
        //     fclose(debugFile);
        // }
        return 0;
    }

    return 0;
}

void hddSetConfigSource(void)
{
    // 官方配置读取流程会强制自动启动HDD，因此在关闭前会多出一个HDD生命周期引用。
    // 在不修改用户启动模式、也不启用或扫描HDD游戏列表的前提下，保持相同的计数关系。
    if (!hddConfigSource && !hddGameList.enabled && hddModulesLoadCount > 0) {
        hddLoadModules();
        hddConfigModulesRetained = 1;
    }

    hddConfigSource = 1;
}

int hddIsConfigSource(void)
{
    return hddConfigSource;
}

void hddInit(item_list_t *itemList)
{
    LOG("HDDSUPPORT Init\n");
    hddForceUpdate = 0; // Use cache at initial startup.
    configGetInt(configGetByType(CONFIG_OPL), "hdd_frames_delay", &hddGameList.delay);
    hddGameList.enabled = hddInitModules() == 0;
}

item_list_t *hddGetObject(int initOnly)
{
    if (initOnly && !hddGameList.enabled)
        return NULL;
    return &hddGameList;
}

static int hddNeedsUpdate(item_list_t *itemList)
{ /* Auto refresh is disabled by setting HDD_MODE_UPDATE_DELAY to MENU_UPD_DELAY_NOUPDATE, within hddsupport.h.
       Hence any update request would be issued by the user, which should be taken as an explicit request to re-scan the HDD. */
    return 1;
}

static int hddUpdateGameList(item_list_t *itemList)
{
    hdl_games_list_t hddGamesNew;
    int ret;

    if (hddForceUpdate || ((ret = hddLoadGameListCache(&hddGames)) != 0)) {
        hddGamesNew.count = 0;
        hddGamesNew.games = NULL;
        ret = hddGetHDLGamelist(&hddGamesNew);
        if (ret == 0) {
            hddUpdateGameListCache(&hddGames, &hddGamesNew);
            hddFreeHDLGamelist(&hddGames);
            hddGames = hddGamesNew;
        }
    }

    sbReadList(&hddIsoGames, gHDDPrefix, &hddIsoFileSize, &hddIsoGameCount);

    hddForceUpdate = 1; // Subsequent refresh operations will cause the HDD to be scanned.
    return (ret == 0 ? hddGames.count + hddIsoGameCount : 0);
}

static int hddGetGameCount(item_list_t *itemList)
{
    return hddGames.count + hddIsoGameCount;
}

static void *hddGetGame(item_list_t *itemList, int id)
{
    if (id < hddGames.count)
        return (void *)&hddGames.games[id];
    else
        return (void *)&hddIsoGames[id - hddGames.count];
}

static char *hddGetGameName(item_list_t *itemList, int id)
{
    if (id < hddGames.count)
        return hddGames.games[id].name;
    else
        return hddIsoGames[id - hddGames.count].name;
}

static int hddGetGameNameLength(item_list_t *itemList, int id)
{
    return id < hddGames.count ? HDL_GAME_NAME_MAX + 1 : ISO_GAME_NAME_MAX + 1;
}

static char *hddGetGameStartup(item_list_t *itemList, int id)
{
    if (id < hddGames.count)
        return hddGames.games[id].startup;
    else
        return hddIsoGames[id - hddGames.count].startup;
}

static void hddDeleteGame(item_list_t *itemList, int id)
{
    if (id < hddGames.count) {
        hddDeleteHDLGame(&hddGames.games[id]);
        hddForceUpdate = 1;
    }
}

static void hddRenameGame(item_list_t *itemList, int id, char *newName)
{
    if (id < hddGames.count) {
        hdl_game_info_t *game = &hddGames.games[id];
        strcpy(game->name, newName);
        hddSetHDLGameInfo(&hddGames.games[id]);
        hddForceUpdate = 1;
    }
}

int hddPreparePfsVMC(config_set_t *configSet, int showErrorDialogs)
{
    apa_sub_t parts[APA_MAXSUB + 1];
    hdd_vmc_infos_t vmcInfos[2];
    char vmc_name[32];
    int i, vmc_id, partitionCount;
    int size_mcemu_irx = 0;

    // 未配置 VMC 时不要额外读取 APA 分区表。
    configGetVMC(configSet, vmc_name, sizeof(vmc_name), 0);
    if (!vmc_name[0]) {
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), 1);
        if (!vmc_name[0])
            return 0;
    }

    partitionCount = hddGetPartitionInfo(gOPLPart, parts);

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        char vmc_path[256];
        int blockCount = 0;
        int have_error = 0;
        pfs_blockinfo_t blocks[11];
        vmc_superblock_t vmc_superblock;

        // 每个插槽都使用全新的映射，避免继承另一个插槽的残留块。
        memset(&vmcInfos[vmc_id], 0, sizeof(vmcInfos[vmc_id]));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);

        if (vmc_name[0]) {
            have_error = 1;

            if (partitionCount > 0 && partitionCount <= 5 && sysCheckVMC(gHDDPrefix, "/", vmc_name, 0, &vmc_superblock) > 0) {
                for (i = 0; i < partitionCount; i++) {
                    vmcInfos[vmc_id].parts[i].start = parts[i].start;
                    vmcInfos[vmc_id].parts[i].length = parts[i].length;
                }

                vmcInfos[vmc_id].flags = vmc_superblock.mc_flag & 0xFF;
                vmcInfos[vmc_id].flags |= 0x100;
                vmcInfos[vmc_id].specs.page_size = vmc_superblock.page_size;
                vmcInfos[vmc_id].specs.block_size = vmc_superblock.pages_per_block;
                vmcInfos[vmc_id].specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                // 写入 VMC 前必须取得完整的 PFS 块链，防止写错物理扇区。
                snprintf(vmc_path, sizeof(vmc_path), "%sVMC/%s.bin", gHDDPrefix, vmc_name);
                blockCount = hddGetFileBlockInfo(vmc_path, parts, blocks, 11);
                if (blockCount > 1) {
                    have_error = 0;
                    for (i = 0; i < blockCount - 1; i++) {
                        if (blocks[i + 1].subpart >= partitionCount) {
                            have_error = 2;
                            break;
                        }

                        vmcInfos[vmc_id].blocks[i].number = blocks[i + 1].number;
                        vmcInfos[vmc_id].blocks[i].subpart = blocks[i + 1].subpart;
                        vmcInfos[vmc_id].blocks[i].count = blocks[i + 1].count;
                    }

                    if (!have_error)
                        vmcInfos[vmc_id].active = 1;
                } else {
                    have_error = 2;
                }
            }

            if (have_error) {
                if (showErrorDialogs) {
                    char error[256];

                    if (have_error == 2)
                        snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTED_CONTINUE), vmc_name, vmc_id + 1);
                    else
                        snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, vmc_id + 1);

                    if (!guiMsgBox(error, 1, NULL))
                        return -1;
                } else {
                    LOG("VMC error\n");
                }
            }
        }

    }

    // 两个插槽全部通过检查后再修改嵌入模块，取消启动时仍可重新尝试。
    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        for (i = 0; i < size_pfs_bdm_mcemu_irx / sizeof(u32); i++) {
            if (((u32 *)&pfs_bdm_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (vmcInfos[vmc_id].active)
                    size_mcemu_irx = size_pfs_bdm_mcemu_irx;
                memcpy(&((u32 *)&pfs_bdm_mcemu_irx)[i], &vmcInfos[vmc_id], sizeof(vmcInfos[vmc_id]));
                break;
            }
        }
    }

    return size_mcemu_irx;
}

void hddLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, size_irx = 0;
    int EnablePS2Logo = 0;
    int result;
    void *irx = NULL;
    char filename[32];
    hdl_game_info_t *game;
    struct cdvdman_settings_bdm *settings;
    hdl_apa_header *hdl_header;
    struct cdvdman_fragfile *iso_frag;

    if (id >= hddGames.count) {
        item_list_t bdmItemList;
        bdm_device_data_t bdmDeviceData;

        memset(&bdmItemList, 0, sizeof(bdmItemList));
        memset(&bdmDeviceData, 0, sizeof(bdmDeviceData));
        bdmItemList.mode = HDD_MODE;
        bdmItemList.priv = &bdmDeviceData;
        bdmDeviceData.bdmGames = hddIsoGames;
        snprintf(bdmDeviceData.bdmPrefix, sizeof(bdmDeviceData.bdmPrefix), "%s", gHDDPrefix);
        strcpy(bdmDeviceData.bdmDriver, "ata");
        bdmDeviceData.bdmDeviceType = BDM_TYPE_ATA;
        bdmDeviceData.massDeviceIndex = 0;
        bdmResolveLBA_UDMA(&bdmDeviceData);
        bdmLaunchGame(&bdmItemList, id - hddGames.count, configSet);
        return;
    }

    if (gAutoLaunchGame == NULL)
        game = &hddGames.games[id];
    else
        game = gAutoLaunchGame;

    int size_mcemu_irx = hddPreparePfsVMC(configSet, gAutoLaunchGame == NULL);
    if (size_mcemu_irx < 0)
        return;

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    char gid[5];
    configGetDiscIDBinary(configSet, gid);

    // 默认为UDMA 4，与官方一致
    int dmaType = 0x40, dmaMode = 7, compatMode = 0;
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatMode);
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

    if (hddReadSectors(game->start_sector, 2, IOBuffer) != 0) {
        guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
        return;
    }

    hdl_header = (hdl_apa_header *)IOBuffer;
    if (hdl_header->num_partitions <= 0 || hdl_header->num_partitions > BDM_MAX_FRAGS) {
        guiMsgBox(_l(_STR_ERR_FRAGMENTED), 0, NULL);
        return;
    }

    size_irx = size_bdm_ata_cdvdman_irx;
    irx = &bdm_ata_cdvdman_irx;

    sbPrepare(NULL, configSet, size_irx, irx, &i);

    if ((result = sbLoadCheats(gHDDPrefix, game->startup)) < 0) {
        if (gAutoLaunchGame == NULL) {
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

    settings = (struct cdvdman_settings_bdm *)((u8 *)irx + i);

    memset(&settings->frags[0], 0, sizeof(bd_fragment_t) * BDM_MAX_FRAGS);
    iso_frag = &settings->fragfile[0];
    iso_frag->frag_start = 0;
    iso_frag->frag_count = hdl_header->num_partitions;
    for (i = 0; i < hdl_header->num_partitions; i++) {
        settings->frags[i].sector = hdl_header->part_specs[i].data_start;
        settings->frags[i].count = hdl_header->part_specs[i].part_size >> 9;
    }
    settings->bdDeviceId = 0;
    settings->hddIsLBA48 = hddIs48bit();
    settings->fragsAre512ByteSectors = 1;
    settings->common.NumParts = 1;
    settings->common.media = hdl_header->discType;

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    if (gPS2Logo)
        EnablePS2Logo = CheckPS2Logo(0, game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET);

    // Check for ZSO to correctly adjust layer1 start
    settings->common.layer1_start = hdl_header->layer1_start;
    hddReadSectors(game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET, 1, IOBuffer);
    if (*(u32 *)IOBuffer == ZSO_MAGIC) {
        probed_fd = 0;
        probed_lba = game->start_sector + OPL_HDD_MODE_PS2LOGO_OFFSET;
        ziso_init((ZISO_header *)IOBuffer, *(u32 *)((u8 *)IOBuffer + sizeof(ZISO_header)));
        ziso_read_sector(IOBuffer, 16, 1);
        u32 maxLBA = *(u32 *)(IOBuffer + 80);
        if (maxLBA > 0 && maxLBA < ziso_total_block) {   // dual layer check
            settings->common.layer1_start = maxLBA - 16; // adjust second layer start
        }
    }

    if (gAutoLaunchGame == NULL)
        deinit(NO_EXCEPTION, HDD_MODE); // CAREFUL: deinit will call hddCleanUp, so hddGames/game will be freed
    else {
        miniDeinit(configSet);

        free(gAutoLaunchGame);
        gAutoLaunchGame = NULL;

        fileXioUmount("pfs0:");
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);
    }

    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_ATAD;

    // adjust ZSO cache
    settings->common.zso_cache = hddCacheSize;
    sysLaunchLoaderElf(filename, "HDD_MODE", size_irx, irx, size_mcemu_irx, pfs_bdm_mcemu_irx, EnablePS2Logo, compatMode);
}

static config_set_t *hddGetConfig(item_list_t *itemList, int id)
{
    if (id >= hddGames.count) {
        return sbPopulateConfig(&hddIsoGames[id - hddGames.count], gHDDPrefix, "/");
    }

    char path[256];
    hdl_game_info_t *game = &hddGames.games[id];

    snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, game->startup);
    config_set_t *config = configAlloc(0, NULL, path);
    configRead(config); // Does not matter if the config file exists or not.

    configSetStr(config, CONFIG_ITEM_NAME, game->name);
    configSetInt(config, CONFIG_ITEM_SIZE, game->total_size_in_kb >> 10);
    configSetStr(config, CONFIG_ITEM_FORMAT, "HDL");
    configSetStr(config, CONFIG_ITEM_MEDIA, game->disctype == SCECdPS2CD ? "CD" : "DVD");
    configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);

    return config;
}

static int hddGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    if (!value)
        return ERR_BAD_FILE;

    char path[256];
    if (isRelative) {
        if (artUseBuckets_APA) {
            int len = strlen(value);
            if (len >= 4 && (value[len - 1] == 'F' || value[len - 1] == 'f'))
                snprintf(path, sizeof(path), "%sART2/APPS/%s/%s_%s", gHDDPrefix, value, value, suffix);
            else
                snprintf(path, sizeof(path), "%sART2/GAMES/%s/%s_%s", gHDDPrefix, value, value, suffix);
        } else
            snprintf(path, sizeof(path), "%s%s/%s_%s", gHDDPrefix, folder, value, suffix);
    } else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);

    return texDiscoverLoad(resultTex, path, -1);
}

static int hddGetTextId(item_list_t *itemList)
{
    return _STR_HDD_GAMES;
}

static int hddGetIconId(item_list_t *itemList)
{
    return HDD_ICON;
}

// This may be called, even if hddInit() was not.
static void hddCleanUp(item_list_t *itemList, int exception)
{
    LOG("HDDSUPPORT CleanUp\n");

    if (hddGameList.enabled) {
        hddFreeHDLGamelist(&hddGames);
        free(hddIsoGames);
        hddIsoGames = NULL;
        hddIsoGameCount = 0;

        if ((exception & UNMOUNT_EXCEPTION) == 0)
            fileXioUmount(hddPrefix);
    } else if (hddConfigSource && (exception & UNMOUNT_EXCEPTION) == 0) {
        // 配置读取流程挂载了pfs0:，但没有启用HDD游戏列表。
        fileXioUmount(hddPrefix);
    }

    // UI may have loaded modules outside of HDD mode, so deinitialize regardless of the enabled status.
    if (hddSupportModulesLoaded) {
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);

        hddSupportModulesLoaded = 0;
    }
}

static int hddCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    return sysCheckVMC(gHDDPrefix, "/", name, createSize, NULL);
}

// This may be called, even if hddInit() was not.
static void hddShutdown(item_list_t *itemList)
{
    LOG("HDDSUPPORT Shutdown\n");

    if (hddGameList.enabled) {
        hddFreeHDLGamelist(&hddGames);
        free(hddIsoGames);
        hddIsoGames = NULL;
        hddIsoGameCount = 0;
        fileXioUmount(hddPrefix);
    } else if (hddConfigSource) {
        // 配置读取流程挂载了pfs0:，但没有启用HDD游戏列表。
        fileXioUmount(hddPrefix);
    }

    // UI may have loaded modules outside of HDD mode, so deinitialize regardless of the enabled status.
    if (hddSupportModulesLoaded) {
        /* Close all files */
        fileXioDevctl("pfs:", PDIOC_CLOSEALL, NULL, 0, NULL, 0);

        hddSupportModulesLoaded = 0;
    }

    if (hddModulesLoadCount > 0) {
        hddModulesLoadCount -= 1;
        if (hddModulesLoadCount == 0) {
            // DEV9 will remain active if ETH is in use, so put the HDD in IDLE state.
            // The HDD should still enter standby state after 21 minutes & 15 seconds, as per the ATAD defaults.
            hddSetIdleImmediate();
        }

        // Only shut down dev9 from here, if it was initialized from here before.
        sysShutdownDev9();
    }
}

static int hddLoadGameListCache(hdl_games_list_t *cache)
{
    if (!gHDDGameListCache)
        return 1;

    char filename[256];
    FILE *file;
    hdl_game_info_t *games;
    int result, size, count;

    hddFreeHDLGamelist(cache);

    sprintf(filename, gTxtRename ? "%stxtCache.bin" : "%sCache.bin", gHDDPrefix);
    file = fopen(filename, "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);

        count = size / sizeof(hdl_game_info_t);
        if (count > 0) {
            games = memalign(64, count * sizeof(hdl_game_info_t));
            if (games != NULL) {
                if (fread(games, sizeof(hdl_game_info_t), count, file) == count) {
                    cache->count = count;
                    cache->games = games;
                    LOG("hddLoadGameListCache: %d games loaded.\n", count);
                    result = 0;
                } else {
                    LOG("hddLoadGameListCache: I/O error.\n");
                    free(games);
                    result = EIO;
                }
            } else {
                LOG("hddLoadGameListCache: failed to allocate memory.\n");
                result = ENOMEM;
            }
        } else {
            result = -1; // Empty file
        }

        fclose(file);
    } else {
        result = ENOENT;
    }

    return result;
}

static int hddUpdateGameListCache(hdl_games_list_t *cache, hdl_games_list_t *game_list)
{
    if (!gHDDGameListCache)
        return 1;

    char filename[256];
    FILE *file;
    int result, i, j, modified;

    if (cache->count > 0) {
        modified = 0;
        for (i = 0; i < game_list->count; i++) {
            for (j = 0; j < cache->count; j++) {
                if (strncmp(game_list->games[i].partition_name, cache->games[j].partition_name, APA_IDMAX + 1) == 0) {
                    // 检查txt映射名是否有修改
                    if (gTxtRename) {
                        if (strncmp(game_list->games[i].transName, cache->games[j].transName, 160) != 0)
                            modified = 1;
                    }
                    break;
                }
            }

            if (modified)
                break;

            if (j == cache->count) {
                LOG("hddUpdateGameListCache: game added.\n");
                modified = 1;
                break;
            }
        }

        if ((!modified) && (game_list->count != cache->count)) {
            LOG("hddUpdateGameListCache: game removed.\n");
            modified = 1;
        }
    } else {
        modified = (game_list->count > 0) ? 1 : 0;
    }

    if (!modified)
        return 0;
    LOG("hddUpdateGameListCache: caching new game list.\n");

    sprintf(filename, gTxtRename ? "%stxtCache.bin" : "%sCache.bin", gHDDPrefix);
    if (game_list->count > 0) {
        file = fopen(filename, "wb");
        if (file != NULL) {
            result = (fwrite(game_list->games, sizeof(hdl_game_info_t), game_list->count, file) == game_list->count) ? 0 : EIO;
            fclose(file);
        } else {
            result = EIO;
        }
    } else {
        // Last game deleted.
        remove(filename);
        result = 0;
    }

    return result;
}

static char *hddGetPrefix(item_list_t *itemList)
{
    return gHDDPrefix;
}

static item_list_t hddGameList = {
    HDD_MODE, 0, 0, MODE_FLAG_COMPAT_DMA, MENU_MIN_INACTIVE_FRAMES, HDD_MODE_UPDATE_DELAY, NULL, NULL, &hddGetTextId, &hddGetPrefix, &hddInit, &hddNeedsUpdate, &hddUpdateGameList,
    &hddGetGameCount, &hddGetGame, &hddGetGameName, &hddGetGameNameLength, &hddGetGameStartup, &hddDeleteGame, &hddRenameGame,
    &hddLaunchGame, &hddGetConfig, &hddGetImage, &hddCleanUp, &hddShutdown, &hddCheckVMC, &hddGetIconId};
