/*
 Per Game Integration by BatRastard
 PADEMU by belek666
*/

#include "include/opl.h"
#include "include/gui.h"
#include "include/ioman.h"
#include "include/lang.h"
#include "include/pad.h"
#include "include/config.h"
#include "include/ethsupport.h"
#include "include/compatupd.h"
#include "include/cheatman.h"
#include "include/system.h"
#include "include/guigame.h"
#include "include/ds34common.h"

#ifdef PADEMU
#include <libds34bt.h>
#include <libds34usb.h>
#endif

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioDevctl("genvmc:", ***)

static int configSourceID;
static int dmaMode;
static int compatMode;

static int EnableGSM;
static int GSMVMode;
static int GSMXOffset;
static int GSMYOffset;
static int GSMFIELDFix;

static int EnableCheat;
static int CheatMode;

static int forceGlobalOSDLanguage;

#ifdef PADEMU
static int EnablePadEmu;
static int PadEmuSettings;
static union
{
    struct
    {
        unsigned int left_stick_slowdown  : 4;
        unsigned int right_stick_slowdown : 4;
        unsigned int l_slowdown_enable    : 1;
        unsigned int l_slowdown_toggle    : 1;
        unsigned int r_slowdown_enable    : 1;
        unsigned int r_slowdown_toggle    : 1;
        unsigned int lx_invert            : 1;
        unsigned int ly_invert            : 1;
        unsigned int rx_invert            : 1;
        unsigned int ry_invert            : 1;
        unsigned int turbo_speed          : 2;
    };
    int raw;
} PadMacroSettings;
#endif

static char hexid[32];
static char altStartup[32];
static char vmc1[32];
static char vmc2[32];
static char hexDiscID[15];
static char configSource[128];

// forward declarations.
static void guiGameLoadGSMConfig(config_set_t *configSet, config_set_t *configGame);
static void guiGameLoadCheatsConfig(config_set_t *configSet, config_set_t *configGame);
#ifdef PADEMU
static void guiGameLoadPadEmuConfig(config_set_t *configSet, config_set_t *configGame);
static void guiGameLoadPadMacroConfig(config_set_t *configSet, config_set_t *configGame);
#endif
static int guiGameSaveOSDLanguageGameConfig(config_set_t *configSet, int result);
static void guiGameLoadOSDLanguageConfig(config_set_t *configSet, config_set_t *configGame);

static int getHighestUdmaMode()
{
    int highestMode = fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, NULL, 0, NULL, 0);
    if ((highestMode < 0) || (highestMode > 7))
        highestMode = 4;

    return highestMode;
}

int guiGameAltStartupNameHandler(char *text, int maxLen)
{
    int i;

    int result = diaShowKeyb(text, maxLen, 0, NULL);
    if (result) {
        for (i = 0; text[i]; i++) {
            if (text[i] > 96 && text[i] < 123)
                text[i] -= 32;
        }
    }

    return result;
}

char *gameConfigSource(void)
{
    char *source = configSource;
    return source;
}

// VMC
typedef struct
{                   // size = 76
    int VMC_status; // 0=available, 1=busy
    int VMC_error;
    int VMC_progress;
    char VMC_msg[64];
} statusVMCparam_t;

#define OPERATION_CREATE   0
#define OPERATION_CREATING 1
#define OPERATION_ABORTING 2
#define OPERATION_ENDING   3
#define OPERATION_END      4

static short vmc_refresh;
static int vmc_operation;
static statusVMCparam_t vmc_status;

int guiGameVmcNameHandler(char *text, int maxLen)
{
    int result = diaShowKeyb(text, maxLen, 0, NULL);

    if (result)
        vmc_refresh = 1;

    return result;
}

static int guiGameRefreshVMCConfig(item_list_t *support, char *name)
{
    int size = support->itemCheckVMC(support, name, 0);

    if (size != -1) {
        diaSetLabel(diaVMC, VMC_STATUS, _l(_STR_VMC_FILE_EXISTS));

        if (size == 8)
            diaSetInt(diaVMC, VMC_SIZE, 0);
        else if (size == 16)
            diaSetInt(diaVMC, VMC_SIZE, 1);
        else if (size == 32)
            diaSetInt(diaVMC, VMC_SIZE, 2);
        else if (size == 64)
            diaSetInt(diaVMC, VMC_SIZE, 3);
        else {
            diaSetInt(diaVMC, VMC_SIZE, 0);
            diaSetLabel(diaVMC, VMC_STATUS, _l(_STR_VMC_FILE_ERROR));
        }

        diaSetLabel(diaVMC, VMC_BUTTON_CREATE, _l(_STR_MODIFY));
        diaSetVisible(diaVMC, VMC_BUTTON_DELETE, 1);
        if (gEnableWrite) {
            diaSetEnabled(diaVMC, VMC_SIZE, 1);
            diaSetEnabled(diaVMC, VMC_BUTTON_CREATE, 1);
            diaSetEnabled(diaVMC, VMC_BUTTON_DELETE, 1);
        } else {
            diaSetEnabled(diaVMC, VMC_SIZE, 0);
            diaSetEnabled(diaVMC, VMC_BUTTON_CREATE, 0);
            diaSetEnabled(diaVMC, VMC_BUTTON_DELETE, 0);
        }
    } else {
        diaSetLabel(diaVMC, VMC_BUTTON_CREATE, _l(_STR_CREATE));
        diaSetLabel(diaVMC, VMC_STATUS, _l(_STR_VMC_FILE_NEW));

        diaSetInt(diaVMC, VMC_SIZE, 0);
        diaSetEnabled(diaVMC, VMC_SIZE, 1);
        diaSetEnabled(diaVMC, VMC_BUTTON_CREATE, 1);
        diaSetVisible(diaVMC, VMC_BUTTON_DELETE, 0);
    }

    return size;
}

static int guiGameVMCUpdater(int modified)
{
    if (vmc_refresh) {
        vmc_refresh = 0;
        return VMC_REFRESH;
    }

    if ((vmc_operation == OPERATION_CREATING) || (vmc_operation == OPERATION_ABORTING)) {
        int result = fileXioDevctl("genvmc:", 0xC0DE0003, NULL, 0, (void *)&vmc_status, sizeof(vmc_status));
        if (result == 0) {
            diaSetLabel(diaVMC, VMC_STATUS, vmc_status.VMC_msg);
            diaSetInt(diaVMC, VMC_PROGRESS, vmc_status.VMC_progress);

            if (vmc_status.VMC_error != 0)
                LOG("GUI VMCUpdater: %d\n", vmc_status.VMC_error);

            if (vmc_status.VMC_status == 0x00) {
                diaSetLabel(diaVMC, VMC_BUTTON_CREATE, _l(_STR_OK));
                vmc_operation = OPERATION_ENDING;
                return VMC_BUTTON_CREATE;
            }
        } else
            LOG("GUI Status result: %d\n", result);
    }

    return 0;
}

static int guiGameShowVMCConfig(int id, item_list_t *support, char *VMCName, int slot, int validate)
{
    int result = validate ? VMC_BUTTON_CREATE : 0;
    char vmc[32];

    if (strlen(VMCName))
        strncpy(vmc, VMCName, sizeof(vmc));
    else {
        if (validate)
            return 1; // nothing to validate if no user input

        char *startup = support->itemGetStartup(support, id);
        snprintf(vmc, sizeof(vmc), "%s_%d", startup, slot);
    }

    vmc_refresh = 0;
    vmc_operation = OPERATION_CREATE;
    diaSetEnabled(diaVMC, VMC_NAME, 1);
    diaSetEnabled(diaVMC, VMC_SIZE, 1);
    diaSetInt(diaVMC, VMC_PROGRESS, 0);

    const char *VMCSizes[] = {"8 Mb", "16 Mb", "32 Mb", "64 Mb", NULL};
    diaSetEnum(diaVMC, VMC_SIZE, VMCSizes);
    int size = guiGameRefreshVMCConfig(support, vmc);
    diaSetString(diaVMC, VMC_NAME, vmc);

    do {
        if (result == VMC_BUTTON_CREATE) {
            if (vmc_operation == OPERATION_CREATE) { // User start creation of VMC
                int sizeUI;
                diaGetInt(diaVMC, VMC_SIZE, &sizeUI);
                if (sizeUI == 1)
                    sizeUI = 16;
                else if (sizeUI == 2)
                    sizeUI = 32;
                else if (sizeUI == 3)
                    sizeUI = 64;
                else
                    sizeUI = 8;

                if (sizeUI != size) {
                    support->itemCheckVMC(support, vmc, sizeUI);

                    diaSetEnabled(diaVMC, VMC_NAME, 0);
                    diaSetEnabled(diaVMC, VMC_SIZE, 0);
                    diaSetLabel(diaVMC, VMC_BUTTON_CREATE, _l(_STR_ABORT));
                    vmc_operation = OPERATION_CREATING;
                } else
                    break;
            } else if (vmc_operation == OPERATION_ENDING) {
                if (validate)
                    break; // directly close VMC config dialog

                vmc_operation = OPERATION_END;
            } else if (vmc_operation == OPERATION_END) { // User closed creation dialog of VMC
                break;
            } else if (vmc_operation == OPERATION_CREATING) { // User canceled creation of VMC
                fileXioDevctl("genvmc:", 0xC0DE0002, NULL, 0, NULL, 0);
                vmc_operation = OPERATION_ABORTING;
            }
        } else if (result == VMC_BUTTON_DELETE) {
            if (guiMsgBox(_l(_STR_DELETE_WARNING), 1, diaVMC)) {
                support->itemCheckVMC(support, vmc, -1);
                diaSetString(diaVMC, VMC_NAME, "");
                break;
            }
        } else if (result == VMC_REFRESH) { // User changed the VMC name
            diaGetString(diaVMC, VMC_NAME, vmc, sizeof(vmc));
            size = guiGameRefreshVMCConfig(support, vmc);
        }

        result = diaExecuteDialog(diaVMC, result, 1, &guiGameVMCUpdater);
        if ((result == 0) && (vmc_operation == OPERATION_CREATE))
            break;

    } while (1);

    return result;
}

