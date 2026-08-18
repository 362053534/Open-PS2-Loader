#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/appsupport.h"
#include "include/themes.h"
#include "include/system.h"
#include "include/ioman.h"
#include "include/util.h"
#include "include/textures.h"
#include "include/extern_irx.h"

#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/hddsupport.h"

#include <elf-loader.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

static int appForceUpdate = 1;
static int appItemCount = 0;
static int appPOPSPrepareStatus;
static int appPOPSPrepareResult;
static int appPOPSPrepareID;

static config_set_t *configApps;
static app_info_t *appsList;

#define APP_POPS_PREPARE_DRIVERS_FAILED  0x01

struct app_info_linked
{
    struct app_info_linked *next;
    app_info_t app;
};

// forward declaration
static item_list_t appItemList;

static void appFreeList(void);
static void appFreeLegacyConfig(void);

#define POPS_BDM_ELF_PREFIX "XX."
#define POPS_SMB_ELF_PREFIX "SB."

#define POPS_BOOT_MAILBOX_MAGIC    0x53504F50
#define POPS_BOOT_MAILBOX_VERSION  1
#define POPS_BOOT_MAILBOX_PATH_MAX 256
#define POPS_BOOT_VCD_PREFIX       "0:/POPS/"

typedef struct
{
    u32 magic;
    u32 version;
    s32 deviceType;
    char vcdPath[POPS_BOOT_MAILBOX_PATH_MAX];
    u32 checksum;
} pops_boot_mailbox_t;

typedef char pops_boot_mailbox_size_must_be_272[(sizeof(pops_boot_mailbox_t) == 272) ? 1 : -1];

static int appIsPOPSLauncher(const app_info_t *app)
{
    return app->popstarter || strstr(app->path, "APPS") == NULL;
}

static int appGetPOPSBDMDeviceType(const app_info_t *app)
{
    int mode = oplPath2Mode(app->path);

    if (mode >= BDM_MODE && mode <= BDM_MODE4)
        return bdmGetDeviceType(mode);

    return BDM_TYPE_UNKNOWN;
}

static u32 appPOPSBootMailboxChecksum(const pops_boot_mailbox_t *mailbox)
{
    const u8 *data = (const u8 *)mailbox;
    u32 checksum = 2166136261u;
    unsigned int i;

    for (i = 0; i < sizeof(*mailbox) - sizeof(mailbox->checksum); i++) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }

    return checksum;
}

static int appBuildPOPSBootMailbox(const app_info_t *app, pops_boot_mailbox_t *mailbox)
{
    int deviceType;
    int mode;

    memset(mailbox, 0, sizeof(*mailbox));

    mode = oplPath2Mode(app->path);
    deviceType = appGetPOPSBDMDeviceType(app);
    if (deviceType == BDM_TYPE_UNKNOWN) {
        /* BDM模式拿不到类型属于来源损坏，交给驱动开放式回退。 */
        if (mode >= BDM_MODE && mode <= BDM_MODE4)
            return -1;

        /* 非BDM模式的未知类型明确表示无需启动BDM硬件。 */
        mailbox->magic = POPS_BOOT_MAILBOX_MAGIC;
        mailbox->version = POPS_BOOT_MAILBOX_VERSION;
        mailbox->deviceType = deviceType;
        mailbox->checksum = appPOPSBootMailboxChecksum(mailbox);
        return 0;
    }

    if (deviceType < BDM_TYPE_USB || deviceType > BDM_TYPE_ATA)
        return -1;

    if (app->vcdName[0] == '\0')
        return -1;

    if (snprintf(mailbox->vcdPath, sizeof(mailbox->vcdPath), "%s%s",
                 POPS_BOOT_VCD_PREFIX, app->vcdName) >= (int)sizeof(mailbox->vcdPath))
        return -1;

    mailbox->magic = POPS_BOOT_MAILBOX_MAGIC;
    mailbox->version = POPS_BOOT_MAILBOX_VERSION;
    mailbox->deviceType = deviceType;
    mailbox->checksum = appPOPSBootMailboxChecksum(mailbox);
    return 0;
}

