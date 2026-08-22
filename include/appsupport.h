#ifndef __APP_SUPPORT_H
#define __APP_SUPPORT_H

#include "include/iosupport.h"

#define APP_MODE_UPDATE_DELAY 240

#define APP_TITLE_MAX 128
#define APP_PATH_MAX  128
// 须能装下 XX./SB. + 最长 ISO 名（160）去掉 .VCD 再加 .ELF
#define APP_BOOT_MAX  192
#define APP_ARGV1_MAX 128
// hdd0: + APA最长32字节分区名
#define APP_HDD_PARTITION_MAX 37

#define APP_CONFIG_TITLE "title"
#define APP_CONFIG_BOOT  "boot"
#define APP_CONFIG_ARGV1 "argv1"

#define APP_TITLE_CONFIG_FILE "title.cfg"

typedef struct
{
    char title[APP_TITLE_MAX + 1];
    char path[APP_PATH_MAX + 1];
    char boot[APP_BOOT_MAX + 1];
    char vcdName[APP_BOOT_MAX + 1];
    // 封面下方文字 / ART KEY（有编号前缀的VCD为11字符编号，其它与boot一致）
    char startup[APP_BOOT_MAX + 1];
    char argv1[APP_ARGV1_MAX + 1];
    char popsHddPartition[APP_HDD_PARTITION_MAX + 1];
    u8 legacy;
    u8 generated;
    u8 popstarter;
    u8 popsHddSource;
} app_info_t;

void appInit(item_list_t *itemList);
item_list_t *appGetObject(int initOnly);
void appForceRefresh(void);
void appPostUpdateCallback(int mode);

#endif