void guiGameShowVMCMenu(int id, item_list_t *support)
{
    int result = -1;

    // show dialog
    do {
        diaSetLabel(diaVMCConfig, COMPAT_VMC1_DEFINE, vmc1);
        diaSetLabel(diaVMCConfig, COMPAT_VMC2_DEFINE, vmc2);

        if (strlen(vmc1))
            diaSetLabel(diaVMCConfig, COMPAT_VMC1_ACTION, _l(_STR_RESET));
        else
            diaSetLabel(diaVMCConfig, COMPAT_VMC1_ACTION, _l(_STR_USE_GENERIC));
        if (strlen(vmc2))
            diaSetLabel(diaVMCConfig, COMPAT_VMC2_ACTION, _l(_STR_RESET));
        else
            diaSetLabel(diaVMCConfig, COMPAT_VMC2_ACTION, _l(_STR_USE_GENERIC));

        result = diaExecuteDialog(diaVMCConfig, result, 1, NULL);
        if (result == COMPAT_VMC1_DEFINE) {
            if (menuCheckParentalLock() == 0) {
                if (guiGameShowVMCConfig(id, support, vmc1, 0, 0))
                    diaGetString(diaVMC, VMC_NAME, vmc1, sizeof(vmc1));
            }
        } else if (result == COMPAT_VMC2_DEFINE) {
            if (menuCheckParentalLock() == 0) {
                if (guiGameShowVMCConfig(id, support, vmc2, 1, 0))
                    diaGetString(diaVMC, VMC_NAME, vmc2, sizeof(vmc2));
            }
        } else if (result == COMPAT_VMC1_ACTION) {
            if (menuCheckParentalLock() == 0) {
                if (strlen(vmc1))
                    vmc1[0] = '\0';
                else
                    snprintf(vmc1, sizeof(vmc1), "generic_%d", 0);
            }
        } else if (result == COMPAT_VMC2_ACTION) {
            if (menuCheckParentalLock() == 0) {
                if (strlen(vmc2))
                    vmc2[0] = '\0';
                else
                    snprintf(vmc2, sizeof(vmc2), "generic_%d", 1);
            }
        }
    } while (result >= COMPAT_NOEXIT);

    guiGameShowVMCConfig(id, support, vmc1, 0, 1);
    guiGameShowVMCConfig(id, support, vmc2, 1, 1);
}

// GSM
static void guiGameSetGSMSettingsState(void)
{
    int previousSource = gGSMSource;

    diaGetInt(diaGSConfig, GSMCFG_GSMSOURCE, &gGSMSource);

    // update GUI to display per-game or global settings if changed
    if (previousSource != gGSMSource && gGSMSource == SETTINGS_GLOBAL) {
        config_set_t *configSet = gameMenuLoadConfig(diaGSConfig);
        configRemoveKey(configSet, CONFIG_ITEM_GSMSOURCE);
        guiGameLoadGSMConfig(configSet, configGetByType(CONFIG_GAME));
    } else if (previousSource != gGSMSource && gGSMSource == SETTINGS_PERGAME) {
        config_set_t *configSet = gameMenuLoadConfig(diaGSConfig);
        configSetInt(configSet, CONFIG_ITEM_GSMSOURCE, gGSMSource);
        guiGameLoadGSMConfig(configSet, configGetByType(CONFIG_GAME));
    }

    diaGetInt(diaGSConfig, GSMCFG_ENABLEGSM, &EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMVMODE, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMXOFFSET, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMYOFFSET, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMFIELDFIX, EnableGSM);
}

static int guiGameGSMUpdater(int modified)
{
    if (modified) {
        guiGameSetGSMSettingsState();
    }

    return 0;
}

void guiGameShowGSConfig(void)
{
    // configure the enumerations
    const char *settingsSource[] = {_l(_STR_GLOBAL_SETTINGS), _l(_STR_PERGAME_SETTINGS), NULL};
    // clang-format off
    const char *gsmvmodeNames[] = {
        "NTSC",
        "NTSC Non Interlaced",
        "PAL",
        "PAL Non Interlaced",
        "PAL @60Hz",
        "PAL @60Hz Non Interlaced",
        "PS1 NTSC (HDTV 480p @60Hz)",
        "PS1 PAL (HDTV 576p @50Hz)",
        "HDTV 480p @60Hz",
        "HDTV 576p @50Hz",
        "HDTV 720p @60Hz",
        "HDTV 1080i @60Hz",
        "HDTV 1080i @60Hz Non Interlaced",
        "VGA 640x480p @60Hz",
        "VGA 640x480p @72Hz",
        "VGA 640x480p @75Hz",
        "VGA 640x480p @85Hz",
        "VGA 640x960i @60Hz",
        "VGA 800x600p @56Hz",
        "VGA 800x600p @60Hz",
        "VGA 800x600p @72Hz",
        "VGA 800x600p @75Hz",
        "VGA 800x600p @85Hz",
        "VGA 1024x768p @60Hz",
        "VGA 1024x768p @70Hz",
        "VGA 1024x768p @75Hz",
        "VGA 1024x768p @85Hz",
        "VGA 1280x1024p @60Hz",
        "VGA 1280x1024p @75Hz",
        NULL};
    // clang-format on

    diaSetEnum(diaGSConfig, GSMCFG_GSMSOURCE, settingsSource);
    diaSetEnum(diaGSConfig, GSMCFG_GSMVMODE, gsmvmodeNames);

    diaSetEnabled(diaGSConfig, GSMCFG_GSMVMODE, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMXOFFSET, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMYOFFSET, EnableGSM);
    diaSetEnabled(diaGSConfig, GSMCFG_GSMFIELDFIX, EnableGSM);

    diaExecuteDialog(diaGSConfig, -1, 1, &guiGameGSMUpdater);
}

// CHEATS
static void guiGameSetCheatSettingsState(void)
{
    int previousSource = gCheatSource;

    diaGetInt(diaCheatConfig, CHTCFG_CHEATSOURCE, &gCheatSource);

    // update GUI to display per-game or global settings if changed
    if (previousSource != gCheatSource && gCheatSource == SETTINGS_GLOBAL) {
        config_set_t *configSet = gameMenuLoadConfig(diaCheatConfig);
        configRemoveKey(configSet, CONFIG_ITEM_CHEATSSOURCE);
        guiGameLoadCheatsConfig(configSet, configGetByType(CONFIG_GAME));
    } else if (previousSource != gCheatSource && gCheatSource == SETTINGS_PERGAME) {
        config_set_t *configSet = gameMenuLoadConfig(diaCheatConfig);
        configSetInt(configSet, CONFIG_ITEM_CHEATSSOURCE, gCheatSource);
        guiGameLoadCheatsConfig(configSet, configGetByType(CONFIG_GAME));
    }

    diaGetInt(diaCheatConfig, CHTCFG_ENABLECHEAT, &EnableCheat);
    diaGetInt(diaCheatConfig, CHTCFG_CHEATMODE, &CheatMode);
    diaSetEnabled(diaCheatConfig, CHTCFG_CHEATMODE, EnableCheat);
}

static int guiGameCheatUpdater(int modified)
{
    if (modified) {
        guiGameSetCheatSettingsState();
    }

    return 0;
}