static struct config_value_t *appGetConfigValue(int id)
{
    struct config_value_t *cur = configApps->head;

    while (id--) {
        cur = cur->next;
    }

    return cur;
}

static char *appGetELFName(char *name)
{
    // Looking for the ELF name
    char *pos = strrchr(name, '/');
    if (!pos)
        pos = strrchr(name, ':');
    if (pos) {
        return pos + 1;
    }

    return name;
}

static float appGetELFSize(char *path)
{
    int fd, size;
    float bytesInMiB = 1048576.0f;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("Failed to open APP %s\n", path);
        return 0.0f;
    }

    size = getFileSize(fd);
    close(fd);

    // Return size in MiB
    return (size / bytesInMiB);
}

static char *appGetBoot(char *device, int max, char *path)
{
    char *pos, *filenamesep;

    // Looking for the boot device & filename from the path
    pos = strrchr(path, ':');
    if (pos != NULL) {
        int len = (int)(pos + 1 - path);
        if (len + 1 > max)
            len = max - 1;
        strncpy(device, path, len);
        device[len] = '\0';
    }

    filenamesep = strchr(path, '/');
    if (filenamesep != NULL)
        return filenamesep + 1;

    if (pos) {
        return pos + 1;
    }

    return path;
}

void appInit(item_list_t *itemList)
{
    LOG("APPSUPPORT Init\n");
    appForceUpdate = !(gAutoDetectPS1Apps && gAPPStartMode == START_MODE_AUTO);
    configGetInt(configGetByType(CONFIG_OPL), "app_frames_delay", &appItemList.delay);
    appFreeLegacyConfig();
    if (!gAutoDetectPS1Apps)
        configApps = oplGetLegacyAppsConfig();
    appsList = NULL;
    appItemList.enabled = 1;
}

item_list_t *appGetObject(int initOnly)
{
    if (initOnly && !appItemList.enabled)
        return NULL;
    return &appItemList;
}

void appForceRefresh(void)
{
    appForceUpdate = 1;
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &appItemList.mode);
}

static int appNeedsUpdate(item_list_t *itemList)
{
    int update;

    update = 0;
    if (appForceUpdate) {
        appForceUpdate = 0;
        update = 1;
    }
    if (oplShouldAppsUpdate())
        update = 1;

    if (update) {
        appFreeLegacyConfig();
        if (!gAutoDetectPS1Apps)
            configApps = oplGetLegacyAppsConfig();
    }

    return update;
}

