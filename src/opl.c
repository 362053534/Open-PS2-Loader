/*
  Copyright 2009, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/guigame.h"
#include "include/renderman.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/pad.h"
#include "include/texcache.h"
#include "include/dia.h"
#include "include/dialogs.h"
#include "include/menusys.h"
#include "include/system.h"
#include "include/debug.h"
#include "include/config.h"
#include "include/util.h"
#include "include/compatupd.h"
#include "include/extern_irx.h"
#include "httpclient.h"

#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/hddsupport.h"
#include "include/appsupport.h"

#include "include/cheatman.h"
#include "include/sound.h"
#include "include/xparam.h"

// FIXME: We should not need this function.
//        Use newlib's 'stat' to get GMT time.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // iox_stat_t
int configGetStat(config_set_t *configSet, iox_stat_t *stat);

#include <unistd.h>
#ifdef PADEMU
#include <libds34bt.h>
#include <libds34usb.h>
#endif

#ifdef __EESIO_DEBUG
#include "SIOCookie.h"
#define LOG_INIT() ee_sio_start(38400, 0, 0, 0, 0, 1)
#define LOG_ENABLE() \
    do {             \
    } while (0)
#else
#ifdef __DEBUG
#include "include/debug.h"
#define LOG_INIT() \
    do {           \
    } while (0)
#define LOG_ENABLE() ioPutRequest(IO_CUSTOM_SIMPLEACTION, &debugSetActive)
#else
#define LOG_INIT() \
    do {           \
    } while (0)
#define LOG_ENABLE() \
    do {             \
    } while (0)
#endif
#endif

// App support stuff.
static unsigned char shouldAppsUpdate;

// Network support stuff.
#define HTTP_IOBUF_SIZE 512
static unsigned int CompatUpdateComplete, CompatUpdateTotal;
static unsigned char CompatUpdateStopFlag, CompatUpdateFlags;
static short int CompatUpdateStatus;

static void clearIOModuleT(opl_io_module_t *mod)
{
    mod->subMenu = NULL;
    mod->support = NULL;
    mod->menuItem.execCross = NULL;
    mod->menuItem.execCircle = NULL;
    mod->menuItem.execSquare = NULL;
    mod->menuItem.execTriangle = NULL;
    mod->menuItem.hints = NULL;
    mod->menuItem.icon_id = -1;
    mod->menuItem.current = NULL;
    mod->menuItem.submenu = NULL;
    mod->menuItem.pagestart = NULL;
    mod->menuItem.remindLast = 0;
    mod->menuItem.refresh = NULL;
    mod->menuItem.text = NULL;
    mod->menuItem.text_id = -1;
    mod->menuItem.userdata = NULL;
}

// forward decl
static void clearMenuGameList(opl_io_module_t *mdl);
static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected);
static void reset(void);
static void deferredAudioInit(void);

// frame counter
static unsigned int frameCounter;

static char errorMessage[256];

static opl_io_module_t list_support[MODE_COUNT];

// Global data
char *gBaseMCDir;
int ps2_ip_use_dhcp;
int ps2_ip[4];
int ps2_netmask[4];
int ps2_gateway[4];
int ps2_dns[4];
int gETHOpMode; // See ETH_OP_MODES.
int gPCShareAddressIsNetBIOS;
int pc_ip[4];
int gPCPort;
char gPCShareNBAddress[17];
char gPCShareName[32];
char gPCUserName[32];
char gPCPassword[32];
int gNetworkStartup;
int gHDDSpindown;
int gRefreshAllModes = 0;
int gBDMStartMode;
int gHDDStartMode;
int gETHStartMode;
int gAPPStartMode;
int bdmCacheSize;
int hddCacheSize;
int smbCacheSize;
int gEnableUSB;
int gEnableILK;
int gEnableMX4SIO;
int gEnableBdmHDD;
int gTxtRename;
int gAutosort;
int gAutoRefresh;
int gEnableNotifications;
int gEnableArt;
int gWideScreen;
int gVMode; // 0 - Auto, 1 - PAL, 2 - NTSC
int gXOff;
int gYOff;
int gOverscan;
int gSelectButton;
int gHDDGameListCache;
int gEnableSFX;
int gEnableBootSND;
int gEnableBGM;
int gSFXVolume;
int gBootSndVolume;
int gBGMVolume;
char gDefaultBGMPath[128];
int gCheatSource;
int gGSMSource;
int gPadEmuSource;
int gFadeDelay;
int toggleSfx;
int showCfgPopup;
#ifdef PADEMU
int gEnablePadEmu;
int gPadEmuSettings;
int gPadMacroSource;
int gPadMacroSettings;
#endif
int gScrollSpeed;
char gExitPath[256];
int gEnableDebug;
int gPS2Logo;
int gDefaultDevice;
int gEnableWrite;
char gBDMPrefix[32];
char gETHPrefix[32];
int gRememberLastPlayed;
int KeyPressedOnce;
int gAutoStartLastPlayed;
int RemainSecs, DisableCron;
clock_t CronStart;
unsigned char gDefaultBgColor[3];
unsigned char gDefaultTextColor[3];
unsigned char gDefaultSelTextColor[3];
unsigned char gDefaultUITextColor[3];
hdl_game_info_t *gAutoLaunchGame;
base_game_info_t *gAutoLaunchBDMGame;
bdm_device_data_t *gAutoLaunchDeviceData;
char gOPLPart[128];
char *gHDDPrefix;
char gExportName[32];

int gXSensitivity;
int gYSensitivity;

int gOSDLanguageValue;
int gOSDTVAspectRatio;
int gOSDVideOutput;
int gOSDLanguageEnable;
int gOSDLanguageSource;

void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged);

void moduleUpdateMenu(int mode, int themeChanged, int langChanged)
{
    if (mode == -1)
        return;

    opl_io_module_t *mod = &list_support[mode];

    //// 手动模式启动时，会获取一次usb的数据
    //if (mode == 0 && bdmManualStarted)
    //    mod->support->itemInit(mod->support);
    
    moduleUpdateMenuInternal(mod, themeChanged, langChanged);

    //// 手动模式启动时，会更新一次usb的数据
    //if (mode == 0 && bdmManualStarted) {
    //    if (!gAutoRefresh || (mod->support->updateDelay == MENU_UPD_DELAY_NOUPDATE))
    //        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &mod->support->mode);
    //}
}

void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged)
{
    if (!mod->support)
        return;

    if (langChanged) {
        guiUpdateScreenScale();
        guiCheckNotifications(0, langChanged);
    }

    // refresh Hints
    menuRemoveHints(&mod->menuItem);

    menuAddHint(&mod->menuItem, _STR_MENU, START_ICON);
    if (!mod->support->enabled)
        menuAddHint(&mod->menuItem, _STR_START_DEVICE, gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON);
    else {
        menuAddHint(&mod->menuItem, _STR_RUN, gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON);

        if (gTheme->infoElems.first)
            menuAddHint(&mod->menuItem, _STR_INFO, SQUARE_ICON);

        if (!(mod->support->flags & MODE_FLAG_NO_COMPAT) || gEnableWrite)
            menuAddHint(&mod->menuItem, _STR_OPTIONS, TRIANGLE_ICON);

        menuAddHint(&mod->menuItem, _STR_REFRESH, SELECT_ICON);
    }

    // refresh Cache
    if (themeChanged) {
        if (mod->subMenu)
            submenuRebuildCache(mod->subMenu);
        guiCheckNotifications(themeChanged, 0);
    }
}

static void itemInitSupport(item_list_t *support)
{
    support->itemInit(support);
    moduleUpdateMenuInternal((opl_io_module_t *)support->owner, 0, 0);
    // Manual refreshing can only be done if either auto refresh is disabled or auto refresh is disabled for the item.
    if (!gAutoRefresh || (support->updateDelay == MENU_UPD_DELAY_NOUPDATE))
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
}

static void itemExecSelect(struct menu_item *curMenu)
{  
    if (mainScreenInitDone)
    {
        sfxPlay(SFX_CONFIRM);
        item_list_t *support = curMenu->userdata;
        if (support) {
            if (support->enabled) {
                if (curMenu->current) {
                    config_set_t *configSet = menuLoadConfig();
                    support->itemLaunch(support, curMenu->current->item.id, configSet);
                }
            } else {
                // If we're trying to enable BDM support we need to enable it for all BDM menu slots.
                if (support->mode == BDM_MODE) {
                    // Initialize support for all bdm modules.
                    for (int i = 0; i <= BDM_MODE4; i++) {
                        
                        opl_io_module_t *mod = &list_support[i];
                        itemInitSupport(mod->support);

                        //// BDM手动模式启动后，USB如果关闭了，页面0保持不变，等重新找到GPT硬盘后再刷新
                        //if (i != 0)
                        //    itemInitSupport(mod->support);                       

                        //// BDM手动模式启动后，页面0保持不更新，等重新找到GPT硬盘后再刷新
                        //support->itemInit(mod->support);
                        //if (i == 0) {
                        // 
                        //} else {
                        //    moduleUpdateMenuInternal((opl_io_module_t *)mod->support->owner, 0, 0);
                        //    // Manual refreshing can only be done if either auto refresh is disabled or auto refresh is disabled for the item.
                        //    if (!gAutoRefresh || (mod->support->updateDelay == MENU_UPD_DELAY_NOUPDATE))
                        //        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &mod->support->mode);
                        //}

                        // 手动模式根据设备开关，设定隐藏初始值（可能有负面影响）
                        mod->menuItem.visible = 0;
                        bdm_device_data_t *pDeviceData = mod->support->priv;
                        if (pDeviceData != NULL) {
                            if (!strcmp(pDeviceData->bdmDriver, "usb"))
                                mod->menuItem.visible = gEnableUSB;
                            else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
                                mod->menuItem.visible = gEnableILK;
                            else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
                                mod->menuItem.visible = gEnableMX4SIO;
                            else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
                                mod->menuItem.visible = gEnableBdmHDD;
                        }
                    }
                    // 手动启动BDM后，需要让gui有时间重新获取一次数据，并刷新主界面;
                    bdmManualTrigger = 1;
                    reFindBDM();
                } else {
                    // Normal initialization.
                    itemInitSupport(support);
                }
            }
        } else
            guiMsgBox("NULL Support object. Please report", 0, NULL);
    }   
}

static void itemExecRefresh(struct menu_item *curMenu)
{
    item_list_t *support = curMenu->userdata;

    if (support && support->enabled) {
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
        sfxPlay(SFX_CONFIRM);
    }
}

static void itemExecCross(struct menu_item *curMenu)
{
    if (gSelectButton == KEY_CROSS)
        itemExecSelect(curMenu);
}

static void itemExecCircle(struct menu_item *curMenu)
{
    if (gSelectButton == KEY_CIRCLE)
        itemExecSelect(curMenu);
}

static void itemExecSquare(struct menu_item *curMenu)
{
    if (curMenu->current && gTheme->infoElems.first)
        guiSwitchScreen(GUI_SCREEN_INFO);
}

static void itemExecTriangle(struct menu_item *curMenu)
{
    if (!curMenu->current)
        return;

    item_list_t *support = curMenu->userdata;

    if (support) {
        if (!(support->flags & MODE_FLAG_NO_COMPAT)) {
            if (menuCheckParentalLock() == 0) {
                menuInitGameMenu();
                guiSwitchScreen(GUI_SCREEN_GAME_MENU);
                guiGameLoadConfig(support, gameMenuLoadConfig(NULL));
            }
        } else {
            if (menuCheckParentalLock() == 0 && gEnableWrite) {
                menuInitAppMenu();
                guiSwitchScreen(GUI_SCREEN_APP_MENU);
            }
        }
    } else
        guiMsgBox("NULL Support object. Please report", 0, NULL);
}

static void initMenuForListSupport(opl_io_module_t *mod)
{
    mod->menuItem.icon_id = mod->support->itemIconId(mod->support);
    mod->menuItem.text = NULL;
    mod->menuItem.text_id = mod->support->itemTextId(mod->support);
    mod->menuItem.visible = 1;

    mod->menuItem.userdata = mod->support;

    mod->subMenu = NULL;

    mod->menuItem.submenu = NULL;
    mod->menuItem.current = NULL;
    mod->menuItem.pagestart = NULL;
    mod->menuItem.remindLast = 0;

    mod->menuItem.refresh = &itemExecRefresh;
    mod->menuItem.execCross = &itemExecCross;
    mod->menuItem.execTriangle = &itemExecTriangle;
    mod->menuItem.execSquare = &itemExecSquare;
    mod->menuItem.execCircle = &itemExecCircle;

    mod->menuItem.hints = NULL;

    moduleUpdateMenuInternal(mod, 0, 0);

    struct gui_update_t *mc = guiOpCreate(GUI_OP_ADD_MENU);
    mc->menu.menu = &mod->menuItem;
    mc->menu.subMenu = &mod->subMenu;
    guiDeferUpdate(mc);
}

static void clearMenuGameList(opl_io_module_t *mdl)
{
    if (mdl->subMenu != NULL) {
        // lock - gui has to be unused here
        guiLock();

        submenuDestroy(&mdl->subMenu);
        mdl->menuItem.submenu = NULL;
        mdl->menuItem.current = NULL;
        mdl->menuItem.pagestart = NULL;
        mdl->menuItem.remindLast = 0;

        // unlock
        guiUnlock();
    }
}

void initSupport(item_list_t *itemList, int mode, int force_reinit)
{
    opl_io_module_t *mod = &list_support[mode];

    // 解决HDD和BDMHDD的冲突问题
    if (gEnableBdmHDD) {
        if (gDefaultDevice == HDD_MODE) {
            gDefaultDevice = BDM_MODE;
        }
        if (gHDDStartMode != START_MODE_DISABLED) {
            gHDDStartMode = START_MODE_DISABLED;
        }
    }

    // Set the start mode flag based on device type.
    int startMode = 0;
    if (mode >= BDM_MODE && mode < ETH_MODE)
        startMode = gBDMStartMode;
    else if (mode == ETH_MODE)
        startMode = gETHStartMode;
    else if (mode == HDD_MODE)
        startMode = gHDDStartMode;
    else if (mode == APP_MODE)
        startMode = gAPPStartMode;

    if (startMode) {
        if (!mod->support) {
            mod->support = itemList;
            mod->support->owner = mod;
            initMenuForListSupport(mod);
        }
        //// 根据开关，提前隐藏不需要的设备，开启设备由BDM负责
        //if (mode >= BDM_MODE && mode < ETH_MODE) {
        //    bdm_device_data_t *pDeviceData = itemList->priv;
        //    if (pDeviceData != NULL) {
        //        if (!strcmp(pDeviceData->bdmDriver, "usb") && !gEnableUSB)
        //            mod->menuItem.visible = gEnableUSB;
        //        else if ((pDeviceData->bdmDeviceType == BDM_TYPE_ILINK) && !gEnableILK)
        //            mod->menuItem.visible = gEnableILK;
        //        else if ((pDeviceData->bdmDeviceType == BDM_TYPE_SDC) && !gEnableMX4SIO)
        //            mod->menuItem.visible = gEnableMX4SIO;
        //        else if ((pDeviceData->bdmDeviceType == BDM_TYPE_ATA) && !gEnableBdmHDD)
        //            mod->menuItem.visible = gEnableBdmHDD;
        //    }
        //}
        //  根据开关，提前设置隐藏状态
        if (mode >= BDM_MODE && mode < ETH_MODE) {
            bdm_device_data_t *pDeviceData = itemList->priv;
            if (pDeviceData != NULL) {
                if (!strcmp(pDeviceData->bdmDriver, "usb"))
                    mod->menuItem.visible = gEnableUSB;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ILINK)
                    mod->menuItem.visible = gEnableILK;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC)
                    mod->menuItem.visible = gEnableMX4SIO;
                else if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
                    mod->menuItem.visible = gEnableBdmHDD;
            }
        }

        if (((force_reinit) && (mod->support->enabled)) || (startMode == START_MODE_AUTO && !mod->support->enabled)) {
            mod->support->itemInit(mod->support);
            moduleUpdateMenuInternal(mod, 0, 0);
            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[mode].support->mode); // can't use mode as the variable will die at end of execution
        }
    } else {
        // If the module has a valid menu instance try to refresh the visibility state.
        mod->menuItem.visible = 0;
    }

    //// debug  打印debug信息，获取GPT设备信息，方便调试
    //if (mode == 1) {
    //    char debugFileDir1[64];
    //    strcpy(debugFileDir1, "mass0:debug-opl.txt");
    //    bdm_device_data_t *pDeviceData = itemList->priv;
    //    // sprintf(debugFileDir, "%sdebug.txt", prefix);
    //    FILE *debugFile1 = fopen(debugFileDir1, "ab+");
    //    if (debugFile1 != NULL) {
    //        if (pDeviceData != NULL) {
    //            if (pDeviceData->bdmDriver[0] == '\0') {
    //                int dir = fileXioDopen("mass1:/");
    //                if (dir >= 0) {
    //                    fprintf(debugFile1, "发现GPT设备，但未初始化，数据为空\r\n\r\n");
    //                } else {
    //                    fprintf(debugFile1, "未识别到GPT设备\r\n\r\n");
    //                }
    //            } else
    //                fprintf(debugFile1, "成功识别GPT设备类型为%s\r\n隐藏属性为%d\r\n路径为%s\r\nbdmDeviceType为%d\r\n\r\n", pDeviceData->bdmDriver, mod->menuItem.visible, pDeviceData->bdmPrefix, pDeviceData->bdmDeviceType);
    //        } else {
    //            fprintf(debugFile1, "未识别到GPT设备\r\n\r\n");
    //        }
    //        fclose(debugFile1);
    //    }
    //}
}

static void initAllSupport(int force_reinit)
{
    bdmEnumerateDevices();
    initSupport(ethGetObject(0), ETH_MODE, force_reinit || (gNetworkStartup >= ERROR_ETH_SMB_CONN));
    initSupport(hddGetObject(0), HDD_MODE, force_reinit);
    initSupport(appGetObject(0), APP_MODE, force_reinit);
}

static void deinitAllSupport(int exception, int modeSelected)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support != NULL)
            moduleCleanup(&list_support[i], exception, modeSelected);
    }
}

// For resolving the mode, given an app's path
int oplPath2Mode(const char *path)
{
    char appsPath[64];
    const char *blkdevnameend;
    int i, blkdevnamelen;
    item_list_t *listSupport;

    for (i = 0; i < MODE_COUNT; i++) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sAPPS", prefix);

            blkdevnameend = strchr(appsPath, ':');
            if (blkdevnameend != NULL) {
                blkdevnamelen = (int)(blkdevnameend - appsPath);

                if (strncmp(path, appsPath, blkdevnamelen) == 0)
                    return listSupport->mode;
            }
        }
    }

    return -1;
}

int oplGetAppImage(const char *device, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    int i, remaining, elfbootmode;
    char priority;
    item_list_t *listSupport;

    elfbootmode = -1;
    if (device != NULL) {
        elfbootmode = oplPath2Mode(device);
        if (elfbootmode >= 0) {
            listSupport = list_support[elfbootmode].support;

            if ((listSupport != NULL) && (listSupport->enabled)) {
                if (listSupport->itemGetImage(listSupport, folder, isRelative, value, suffix, resultTex, psm) >= 0)
                    return 0;
            }
        }
    }

    // We search on ever devices from fatest to slowest.
    for (remaining = MODE_COUNT, priority = 0; remaining > 0 && priority < 4; priority++) {
        for (i = 0; i < MODE_COUNT; i++) {
            listSupport = list_support[i].support;

            if (i == elfbootmode)
                continue;

            if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->appsPriority == priority)) {
                if (listSupport->itemGetImage(listSupport, folder, isRelative, value, suffix, resultTex, psm) >= 0)
                    return 0;
                remaining--;
            }
        }
    }

    return -1;
}

static int scanApps(int (*callback)(const char *path, config_set_t *appConfig, void *arg), void *arg, char *appsPath, int exception)
{
    struct dirent *pdirent;
    DIR *pdir;
    int count, ret;
    config_set_t *appConfig;
    char dir[128];
    char path[128];

    count = 0;
    if ((pdir = opendir(appsPath)) != NULL) {
        while ((pdirent = readdir(pdir)) != NULL) {
            if (exception && strchr(pdirent->d_name, '_') == NULL)
                continue;

            if (strcmp(pdirent->d_name, ".") == 0 || strcmp(pdirent->d_name, "..") == 0)
                continue;

            snprintf(dir, sizeof(dir), "%s/%s", appsPath, pdirent->d_name);
            if (pdirent->d_type != DT_DIR)
                continue;

            snprintf(path, sizeof(path), "%s/%s", dir, APP_TITLE_CONFIG_FILE);
            appConfig = configAlloc(0, NULL, path);
            if (appConfig != NULL) {
                configRead(appConfig);

                ret = callback(dir, appConfig, arg);
                configFree(appConfig);

                if (ret == 0)
                    count++;
                else if (ret < 0) { // Stopped because of unrecoverable error.
                    break;
                }
            }
        }

        closedir(pdir);
    } else
        LOG("APPS failed to open dir %s\n", appsPath);

    return count;
}

int oplScanApps(int (*callback)(const char *path, config_set_t *appConfig, void *arg), void *arg)
{
    int i, count;
    item_list_t *listSupport;
    char appsPath[64];

    count = 0;
    for (i = 0; i < MODE_COUNT; i++) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sAPPS", prefix);
            count += scanApps(callback, arg, appsPath, 0);
        }
    }

    for (i = 0; i < 2; i++) {
        snprintf(appsPath, sizeof(appsPath), "mc%d:", i);
        count += scanApps(callback, arg, appsPath, 1);
    }

    return count;
}

int oplShouldAppsUpdate(void)
{
    int result;

    result = (int)shouldAppsUpdate;
    shouldAppsUpdate = 0;

    return result;
}

config_set_t *oplGetLegacyAppsConfig(void)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128];

    snprintf(appsPath, sizeof(appsPath), "mc?:OPL/conf_apps.cfg");
    fd = openFile(appsPath, O_RDONLY);
    if (fd >= 0) {
        appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);
        close(fd);
        return appConfig;
    }

    for (i = MODE_COUNT - 1; i >= 0; i--) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sconf_apps.cfg", prefix);

            fd = openFile(appsPath, O_RDONLY);
            if (fd >= 0) {
                appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);
                close(fd);
                return appConfig;
            }
        }
    }

    /* Apps config not found on any device, go with last tested device.
       Does not matter if the config file could be loaded or not */
    appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);

    return appConfig;
}