void guiGameShowCheatConfig(void)
{
    // configure the enumerations
    const char *settingsSource[] = {_l(_STR_GLOBAL_SETTINGS), _l(_STR_PERGAME_SETTINGS), NULL};
    const char *cheatmodeNames[] = {_l(_STR_CHEATMODEAUTO), _l(_STR_CHEATMODESELECT), NULL};

    diaSetEnum(diaCheatConfig, CHTCFG_CHEATSOURCE, settingsSource);
    diaSetEnum(diaCheatConfig, CHTCFG_CHEATMODE, cheatmodeNames);
    diaSetEnabled(diaCheatConfig, CHTCFG_CHEATMODE, EnableCheat);

    diaExecuteDialog(diaCheatConfig, -1, 1, &guiGameCheatUpdater);
}

// PADEMU
#ifdef PADEMU
// from https://www.bluetooth.com/specifications/assigned-numbers/host-controller-interface
static char *bt_ver_str[] = {
    "1.0b",
    "1.1",
    "1.2",
    "2.0 + EDR",
    "2.1 + EDR",
    "3.0 + HS",
    "4.0",
    "4.1",
    "4.2",
    "5.0",
    "5.1",
    "5.2",
    "5.3",
    "5.4",
};

static const char *PadEmuPorts_enums[][5] = {
    {"1P", "2P", NULL, NULL, NULL},
    {"1A", "1B", "1C", "1D", NULL},
    {"2A", "2B", "2C", "2D", NULL},
};

#define BtnBit_Off 16
// This enum, along with lookup tables, can be re-used anywhere an enum containing all PS2 buttons is necessary.
static const char *button_names_enum[] = {
    "Off",
    "Up", "Down", "Left", "Right",
    "L1", "R1", "L2", "R2",
    "Cross", "Circle", "Square", "Triangle",
    "L3", "R3", "Start", "Select",
    NULL};

static const char button_enum_to_bit_number[] = {
    BtnBit_Off, // Off
    DS2BtnBit_Up,
    DS2BtnBit_Down,
    DS2BtnBit_Left,
    DS2BtnBit_Right,
    DS2BtnBit_L1,
    DS2BtnBit_R1,
    DS2BtnBit_L2,
    DS2BtnBit_R2,
    DS2BtnBit_Cross,
    DS2BtnBit_Circle,
    DS2BtnBit_Square,
    DS2BtnBit_Triangle,
    DS2BtnBit_L3,
    DS2BtnBit_R3,
    DS2BtnBit_Start,
    DS2BtnBit_Select,
};

static const char bit_number_to_button_enum[] = {
    [BtnBit_Off] = 0, // Off
    [DS2BtnBit_Up] = 1,
    [DS2BtnBit_Down] = 2,
    [DS2BtnBit_Left] = 3,
    [DS2BtnBit_Right] = 4,
    [DS2BtnBit_L1] = 5,
    [DS2BtnBit_R1] = 6,
    [DS2BtnBit_L2] = 7,
    [DS2BtnBit_R2] = 8,
    [DS2BtnBit_Cross] = 9,
    [DS2BtnBit_Circle] = 10,
    [DS2BtnBit_Square] = 11,
    [DS2BtnBit_Triangle] = 12,
    [DS2BtnBit_L3] = 13,
    [DS2BtnBit_R3] = 14,
    [DS2BtnBit_Start] = 15,
    [DS2BtnBit_Select] = 16,
};


static u8 ds3_mac[6];
static u8 dg_mac[6];
static char ds3_str[18];
static char dg_str[18];
static char vid_str[5];
static char pid_str[5];
static char rev_str[5];
static char hci_str[26];
static char lmp_str[26];
static char man_str[5];
static int ds3macset = 0;
static int dgmacset = 0;
static int dg_discon = 0;
static int ver_set = 0, feat_set = 0;

static int forceGlobalPadEmu;
static int forceGlobalPadMacro;

static char *bdaddr_to_str(u8 *bdaddr, char *addstr)
{
    snprintf(addstr, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bdaddr[0], bdaddr[1], bdaddr[2], bdaddr[3], bdaddr[4], bdaddr[5]);

    return addstr;
}

static char *hex_to_str(char *str, u16 hex)
{
    sprintf(str, "%04X", hex);

    return str;
}

static char *ver_to_str(char *str, u8 ma, u16 mi)
{
    if (ma > 13)
        ma = 0;

    sprintf(str, "%X.%04X    BT %s", ma, mi, bt_ver_str[ma]);

    return str;
}

static int guiGamePadEmuUpdater(int modified)
{
    int PadEmuMode, PadPort, PadEmuVib, PadEmuPort, PadEmuMtap, PadEmuMtapPort, PadEmuWorkaround;
    static int oldPadPort;
    int previousSource = gPadEmuSource;

    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_SOURCE, &gPadEmuSource);

    // update GUI to display per-game or global settings if changed
    if (previousSource != gPadEmuSource && gPadEmuSource == SETTINGS_GLOBAL) {
        config_set_t *configSet = gameMenuLoadConfig(diaPadEmuConfig);
        configRemoveKey(configSet, CONFIG_ITEM_PADEMUSOURCE);
        guiGameLoadPadEmuConfig(configSet, configGetByType(CONFIG_GAME));
    } else if (previousSource != gPadEmuSource && gPadEmuSource == SETTINGS_PERGAME) {
        config_set_t *configSet = gameMenuLoadConfig(diaPadEmuConfig);
        configSetInt(configSet, CONFIG_ITEM_PADEMUSOURCE, gPadEmuSource);
        guiGameLoadPadEmuConfig(configSet, configGetByType(CONFIG_GAME));
    }

    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_ENABLE, &EnablePadEmu);
    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_MODE, &PadEmuMode);
    diaGetInt(diaPadEmuConfig, PADCFG_PADPORT, &PadPort);
    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_PORT, &PadEmuPort);
    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_VIB, &PadEmuVib);

    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_MTAP, &PadEmuMtap);
    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_MTAP_PORT, &PadEmuMtapPort);
    diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND, &PadEmuWorkaround);

    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_MTAP, EnablePadEmu);
    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_MTAP_PORT, PadEmuMtap);

    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_MODE, EnablePadEmu);

    diaSetEnabled(diaPadEmuConfig, PADCFG_PADPORT, EnablePadEmu);
    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_VIB, PadEmuPort & EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_USBDG_MAC, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAD_MAC, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAIR, (PadEmuMode == 1) & EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_USBDG_MAC_STR, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAD_MAC_STR, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAIR_STR, (PadEmuMode == 1) & EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_BTINFO, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND, (PadEmuMode == 1) & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND_STR, (PadEmuMode == 1) & EnablePadEmu);

    if (modified) {
        if (PadEmuMtap) {
            diaSetEnum(diaPadEmuConfig, PADCFG_PADPORT, PadEmuPorts_enums[PadEmuMtapPort]);
            diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_PORT, (PadPort == 0) & EnablePadEmu);
            PadEmuSettings |= 0x00000E00;
        } else {
            diaSetEnum(diaPadEmuConfig, PADCFG_PADPORT, PadEmuPorts_enums[0]);
            diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_PORT, EnablePadEmu);
            PadEmuSettings &= 0xFFFF03FF;
            if (PadPort > 1) {
                PadPort = 0;
                diaSetInt(diaPadEmuConfig, PADCFG_PADPORT, PadPort);
            }
        }

        if (PadPort != oldPadPort) {
            diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_PORT, (PadEmuSettings >> (8 + PadPort)) & 1);
            diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_VIB, (PadEmuSettings >> (16 + PadPort)) & 1);

            oldPadPort = PadPort;
        }
    }

    PadEmuSettings |= PadEmuMode | (PadEmuPort << (8 + PadPort)) | (PadEmuVib << (16 + PadPort)) | (PadEmuMtap << 24) | ((PadEmuMtapPort - 1) << 25) | (PadEmuWorkaround << 26);
    PadEmuSettings &= (~(PadEmuMode ? 0 : 1) & ~(!PadEmuPort << (8 + PadPort)) & ~(!PadEmuVib << (16 + PadPort)) & ~(!PadEmuMtap << 24) & ~(!(PadEmuMtapPort - 1) << 25) & ~(!PadEmuWorkaround << 26));

    if (PadEmuMode == 1) {
        if (ds34bt_get_status(0) & DS34BT_STATE_USB_CONFIGURED) {
            if (dg_discon) {
                dgmacset = 0;
                dg_discon = 0;
            }
            if (!dgmacset) {
                if (ds34bt_get_bdaddr(dg_mac)) {
                    dgmacset = 1;
                    diaSetLabel(diaPadEmuConfig, PADCFG_USBDG_MAC, bdaddr_to_str(dg_mac, dg_str));
                } else {
                    dgmacset = 0;
                }
            }
        } else {
            dg_discon = 1;
        }

        if (!dgmacset) {
            diaSetLabel(diaPadEmuConfig, PADCFG_USBDG_MAC, _l(_STR_NOT_CONNECTED));
        }

        if (ds34usb_get_status(0) & DS34USB_STATE_RUNNING) {
            if (!ds3macset) {
                if (ds34usb_get_bdaddr(0, ds3_mac)) {
                    ds3macset = 1;
                    diaSetLabel(diaPadEmuConfig, PADCFG_PAD_MAC, bdaddr_to_str(ds3_mac, ds3_str));
                } else {
                    ds3macset = 0;
                }
            }
        } else {
            diaSetLabel(diaPadEmuConfig, PADCFG_PAD_MAC, _l(_STR_NOT_CONNECTED));
            ds3macset = 0;
        }
    }

    return 0;
}