static int addAppsLegacyList(struct app_info_linked **appsLinkedList)
{
    struct config_value_t *cur;
    struct app_info_linked *app;
    int count;

    configClear(configApps);
    configRead(configApps);

    count = 0;
    cur = configApps->head;
    while (cur != NULL) {
        if (*appsLinkedList == NULL) {
            *appsLinkedList = malloc(sizeof(struct app_info_linked));
            app = *appsLinkedList;
            app->next = NULL;
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            break;
        }

        strncpy(app->app.title, cur->key, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';

        // Split the boot filename from the path.
        const char *elfname = appGetELFName(cur->val);
        if (elfname != cur->val) {
            strncpy(app->app.boot, elfname, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';

            int pathlen = (int)(elfname - cur->val) - 1;
            if (cur->val[pathlen] == ':') // Discard only '/'.
                pathlen++;
            if (pathlen > APP_PATH_MAX)
                pathlen = APP_PATH_MAX;
            strncpy(app->app.path, cur->val, pathlen);
            app->app.path[pathlen] = '\0';
        } else {
            // Cannot split boot filename from the path, somehow.
            strncpy(app->app.boot, cur->val, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';
            strncpy(app->app.path, cur->val, APP_PATH_MAX + 1);
            app->app.path[APP_BOOT_MAX] = '\0';
        }
        strncpy(app->app.startup, app->app.boot, APP_BOOT_MAX + 1);
        app->app.startup[APP_BOOT_MAX] = '\0';
        app->app.vcdName[0] = '\0';

        app->app.legacy = 1;
        app->app.generated = 0;
        app->app.popstarter = 0;
        count++;
        cur = cur->next;
    }

    return count;
}

static int appScanCallback(const char *path, config_set_t *appConfig, void *arg)
{
    struct app_info_linked **appsLinkedList = (struct app_info_linked **)arg;
    struct app_info_linked *app;
    const char *title, *boot, *argv1;

    if (configGetStr(appConfig, APP_CONFIG_TITLE, &title) != 0 && configGetStr(appConfig, APP_CONFIG_BOOT, &boot) != 0) {
        if (*appsLinkedList == NULL) {
            *appsLinkedList = malloc(sizeof(struct app_info_linked));
            app = *appsLinkedList;
            app->next = NULL;
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            return -1;
        }

        strncpy(app->app.title, title, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';
        strncpy(app->app.boot, boot, APP_BOOT_MAX + 1);
        app->app.boot[APP_BOOT_MAX] = '\0';
        strncpy(app->app.startup, app->app.boot, APP_BOOT_MAX + 1);
        app->app.startup[APP_BOOT_MAX] = '\0';
        strncpy(app->app.path, path, APP_PATH_MAX + 1);
        app->app.path[APP_PATH_MAX] = '\0';
        app->app.vcdName[0] = '\0';
        if (configGetStr(appConfig, APP_CONFIG_ARGV1, &argv1) != 0) {
            strncpy(app->app.argv1, argv1, APP_ARGV1_MAX + 1);
            app->app.argv1[APP_ARGV1_MAX] = '\0';
        } else
            app->app.argv1[0] = '\0';
        app->app.legacy = 0;
        app->app.generated = 0;
        app->app.popstarter = 0;
        return 0;
    } else {
        LOG("APPSUPPORT item has no boot/title.\n");
        return 1;
    }

    return -1;
}

static int appIsNumberedVcdName(const char *vcdName, int nameLength)
{
    // 对齐旧ISO：SCUS_XXX.XX.显示名.VCD（编号11字符 + '.' + 标题 + .VCD）
    return nameLength >= 17 && vcdName[4] == '_' && vcdName[8] == '.' && vcdName[11] == '.' &&
           strcasecmp(&vcdName[nameLength - 4], ".VCD") == 0;
}

static int appAddPOPSItem(const char *path, const char *vcdName, void *arg, const char *elfPrefix)
{
    struct app_info_linked **appsLinkedList = (struct app_info_linked **)arg;
    struct app_info_linked *app;
    char title[APP_TITLE_MAX + 1];
    char boot[APP_BOOT_MAX + 1];
    char startup[APP_BOOT_MAX + 1];
    int nameLength, titleLength, pathLength;

    nameLength = strlen(vcdName);
    if (appIsNumberedVcdName(vcdName, nameLength)) {
        // 编号前缀：boot芯=前11字符，显示title=其后至.VCD前
        titleLength = nameLength - 16; // 11编号 + 1点 + 4(.VCD)
        if (titleLength <= 0 || titleLength > APP_TITLE_MAX) {
            LOG("APPSUPPORT POPS numbered VCD title is invalid: %s\n", vcdName);
            return 1;
        }

        memcpy(startup, vcdName, 11);
        startup[11] = '\0';
        memcpy(title, &vcdName[12], titleLength);
        title[titleLength] = '\0';

        // boot = {elfPrefix}{boot芯}.{显示title}.ELF；封面下/ART KEY = boot芯
        if (snprintf(boot, sizeof(boot), "%s%s.%s.ELF", elfPrefix, startup, title) >= sizeof(boot)) {
            LOG("APPSUPPORT POPS ELF filename is too long: %s\n", vcdName);
            return 1;
        }
    } else {
        titleLength = nameLength - 4; // Remove the .VCD extension.
        if (titleLength <= 0 || titleLength > APP_TITLE_MAX) {
            LOG("APPSUPPORT POPS VCD filename is too long: %s\n", vcdName);
            return 1;
        }

        memcpy(title, vcdName, titleLength);
        title[titleLength] = '\0';

        if (snprintf(boot, sizeof(boot), "%s%s.ELF", elfPrefix, title) >= sizeof(boot)) {
            LOG("APPSUPPORT POPS ELF filename is too long: %s\n", vcdName);
            return 1;
        }
        // 无前缀：封面下方/ART KEY 与boot一致
        strncpy(startup, boot, sizeof(startup));
        startup[sizeof(startup) - 1] = '\0';
    }

    if (*appsLinkedList == NULL) {
        *appsLinkedList = malloc(sizeof(struct app_info_linked));
        app = *appsLinkedList;
        app->next = NULL;
    } else {
        app = malloc(sizeof(struct app_info_linked));
        if (app != NULL) {
            app->next = *appsLinkedList;
            *appsLinkedList = app;
        }
    }

    if (app == NULL) {
        LOG("APPSUPPORT unable to allocate memory.\n");
        return -1;
    }

    strcpy(app->app.title, title);
    strcpy(app->app.boot, boot);
    strcpy(app->app.vcdName, vcdName);
    strcpy(app->app.startup, startup);
    strcpy(app->app.path, path);
    pathLength = strlen(app->app.path);
    if (pathLength > 0 && app->app.path[pathLength - 1] == '/')
        app->app.path[pathLength - 1] = '\0';
    app->app.argv1[0] = '\0';
    app->app.legacy = 0;
    app->app.generated = 1;
    app->app.popstarter = 1;

    return 0;
}

static int appScanBDMPOPSCallback(const char *path, const char *vcdName, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, POPS_BDM_ELF_PREFIX);
}

static int appScanSMBPOPSCallback(const char *path, const char *vcdName, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, POPS_SMB_ELF_PREFIX);
}

static int appScanHDDPOPSCallback(const char *path, const char *vcdName, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, "");
}

static int appScanELFCallback(const char *path, const char *elfName, void *arg)
{
    struct app_info_linked **appsLinkedList = (struct app_info_linked **)arg;
    struct app_info_linked *app;
    char title[APP_TITLE_MAX + 1];
    int titleLength;

    if (strlen(elfName) > APP_BOOT_MAX) {
        LOG("APPSUPPORT APP ELF filename is too long: %s\n", elfName);
        return 1;
    }

    if (*appsLinkedList == NULL) {
        *appsLinkedList = malloc(sizeof(struct app_info_linked));
        app = *appsLinkedList;
        app->next = NULL;
    } else {
        app = malloc(sizeof(struct app_info_linked));
        if (app != NULL) {
            app->next = *appsLinkedList;
            *appsLinkedList = app;
        }
    }

    if (app == NULL) {
        LOG("APPSUPPORT unable to allocate memory.\n");
        return -1;
    }

    titleLength = strlen(elfName) - 4; // Remove the .ELF extension.
    memcpy(title, elfName, titleLength);
    title[titleLength] = '\0';
    strcpy(app->app.title, title);
    strcpy(app->app.boot, elfName);
    app->app.vcdName[0] = '\0';
    strcpy(app->app.startup, elfName);
    strcpy(app->app.path, path);
    app->app.argv1[0] = '\0';
    app->app.legacy = 0;
    app->app.generated = 1;
    app->app.popstarter = 0;

    return 0;
}

static int appUpdateItemList(item_list_t *itemList)
{
    struct app_info_linked *appsLinkedList, *appNext;

    appFreeList();

    appsLinkedList = NULL;

    if (gAutoDetectPS1Apps) {
        // Add ELF files found in APPS folders on memory cards.
        appItemCount += oplScanMCApps(&appScanELFCallback, &appsLinkedList);

        // Add ELF files found in APPS folders on BDM devices.
        appItemCount += oplScanBDMApps(&appScanELFCallback, &appsLinkedList);

        // Add ELF files found in the SMB APPS folder.
        appItemCount += oplScanSMBApps(&appScanELFCallback, &appsLinkedList);

        // Add ELF files found in the APA HDD APPS folder.
        appItemCount += oplScanHDDApps(&appScanELFCallback, &appsLinkedList);

        // Add a POPSTARTER entry for every VCD found on BDM devices.
        appItemCount += oplScanBDMPOPS(&appScanBDMPOPSCallback, &appsLinkedList);

        // Add a POPSTARTER entry for every VCD found in the SMB POPS folder.
        appItemCount += oplScanSMBPOPS(&appScanSMBPOPSCallback, &appsLinkedList);

        // Add a POPSTARTER entry for every VCD found in the APA HDD POPS partition.
        appItemCount += oplScanHDDPOPS(&appScanHDDPOPSCallback, &appsLinkedList);
    } else {
        // Get legacy apps list first, so it is possible to use appGetConfigValue(id).
        appItemCount += addAppsLegacyList(&appsLinkedList);

        // Scan title.cfg files on devices.
        appItemCount += oplScanApps(&appScanCallback, &appsLinkedList);
    }

    // Generate apps list
    if (appItemCount > 0) {
        appsList = malloc(appItemCount * sizeof(app_info_t));

        if (appsList != NULL) {
            int i;
            for (i = 0; appsLinkedList != NULL; i++) { // appsLinkedList contains items in reverse order.
                memcpy(&appsList[appItemCount - i - 1], &appsLinkedList->app, sizeof(app_info_t));

                appNext = appsLinkedList->next;
                free(appsLinkedList);
                appsLinkedList = appNext;
            }
        } else {
            LOG("APPSUPPORT unable to allocate memory.\n");
            appItemCount = 0;
        }
    }

    LOG("APPSUPPORT %d apps loaded\n", appItemCount);

    return appItemCount;
}

static void appFreeList(void)
{
    if (appsList != NULL) {
        free(appsList);
        appsList = NULL;
    }

    appItemCount = 0;
}

static void appFreeLegacyConfig(void)
{
    if (configApps != NULL) {
        configFree(configApps);
        configApps = NULL;
    }
}

static int appGetItemCount(item_list_t *itemList)
{
    return appItemCount;
}

static char *appGetItemName(item_list_t *itemList, int id)
{
    return appsList[id].title;
}

static int appGetItemNameLength(item_list_t *itemList, int id)
{
    return CONFIG_KEY_NAME_LEN;
}

/* appGetItemStartup()：封面下方文字，同时作为 ART KEY。 */
static char *appGetItemStartup(item_list_t *itemList, int id)
{
    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        return appGetELFName(cur->val);
    } else {
        return appsList[id].startup;
    }
}