config_set_t *oplGetLegacyAppsInfo(char *name)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128];

    for (i = MODE_COUNT - 1; i >= 0; i--) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sCFG%s%s.cfg", prefix, i == ETH_MODE ? "\\" : "/", name);

            fd = openFile(appsPath, O_RDONLY);
            if (fd >= 0) {
                appConfig = configAlloc(0, NULL, appsPath);
                close(fd);
                return appConfig;
            }
        }
    }

    /* Apps config not found on any device, go with last tested device.
       Does not matter if the config file could be loaded or not */
    appConfig = configAlloc(0, NULL, appsPath);

    return appConfig;
}

// ----------------------------------------------------------
// ----------------------- Updaters -------------------------
// ----------------------------------------------------------
static void updateMenuFromGameList(opl_io_module_t *mdl)
{
    guiExecDeferredOps();
    clearMenuGameList(mdl);

    const char *temp = NULL;
    if (gRememberLastPlayed)
        configGetStr(configGetByType(CONFIG_LAST), "last_played", &temp);

    // refresh device icon and text (for bdm)
    mdl->menuItem.icon_id = mdl->support->itemIconId(mdl->support);
    mdl->menuItem.text_id = mdl->support->itemTextId(mdl->support);

    // read the new game list
    struct gui_update_t *gup = NULL;
    int count = mdl->support->itemUpdate(mdl->support);
    if (count > 0) {
        int i;

        for (i = 0; i < count; ++i) {

            gup = guiOpCreate(GUI_OP_APPEND_MENU);

            gup->menu.menu = &mdl->menuItem;
            gup->menu.subMenu = &mdl->subMenu;

            gup->submenu.icon_id = -1;
            gup->submenu.id = i;
            gup->submenu.text = mdl->support->itemGetName(mdl->support, i);
            gup->submenu.text_id = -1;
            gup->submenu.selected = 0;

            if (gRememberLastPlayed && temp && strcmp(temp, mdl->support->itemGetStartup(mdl->support, i)) == 0) {
                gup->submenu.selected = 1; // Select Last Played Game
            }

            guiDeferUpdate(gup);
        }
    }

    if (gAutosort) {
        gup = guiOpCreate(GUI_OP_SORT);
        gup->menu.menu = &mdl->menuItem;
        gup->menu.subMenu = &mdl->subMenu;
        guiDeferUpdate(gup);
    }
}