static int guiGamePadEmuInfoUpdater(int modified)
{
    hci_information_t info;
    u8 feat[8];
    int i, j;
    u8 data;
    int supported;

    if (ds34bt_get_status(0) & DS34BT_STATE_USB_CONFIGURED) {
        if (!ver_set) {
            if (ds34bt_get_version(&info)) {
                ver_set = 1;
                diaSetLabel(diaPadEmuInfo, PADCFG_VID, hex_to_str(vid_str, info.vid));
                diaSetLabel(diaPadEmuInfo, PADCFG_PID, hex_to_str(pid_str, info.pid));
                diaSetLabel(diaPadEmuInfo, PADCFG_REV, hex_to_str(rev_str, info.rev));
                diaSetLabel(diaPadEmuInfo, PADCFG_HCIVER, ver_to_str(hci_str, info.hci_ver, info.hci_rev));
                diaSetLabel(diaPadEmuInfo, PADCFG_LMPVER, ver_to_str(lmp_str, info.lmp_ver, info.lmp_subver));
                diaSetLabel(diaPadEmuInfo, PADCFG_MANID, hex_to_str(man_str, info.mf_name));
            } else {
                ver_set = 0;
            }
        }

        if (!feat_set) {
            if (ds34bt_get_features(feat)) {
                feat_set = 1;
                supported = 0;
                for (i = 0, j = 0; i < 64; i++) {
                    data = (feat[j] >> (i - j * 8)) & 1;
                    diaSetLabel(diaPadEmuInfo, PADCFG_FEAT_START + i, _l(_STR_NO - data));
                    j = (i + 1) / 8;
                    if (i == 25 || i == 26 || i == 39) {
                        if (data)
                            supported++;
                    }
                }
                if (supported == 3)
                    diaSetLabel(diaPadEmuInfo, PADCFG_BT_SUPPORTED, _l(_STR_BT_SUPPORTED));
                else
                    diaSetLabel(diaPadEmuInfo, PADCFG_BT_SUPPORTED, _l(_STR_BT_NOTSUPPORTED));
            } else {
                feat_set = 0;
            }
        }
    } else {
        ver_set = 0;
        feat_set = 0;
    }

    return 0;
}

void guiGameShowPadEmuConfig(int forceGlobal)
{
    const char *settingsSource[] = {_l(_STR_GLOBAL_SETTINGS), _l(_STR_PERGAME_SETTINGS), NULL};
    const char *PadEmuModes[] = {_l(_STR_DS34USB_MODE), _l(_STR_DS34BT_MODE), NULL};

    int PadEmuMtap, PadEmuMtapPort, i;

    forceGlobalPadEmu = forceGlobal;
    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_SOURCE, !forceGlobalPadEmu);

    if (forceGlobalPadEmu)
        guiGameLoadPadEmuConfig(NULL, configGetByType(CONFIG_GAME));

    diaSetEnum(diaPadEmuConfig, PADCFG_PADEMU_SOURCE, settingsSource);
    diaSetEnum(diaPadEmuConfig, PADCFG_PADEMU_MODE, PadEmuModes);

    PadEmuMtap = (PadEmuSettings >> 24) & 1;
    PadEmuMtapPort = ((PadEmuSettings >> 25) & 1) + 1;

    diaSetEnabled(diaPadEmuConfig, PADCFG_PADEMU_PORT, EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_USBDG_MAC, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAD_MAC, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAIR, PadEmuSettings & EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_USBDG_MAC_STR, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAD_MAC_STR, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PAIR_STR, PadEmuSettings & EnablePadEmu);

    diaSetVisible(diaPadEmuConfig, PADCFG_BTINFO, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND, PadEmuSettings & EnablePadEmu);
    diaSetVisible(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND_STR, PadEmuSettings & EnablePadEmu);

    if (PadEmuMtap) {
        diaSetEnum(diaPadEmuConfig, PADCFG_PADPORT, PadEmuPorts_enums[PadEmuMtapPort]);
        PadEmuSettings |= 0x00000E00;
    } else {
        diaSetEnum(diaPadEmuConfig, PADCFG_PADPORT, PadEmuPorts_enums[0]);
        PadEmuSettings &= 0xFFFF03FF;
    }

    int result = -1;

    while (result != 0) {
        result = diaExecuteDialog(diaPadEmuConfig, result, 1, &guiGamePadEmuUpdater);

        if (result == PADCFG_PAIR) {
            if (ds3macset && dgmacset) {
                if (ds34usb_get_status(0) & DS34USB_STATE_RUNNING) {
                    if (ds34usb_set_bdaddr(0, dg_mac))
                        ds3macset = 0;
                }
            }
        }

        if (result == PADCFG_BTINFO) {
            for (i = PADCFG_FEAT_START; i < PADCFG_FEAT_END + 1; i++)
                diaSetLabel(diaPadEmuInfo, i, _l(_STR_NO));

            diaSetLabel(diaPadEmuInfo, PADCFG_VID, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_PID, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_REV, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_HCIVER, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_LMPVER, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_MANID, _l(_STR_NOT_CONNECTED));
            diaSetLabel(diaPadEmuInfo, PADCFG_BT_SUPPORTED, _l(_STR_NOT_CONNECTED));
            ver_set = 0;
            feat_set = 0;
            diaExecuteDialog(diaPadEmuInfo, -1, 1, &guiGamePadEmuInfoUpdater);
        }

        if (result == UIID_BTN_OK)
            break;
    }
}

static int guiGamePadMacroUpdater(int modified)
{
    int previousSource = gPadMacroSource;
    diaGetInt(diaPadMacroConfig, PADMACRO_CFG_SOURCE, &gPadMacroSource);

    // update GUI to display per-game or global settings if changed
    if (previousSource != gPadMacroSource && gPadMacroSource == SETTINGS_GLOBAL) {
        config_set_t *configSet = gameMenuLoadConfig(diaPadMacroConfig);
        configRemoveKey(configSet, CONFIG_ITEM_PADMACROSOURCE);
        guiGameLoadPadMacroConfig(configSet, configGetByType(CONFIG_GAME));
    } else if (previousSource != gPadMacroSource && gPadMacroSource == SETTINGS_PERGAME) {
        config_set_t *configSet = gameMenuLoadConfig(diaPadMacroConfig);
        configSetInt(configSet, CONFIG_ITEM_PADMACROSOURCE, gPadMacroSource);
        guiGameLoadPadMacroConfig(configSet, configGetByType(CONFIG_GAME));
    }

    int slowdown_l, slowdown_r;
    diaGetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_L, &slowdown_l);
    PadMacroSettings.l_slowdown_enable = slowdown_l != 0 ? 1 : 0;
    PadMacroSettings.left_stick_slowdown = button_enum_to_bit_number[slowdown_l];

    diaGetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_R, &slowdown_r);
    PadMacroSettings.r_slowdown_enable = slowdown_r != 0 ? 1 : 0;
    PadMacroSettings.right_stick_slowdown = button_enum_to_bit_number[slowdown_r];

    int toggle_l, toggle_r;
    diaGetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_L, &toggle_l);
    PadMacroSettings.l_slowdown_toggle = toggle_l;
    diaGetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_R, &toggle_r);
    PadMacroSettings.r_slowdown_toggle = toggle_r;

    int lx_invert, ly_invert, rx_invert, ry_invert;
    diaGetInt(diaPadMacroConfig, PADMACRO_INVERT_LX, &lx_invert);
    PadMacroSettings.lx_invert = lx_invert;
    diaGetInt(diaPadMacroConfig, PADMACRO_INVERT_LY, &ly_invert);
    PadMacroSettings.ly_invert = ly_invert;
    diaGetInt(diaPadMacroConfig, PADMACRO_INVERT_RX, &rx_invert);
    PadMacroSettings.rx_invert = rx_invert;
    diaGetInt(diaPadMacroConfig, PADMACRO_INVERT_RY, &ry_invert);
    PadMacroSettings.ry_invert = ry_invert;

    int turbo_speed;
    diaGetInt(diaPadMacroConfig, PADMACRO_TURBO_SPEED, &turbo_speed);
    PadMacroSettings.turbo_speed = 4 - turbo_speed;

    return 0;
}