static void appDeleteItem(item_list_t *itemList, int id)
{
    if (appsList[id].generated)
        return;

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        unlink(cur->val);
        cur->key[0] = '\0';
        configApps->modified = 1;
        configWrite(configApps);
    } else {
        sysDeleteFolder(appsList[id].path);
    }

    appForceUpdate = 1;
}

static void appRenameItem(item_list_t *itemList, int id, char *newName)
{
    char value[256];

    if (appsList[id].generated)
        return;

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);

        strncpy(value, cur->val, sizeof(value));
        configRemoveKey(configApps, cur->key);
        configSetStr(configApps, newName, value);
        configWrite(configApps);
    } else {
        config_set_t *appConfig;

        snprintf(value, sizeof(value), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);

        appConfig = configAlloc(0, NULL, value);
        if (appConfig != NULL) {
            configRead(appConfig);
            configSetStr(appConfig, APP_CONFIG_TITLE, newName);
            configWrite(appConfig);

            configFree(appConfig);
        }
    }

    appForceUpdate = 1;
}

static void appPreparePOPSLauncher(void)
{
    appPOPSPrepareResult = 0;

    if (gAutoDetectPS1Apps && installPopstarterDrivers(oplPath2Mode(appsList[appPOPSPrepareID].path), appGetPOPSBDMDeviceType(&appsList[appPOPSPrepareID])) < 0)
        appPOPSPrepareResult |= APP_POPS_PREPARE_DRIVERS_FAILED;

    appPOPSPrepareStatus = 0;
}

