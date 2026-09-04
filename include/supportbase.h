#ifndef __SUPPORT_BASE_H
#define __SUPPORT_BASE_H

//#include <sys/stat.h>

#define UL_GAME_NAME_MAX       32
#define ISO_GAME_NAME_MAX      160
#define ISO_GAME_EXTENSION_MAX 4
#define GAME_STARTUP_MAX       12

#define ISO_GAME_FNAME_MAX (ISO_GAME_NAME_MAX + ISO_GAME_EXTENSION_MAX)

enum GAME_FORMAT {
    GAME_FORMAT_USBLD = 0,
    GAME_FORMAT_OLD_ISO,
    GAME_FORMAT_ISO,
};

typedef struct
{
    char name[ISO_GAME_NAME_MAX + 1]; // MUST be the higher value from UL / ISO
    char startup[GAME_STARTUP_MAX + 1];
    char extension[ISO_GAME_EXTENSION_MAX + 1];
    u8 parts;
    u8 media;
    u8 format;
    u32 sizeMB;
    char crc32name[9];
    char indexName[128];
    char transName[128];
} base_game_info_t;

typedef struct
{
    char name[UL_GAME_NAME_MAX];    // it is not a string but character array, terminating NULL is not necessary
    char magic[3];                  // magic string "ul."
    char startup[GAME_STARTUP_MAX]; // it is not a string but character array, terminating NULL is not necessary
    u8 parts;                       // slice count
    u8 media;                       // Disc type
    u8 unknown[4];                  // Always zero
    u8 Byte08;                      // Always 0x08
    u8 unknown2[10];                // Always zero
} USBExtreme_game_entry_t;

int isValidIsoName(char *name, int *pNameLen);
int sbIsSameSize(const char *prefix, int prevSize);
int sbCreateSemaphore(void);
int sbReadList(base_game_info_t **list, const char *prefix, int *fsize, int *gamecount);
struct _item_list_t;
int sbBdmCompatDefaultsMode1(int bdmDeviceType, const char *bdmDriver);
int sbCompatDefaultsMode1(struct _item_list_t *support);
int sbGetCompatMask(config_set_t *configSet, int defaultMode1);
int sbPrepare(base_game_info_t *game, config_set_t *configSet, int size_cdvdman, void **cdvdman_irx, int *patchindex, int defaultMode1);
void sbUnprepare(void *pCommon);
void sbRebuildULCfg(base_game_info_t **list, const char *prefix, int gamecount, int excludeID);
void sbCreatePath(const base_game_info_t *game, char *path, const char *prefix, const char *sep, int part);
void sbDelete(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id);
void sbRename(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id, char *newname);
config_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep);
void sbCreateFolders(const char *path, int createDiscImgFolders);

/* 内部标记，不能当真实目录名。回退 ART 时要还原成 ART_FOLDER_NAME。 */
#define ART_FOLDER_NAME "ART"
#define ART_FOLDER_PS1  "ART_PS1"
#define ART_FOLDER_ELF  "ART_ELF"

/* 设备初始化时探测一次，读图只看这些标志，不再 opendir。 */
typedef struct
{
    unsigned char useBuckets;
    unsigned char hasPS2;
    unsigned char hasPS1;
    unsigned char hasAPPS;
    unsigned char hasGAMES;
} art_buckets_t;

void sbDetectArtBuckets(const char *prefix, const char *sep, art_buckets_t *buckets);
void sbBuildArtImagePath(char *path, int pathSize, const char *prefix, const char *sep,
                         const art_buckets_t *buckets, const char *folder, int isRelative,
                         const char *value, const char *suffix);

// ISO9660 filesystem management functions.
u32 sbGetISO9660MaxLBA(const char *path);
int sbProbeISO9660(const char *path, base_game_info_t *game, u32 layer1_offset);
int sbProbeISO9660_64(const char *path, base_game_info_t *game, u32 layer1_offset);
int sbIsValidStartupExecName(const char *startup);
int sbGetPOPSStartupExecName(const char *path, char *filename, int maxlength);
void sbGetStartupExecNameForLaunch(const char *path, const char *startup, char *filename, int maxlength);

int sbLoadCheats(const char *path, const char *file);

#endif