void guiGameShowPadMacroConfig(int forceGlobal)
{
    const char *settingsSource[] = {_l(_STR_GLOBAL_SETTINGS), _l(_STR_PERGAME_SETTINGS), NULL};
    forceGlobalPadMacro = forceGlobal;
    diaSetEnabled(diaPadMacroConfig, PADMACRO_CFG_SOURCE, !forceGlobalPadMacro);

    if (forceGlobalPadMacro) {
        guiGameLoadPadMacroConfig(NULL, configGetByType(CONFIG_GAME));
    }

    diaSetEnum(diaPadMacroConfig, PADMACRO_CFG_SOURCE, settingsSource);
    diaSetEnum(diaPadMacroConfig, PADMACRO_SLOWDOWN_L, button_names_enum);
    diaSetEnum(diaPadMacroConfig, PADMACRO_SLOWDOWN_R, button_names_enum);
    const char *toggle_enum[] = {_l(_STR_ACT_WHILE_PRESSED), _l(_STR_ACT_TOGGLED), NULL};
    diaSetEnum(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_L, toggle_enum);
    diaSetEnum(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_R, toggle_enum);

    int result = -1;
    while (result != 0) {
        result = diaExecuteDialog(diaPadMacroConfig, result, 1, &guiGamePadMacroUpdater);

        if (result == UIID_BTN_OK)
            break;
    }
}

static int guiGameSavePadEmuGameConfig(config_set_t *configSet, int result)
{
    if (gPadEmuSource == SETTINGS_PERGAME) {
        diaGetInt(diaPadEmuConfig, PADCFG_PADEMU_ENABLE, &EnablePadEmu);

        result = configSetInt(configSet, CONFIG_ITEM_PADEMUSOURCE, gPadEmuSource);
        if (EnablePadEmu != 0)
            result = configSetInt(configSet, CONFIG_ITEM_ENABLEPADEMU, EnablePadEmu);
        else
            configRemoveKey(configSet, CONFIG_ITEM_ENABLEPADEMU);

        if (PadEmuSettings != 0)
            result = configSetInt(configSet, CONFIG_ITEM_PADEMUSETTINGS, PadEmuSettings);
        else
            configRemoveKey(configSet, CONFIG_ITEM_PADEMUSETTINGS);
    }

    return result;
}

static int guiGameSavePadMacroGameConfig(config_set_t *configSet, int result)
{
    if (gPadMacroSource == SETTINGS_PERGAME) {
        result = configSetInt(configSet, CONFIG_ITEM_PADMACROSOURCE, gPadMacroSource);
        if (PadMacroSettings.raw != 0)
            result = configSetInt(configSet, CONFIG_ITEM_PADMACROSETTINGS, PadMacroSettings.raw);
        else
            configRemoveKey(configSet, CONFIG_ITEM_PADMACROSETTINGS);
    }

    return result;
}

void guiGameSavePadEmuGlobalConfig(config_set_t *configGame)
{
    if (gPadEmuSource == SETTINGS_GLOBAL) {
        configSetInt(configGame, CONFIG_ITEM_ENABLEPADEMU, EnablePadEmu);
        configSetInt(configGame, CONFIG_ITEM_PADEMUSETTINGS, PadEmuSettings);
    }
}

void guiGameSavePadMacroGlobalConfig(config_set_t *configGame)
{
    if (gPadMacroSource == SETTINGS_GLOBAL) {
        configSetInt(configGame, CONFIG_ITEM_PADMACROSETTINGS, PadMacroSettings.raw);
    }
}
#endif

void guiGameShowCompatConfig(int id, item_list_t *support, config_set_t *configSet)
{
    int i;

    if (support->flags & MODE_FLAG_COMPAT_DMA) {
        int ataHighestUDMAMode = getHighestUdmaMode();
        if (ataHighestUDMAMode == 5) {
            const char *dmaModes[] = {"MDMA 0", "MDMA 1", "MDMA 2", "UDMA 0", "UDMA 1", "UDMA 2", "UDMA 3", "UDMA 4", "UDMA 5 检测到的最高模式", "UDMA 6", "UDMA 7 极速模式", NULL};
            diaSetEnum(diaCompatConfig, COMPAT_DMA, dmaModes);
        } else if (ataHighestUDMAMode == 6) {
            const char *dmaModes[] = {"MDMA 0", "MDMA 1", "MDMA 2", "UDMA 0", "UDMA 1", "UDMA 2", "UDMA 3", "UDMA 4", "UDMA 5", "UDMA 6 检测到的最高模式", "UDMA 7 极速模式", NULL};
            diaSetEnum(diaCompatConfig, COMPAT_DMA, dmaModes);
        } else if (ataHighestUDMAMode == 7) {
            const char *dmaModes[] = {"MDMA 0", "MDMA 1", "MDMA 2", "UDMA 0", "UDMA 1", "UDMA 2", "UDMA 3", "UDMA 4", "UDMA 5", "UDMA 6", "UDMA 7 极速模式", NULL};
            diaSetEnum(diaCompatConfig, COMPAT_DMA, dmaModes);
        } else {
            const char *dmaModes[] = {"MDMA 0", "MDMA 1", "MDMA 2", "UDMA 0", "UDMA 1", "UDMA 2", "UDMA 3", "UDMA 4 检测到的最高模式", "UDMA 5", "UDMA 6", "UDMA 7 极速模式", NULL};
            diaSetEnum(diaCompatConfig, COMPAT_DMA, dmaModes);
        }
    } else {
        const char *dmaModes[] = {NULL};
        diaSetEnum(diaCompatConfig, COMPAT_DMA, dmaModes);
    }



    int result = diaExecuteDialog(diaCompatConfig, -1, 1, NULL);
    if (result) {
        compatMode = 0;
        for (i = 0; i < COMPAT_MODE_COUNT; ++i) {
            int mdpart;
            diaGetInt(diaCompatConfig, COMPAT_MODE_BASE + i, &mdpart);
            compatMode |= (mdpart ? 1 : 0) << i;
        }

        if (result == COMPAT_LOADFROMDISC) {
            if (sysGetDiscID(hexDiscID) >= 0)
                diaSetString(diaCompatConfig, COMPAT_GAMEID, hexDiscID);
            else
                guiMsgBox(_l(_STR_ERROR_LOADING_ID), 0, NULL);
        }

        if (result == COMPAT_DL_DEFAULTS)
            guiShowNetCompatUpdateSingle(id, support, configSet);

        diaGetInt(diaCompatConfig, COMPAT_DMA, &dmaMode);
        diaGetString(diaCompatConfig, COMPAT_GAMEID, hexid, sizeof(hexid));
        diaGetString(diaCompatConfig, COMPAT_ALTSTARTUP, altStartup, sizeof(altStartup));
    }
}