static void appLaunchItem(item_list_t *itemList, int id, config_set_t *configSet)
{
    int fd;
    char filename[256];
    const char *argv1;

    if (gAutoDetectPS1Apps && appIsPOPSLauncher(&appsList[id])) {
        char cheatPath[APP_PATH_MAX + 14];
        char popstarterArg[APP_BOOT_MAX + 5];
        char *argv[1];
        pops_boot_mailbox_t bootMailbox;
        const char *cheats;
        int mode;

        // 初次启动时写入玩家选择的 POPStarter 分辨率配置。
        mode = oplPath2Mode(appsList[id].path);
        if (mode == ETH_MODE && gETHPrefix[0]) {
            guiMsgBox("POPS不支持SMB前缀路径！", 0, NULL);
            return;
        }

        if (mode == HDD_MODE) {
            fileXioUmount(OPL_HDD_POPS_MOUNTPOINT);
            if (fileXioMount(OPL_HDD_POPS_MOUNTPOINT, "hdd0:__common", FIO_MT_RDWR) == 0) {
                char missingFiles[32];

                missingFiles[0] = '\0';
                fd = openFile("pfs0:POPS/IOPRP252.IMG", O_RDONLY);
                if (fd >= 0)
                    close(fd);
                else
                    strcpy(missingFiles, "IOPRP252.IMG");

                fd = openFile("pfs0:POPS/POPS.ELF", O_RDONLY);
                if (fd >= 0)
                    close(fd);
                else {
                    if (missingFiles[0] != '\0')
                        strcat(missingFiles, " 和 ");
                    strcat(missingFiles, "POPS.ELF");
                }

                if (missingFiles[0] != '\0') {
                    char message[96];

                    snprintf(message, sizeof(message), "__common/POPS 目录下缺少 %s", missingFiles);
                    guiMsgBox(message, 0, NULL);
                    oplRestoreHDDOPLPartition();
                    return;
                }

                snprintf(cheatPath, sizeof(cheatPath), "pfs0:POPS/CHEATS.TXT");
            } else {
                cheatPath[0] = '\0';
                oplRestoreHDDOPLPartition();
            }
        } else {
            snprintf(cheatPath, sizeof(cheatPath), "%s/POPS_IOX.PAK", appsList[id].path);
            fd = openFile(cheatPath, O_RDONLY);
            if (fd < 0) {
                guiMsgBox("POPS 目录下缺少 POPS_IOX.PAK", 0, NULL);
                return;
            }
            close(fd);

            snprintf(cheatPath, sizeof(cheatPath), "%s/CHEATS.TXT", appsList[id].path);
        }

        if (cheatPath[0] != '\0') {
            fd = openFile(cheatPath, O_RDONLY);
            if (fd >= 0)
                close(fd);
            else {
                if (guiMsgBoxCustom("初次启动，请选择适合的分辨率，避免黑屏！", "480i (液晶)", "240p (CRT)", NULL))
                    cheats = "$SAFEMODE\r\n$HDTVFIX\r\n480p";
                else
                    cheats = "$SAFEMODE\r\nHDTVFIX\r\n480p";

                guiMsgBox("若黑屏，请删除 POPS/CHEATS.TXT 即可重新选择分辨率", 0, NULL);

                fd = openFile(cheatPath, O_WRONLY | O_CREAT | O_TRUNC);
                if (fd >= 0) {
                    write(fd, cheats, strlen(cheats));
                    close(fd);
                }
            }
        }

        appPOPSPrepareStatus = 1;
        appPOPSPrepareID = id;
        guiHandleDeferedIO(&appPOPSPrepareStatus, _l(_STR_PLEASE_WAIT), IO_CUSTOM_SIMPLEACTION, &appPreparePOPSLauncher);

        /* APA HDD不依赖记忆卡中的外部驱动，修补失败不能阻止启动。 */
        if (mode != HDD_MODE &&
            (appPOPSPrepareResult & APP_POPS_PREPARE_DRIVERS_FAILED) &&
            !guiMsgBox("无法注入驱动，请检查记忆卡！是否强行启动？", 1, NULL)) {
            return;
        }

        // uLE: + XX./SB. 假 ELF 名。deinit 前先拷到栈上。
        if (snprintf(popstarterArg, sizeof(popstarterArg), "uLE:%s", appsList[id].boot) >= (int)sizeof(popstarterArg)) {
            guiMsgBox("POPSTARTER启动参数过长", 0, NULL);
            if (mode == HDD_MODE)
                oplRestoreHDDOPLPartition();
            return;
        }

        argv[0] = popstarterArg;

        /* 生成失败时明确清空邮箱；POPStarter继续启动，BDMA改用PAK就绪探测。 */
        if (appBuildPOPSBootMailbox(&appsList[id], &bootMailbox) < 0)
            memset(&bootMailbox, 0, sizeof(bootMailbox));

        if (mode < 0)
            mode = APP_MODE;
        if (gRememberLastPlayed) {
            configSetStr(configGetByType(CONFIG_LAST), "last_played", appsList[id].startup);
            saveConfig(CONFIG_LAST, 0);
        }
        deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call appCleanUp, so configApps/cur will be freed
        LoadELFFromMemoryNoResetWithResidentData(popstarter_elf, &bootMailbox, sizeof(bootMailbox), 1, argv);
        if (mode == HDD_MODE)
            oplRestoreHDDOPLPartition();
        return;
    }

    // Retrieve configuration set by appGetConfig()
    configGetStrCopy(configSet, CONFIG_ITEM_STARTUP, filename, sizeof(filename));

    // If no device number is specified use mass? to auto find device number
    const char *oldPrefix = "mass:";
    const char *newPrefix = "mass?:";

    if (strncmp(filename, oldPrefix, strlen(oldPrefix)) == 0) {
        size_t oldPrefixLen = strlen(oldPrefix);
        size_t newPrefixLen = strlen(newPrefix);
        memmove(filename + newPrefixLen, filename + oldPrefixLen, strlen(filename) - oldPrefixLen + 1);

        memcpy(filename, newPrefix, newPrefixLen);
    }

    // If legacy apps state mass? find the first connected mass device with the corresponding filename and set the unit number for launch.
    if (!strncmp("mass?", filename, 5)) {
        for (int i = 0; i < BDM_MODE4; i++) {
            filename[4] = i + '0';
            fd = open(filename, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                break;
            }
        }
    }

    //// 修复SMB加载APP卡死问题
    //const char *smbOldPrefix = "smb:";
    //const char *smbNewPrefix = "smb0:";
    //if (strncmp(filename, smbOldPrefix, strlen(smbOldPrefix)) == 0) {
    //    size_t oldPrefixLen = strlen(smbOldPrefix);
    //    size_t newPrefixLen = strlen(smbNewPrefix);
    //    memmove(filename + newPrefixLen, filename + oldPrefixLen, strlen(filename) - oldPrefixLen + 1);

    //    memcpy(filename, smbNewPrefix, newPrefixLen);
    //}
    fd = open(filename, O_RDONLY);
    if (fd >= 0) {
        int mode, argc = 0;
        char partition[128];
        char *argv[1];
        close(fd);

        strcpy(partition, "");

        // To keep the necessary device accessible, we will assume the mode that owns the device which contains the file to boot.
        mode = oplPath2Mode(filename);
        if (mode < 0)
            mode = APP_MODE; // Legacy apps mode on memory card (mc?:/*)

        if (mode == HDD_MODE)
            snprintf(partition, sizeof(partition), "%s:", gOPLPart);

        if (configGetStr(configSet, CONFIG_ITEM_ALTSTARTUP, &argv1) != 0) {
            argv[0] = (char *)argv1;
            argc = 1;
        }
        if (gRememberLastPlayed) {
            configSetStr(configGetByType(CONFIG_LAST), "last_played", appsList[id].startup);
            saveConfig(CONFIG_LAST, 0);
        }
        deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call appCleanUp, so configApps/cur will be freed
        LoadELFFromFileWithPartition(filename, partition, argc, argv);
    } else {
        guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
    }
}