void menuDeferredUpdate(void *data)
{
    short int *mode = data;

    opl_io_module_t *mod = &list_support[*mode];
    if (!mod->support)
        return;

    // see if we have to update
    if (mod->support->itemNeedsUpdate(mod->support)) {
        updateMenuFromGameList(mod);

        // If other modes have been updated, then the apps list should be updated too.
        if (mod->support->mode != APP_MODE)
            shouldAppsUpdate = 1;
    }
}

#define MENU_GENERAL_UPDATE_DELAY 60

static void menuUpdateHook()
{
    int i;

    // if timer exceeds some threshold, schedule updates of the available input sources
    frameCounter++;

    // schedule updates of all the list handlers
    if (gAutoRefresh) {
        for (i = 0; i < MODE_COUNT; i++) {
            if ((list_support[i].support && list_support[i].support->enabled) && ((list_support[i].support->updateDelay > 0) && (frameCounter % list_support[i].support->updateDelay == 0)))
                ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[i].support->mode);
        }
    }

    // Schedule updates of all list handlers that are to run every frame, regardless of whether auto refresh is active or not.
    //if (!mainScreenInitDone && (frameCounter % 5 == 0)) { // 列表界面没有准备好时，检测频率上升
    //    for (i = 0; i < MODE_COUNT; i++) {
    //        if ((list_support[i].support && list_support[i].support->enabled) && (list_support[i].support->updateDelay == 0))
    //            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[i].support->mode);
    //    }
    //} else
    //if ((frameCounter % MENU_GENERAL_UPDATE_DELAY == 0) || !mainScreenInitDone) {
    if (gRefreshAllModes || !mainScreenInitDone) {
        gRefreshAllModes = 0;
        for (i = 0; i < MODE_COUNT; i++) {
            if ((list_support[i].support && list_support[i].support->enabled) && (list_support[i].support->updateDelay == 0))
                ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[i].support->mode);
        }
    }
}

static void clearErrorMessage(void)
{
    // reset the original frame hook
    frameCounter = 0;
    guiSetFrameHook(&menuUpdateHook);
}