// sets variables without writing to users cfg file.. follow up with menuSaveConfig() to write
int guiGameSaveConfig(config_set_t *configSet, item_list_t *support)
{
    int i;
    int result = 0;
    config_set_t *configGame = configGetByType(CONFIG_GAME);

    compatMode = 0;
    for (i = 0; i < COMPAT_MODE_COUNT; ++i) {
        int mdpart;
        diaGetInt(diaCompatConfig, COMPAT_MODE_BASE + i, &mdpart);
        compatMode |= (mdpart ? 1 : 0) << i;
    }

    if (support->flags & MODE_FLAG_COMPAT_DMA) {
        diaGetInt(diaCompatConfig, COMPAT_DMA, &dmaMode);

        //// 将默认UDMA模式设为最高UDMA模式+1
        //if (dmaMode != (getHighestUdmaMode() + 1 + 3))
        //  将默认UDMA模式设为UDMA 7
        if (dmaMode != 10)
            result = configSetInt(configSet, CONFIG_ITEM_DMA, dmaMode);
        else
            configRemoveKey(configSet, CONFIG_ITEM_DMA);
    }

    if (compatMode != 0)
        result = configSetInt(configSet, CONFIG_ITEM_COMPAT, compatMode);
    else
        configRemoveKey(configSet, CONFIG_ITEM_COMPAT);

    /// GSM ///
    diaGetInt(diaGSConfig, GSMCFG_ENABLEGSM, &EnableGSM);
    diaGetInt(diaGSConfig, GSMCFG_GSMVMODE, &GSMVMode);
    diaGetInt(diaGSConfig, GSMCFG_GSMXOFFSET, &GSMXOffset);
    diaGetInt(diaGSConfig, GSMCFG_GSMYOFFSET, &GSMYOffset);
    diaGetInt(diaGSConfig, GSMCFG_GSMFIELDFIX, &GSMFIELDFix);

    if (gGSMSource == SETTINGS_PERGAME) {
        result = configSetInt(configSet, CONFIG_ITEM_GSMSOURCE, gGSMSource);
        if (EnableGSM != 0)
            result = configSetInt(configSet, CONFIG_ITEM_ENABLEGSM, EnableGSM);
        else
            configRemoveKey(configSet, CONFIG_ITEM_ENABLEGSM);

        if (GSMVMode != 0)
            result = configSetInt(configSet, CONFIG_ITEM_GSMVMODE, GSMVMode);
        else
            configRemoveKey(configSet, CONFIG_ITEM_GSMVMODE);

        if (GSMXOffset != 0)
            result = configSetInt(configSet, CONFIG_ITEM_GSMXOFFSET, GSMXOffset);
        else
            configRemoveKey(configSet, CONFIG_ITEM_GSMXOFFSET);

        if (GSMYOffset != 0)
            result = configSetInt(configSet, CONFIG_ITEM_GSMYOFFSET, GSMYOffset);
        else
            configRemoveKey(configSet, CONFIG_ITEM_GSMYOFFSET);

        if (GSMFIELDFix != 0)
            result = configSetInt(configSet, CONFIG_ITEM_GSMFIELDFIX, GSMFIELDFix);
        else
            configRemoveKey(configSet, CONFIG_ITEM_GSMFIELDFIX);
    } else if (gGSMSource == SETTINGS_GLOBAL) {
        configSetInt(configGame, CONFIG_ITEM_ENABLEGSM, EnableGSM);
        configSetInt(configGame, CONFIG_ITEM_GSMVMODE, GSMVMode);
        configSetInt(configGame, CONFIG_ITEM_GSMXOFFSET, GSMXOffset);
        configSetInt(configGame, CONFIG_ITEM_GSMYOFFSET, GSMYOffset);
        configSetInt(configGame, CONFIG_ITEM_GSMFIELDFIX, GSMFIELDFix);
    }

    /// Cheats ///
    diaGetInt(diaCheatConfig, CHTCFG_CHEATSOURCE, &gCheatSource);
    diaGetInt(diaCheatConfig, CHTCFG_ENABLECHEAT, &EnableCheat);
    diaGetInt(diaCheatConfig, CHTCFG_CHEATMODE, &CheatMode);

    if (gCheatSource == SETTINGS_PERGAME) {
        result = configSetInt(configSet, CONFIG_ITEM_CHEATSSOURCE, gCheatSource);
        if (EnableCheat != 0)
            result = configSetInt(configSet, CONFIG_ITEM_ENABLECHEAT, EnableCheat);
        else
            configRemoveKey(configSet, CONFIG_ITEM_ENABLECHEAT);

        if (CheatMode != 0)
            result = configSetInt(configSet, CONFIG_ITEM_CHEATMODE, CheatMode);
        else
            configRemoveKey(configSet, CONFIG_ITEM_CHEATMODE);
    } else if (gCheatSource == SETTINGS_GLOBAL) {
        configSetInt(configGame, CONFIG_ITEM_ENABLECHEAT, EnableCheat);
        configSetInt(configGame, CONFIG_ITEM_CHEATMODE, CheatMode);
    }

#ifdef PADEMU
    /// PADEMU ///
    result = guiGameSavePadEmuGameConfig(configSet, result);
    guiGameSavePadEmuGlobalConfig(configGame);
    result = guiGameSavePadMacroGameConfig(configSet, result);
    guiGameSavePadMacroGlobalConfig(configGame);
#endif

    diaGetString(diaCompatConfig, COMPAT_GAMEID, hexid, sizeof(hexid));
    if (hexid[0] != '\0')
        result = configSetStr(configSet, CONFIG_ITEM_DNAS, hexid);

    diaGetString(diaCompatConfig, COMPAT_ALTSTARTUP, altStartup, sizeof(altStartup));
    if (altStartup[0] != '\0')
        result = configSetStr(configSet, CONFIG_ITEM_ALTSTARTUP, altStartup);
    else
        configRemoveKey(configSet, CONFIG_ITEM_ALTSTARTUP);

    /// VMC ///
    configSetVMC(configSet, vmc1, 0);
    configSetVMC(configSet, vmc2, 1);

    result = guiGameSaveOSDLanguageGameConfig(configSet, result);
    guiGameSaveOSDLanguageGlobalConfig(configGame);

    return result;
}

void guiGameRemoveGlobalSettings(config_set_t *configGame)
{
    if (menuCheckParentalLock() == 0) {
        // Cheats
        configRemoveKey(configGame, CONFIG_ITEM_ENABLECHEAT);
        configRemoveKey(configGame, CONFIG_ITEM_CHEATMODE);

        // GSM
        configRemoveKey(configGame, CONFIG_ITEM_ENABLEGSM);
        configRemoveKey(configGame, CONFIG_ITEM_GSMVMODE);
        configRemoveKey(configGame, CONFIG_ITEM_GSMXOFFSET);
        configRemoveKey(configGame, CONFIG_ITEM_GSMYOFFSET);
        configRemoveKey(configGame, CONFIG_ITEM_GSMFIELDFIX);
        // OSD Language
        configRemoveKey(configGame, CONFIG_ITEM_OSD_SETTINGS_LANGID);
        configRemoveKey(configGame, CONFIG_ITEM_OSD_SETTINGS_TV_ASP);
        configRemoveKey(configGame, CONFIG_ITEM_OSD_SETTINGS_VMODE);
        configRemoveKey(configGame, CONFIG_ITEM_OSD_SETTINGS_ENABLE);

#ifdef PADEMU
        // PADEMU
        configRemoveKey(configGame, CONFIG_ITEM_ENABLEPADEMU);
        configRemoveKey(configGame, CONFIG_ITEM_PADEMUSETTINGS);
        configRemoveKey(configGame, CONFIG_ITEM_PADMACROSETTINGS);
#endif
        saveConfig(CONFIG_GAME, 0);
    }
}

void guiGameRemoveSettings(config_set_t *configSet)
{
    if (menuCheckParentalLock() == 0) {
        configRemoveKey(configSet, CONFIG_ITEM_CONFIGSOURCE);
        configRemoveKey(configSet, CONFIG_ITEM_DMA);
        configRemoveKey(configSet, CONFIG_ITEM_COMPAT);
        configRemoveKey(configSet, CONFIG_ITEM_DNAS);
        configRemoveKey(configSet, CONFIG_ITEM_ALTSTARTUP);

        // GSM
        configRemoveKey(configSet, CONFIG_ITEM_GSMSOURCE);
        configRemoveKey(configSet, CONFIG_ITEM_ENABLEGSM);
        configRemoveKey(configSet, CONFIG_ITEM_GSMVMODE);
        configRemoveKey(configSet, CONFIG_ITEM_GSMXOFFSET);
        configRemoveKey(configSet, CONFIG_ITEM_GSMYOFFSET);
        configRemoveKey(configSet, CONFIG_ITEM_GSMFIELDFIX);

        // Cheats
        configRemoveKey(configSet, CONFIG_ITEM_CHEATSSOURCE);
        configRemoveKey(configSet, CONFIG_ITEM_ENABLECHEAT);
        configRemoveKey(configSet, CONFIG_ITEM_CHEATMODE);

        // OSD Language
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_LANGID);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_TV_ASP);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_VMODE);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_ENABLE);

#ifdef PADEMU
        // PADEMU
        configRemoveKey(configSet, CONFIG_ITEM_PADEMUSOURCE);
        configRemoveKey(configSet, CONFIG_ITEM_ENABLEPADEMU);
        configRemoveKey(configSet, CONFIG_ITEM_PADEMUSETTINGS);
        configRemoveKey(configSet, CONFIG_ITEM_PADMACROSETTINGS);
        configRemoveKey(configSet, CONFIG_ITEM_PADMACROSOURCE);
#endif
        // VMC
        configRemoveVMC(configSet, 0);
        configRemoveVMC(configSet, 1);

        menuSaveConfig();
    }
}

void guiGameTestSettings(int id, item_list_t *support, config_set_t *configSet)
{
    guiGameSaveConfig(configSet, support);
    support->itemLaunch(support, id, configSet);
}

