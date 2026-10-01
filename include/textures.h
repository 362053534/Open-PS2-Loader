#ifndef __TEXTURES_H
#define __TEXTURES_H

enum INTERNAL_TEXTURE {
    LOAD0_ICON = 0,
    LOAD1_ICON,
    LOAD2_ICON,
    LOAD3_ICON,
    LOAD4_ICON,
    LOAD5_ICON,
    LOAD6_ICON,
    LOAD7_ICON,
    BDM_ICON,
    USB_ICON,
    ILINK_ICON,
    MX4SIO_ICON,
    HDD_BD_ICON,
    HDD_ICON,
    ETH_ICON,
    APP_ICON,
    INDEX_0,
    INDEX_1,
    INDEX_2,
    INDEX_3,
    INDEX_4,
    // WOPL 风格 BdmIndex 资源，仅由内置 Coverflow 使用；默认主题继续使用 INDEX_*。
    CF_INDEX_0,
    CF_INDEX_1,
    CF_INDEX_2,
    CF_INDEX_3,
    CF_INDEX_4,
    LEFT_ICON,
    RIGHT_ICON,
    CROSS_ICON,
    TRIANGLE_ICON,
    CIRCLE_ICON,
    SQUARE_ICON,
    SELECT_ICON,
    START_ICON,
    /* currently unused.
    UP_ICON,
    DOWN_ICON,
    L1_ICON,
    L2_ICON,
    L3_ICON,
    R1_ICON,
    R2_ICON,
    R3_ICON, */
    // 兜底背景纹理（内置两套默认主题共用），文件为 gfx/settings_bg.png。
    SETTINGS_BG,
    MAIN_BG_MASK,
    INFO_BG,
    COVER_DEFAULT,
    /*DISC_DEFAULT,*/
    SCREEN_DEFAULT,
    ELF_FORMAT,
    HDL_FORMAT,
    ISO_FORMAT,
    ZSO_FORMAT,
    UL_FORMAT,
    APP_MEDIA,
    CD_MEDIA,
    DVD_MEDIA,
    ASPECT_STD,
    ASPECT_WIDE,
    ASPECT_WIDE1,
    ASPECT_WIDE2,
    DEVICE_1,
    DEVICE_2,
    DEVICE_3,
    DEVICE_4,
    DEVICE_5,
    DEVICE_6,
    DEVICE_ALL,
    RATING_0,
    RATING_1,
    RATING_2,
    RATING_3,
    RATING_4,
    RATING_5,
    SCAN_240P,
    SCAN_240P1,
    SCAN_480I,
    SCAN_480P,
    SCAN_480P1,
    SCAN_480P2,
    SCAN_480P3,
    SCAN_480P4,
    SCAN_480P5,
    SCAN_576I,
    SCAN_576P,
    SCAN_720P,
    SCAN_1080I,
    SCAN_1080I2,
    SCAN_1080P,
    VMODE_MULTI,
    VMODE_NTSC,
    VMODE_PAL,
    LOGO_PICTURE,
    CASE_OVERLAY,
    APPS_CASE_OVERLAY,
    // Coverflow(内置主题, 对齐 wOPL)专用内置贴图:
    // plank 底部搁板 + wOPL 版盒装/软件外壳
    // (以 cf_ 前缀命名, 避免覆盖默认列表主题共用的 case/apps_case;
    //  封面占位图沿用默认主题的 cover, 兜底背景复用默认的 background, 不再单独引入)
    PLANK_PICTURE,
    CF_CASE_OVERLAY,
    CF_APPS_CASE_OVERLAY,
    // wOPL 风格的单设备图标(带 BDM/SMB/APA/APPS 标签), 仅 Coverflow 内置主题使用,
    // 在 thmLoad 里覆盖到对应的设备图标槽(BDM_ICON..APP_ICON), 默认列表主题不受影响。
    CF_DEV_BDM,
    CF_DEV_USB,
    CF_DEV_ILK,
    CF_DEV_M4S,
    CF_DEV_HDD_BD,
    CF_DEV_HDD,
    CF_DEV_ETH,
    CF_DEV_APP,

    TEXTURES_COUNT
};

#define ERR_BAD_FILE      -1
#define ERR_READ_STRUCT   -2
#define ERR_INFO_STRUCT   -3
#define ERR_SET_JMP       -4
#define ERR_BAD_DIMENSION      -5
#define ERR_MISSING_ALPHA      -6
#define ERR_BAD_DEPTH          -7
// 资源本身可能存在，但当前视频模式的 gsKit 纹理池容纳不了；调用方可重试。
#define ERR_TEXTURE_TOO_LARGE  -8

int texLookupInternalTexId(const char *name);
int texLoadInternal(GSTEXTURE *texture, int texId);
int texDiscoverLoad(GSTEXTURE *texture, const char *path, int texId);
void texFree(GSTEXTURE *texture);
void texInit(void);
void texFinish(void);

// 视频模式/帧缓冲确定后，把单张纹理大小上限(maxSize)夹到 gsKit 真实显存池以内，
// 避免大于池子的纹理触发 gsKit _blockAlloc 死循环把主机挂死(OPL issue #1776)。
// poolBytes 应为 __VRAM_SIZE - gsGlobal->CurrentPointer。
void texCheckBudget(unsigned int poolBytes);

//extern s32 fileLockId;
#endif