static void errorMessageHook()
{
    guiMsgBox(errorMessage, 0, NULL);
    clearErrorMessage();
}

void setErrorMessageWithCode(int strId, int error)
{
    snprintf(errorMessage, sizeof(errorMessage), _l(strId), error);
    guiSetFrameHook(&errorMessageHook);
}

void setErrorMessage(int strId)
{
    snprintf(errorMessage, sizeof(errorMessage), _l(strId));
    guiSetFrameHook(&errorMessageHook);
}

// ----------------------------------------------------------
// ------------------ Configuration handling ----------------
// ----------------------------------------------------------

static int lscstatus = CONFIG_ALL;
static int lscret = 0;

static int checkLoadConfigBDM(int types)
{
    char path[64];
    int value;

    // check USB
    if (bdmFindPartition(path, "conf_opl.cfg", 0)) {
        configEnd();
        configInit(path);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, START_MODE_AUTO);
        return value;
    }

    return 0;
}

static int checkLoadConfigHDD(int types)
{
    int value;
    char path[64];

    hddLoadModules();
    hddLoadSupportModules();

    snprintf(path, sizeof(path), "%sconf_opl.cfg", gHDDPrefix);
    value = open(path, O_RDONLY);
    if (value >= 0) {
        close(value);
        configEnd();
        configInit(gHDDPrefix);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_HDD_MODE, START_MODE_AUTO);
        return value;
    }

    return 0;
}

// When this function is called, the current device for loading/saving config is the memory card.
static int tryAlternateDevice(int types)
{
    char pwd[8];
    int value;
    DIR *dir;

    getcwd(pwd, sizeof(pwd));

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = checkLoadConfigBDM(types)) != 0)
            return value;
    } else if (!strncmp(pwd, "hdd", 3) && (pwd[3] == ':' || pwd[4] == ':')) {
        if ((value = checkLoadConfigHDD(types)) != 0)
            return value;
    }

    // Config was not found on the boot device. Check all supported devices.
    //  Check USB device
    if ((value = checkLoadConfigBDM(types)) != 0)
        return value;
    // Check HDD
    if ((value = checkLoadConfigHDD(types)) != 0)
        return value;

    // At this point, the user has no loadable config files on any supported device, so try to find a device to save on.
    // We don't want to get users into alternate mode for their very first launch of OPL (i.e no config file at all, but still want to save on MC)
    // Check for a memory card inserted.
    if (sysCheckMC() >= 0) {
        configPrepareNotifications(gBaseMCDir);
        showCfgPopup = 0;
        return 0;
    }
    // No memory cards? Try a USB device...
    dir = opendir("mass0:");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mass0:");
    } else {
        // No? Check if the save location on the HDD is available.
        dir = opendir(gHDDPrefix);
        if (dir != NULL) {
            closedir(dir);
            configEnd();
            configInit(gHDDPrefix);
        }
    }
    showCfgPopup = 0;

    return 0;
}