static void guiGameLoadGSMConfig(config_set_t *configSet, config_set_t *configGame)
{
    EnableGSM = 0;
    GSMVMode = 0;
    GSMXOffset = 0;
    GSMYOffset = 0;
    GSMFIELDFix = 0;

    // set global settings.
    gGSMSource = 0;
    configGetInt(configGame, CONFIG_ITEM_ENABLEGSM, &EnableGSM);
    configGetInt(configGame, CONFIG_ITEM_GSMVMODE, &GSMVMode);
    configGetInt(configGame, CONFIG_ITEM_GSMXOFFSET, &GSMXOffset);
    configGetInt(configGame, CONFIG_ITEM_GSMYOFFSET, &GSMYOffset);
    configGetInt(configGame, CONFIG_ITEM_GSMFIELDFIX, &GSMFIELDFix);

    // override global with per-game settings if available and selected.
    configGetInt(configSet, CONFIG_ITEM_GSMSOURCE, &gGSMSource);
    if (gGSMSource == SETTINGS_PERGAME) {
        if (!configGetInt(configSet, CONFIG_ITEM_ENABLEGSM, &EnableGSM))
            EnableGSM = 0;
        if (!configGetInt(configSet, CONFIG_ITEM_GSMVMODE, &GSMVMode))
            GSMVMode = 0;
        if (!configGetInt(configSet, CONFIG_ITEM_GSMXOFFSET, &GSMXOffset))
            GSMXOffset = 0;
        if (!configGetInt(configSet, CONFIG_ITEM_GSMYOFFSET, &GSMYOffset))
            GSMYOffset = 0;
        if (!configGetInt(configSet, CONFIG_ITEM_GSMFIELDFIX, &GSMFIELDFix))
            GSMFIELDFix = 0;
    }

    // set gui settings.
    diaSetInt(diaGSConfig, GSMCFG_GSMSOURCE, gGSMSource);
    diaSetInt(diaGSConfig, GSMCFG_ENABLEGSM, EnableGSM);
    diaSetInt(diaGSConfig, GSMCFG_GSMVMODE, GSMVMode);
    diaSetInt(diaGSConfig, GSMCFG_GSMXOFFSET, GSMXOffset);
    diaSetInt(diaGSConfig, GSMCFG_GSMYOFFSET, GSMYOffset);
    diaSetInt(diaGSConfig, GSMCFG_GSMFIELDFIX, GSMFIELDFix);
}

static void guiGameLoadCheatsConfig(config_set_t *configSet, config_set_t *configGame)
{
    EnableCheat = 0;
    CheatMode = 0;

    // set global settings.
    gCheatSource = 0;
    configGetInt(configGame, CONFIG_ITEM_ENABLECHEAT, &EnableCheat);
    configGetInt(configGame, CONFIG_ITEM_CHEATMODE, &CheatMode);

    // override global with per-game settings if available and selected.
    configGetInt(configSet, CONFIG_ITEM_CHEATSSOURCE, &gCheatSource);
    if (gCheatSource == SETTINGS_PERGAME) {
        if (!configGetInt(configSet, CONFIG_ITEM_ENABLECHEAT, &EnableCheat))
            EnableCheat = 0;
        if (!configGetInt(configSet, CONFIG_ITEM_CHEATMODE, &CheatMode))
            CheatMode = 0;
    }

    // set gui settings.
    diaSetInt(diaCheatConfig, CHTCFG_CHEATSOURCE, gCheatSource);
    diaSetInt(diaCheatConfig, CHTCFG_ENABLECHEAT, EnableCheat);
    diaSetInt(diaCheatConfig, CHTCFG_CHEATMODE, CheatMode);
}

#ifdef PADEMU
static void guiGameLoadPadEmuConfig(config_set_t *configSet, config_set_t *configGame)
{
    EnablePadEmu = 0;
    PadEmuSettings = 0;

    // set global settings.
    gPadEmuSource = 0;
    configGetInt(configGame, CONFIG_ITEM_ENABLEPADEMU, &EnablePadEmu);
    configGetInt(configGame, CONFIG_ITEM_PADEMUSETTINGS, &PadEmuSettings);

    // override global with per-game settings if available and selected.
    if (!forceGlobalPadEmu) {
        configGetInt(configSet, CONFIG_ITEM_PADEMUSOURCE, &gPadEmuSource);
        if (gPadEmuSource == SETTINGS_PERGAME) {
            if (!configGetInt(configSet, CONFIG_ITEM_ENABLEPADEMU, &EnablePadEmu))
                EnablePadEmu = 0;
            if (!configGetInt(configSet, CONFIG_ITEM_PADEMUSETTINGS, &PadEmuSettings))
                PadEmuSettings = 0;
        }
    }
    // set gui settings.
    int PadEmuMtap = (PadEmuSettings >> 24) & 1;
    int PadEmuMtapPort = ((PadEmuSettings >> 25) & 1) + 1;

    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_SOURCE, gPadEmuSource);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_ENABLE, EnablePadEmu);

    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_MODE, PadEmuSettings & 0xFF);
    diaSetInt(diaPadEmuConfig, PADCFG_PADPORT, 0);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_PORT, (PadEmuSettings >> 8) & 1);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_VIB, (PadEmuSettings >> 16) & 1);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_MTAP, PadEmuMtap);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_MTAP_PORT, PadEmuMtapPort);
    diaSetInt(diaPadEmuConfig, PADCFG_PADEMU_WORKAROUND, ((PadEmuSettings >> 26) & 1));
}

static void guiGameLoadPadMacroConfig(config_set_t *configSet, config_set_t *configGame)
{
    PadMacroSettings.raw = 0;
    gPadMacroSource = 0;

    configGetInt(configGame, CONFIG_ITEM_PADMACROSETTINGS, &PadMacroSettings.raw);
    // override global with per-game settings if available and selected.
    if (!forceGlobalPadMacro) {
        configGetInt(configSet, CONFIG_ITEM_PADMACROSOURCE, &gPadMacroSource);
        if (gPadMacroSource == SETTINGS_PERGAME) {
            if (!configGetInt(configSet, CONFIG_ITEM_PADMACROSETTINGS, &PadMacroSettings.raw))
                PadMacroSettings.raw = 0;
        }
    }

    diaSetInt(diaPadMacroConfig, PADMACRO_CFG_SOURCE, gPadMacroSource);

    int slowdown_l_ui = 0, slowdown_r_ui = 0;
    if (PadMacroSettings.l_slowdown_enable) {
        slowdown_l_ui = bit_number_to_button_enum[PadMacroSettings.left_stick_slowdown];
    }

    if (PadMacroSettings.r_slowdown_enable) {
        slowdown_r_ui = bit_number_to_button_enum[PadMacroSettings.right_stick_slowdown];
    }

    diaSetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_L, slowdown_l_ui);
    diaSetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_R, slowdown_r_ui);
    diaSetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_L, PadMacroSettings.l_slowdown_toggle);
    diaSetInt(diaPadMacroConfig, PADMACRO_SLOWDOWN_TOGGLE_R, PadMacroSettings.r_slowdown_toggle);
    diaSetInt(diaPadMacroConfig, PADMACRO_INVERT_LX, PadMacroSettings.lx_invert);
    diaSetInt(diaPadMacroConfig, PADMACRO_INVERT_LY, PadMacroSettings.ly_invert);
    diaSetInt(diaPadMacroConfig, PADMACRO_INVERT_RX, PadMacroSettings.rx_invert);
    diaSetInt(diaPadMacroConfig, PADMACRO_INVERT_RY, PadMacroSettings.ry_invert);
    diaSetInt(diaPadMacroConfig, PADMACRO_TURBO_SPEED, 4 - PadMacroSettings.turbo_speed);
}
#endif

// OSD