static config_set_t *appGetConfig(item_list_t *itemList, int id)
{
    config_set_t *config;
    char tmp[8];

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        config = oplGetLegacyAppsInfo(appGetELFName(cur->val));
        configRead(config);

        configSetStr(config, CONFIG_ITEM_NAME, appGetELFName(cur->val));
        configSetStr(config, CONFIG_ITEM_LONGNAME, cur->key);
        configSetStr(config, CONFIG_ITEM_STARTUP, cur->val);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(cur->val));
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    } else {
        char path[256];
        if (appsList[id].generated) {
            config = configAlloc(0, NULL, NULL);
        } else {
            snprintf(path, sizeof(path), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);
            config = configAlloc(0, NULL, path);
            configRead(config); // Does not matter if the config file could be loaded or not.
        }

        configSetStr(config, CONFIG_ITEM_NAME, appsList[id].boot);
        configSetStr(config, CONFIG_ITEM_LONGNAME, appsList[id].title);
        configSetStr(config, CONFIG_ITEM_ALTSTARTUP, appsList[id].argv1); // reuse AltStartup for argument 1
        snprintf(path, sizeof(path), "%s/%s", appsList[id].path, appsList[id].boot);
        configSetStr(config, CONFIG_ITEM_STARTUP, path);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        if (appsList[id].popstarter)
            snprintf(tmp, sizeof(tmp), "%.2f", size_popstarter_elf / 1048576.0f);
        else
            snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(path));
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    }
    return config;
}