static void _loadConfig()
{
    int value, themeID = -1, langID = -1;
    const char *temp;
    int result = configReadMulti(lscstatus);

    if (lscstatus & CONFIG_OPL) {
        if (!(result & CONFIG_OPL)) {
            result = tryAlternateDevice(lscstatus);
        }

        if (result & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_SCROLLING, &gScrollSpeed);
            configGetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
            configGetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
            configGetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
            configGetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, &gEnableNotifications);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, &gEnableArt);
            configGetInt(configOPL, CONFIG_OPL_WIDESCREEN, &gWideScreen);

            if (!(getKeyPressed(KEY_TRIANGLE) && getKeyPressed(KEY_CROSS))) {
                configGetInt(configOPL, CONFIG_OPL_VMODE, &gVMode);
            } else {
                LOG("--- Triangle + Cross held at boot - setting Video Mode to Auto ---\n");
                gVMode = 0;
                configSetInt(configOPL, CONFIG_OPL_VMODE, gVMode);
            }

            configGetInt(configOPL, CONFIG_OPL_XOFF, &gXOff);
            configGetInt(configOPL, CONFIG_OPL_YOFF, &gYOff);
            configGetInt(configOPL, CONFIG_OPL_OVERSCAN, &gOverscan);

            configGetInt(configOPL, CONFIG_OPL_BDM_CACHE, &bdmCacheSize);
            configGetInt(configOPL, CONFIG_OPL_HDD_CACHE, &hddCacheSize);
            configGetInt(configOPL, CONFIG_OPL_SMB_CACHE, &smbCacheSize);

            if (configGetStr(configOPL, CONFIG_OPL_THEME, &temp))
                themeID = thmFindGuiID(temp);

            if (configGetStr(configOPL, CONFIG_OPL_LANGUAGE, &temp))
                langID = lngFindGuiID(temp);

            if (configGetInt(configOPL, CONFIG_OPL_SWAP_SEL_BUTTON, &value))
                gSelectButton = value == 0 ? KEY_CIRCLE : KEY_CROSS;

            configGetInt(configOPL, CONFIG_OPL_XSENSITIVITY, &gXSensitivity);
            configGetInt(configOPL, CONFIG_OPL_YSENSITIVITY, &gYSensitivity);
            configGetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, &gEnableDebug);
            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, &gHDDGameListCache);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetInt(configOPL, CONFIG_OPL_TXT_RENAME, &gTxtRename);
            configGetInt(configOPL, CONFIG_OPL_AUTO_SORT, &gAutosort);
            configGetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, &gAutoRefresh);
            configGetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, &gDefaultDevice);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, &gEnableWrite);
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
            configGetStrCopy(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix, sizeof(gETHPrefix));
            configGetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, &gRememberLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, &gAutoStartLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_BDM_MODE, &gBDMStartMode);
            configGetInt(configOPL, CONFIG_OPL_HDD_MODE, &gHDDStartMode);
            //if (gETHStartMode != START_MODE_DISABLED) {
            //    gETHStartMode = START_MODE_DISABLED;
            //} else {
            //    configGetInt(configOPL, CONFIG_OPL_ETH_MODE, &gETHStartMode);
            //}
            configGetInt(configOPL, CONFIG_OPL_ETH_MODE, &gETHStartMode);
            configGetInt(configOPL, CONFIG_OPL_APP_MODE, &gAPPStartMode);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_USB, &gEnableUSB);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, &gEnableILK);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, &gEnableMX4SIO);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, &gEnableBdmHDD);
            configGetInt(configOPL, CONFIG_OPL_SFX, &gEnableSFX);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND, &gEnableBootSND);
            configGetInt(configOPL, CONFIG_OPL_BGM, &gEnableBGM);
            configGetInt(configOPL, CONFIG_OPL_SFX_VOLUME, &gSFXVolume);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, &gBootSndVolume);
            configGetInt(configOPL, CONFIG_OPL_BGM_VOLUME, &gBGMVolume);
            configGetStrCopy(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath, sizeof(gDefaultBGMPath));
        }
    }

    if (lscstatus & CONFIG_NETWORK) {
        if (!(result & CONFIG_NETWORK)) {
            result = tryAlternateDevice(lscstatus);
        }

        if (result & CONFIG_NETWORK) {
            config_set_t *configNet = configGetByType(CONFIG_NETWORK);

            configGetInt(configNet, CONFIG_NET_ETH_LINKM, &gETHOpMode);

            configGetInt(configNet, CONFIG_NET_PS2_DHCP, &ps2_ip_use_dhcp);
            configGetInt(configNet, CONFIG_NET_SMB_NBNS, &gPCShareAddressIsNetBIOS);
            configGetStrCopy(configNet, CONFIG_NET_SMB_NB_ADDR, gPCShareNBAddress, sizeof(gPCShareNBAddress));

            if (configGetStr(configNet, CONFIG_NET_SMB_IP_ADDR, &temp))
                sscanf(temp, "%d.%d.%d.%d", &pc_ip[0], &pc_ip[1], &pc_ip[2], &pc_ip[3]);

            configGetInt(configNet, CONFIG_NET_SMB_PORT, &gPCPort);

            configGetStrCopy(configNet, CONFIG_NET_SMB_SHARE, gPCShareName, sizeof(gPCShareName));
            configGetStrCopy(configNet, CONFIG_NET_SMB_USER, gPCUserName, sizeof(gPCUserName));
            configGetStrCopy(configNet, CONFIG_NET_SMB_PASSW, gPCPassword, sizeof(gPCPassword));

            if (configGetStr(configNet, CONFIG_NET_PS2_IP, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_ip[0], &ps2_ip[1], &ps2_ip[2], &ps2_ip[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_NETM, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_netmask[0], &ps2_netmask[1], &ps2_netmask[2], &ps2_netmask[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_GATEW, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_gateway[0], &ps2_gateway[1], &ps2_gateway[2], &ps2_gateway[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_DNS, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_dns[0], &ps2_dns[1], &ps2_dns[2], &ps2_dns[3]);

            configGetStrCopy(configNet, CONFIG_NET_NBD_DEFAULT_EXPORT, gExportName, sizeof(gExportName));
        }
    }

    applyConfig(themeID, langID, 0);

    lscret = result;
    lscstatus = 0;
    showCfgPopup = 1;
}

static int trySaveConfigBDM(int types)
{
    char path[64];

    // check USB
    if (bdmFindPartition(path, "conf_opl.cfg", 1)) {
        configSetMove(path);
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigHDD(int types)
{
    hddLoadModules();
    // Check that the formatted & usable HDD is connected.
    if (hddCheck() == 0) {
        configSetMove(gHDDPrefix);
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigMC(int types)
{
    configSetMove(NULL);
    return configWriteMulti(types);
}

static int trySaveAlternateDevice(int types)
{
    char pwd[8];
    int value;

    getcwd(pwd, sizeof(pwd));

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = trySaveConfigBDM(types)) > 0)
            return value;
    } else if (!strncmp(pwd, "hdd", 3) && (pwd[3] == ':' || pwd[4] == ':')) {
        if ((value = trySaveConfigHDD(types)) > 0)
            return value;
    }

    // Config was not saved to the boot device. Try all supported devices.
    // Try memory cards
    if (sysCheckMC() >= 0) {
        if ((value = trySaveConfigMC(types)) > 0)
            return value;
    }
    // Try a USB device
    if ((value = trySaveConfigBDM(types)) > 0)
        return value;
    // Try the HDD
    if ((value = trySaveConfigHDD(types)) > 0)
        return value;

    // We tried everything, but...
    return 0;
}

static void _saveConfig()
{
    char temp[256];

    if (lscstatus & CONFIG_OPL) {
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_SCROLLING, gScrollSpeed);
        configSetStr(configOPL, CONFIG_OPL_THEME, thmGetValue());
        configSetStr(configOPL, CONFIG_OPL_LANGUAGE, lngGetValue());
        configSetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
        configSetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
        configSetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
        configSetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, gEnableNotifications);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, gEnableArt);
        configSetInt(configOPL, CONFIG_OPL_WIDESCREEN, gWideScreen);
        configSetInt(configOPL, CONFIG_OPL_VMODE, gVMode);
        configSetInt(configOPL, CONFIG_OPL_XOFF, gXOff);
        configSetInt(configOPL, CONFIG_OPL_YOFF, gYOff);
        configSetInt(configOPL, CONFIG_OPL_OVERSCAN, gOverscan);
        configSetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, gEnableDebug);
        configSetInt(configOPL, CONFIG_OPL_PS2LOGO, gPS2Logo);
        configSetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, gHDDGameListCache);
        configSetStr(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath);
        configSetInt(configOPL, CONFIG_OPL_TXT_RENAME, gTxtRename);
        configSetInt(configOPL, CONFIG_OPL_AUTO_SORT, gAutosort);
        configSetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, gAutoRefresh);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, gDefaultDevice);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, gEnableWrite);
        configSetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, gHDDSpindown);
        configSetStr(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix);
        configSetStr(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix);
        configSetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, gRememberLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, gAutoStartLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, gBDMStartMode);
        configSetInt(configOPL, CONFIG_OPL_HDD_MODE, gHDDStartMode);
        //if (gETHStartMode != START_MODE_DISABLED) {
        //    gETHStartMode = START_MODE_DISABLED;
        //} 
        configSetInt(configOPL, CONFIG_OPL_ETH_MODE, gETHStartMode);
        configSetInt(configOPL, CONFIG_OPL_APP_MODE, gAPPStartMode);
        configSetInt(configOPL, CONFIG_OPL_BDM_CACHE, bdmCacheSize);
        configSetInt(configOPL, CONFIG_OPL_HDD_CACHE, hddCacheSize);
        configSetInt(configOPL, CONFIG_OPL_SMB_CACHE, smbCacheSize);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_USB, gEnableUSB);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, gEnableILK);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, gEnableMX4SIO);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
        configSetInt(configOPL, CONFIG_OPL_SFX, gEnableSFX);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND, gEnableBootSND);
        configSetInt(configOPL, CONFIG_OPL_BGM, gEnableBGM);
        configSetInt(configOPL, CONFIG_OPL_SFX_VOLUME, gSFXVolume);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, gBootSndVolume);
        configSetInt(configOPL, CONFIG_OPL_BGM_VOLUME, gBGMVolume);
        configSetStr(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath);
        configSetInt(configOPL, CONFIG_OPL_XSENSITIVITY, gXSensitivity);
        configSetInt(configOPL, CONFIG_OPL_YSENSITIVITY, gYSensitivity);

        configSetInt(configOPL, CONFIG_OPL_SWAP_SEL_BUTTON, gSelectButton == KEY_CIRCLE ? 0 : 1);
    }

    if (lscstatus & CONFIG_NETWORK) {
        config_set_t *configNet = configGetByType(CONFIG_NETWORK);

        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        configSetStr(configNet, CONFIG_NET_PS2_IP, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_netmask[0], ps2_netmask[1], ps2_netmask[2], ps2_netmask[3]);
        configSetStr(configNet, CONFIG_NET_PS2_NETM, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_gateway[0], ps2_gateway[1], ps2_gateway[2], ps2_gateway[3]);
        configSetStr(configNet, CONFIG_NET_PS2_GATEW, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_dns[0], ps2_dns[1], ps2_dns[2], ps2_dns[3]);
        configSetStr(configNet, CONFIG_NET_PS2_DNS, temp);

        configSetInt(configNet, CONFIG_NET_ETH_LINKM, gETHOpMode);
        configSetInt(configNet, CONFIG_NET_PS2_DHCP, ps2_ip_use_dhcp);
        configSetInt(configNet, CONFIG_NET_SMB_NBNS, gPCShareAddressIsNetBIOS);
        configSetStr(configNet, CONFIG_NET_SMB_NB_ADDR, gPCShareNBAddress);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
        configSetStr(configNet, CONFIG_NET_SMB_IP_ADDR, temp);
        configSetInt(configNet, CONFIG_NET_SMB_PORT, gPCPort);
        configSetStr(configNet, CONFIG_NET_SMB_SHARE, gPCShareName);
        configSetStr(configNet, CONFIG_NET_SMB_USER, gPCUserName);
        configSetStr(configNet, CONFIG_NET_SMB_PASSW, gPCPassword);
    }

    char *path = configGetDir();
    if (!strncmp(path, "mc", 2)) {
        checkMCFolder();
        configPrepareNotifications(gBaseMCDir);
    }

    lscret = configWriteMulti(lscstatus);
    if (lscret == 0)
        lscret = trySaveAlternateDevice(lscstatus);
    lscstatus = 0;
}

void applyConfig(int themeID, int langID, int skipDeviceRefresh)
{
    if (gDefaultDevice < 0 || gDefaultDevice > APP_MODE)
        gDefaultDevice = APP_MODE;

    guiUpdateScrollSpeed();

    guiSetFrameHook(&menuUpdateHook);

    int changed = rmSetMode(0);
    if (changed) {
        bgmMute();
        // reinit the graphics...
        thmReloadScreenExtents();
        guiReloadScreenExtents();
    }

    // theme must be set after color, and lng after theme
    changed = thmSetGuiValue(themeID, changed);
    int langChanged = lngSetGuiValue(langID);

    guiUpdateScreenScale();

    // Check if we should refresh device support as well.
    if (skipDeviceRefresh == 0) {
        initAllSupport(0);

        for (int i = 0; i < MODE_COUNT; i++) {
            if (list_support[i].support == NULL)
                continue;

            moduleUpdateMenuInternal(&list_support[i], changed, langChanged);
        }
    } else {
        if (changed) {
            for (int i = 0; i < MODE_COUNT; i++) {
                if (list_support[i].support && list_support[i].subMenu)
                    submenuRebuildCache(list_support[i].subMenu);
            }
        }
    }

    bgmUnMute();

#ifdef __DEBUG
    debugApplyConfig();
#endif
}

int loadConfig(int types)
{
    lscstatus = types;
    lscret = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_loadConfig);

    return lscret;
}

int saveConfig(int types, int showUI)
{
    char notification[128];
    lscstatus = types;
    lscret = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_saveConfig);

    if (showUI) {
        if (lscret) {
            char *path = configGetDir();

            snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_SAVED), path);

            guiMsgBox(notification, 0, NULL);
        } else
            guiMsgBox(_l(_STR_ERROR_SAVING_SETTINGS), 0, NULL);
    }

    return lscret;
}

#define COMPAT_UPD_MODE_UPD_USR   1 // Update all records, even those that were modified by the user.
#define COMPAT_UPD_MODE_NO_MTIME  2 // Do not check the modified time-stamp.
#define COMPAT_UPD_MODE_MTIME_GMT 4 // Modified time-stamp is in GMT, not JST.

#define EOPLCONNERR 0x4000 // Special error code for connection errors.

static int CompatAttemptConnection(void)
{
    unsigned char retries;
    int HttpSocket;

    for (retries = OPL_COMPAT_HTTP_RETRIES, HttpSocket = -1; !CompatUpdateStopFlag && retries > 0; retries--) {
        if ((HttpSocket = HttpEstabConnection(OPL_COMPAT_HTTP_HOST, OPL_COMPAT_HTTP_PORT)) >= 0) {
            break;
        }
    }

    return HttpSocket;
}

