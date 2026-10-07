#ifndef __THEMES_H
#define __THEMES_H

#include "include/textures.h"
#include "include/texcache.h"
#include "include/menusys.h"

#define THM_MAX_FILES 64
#define THM_MAX_FONTS 16

// 内置主题数量：GUI 主题列表开头固定的两套内置主题
//   索引 0 = 内置 Coverflow 封面流主题（默认主题）
//   索引 1 = 强化原生主题-支持背景图（原第一套主题，顺移到 Coverflow 之后）
// 用户从存储设备加载的主题从索引 THM_NUM_BUILTIN 开始排列。
#define THM_NUM_BUILTIN 2
// 内置 Coverflow 主题在 GUI 列表中显示的名称（同时用于配置保存/匹配）
#define THM_COVERFLOW_NAME "封面流主题(Coverflow)"
// 原第一套主题（强化原生列表主题）在 GUI 列表中显示的名称
#define THM_LIST_NAME "强化原生主题-支持背景图"

typedef struct
{
    // optional, only for overlays
    int upperLeft_x;
    int upperLeft_y;
    int upperRight_x;
    int upperRight_y;
    int lowerLeft_x;
    int lowerLeft_y;
    int lowerRight_x;
    int lowerRight_y;

    // basic texture information
    char *name;
    GSTEXTURE source;
} image_texture_t;

typedef struct
{
    // Attributes for: AttributeImage
    int currentUid;
    u32 currentConfigId;
    char *currentValue;

    // Attributes  for: AttributeImage & GameImage
    image_cache_t *cache;
    int cacheLinked;

    // Attributes for: AttributeImage & GameImage & StaticImage
    image_texture_t *defaultTexture;
    int defaultTextureLinked;

    image_texture_t *overlayTexture;
    int overlayTextureLinked;

    // 仅背景(Background)元素使用：只有当真正画出"当前游戏的背景图(BG art)"时，
    // 才在其上叠加这张 1 像素 alpha 透明遮罩；回退到兜底默认背景时不绘制，
    // 从而不让遮罩压暗兜底背景图。通过 cfg 键 <元素>_mask 指定贴图名。
    image_texture_t *maskTexture;
    int maskTextureLinked;
} mutable_image_t;

typedef struct
{
    // Attributes for: AttributeText & StaticText
    char *value;
    int sizingMode;

    // Attributes for: AttributeText
    char *alias;
    int displayMode;

    u32 currentConfigId;
    char *currentValue;
} mutable_text_t;

typedef struct
{
    int displayedItems;

    const char *decorator;
    mutable_image_t *decoratorImage;
} items_list_t;

typedef struct theme_element
{
    int type;
    int posX;
    int posY;
    int wsX;
    short aligned;
    int width;
    int height;
    short scaled;
    u64 color;
    int font;

    // 非 0 时，图像类元素会在自身下方绘制镜像倒影。
    // 目前由 Coverflow 元素使用。
    int reflection;

    void *extended;

    void (*drawElem)(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem);
    void (*endElem)(struct theme_element *elem);

    struct theme_element *next;
} theme_element_t;

typedef struct
{
    theme_element_t *first;
    theme_element_t *last;
} theme_elems_t;

typedef struct
{
    char *filePath;
    char *name;
} theme_file_t;

typedef struct theme
{
    int useDefault;
    int usedHeight;

    unsigned char bgColor[3];
    u64 textColor;
    u64 uiTextColor;
    u64 selTextColor;

    theme_elems_t mainElems;
    theme_elems_t infoElems;
    theme_element_t *gamesItemsList;

    theme_elems_t appsMainElems;
    theme_elems_t appsInfoElems;
    theme_element_t *appsItemsList;

    int gameCacheCount;

    theme_element_t *itemsList;
    theme_element_t *loadingIcon;
    int loadingIconCount;

    // Coverflow：当前主题声明了 Coverflow 元素时非 NULL。
    // 设置后，游戏列表会以滚动封面轮播的形式渲染。
    theme_element_t *coverflow;
    int coverflowCoverOffset;

    // Coverflow 停止移动后显示当前游戏 ICO 的专用单槽缓存；不复用封面缓存，
    // 这样不会改变 Coverflow 封面的淘汰顺序。
    image_cache_t *coverflowIcoCache;
    int coverflowIcoCacheId;
    int coverflowIcoCacheUID;
    submenu_list_t *coverflowIcoItem;
    int coverflowIcoLoaded;
    int coverflowIcoPopupActive;
    u64 coverflowIcoPopupStartTime;

    GSTEXTURE textures[TEXTURES_COUNT];
    // 外部 Coverflow 主题缺少菜单图标/BDM 索引时，记录对应槽位是否用了内置 CF 回退资源。
    // 用于忽略第三方主题给这两个模块配置的宽高，避免内置美术被拉伸。
    unsigned char coverflowTextureFallback[TEXTURES_COUNT];
    int fonts[THM_MAX_FONTS]; //!< Storage of font handles for removal once not needed
} theme_t;

extern theme_t *gTheme;

// 触发 Coverflow 滑动动画。direction：-1 = 下一个（向左滚动），
// 1 = 上一个（向右滚动）。即使当前没有启用 Coverflow 主题，调用也是安全的。
void thmTriggerCoverflowAnim(int direction);

// 翻页滚动专用：从 startItem 一次性滑到已计算好的目标项，跨过 steps 格；
// current 会在调用方先写成目标项，动画仍复用单步的纯三次缓出。
void thmTriggerCoverflowAnimMulti(int direction, int steps, submenu_list_t *startItem);

// 当前是否正处于 Coverflow 滑动动画中。
int thmCoverflowIsAnimating(void);

// 刷新列表前由主线程调用：取消旧 Coverflow 动画，避免旧 submenu 指针和旧封面继续参与绘制。
void thmCancelCoverflowAnimation(void);

// Coverflow 动画是否启用（主题配置滑动时长 >0）。
int thmCoverflowAnimEnabled(void);

// Coverflow 主题下 L1/R1 整页跳转的步长（= 同屏封面数，1..COVERFLOW_MAX）。
// 未启用 Coverflow 主题时返回 0，调用方回退到列表主题的 displayedItems 步长。
int thmGetCoverflowJumpCount(void);

void thmInit(void);
void thmReinit(const char *path);
void thmReloadScreenExtents(void);
int thmAddElements(char *path, const char *separator, int forceRefresh);
const char *thmGetValue(void);
GSTEXTURE *thmGetTexture(unsigned int id);
void thmEnd(void);

// Indices are shifted in GUI, as we add the internal default theme at 0
int thmSetGuiValue(int themeID, int reload);
int thmGetGuiValue(void);
int thmFindGuiID(const char *theme);
const char **thmGetGuiList(void);
char *thmGetFilePath(int themeID);

#endif