static int appGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char device[8] = "", *startup;
    int id;

    for (id = 0; id < appItemCount; id++) {
        if (appsList[id].legacy) {
            struct config_value_t *cur = appGetConfigValue(id);
            if (value == appGetELFName(cur->val)) {
                appGetBoot(device, sizeof(device), cur->val);
                break;
            }
        } else if (value == appsList[id].startup) {
            appGetBoot(device, sizeof(device), appsList[id].path);
            break;
        }
    }

    startup = appGetBoot(device, sizeof(device), value);

    if (!strcmp(folder, "ART")) {
        // 记忆卡ELF：只从卡根 ART/ 读取，不扫其它设备
        if (!strncmp(device, "mc", 2)) {
            char path[128];

            snprintf(path, sizeof(path), "%sART/%s_%s", device, startup, suffix);
            return texDiscoverLoad(resultTex, path, -1);
        }
        return oplGetAppImage(device, folder, isRelative, startup, suffix, resultTex, psm);
    } else
        return oplGetAppImage(device, folder, isRelative, value, suffix, resultTex, psm);
}

static int appGetTextId(item_list_t *itemList)
{
    return _STR_APPS;
}

static int appGetIconId(item_list_t *itemList)
{
    return APP_ICON;
}

// This may be called, even if appInit() was not.
static void appCleanUp(item_list_t *itemList, int exception)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT CleanUp\n");

        appFreeList();
        appFreeLegacyConfig();
    }
}

// This may be called, even if appInit() was not.
static void appShutdown(item_list_t *itemList)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT Shutdown\n");

        appFreeList();
        appFreeLegacyConfig();
    }
}

static item_list_t appItemList = {
    APP_MODE, -1, 0, MODE_FLAG_NO_COMPAT | MODE_FLAG_NO_UPDATE, MENU_MIN_INACTIVE_FRAMES, APP_MODE_UPDATE_DELAY, NULL, NULL, &appGetTextId, NULL, &appInit, &appNeedsUpdate, &appUpdateItemList,
    &appGetItemCount, NULL, &appGetItemName, &appGetItemNameLength, &appGetItemStartup, &appDeleteItem, &appRenameItem, &appLaunchItem,
    &appGetConfig, &appGetImage, &appCleanUp, &appShutdown, NULL, &appGetIconId};