static void compatUpdate(item_list_t *support, unsigned char mode, config_set_t *configSet, int id)
{
    sceCdCLOCK clock;
    config_set_t *itemConfig, *downloadedConfig;
    u16 length;
    s8 ConnMode, hasMtime;
    char *HttpBuffer;
    int i, count, HttpSocket, result, retries, ConfigSource;
    iox_stat_t stat;
    u8 mtime[6];
    char device, uri[64];
    const char *startup;

    switch (support->mode) {
        case BDM_MODE:
            device = 3;
            break;
        case ETH_MODE:
            mode |= COMPAT_UPD_MODE_MTIME_GMT;
            device = 2;
            break;
        case HDD_MODE:
            device = 1;
            break;
        default:
            device = -1;
    }

    if (device < 0) {
        LOG("CompatUpdate: unrecognized mode: %d\n", support->mode);
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_ERROR;
        return; // Shouldn't happen, but what if?
    }

    result = 0;
    LOG("CompatUpdate: updating for: device %d game %d\n", device, configSet == NULL ? -1 : id);

    if ((HttpBuffer = memalign(64, HTTP_IOBUF_SIZE)) != NULL) {
        count = configSet != NULL ? 1 : support->itemGetCount(support);

        if (count > 0) {
            ConnMode = HTTP_CMODE_PERSISTENT;
            if ((HttpSocket = CompatAttemptConnection()) >= 0) {
                // Update compatibility list.
                for (i = 0; !CompatUpdateStopFlag && result >= 0 && i < count; i++, CompatUpdateComplete++) {
                    startup = support->itemGetStartup(support, configSet != NULL ? id : i);

                    if (ConnMode == HTTP_CMODE_CLOSED) {
                        ConnMode = HTTP_CMODE_PERSISTENT;
                        if ((HttpSocket = CompatAttemptConnection()) < 0) {
                            result = HttpSocket | EOPLCONNERR;
                            break;
                        }
                    }

                    itemConfig = configSet != NULL ? configSet : support->itemGetConfig(support, i);
                    if (itemConfig != NULL) {
                        ConfigSource = CONFIG_SOURCE_DEFAULT;
                        if ((mode & COMPAT_UPD_MODE_UPD_USR) || !configGetInt(itemConfig, CONFIG_ITEM_CONFIGSOURCE, &ConfigSource) || ConfigSource != CONFIG_SOURCE_USER) {
                            if (!(mode & COMPAT_UPD_MODE_NO_MTIME) && (ConfigSource == CONFIG_SOURCE_DLOAD) && configGetStat(itemConfig, &stat)) { // Only perform a stat operation for downloaded setting files.
                                if (!(mode & COMPAT_UPD_MODE_MTIME_GMT)) {
                                    clock.second = itob(stat.mtime[1]);
                                    clock.minute = itob(stat.mtime[2]);
                                    clock.hour = itob(stat.mtime[3]);
                                    clock.day = itob(stat.mtime[4]);
                                    clock.month = itob(stat.mtime[5]);
                                    clock.year = itob((stat.mtime[6] | ((unsigned short int)stat.mtime[7] << 8)) - 2000);
                                    configConvertToGmtTime(&clock);

                                    mtime[0] = btoi(clock.year);      // Year
                                    mtime[1] = btoi(clock.month) - 1; // Month
                                    mtime[2] = btoi(clock.day) - 1;   // Day
                                    mtime[3] = btoi(clock.hour);      // Hour
                                    mtime[4] = btoi(clock.minute);    // Minute
                                    mtime[5] = btoi(clock.second);    // Second
                                } else {
                                    mtime[0] = (stat.mtime[6] | ((unsigned short int)stat.mtime[7] << 8)) - 2000; // Year
                                    mtime[1] = stat.mtime[5] - 1;                                                 // Month
                                    mtime[2] = stat.mtime[4] - 1;                                                 // Day
                                    mtime[3] = stat.mtime[3];                                                     // Hour
                                    mtime[4] = stat.mtime[2];                                                     // Minute
                                    mtime[5] = stat.mtime[1];                                                     // Second
                                }
                                hasMtime = 1;

                                LOG("CompatUpdate: LAST MTIME %04u/%02u/%02u %02u:%02u:%02u\n", (unsigned short int)mtime[0] + 2000, mtime[1] + 1, mtime[2] + 1, mtime[3], mtime[4], mtime[5]);
                            } else {
                                hasMtime = 0;
                            }

                            sprintf(uri, OPL_COMPAT_HTTP_URI, startup, device);
                            for (retries = OPL_COMPAT_HTTP_RETRIES; !CompatUpdateStopFlag && retries > 0; retries--) {
                                length = HTTP_IOBUF_SIZE;
                                result = HttpSendGetRequest(HttpSocket, OPL_USER_AGENT, OPL_COMPAT_HTTP_HOST, &ConnMode, hasMtime ? mtime : NULL, uri, HttpBuffer, &length);
                                if (result >= 0) {
                                    if (result == 200) {
                                        if ((downloadedConfig = configAlloc(0, NULL, NULL)) != NULL) {
                                            configReadBuffer(downloadedConfig, HttpBuffer, length);
                                            configMerge(itemConfig, downloadedConfig);
                                            configFree(downloadedConfig);
                                            configSetInt(itemConfig, CONFIG_ITEM_CONFIGSOURCE, CONFIG_SOURCE_DLOAD);
                                            if (!configWrite(itemConfig))
                                                result = -EIO;
                                        } else
                                            result = -ENOMEM;
                                    }

                                    break;
                                } else
                                    result |= EOPLCONNERR;

                                HttpCloseConnection(HttpSocket);

                                LOG("CompatUpdate: Connection lost. Retrying.\n");

                                // Connection lost. Attempt to re-connect.
                                ConnMode = HTTP_CMODE_PERSISTENT;
                                if ((HttpSocket = CompatAttemptConnection()) < 0) {
                                    result = HttpSocket | EOPLCONNERR;
                                    break;
                                }
                            }

                            LOG("CompatUpdate %d. %d, %s: %s %d\n", i + 1, device, startup, ConnMode == HTTP_CMODE_CLOSED ? "CLOSED" : "PERSISTENT", result);
                        } else {
                            LOG("CompatUpdate: skipping %s\n", startup);
                        }

                        if (configSet == NULL) // Do not free what is not ours.
                            configFree(itemConfig);
                    } else {
                        // Can't do anything because the config file cannot be opened/created.
                        LOG("CompatUpdate: skipping %s (no config)\n", startup);
                    }

                    if (ConnMode == HTTP_CMODE_CLOSED)
                        HttpCloseConnection(HttpSocket);
                }

                if (ConnMode == HTTP_CMODE_PERSISTENT)
                    HttpCloseConnection(HttpSocket);
            } else {
                result = HttpSocket | EOPLCONNERR;
            }
        }

        free(HttpBuffer);
    } else {
        result = -ENOMEM;
    }

    if (CompatUpdateStopFlag)
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_ABORTED;
    else {
        if (result >= 0)
            CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_DONE;
        else {
            CompatUpdateStatus = (result & EOPLCONNERR) ? OPL_COMPAT_UPDATE_STAT_CONN_ERROR : OPL_COMPAT_UPDATE_STAT_ERROR;
        }
    }
    LOG("CompatUpdate: completed with status %d\n", CompatUpdateStatus);
}

static void compatDeferredUpdate(void *data)
{
    opl_io_module_t *mod = &list_support[*(short int *)data];

    compatUpdate(mod->support, CompatUpdateFlags, NULL, -1);
}

int oplGetUpdateGameCompatStatus(unsigned int *done, unsigned int *total)
{
    *done = CompatUpdateComplete;
    *total = CompatUpdateTotal;
    return CompatUpdateStatus;
}

void oplAbortUpdateGameCompat(void)
{
    CompatUpdateStopFlag = 1;
    ioRemoveRequests(IO_COMPAT_UPDATE_DEFFERED);
}

void oplUpdateGameCompat(int UpdateAll)
{
    int i, started, count;

    CompatUpdateTotal = 0;
    CompatUpdateComplete = 0;
    CompatUpdateStopFlag = 0;
    CompatUpdateFlags = UpdateAll ? (COMPAT_UPD_MODE_NO_MTIME | COMPAT_UPD_MODE_UPD_USR) : 0;
    CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_WIP;

    // Schedule compatibility updates of all the list handlers
    for (i = 0, started = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support && list_support[i].support->enabled && !(list_support[i].support->flags & MODE_FLAG_NO_UPDATE) && (count = list_support[i].support->itemGetCount(list_support[i].support)) > 0) {
            CompatUpdateTotal += count;
            ioPutRequest(IO_COMPAT_UPDATE_DEFFERED, &list_support[i].support->mode);
            started++;

            LOG("CompatUpdate: started for mode %d (%d games)\n", list_support[i].support->mode, count);
        }
    }

    if (started < 1) // Nothing done
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_DONE;
}