static int guiGameOSDLanguageUpdater(int modified)
{
    int previousSource = gOSDLanguageSource;
    diaGetInt(diaOSDConfig, OSD_LANGUAGE_SOURCE, &gOSDLanguageSource);

    // update GUI to display per-game or global settings if changed
    if (previousSource != gOSDLanguageSource && gOSDLanguageSource == SETTINGS_GLOBAL) {
        config_set_t *configSet = gameMenuLoadConfig(diaOSDConfig);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE);
        guiGameLoadOSDLanguageConfig(configSet, configGetByType(CONFIG_GAME));
    } else if (previousSource != gOSDLanguageSource && gOSDLanguageSource == SETTINGS_PERGAME) {
        config_set_t *configSet = gameMenuLoadConfig(diaOSDConfig);
        configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE, gOSDLanguageSource);
        guiGameLoadOSDLanguageConfig(configSet, configGetByType(CONFIG_GAME));
    }

    diaGetInt(diaOSDConfig, OSD_LANGUAGE_ENABLE, &gOSDLanguageEnable);
    diaGetInt(diaOSDConfig, OSD_LANGUAGE_VALUE, &gOSDLanguageValue);
    diaGetInt(diaOSDConfig, OSD_TVASPECT_VALUE, &gOSDTVAspectRatio);
    diaGetInt(diaOSDConfig, OSD_VMODE_VALUE, &gOSDVideOutput);
    diaSetEnabled(diaOSDConfig, OSD_LANGUAGE_VALUE, gOSDLanguageEnable);
    diaSetEnabled(diaOSDConfig, OSD_TVASPECT_VALUE, gOSDLanguageEnable);
    diaSetEnabled(diaOSDConfig, OSD_VMODE_VALUE, gOSDLanguageEnable);
    return 0;
}

void guiGameShowOSDLanguageConfig(int forceGlobal)
{
    /// `_STR_SYSTEM_DEFAULT` MUST BE AT THE END OF THE LIST. ELSE, THE ENUMERATOR VALUES WILL NOT MATCH THE ONES ON OSDCONFIG. BREAKING THIS FEATURE
    const char *Lngs[] = {_l(_STR_LANGUAGE_JAPANESE), _l(_STR_LANGUAGE_ENGLISH), _l(_STR_LANGUAGE_FRENCH), _l(_STR_LANGUAGE_SPANISH), _l(_STR_LANGUAGE_GERMAN), _l(_STR_LANGUAGE_ITALIAN), _l(_STR_LANGUAGE_DUTCH), _l(_STR_LANGUAGE_PORTUGUESE), _l(_STR_SYSTEM_DEFAULT), NULL};
    const char *sources[] = {_l(_STR_GLOBAL_SETTINGS), _l(_STR_PERGAME_SETTINGS), NULL};
    const char *TV[] = {"4:3", _l(_STR_FULL_SCREEN), "16:9", _l(_STR_SYSTEM_DEFAULT), NULL};
    const char *VMOD[] = {"RGB", "Y Cb/Pb Cr/Pr", _l(_STR_SYSTEM_DEFAULT), NULL};
    diaSetEnum(diaOSDConfig, OSD_LANGUAGE_VALUE, Lngs);
    diaSetEnum(diaOSDConfig, OSD_LANGUAGE_SOURCE, sources);
    diaSetEnum(diaOSDConfig, OSD_TVASPECT_VALUE, TV);
    diaSetEnum(diaOSDConfig, OSD_VMODE_VALUE, VMOD);
    forceGlobalOSDLanguage = forceGlobal;
    diaSetEnabled(diaOSDConfig, OSD_LANGUAGE_SOURCE, !forceGlobalOSDLanguage);

    if (forceGlobalOSDLanguage) {
        guiGameLoadOSDLanguageConfig(NULL, configGetByType(CONFIG_GAME));
    }


    int result = -1;
    while (result != 0) {
        result = diaExecuteDialog(diaOSDConfig, result, 1, &guiGameOSDLanguageUpdater);

        if (result == UIID_BTN_OK)
            break;
    }
}
static int guiGameSaveOSDLanguageGameConfig(config_set_t *configSet, int result)
{
    if (gOSDLanguageSource == SETTINGS_PERGAME) {
        if ((result = configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE, gOSDLanguageSource)))
            if ((result = configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_ENABLE, gOSDLanguageEnable))) {
                configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_LANGID, gOSDLanguageValue);
                configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, gOSDTVAspectRatio);
                result = configSetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_VMODE, gOSDVideOutput);
            }
    } else {
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_LANGID);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_TV_ASP);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_VMODE);
        configRemoveKey(configSet, CONFIG_ITEM_OSD_SETTINGS_ENABLE);
    }

    return result;
}

void guiGameSaveOSDLanguageGlobalConfig(config_set_t *configGame)
{
    if (gOSDLanguageSource == SETTINGS_GLOBAL) {
        configSetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_ENABLE, gOSDLanguageEnable);
        configSetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_LANGID, gOSDLanguageValue);
        configSetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, gOSDTVAspectRatio);
        configSetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_VMODE, gOSDVideOutput);
    }
}

static void guiGameLoadOSDLanguageConfig(config_set_t *configSet, config_set_t *configGame)
{
    gOSDLanguageValue = 0;
    gOSDLanguageEnable = 0;
    gOSDLanguageSource = 0;
    gOSDTVAspectRatio = 0;
    gOSDVideOutput = 0;

    configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_ENABLE, &gOSDLanguageEnable);
    configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_LANGID, &gOSDLanguageValue);
    configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, &gOSDTVAspectRatio);
    configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_VMODE, &gOSDVideOutput);
    // override global with per-game settings if available and selected.
    if (!forceGlobalOSDLanguage) {
        configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE, &gOSDLanguageSource);
        if (gOSDLanguageSource == SETTINGS_PERGAME) {
            if (!configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_ENABLE, &gOSDLanguageEnable))
                gOSDLanguageEnable = 0;
            if (!configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_LANGID, &gOSDLanguageValue))
                gOSDLanguageValue = 0;
            if (!configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, &gOSDTVAspectRatio))
                gOSDTVAspectRatio = 0;
            if (!configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_VMODE, &gOSDVideOutput))
                gOSDVideOutput = 0;
        }
    }

    diaSetInt(diaOSDConfig, OSD_LANGUAGE_SOURCE, gOSDLanguageSource);
    diaSetInt(diaOSDConfig, OSD_LANGUAGE_ENABLE, gOSDLanguageEnable);
    diaSetInt(diaOSDConfig, OSD_LANGUAGE_VALUE, gOSDLanguageValue);
    diaSetInt(diaOSDConfig, OSD_TVASPECT_VALUE, gOSDTVAspectRatio);
    diaSetInt(diaOSDConfig, OSD_VMODE_VALUE, gOSDVideOutput);
}
// OSD Language

// loads defaults if no config found
void guiGameLoadConfig(item_list_t *support, config_set_t *configSet)
{
    int i;
    config_set_t *configGame = configGetByType(CONFIG_GAME);

    configSource[0] = '\0';
    configSourceID = CONFIG_SOURCE_DEFAULT;
    configGetInt(configSet, CONFIG_ITEM_CONFIGSOURCE, &configSourceID);
    if (configSourceID == CONFIG_SOURCE_USER)
        snprintf(configSource, sizeof(configSource), _l(_STR_CUSTOMIZED_SETTINGS));
    else if (configSourceID == CONFIG_SOURCE_DLOAD)
        snprintf(configSource, sizeof(configSource), _l(_STR_DOWNLOADED_DEFAULTS));

    //// 将默认UDMA模式设为最高UDMA模式+1
    //dmaMode = getHighestUdmaMode() + 1 + 3; // defaulting to HighestUdmaMode
    //  将默认UDMA模式设为UDMA 7
    dmaMode = 10;
    
    if (support->flags & MODE_FLAG_COMPAT_DMA) {
        configGetInt(configSet, CONFIG_ITEM_DMA, &dmaMode);
        diaSetInt(diaCompatConfig, COMPAT_DMA, dmaMode);
    }
    else
        diaSetInt(diaCompatConfig, COMPAT_DMA, 0);

    compatMode = 0;
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatMode);
    for (i = 0; i < COMPAT_MODE_COUNT; ++i)
        diaSetInt(diaCompatConfig, COMPAT_MODE_BASE + i, (compatMode & (1 << i)) > 0 ? 1 : 0);

    guiGameLoadGSMConfig(configSet, configGame);

    guiGameLoadCheatsConfig(configSet, configGame);
#ifdef PADEMU
    guiGameLoadPadEmuConfig(configSet, configGame);
    guiGameLoadPadMacroConfig(configSet, configGame);
#endif

    guiGameLoadOSDLanguageConfig(configSet, configGame);

    /// Find out the current game ID ///
    hexid[0] = '\0';
    configGetStrCopy(configSet, CONFIG_ITEM_DNAS, hexid, sizeof(hexid));
    diaSetString(diaCompatConfig, COMPAT_GAMEID, hexid);

    altStartup[0] = '\0';
    configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, altStartup, sizeof(altStartup));
    diaSetString(diaCompatConfig, COMPAT_ALTSTARTUP, altStartup);

    /// VMC ///
    vmc1[0] = '\0';
    configGetVMC(configSet, vmc1, sizeof(vmc1), 0);

    vmc2[0] = '\0';
    configGetVMC(configSet, vmc2, sizeof(vmc2), 1);
}