static int CompatUpdSingleID, CompatUpdSingleStatus;
static item_list_t *CompatUpdSingleSupport;
static config_set_t *CompatUpdSingleConfigSet;

static void _updateCompatSingle(void)
{
    compatUpdate(CompatUpdSingleSupport, COMPAT_UPD_MODE_UPD_USR, CompatUpdSingleConfigSet, CompatUpdSingleID);
    CompatUpdSingleStatus = 0;
}

int oplUpdateGameCompatSingle(int id, item_list_t *support, config_set_t *configSet)
{
    CompatUpdateStopFlag = 0;
    CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_WIP;
    CompatUpdateTotal = 1;
    CompatUpdateComplete = 0;
    CompatUpdSingleID = id;
    CompatUpdSingleSupport = support;
    CompatUpdSingleConfigSet = configSet;
    CompatUpdSingleStatus = 1;

    guiHandleDeferedIO(&CompatUpdSingleStatus, _l(_STR_PLEASE_WAIT), IO_CUSTOM_SIMPLEACTION, &_updateCompatSingle);

    return CompatUpdateStatus;
}

// ----------------------------------------------------------
// -------------------- NBD SRV Support ---------------------
// ----------------------------------------------------------


static int loadLwnbdSvr(void)
{
    int ret, padStatus;
    struct lwnbd_config
    {
        char defaultexport[32];
        uint8_t readonly;
    };
    struct lwnbd_config config;

    // deint audio lib while nbd server is running
    audioEnd();

    // block all io ops, wait for the ones still running to finish
    ioBlockOps(1);
    guiExecDeferredOps();

    // Deinitialize all support without shutting down the HDD unit.
    deinitAllSupport(NO_EXCEPTION, IO_MODE_SELECTED_ALL);
    clearErrorMessage(); /* At this point, an error might have been displayed (since background tasks were completed).
                            Clear it, otherwise it will get displayed after the server is closed. */

    unloadPads();
    // sysReset(0); // usefull ? printf doesn't work with it.

    /* compat stuff for user not providing name export (useless when there was only one export) */
    ret = strlen(gExportName);
    if (ret == 0)
        strcpy(config.defaultexport, "hdd0");
    else
        strcpy(config.defaultexport, gExportName);

    config.readonly = !gEnableWrite;

    // see gETHStartMode, gNetworkStartup ? this is slow, so if we don't have to do it (like debug build).
    ret = ethLoadInitModules();
    if (ret == 0) {
        ret = sysLoadModuleBuffer(&ps2atad_irx, size_ps2atad_irx, 0, NULL); /* gHDDStartMode ? */
        if (ret >= 0) {
            ret = sysLoadModuleBuffer(&lwnbdsvr_irx, size_lwnbdsvr_irx, sizeof(config), (char *)&config);
            if (ret >= 0)
                ret = 0;
        }
    }

    padInit(0);

    // init all pads
    padStatus = 0;
    while (!padStatus)
        padStatus = startPads();

    // now ready to display some status

    return ret;
}

static void unloadLwnbdSvr(void)
{
    ethDeinitModules();
    unloadPads();

    reset();

    LOG_INIT();
    LOG_ENABLE();

    // reinit the input pads
    padInit(0);

    int ret = 0;
    while (!ret)
        ret = startPads();

    // now start io again
    ioBlockOps(0);

    // init all supports again
    initAllSupport(1);

    audioInit();
    sfxInit(0);
    if (gEnableBGM)
        bgmStart();
}

void handleLwnbdSrv()
{
    char temp[256];
    // prepare for lwnbd, display screen with info
    guiRenderTextScreen(_l(_STR_STARTINGNBD));
    if (loadLwnbdSvr() == 0) {
        snprintf(temp, sizeof(temp), "%s", _l(_STR_RUNNINGNBD));
        guiMsgBox(temp, 0, NULL);
    } else
        guiMsgBox(_l(_STR_STARTFAILNBD), 0, NULL);

    // restore normal functionality again
    guiRenderTextScreen(_l(_STR_UNLOADNBD));
    unloadLwnbdSvr();
}

// ----------------------------------------------------------
// --------------------- Init/Deinit ------------------------
// ----------------------------------------------------------
static void reset(void)
{
    sysReset(SYS_LOAD_MC_MODULES | SYS_LOAD_USB_MODULES | SYS_LOAD_ISOFS_MODULE);

    mcInit(MC_TYPE_XMC);
}

static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected)
{
    if (!mod->support)
        return;

    // Shutdown if not required anymore.
    if ((mod->support->mode != modeSelected) && (modeSelected != IO_MODE_SELECTED_ALL)) {
        if (mod->support->itemShutdown)
            mod->support->itemShutdown(mod->support);
    } else {
        if (mod->support->itemCleanUp)
            mod->support->itemCleanUp(mod->support, exception);
    }

    clearMenuGameList(mod);
}

void deinit(int exception, int modeSelected)
{
    // block all io ops, wait for the ones still running to finish
    ioBlockOps(1);
    guiExecDeferredOps();

#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    unloadPads();

    deinitAllSupport(exception, modeSelected);

    audioEnd();
    ioEnd();
    guiEnd();
    menuEnd();
    lngEnd();
    thmEnd();
    rmEnd();
    configEnd();
}

void setDefaultColors(void)
{
    gDefaultBgColor[0] = 0x28;
    gDefaultBgColor[1] = 0xC5;
    gDefaultBgColor[2] = 0xF9;

    gDefaultTextColor[0] = 0xFF;
    gDefaultTextColor[1] = 0xFF;
    gDefaultTextColor[2] = 0xFF;

    gDefaultSelTextColor[0] = 0x00;
    gDefaultSelTextColor[1] = 0xAE;
    gDefaultSelTextColor[2] = 0xFF;

    gDefaultUITextColor[0] = 0x58;
    gDefaultUITextColor[1] = 0x68;
    gDefaultUITextColor[2] = 0xB4;
}

static void setDefaults(void)
{
    for (int i = 0; i < MODE_COUNT; i++)
        clearIOModuleT(&list_support[i]);

    gAutoLaunchGame = NULL;
    gAutoLaunchBDMGame = NULL;
    gAutoLaunchDeviceData = NULL;
    gOPLPart[0] = '\0';
    gHDDPrefix = "pfs0:";
    gBaseMCDir = "mc?:OPL";

    bdmCacheSize = 16;
    hddCacheSize = 8;
    smbCacheSize = 16;

    ps2_ip_use_dhcp = 1;
    gETHOpMode = ETH_OP_MODE_AUTO;
    gPCShareAddressIsNetBIOS = 1;
    gPCShareNBAddress[0] = '\0';
    ps2_ip[0] = 192;
    ps2_ip[1] = 168;
    ps2_ip[2] = 0;
    ps2_ip[3] = 10;
    ps2_netmask[0] = 255;
    ps2_netmask[1] = 255;
    ps2_netmask[2] = 255;
    ps2_netmask[3] = 0;
    ps2_gateway[0] = 192;
    ps2_gateway[1] = 168;
    ps2_gateway[2] = 0;
    ps2_gateway[3] = 1;
    pc_ip[0] = 192;
    pc_ip[1] = 168;
    pc_ip[2] = 0;
    pc_ip[3] = 2;
    ps2_dns[0] = 192;
    ps2_dns[1] = 168;
    ps2_dns[2] = 0;
    ps2_dns[3] = 1;
    gPCPort = 445;
    gPCShareName[0] = '\0';
    gPCUserName[0] = '\0';
    gPCPassword[0] = '\0';
    gNetworkStartup = ERROR_ETH_NOT_STARTED;
    gHDDSpindown = 20;
    gScrollSpeed = 1;
    gExitPath[0] = '\0';
    gDefaultDevice = BDM_MODE;
    gTxtRename = 1;
    gAutosort = 1;
    gAutoRefresh = 0;
    gEnableDebug = 0;
    gPS2Logo = 1;
    gHDDGameListCache = 0;
    gEnableWrite = 0;
    gRememberLastPlayed = 1;
    gAutoStartLastPlayed = 0;
    gSelectButton = KEY_CIRCLE; // Default to Japan.
    gBDMPrefix[0] = '\0';
    gETHPrefix[0] = '\0';
    gEnableNotifications = 0;
    gEnableArt = 1;
    gWideScreen = 0;
    gEnableSFX = 1;
    gEnableBootSND = 0;
    gEnableBGM = 0;
    gSFXVolume = 80;
    gBootSndVolume = 80;
    gBGMVolume = 70;
    gDefaultBGMPath[0] = '\0';
    gXSensitivity = 1;
    gYSensitivity = 1;

    gBDMStartMode = START_MODE_DISABLED;
    gHDDStartMode = START_MODE_DISABLED;
    gETHStartMode = START_MODE_DISABLED;
    gAPPStartMode = START_MODE_DISABLED;

    gEnableUSB = 1;
    gEnableILK = 0;
    gEnableMX4SIO = 0;
    gEnableBdmHDD = 0;

    frameCounter = 0;

    gVMode = 0;
    gXOff = 0;
    gYOff = 0;
    gOverscan = 0;

    setDefaultColors();

    // Last Played Auto Start
    KeyPressedOnce = 0;
    DisableCron = 1; // Auto Start Last Played counter disabled by default
    CronStart = 0;
    RemainSecs = 0;
}

static void init(void)
{
    // default variable values
    setDefaults();

    padInit(0);
    int padStatus = 0;
    configInit(NULL);

    rmInit();
    lngInit();
    thmInit();
    guiInit();
    ioInit();
    menuInit();

    startPads();

    bdmInitSemaphore();

    // compatibility update handler
    ioRegisterHandler(IO_COMPAT_UPDATE_DEFFERED, &compatDeferredUpdate);

    // handler for deffered menu updates
    ioRegisterHandler(IO_MENU_UPDATE_DEFFERED, &menuDeferredUpdate);
    cacheInit();

    gSelectButton = (InitConsoleRegionData() == CONSOLE_REGION_JAPAN) ? KEY_CIRCLE : KEY_CROSS;

    while (!padStatus)
        padStatus = startPads();
    readPads();
    if (!getKeyPressed(KEY_START)) {
        _loadConfig(); // only try to restore config if emergency key is not being pressed
    } else {
        LOG("--- SKIPPING OPL CONFIG LOADING\n");
        applyConfig(-1, -1, 0);
    }


    // queue deffered init of sound effects, which will take place after the preceding initialization steps within the queue are complete.
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &deferredAudioInit);
}

static void deferredInit(void)
{

    // inform GUI main init part is over
    struct gui_update_t *id = guiOpCreate(GUI_INIT_DONE);
    guiDeferUpdate(id);

    if (list_support[gDefaultDevice].support) {
        id = guiOpCreate(GUI_OP_SELECT_MENU);
        id->menu.menu = &list_support[gDefaultDevice].menuItem;
        guiDeferUpdate(id);
    }
}

static void deferredAudioInit(void)
{
    int ret;

    audioInit();
    ret = sfxInit(1);
    if (ret < 0)
        LOG("sfxInit: failed to initialize - %d.\n", ret);
    else
        LOG("sfxInit: %d samples loaded.\n", ret);
}

// ----------------------------------------------------------
// --------------------- Auto Loading -----------------------
// ----------------------------------------------------------

static void miniInit(int mode)
{
    int ret;

    setDefaults();
    configInit(NULL);

    ioInit();
    LOG_ENABLE();

    if (mode == BDM_MODE) {
        bdmInitSemaphore();

        // Force load iLink & mx4sio modules.. we aren't using the gui so this is fine.
        gEnableUSB = 1;
        gEnableILK = 1; // iLink will break pcsx2 however.
        gEnableMX4SIO = 1;
        gEnableBdmHDD = 1;
        bdmLoadModules();

    } else if (mode == HDD_MODE) {
        hddLoadModules();
        hddLoadSupportModules();
    }

    InitConsoleRegionData();

    ret = configReadMulti(CONFIG_ALL);
    if (CONFIG_ALL & CONFIG_OPL) {
        if (!(ret & CONFIG_OPL)) {
            if (mode == BDM_MODE)
                ret = checkLoadConfigBDM(CONFIG_ALL);
            else if (mode == HDD_MODE)
                ret = checkLoadConfigHDD(CONFIG_ALL);
        }

        if (ret & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            if (mode == BDM_MODE) {
                configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
                configGetInt(configOPL, CONFIG_OPL_BDM_CACHE, &bdmCacheSize);
            } else if (mode == HDD_MODE)
                configGetInt(configOPL, CONFIG_OPL_HDD_CACHE, &hddCacheSize);
        }
    }
}

void miniDeinit(config_set_t *configSet)
{
    ioBlockOps(1);
#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    configFree(configSet);

    ioEnd();
    configEnd();
}

static void autoLaunchHDDGame(char *argv[])
{
    char path[256];
    config_set_t *configSet;

    miniInit(HDD_MODE);

    gAutoLaunchGame = malloc(sizeof(hdl_game_info_t));
    memset(gAutoLaunchGame, 0, sizeof(hdl_game_info_t));

    snprintf(gAutoLaunchGame->startup, sizeof(gAutoLaunchGame->startup), argv[1]);
    gAutoLaunchGame->start_sector = strtoul(argv[2], NULL, 0);
    snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:%s", argv[3]);

    snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, gAutoLaunchGame->startup);
    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

    hddLaunchGame(NULL, -1, configSet);
}

static void autoLaunchBDMGame(char *argv[])
{
    char path[256];
    config_set_t *configSet;

    miniInit(BDM_MODE);

    gAutoLaunchBDMGame = malloc(sizeof(base_game_info_t));
    memset(gAutoLaunchBDMGame, 0, sizeof(base_game_info_t));

    int nameLen;
    int format = isValidIsoName(argv[1], &nameLen);
    if (format == GAME_FORMAT_OLD_ISO) {
        strncpy(gAutoLaunchBDMGame->name, &argv[1][GAME_STARTUP_MAX], nameLen);
        gAutoLaunchBDMGame->name[nameLen] = '\0';
        strncpy(gAutoLaunchBDMGame->extension, &argv[1][GAME_STARTUP_MAX + nameLen], sizeof(gAutoLaunchBDMGame->extension));
        gAutoLaunchBDMGame->extension[sizeof(gAutoLaunchBDMGame->extension) - 1] = '\0';
    } else {
        strncpy(gAutoLaunchBDMGame->name, argv[1], nameLen);
        gAutoLaunchBDMGame->name[nameLen] = '\0';
        strncpy(gAutoLaunchBDMGame->extension, &argv[1][nameLen], sizeof(gAutoLaunchBDMGame->extension));
        gAutoLaunchBDMGame->extension[sizeof(gAutoLaunchBDMGame->extension) - 1] = '\0';
    }

    snprintf(gAutoLaunchBDMGame->startup, sizeof(gAutoLaunchBDMGame->startup), argv[2]);

    if (strcasecmp("DVD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2DVD;
    else if (strcasecmp("CD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2CD;

    gAutoLaunchBDMGame->format = format;
    gAutoLaunchBDMGame->parts = 1; // ul not supported.

    gAutoLaunchDeviceData = malloc(sizeof(bdm_device_data_t));
    memset(gAutoLaunchDeviceData, 0, sizeof(bdm_device_data_t));

    char apaDevicePrefix[8] = {0};
    delay(8);
    snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass0:");
    // Loop through mass0: to mass4:
    for (int i = 0; i <= 4; i++) {
        snprintf(path, sizeof(path), "mass%d:", i);
        int dir = fileXioDopen(path);

        if (dir >= 0) {
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &gAutoLaunchDeviceData->bdmDriver, sizeof(gAutoLaunchDeviceData->bdmDriver) - 1);
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &gAutoLaunchDeviceData->massDeviceIndex, sizeof(gAutoLaunchDeviceData->massDeviceIndex));

            if (!strcmp(gAutoLaunchDeviceData->bdmDriver, "ata") && strlen(gAutoLaunchDeviceData->bdmDriver) == 3) {
                bdmResolveLBA_UDMA(gAutoLaunchDeviceData);
                snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass%d:", i);
                fileXioDclose(dir);
                break; // Exit the loop if "ata" device is found
            }

            fileXioDclose(dir);
        } else {
            // Retry for mass0: only
            if (i == 0) {
                delay(6);
                i--;
            } else {
                break;
            }
        }
        delay(6);
    }

    if (gBDMPrefix[0] != '\0') {
        snprintf(path, sizeof(path), "%s%s/CFG/%s.cfg", apaDevicePrefix, gBDMPrefix, gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "%s%s/", apaDevicePrefix, gBDMPrefix);
    } else {
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", apaDevicePrefix, gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "%s", apaDevicePrefix);
    }


    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

    bdmLaunchGame(NULL, -1, configSet);
}

// --------------------- Main --------------------
int main(int argc, char *argv[])
{
#ifdef __DECI2_DEBUG
    sysInitDECI2();
#endif

    LOG_INIT();
    PREINIT_LOG("OPL GUI start!\n");

    ChangeThreadPriority(GetThreadId(), 31);

    // reset, load modules
    reset();
    ResetDeckardXParams();

    if (argc >= 5) {
        /* argv[0] boot path
           argv[1] game->startup
           argv[2] str to u32 game->start_sector
           argv[3] opl partition read from hdd0:__common/OPL/conf_hdd.cfg
           argv[4] "mini" */
        if (!strcmp(argv[4], "mini"))
            autoLaunchHDDGame(argv);
        /* argv[0] boot path
           argv[1] file name (including extention)
           argv[2] game->startup
           argv[3] game->media ("CD" / "DVD")
           argv[4] "bdm" */
        if (!strcmp(argv[4], "bdm"))
            autoLaunchBDMGame(argv);
    }

    init();

    // until this point in the code is reached, only PREINIT_LOG macro should be used
    LOG_ENABLE();

    // queue deffered init which shuts down the intro screen later
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &deferredInit);

    guiIntroLoop();
    guiMainLoop();

    return 0;
}
