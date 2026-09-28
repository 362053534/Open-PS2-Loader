#include "include/opl.h"
#include "include/themes.h"
#include "include/util.h"
#include "include/gui.h"
#include "include/renderman.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/fntsys.h"
#include "include/lang.h"
#include "include/pad.h"
#include "include/sound.h"

#include <math.h>
#include <time.h>

#define MENU_POS_V     50
#define HINT_HEIGHT    32
#define DECORATOR_SIZE 20

extern const char conf_theme_OPL_cfg;
extern u16 size_conf_theme_OPL_cfg;
// 内置 Coverflow 主题模板（由 misc/conf_theme_coverflow.cfg 经 bin2c 嵌入）
extern const char conf_theme_coverflow_cfg;
extern u16 size_conf_theme_coverflow_cfg;

// thmLoad(NULL) 时选择加载哪一套内置主题：
//   0 = 默认主题；1 = 内置 Coverflow 主题。
// 由 thmSetGuiValue()/thmReinit() 在调用 thmLoad(NULL) 前设置。
static int builtinThemeID = 0;

static int screenWidth;
static int screenHeight;
static int guiThemeID = 0;

static int nThemes = 0;
static theme_file_t themes[THM_MAX_FILES];
static const char **guiThemesNames = NULL;

// Global data
theme_t *gTheme;

enum ELEM_ATTRIBUTE_TYPE {
    ELEM_TYPE_ATTRIBUTE_TEXT = 0,
    ELEM_TYPE_STATIC_TEXT,
    ELEM_TYPE_ATTRIBUTE_IMAGE,
    ELEM_TYPE_GAME_IMAGE,
    ELEM_TYPE_STATIC_IMAGE,
    ELEM_TYPE_BACKGROUND, // A static image can be specified as the background. Otherwise, the plasma background will be drawn.
    ELEM_TYPE_MENU_ICON,
    ELEM_TYPE_MENU_TEXT,
    ELEM_TYPE_ITEMS_LIST,
    ELEM_TYPE_ITEM_ICON,
    ELEM_TYPE_ITEM_COVER,
    ELEM_TYPE_ITEM_TEXT,
    ELEM_TYPE_HINT_TEXT,
    ELEM_TYPE_INFO_HINT_TEXT,
    ELEM_TYPE_LOADING_ICON,
    ELEM_TYPE_BDM_INDEX,
    ELEM_TYPE_GAME_COUNT_TEXT,
    ELEM_TYPE_COVERFLOW,
    ELEM_TYPE_COUNT
};

#define DISPLAY_ALWAYS  0
#define DISPLAY_DEFINED 1
#define DISPLAY_NEVER   2

#define SIZING_NONE -1
#define SIZING_CLIP 0
#define SIZING_WRAP 1

static const char *elementsType[ELEM_TYPE_COUNT] = {
    "AttributeText",
    "StaticText",
    "AttributeImage",
    "GameImage",
    "StaticImage",
    "Background",
    "MenuIcon",
    "MenuText",
    "ItemsList",
    "ItemIcon",
    "ItemCover",
    "ItemText",
    "HintText",
    "InfoHintText",
    "LoadingIcon",
    "BdmIndex",
    "GameCountText",
    "Coverflow"};

// Common functions for Text ////////////////////////////////////////////////////////////////////////////////////////////////

static void endMutableText(theme_element_t *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (mutableText) {
        if (mutableText->value)
            free(mutableText->value);

        if (mutableText->alias)
            free(mutableText->alias);

        free(mutableText);
    }

    free(elem);
}

static mutable_text_t *initMutableText(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, struct theme_element *elem, const char *value, const char *alias, int displayMode, int sizingMode)
{
    mutable_text_t *mutableText = (mutable_text_t *)malloc(sizeof(mutable_text_t));
    mutableText->currentConfigId = 0;
    mutableText->currentValue = NULL;
    mutableText->alias = NULL;

    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_display", name);
    configGetInt(themeConfig, elemProp, &displayMode);
    mutableText->displayMode = displayMode;

    int length = strlen(value) + 1;
    mutableText->value = (char *)malloc(length * sizeof(char));
    memcpy(mutableText->value, value, length);

    snprintf(elemProp, sizeof(elemProp), "%s_wrap", name);
    if (configGetInt(themeConfig, elemProp, &sizingMode)) {
        if (sizingMode > 0)
            sizingMode = SIZING_WRAP;
    }

    if ((elem->width != DIM_UNDEF) || (elem->height != DIM_UNDEF)) {
        if (sizingMode == SIZING_NONE)
            sizingMode = SIZING_CLIP;

        if (elem->width == DIM_UNDEF)
            elem->width = screenWidth;

        if (elem->height == DIM_UNDEF)
            elem->height = screenHeight;
    } else
        sizingMode = SIZING_NONE;
    mutableText->sizingMode = sizingMode;

    if (type == ELEM_TYPE_ATTRIBUTE_TEXT) {
        snprintf(elemProp, sizeof(elemProp), "%s_title", name);
        configGetStr(themeConfig, elemProp, &alias);
        if (!alias) {
            if (value[0] == '#')
                alias = &value[1];
            else
                alias = value;
        }

        char *temp;
        if (!strncmp(alias, "Title", 5))
            temp = _l(_STR_INFO_TITLE);
        else if (!strncmp(alias, "Genre", 5))
            temp = _l(_STR_INFO_GENRE);
        else if (!strncmp(alias, "Release", 7))
            temp = _l(_STR_INFO_RELEASE);
        else if (!strncmp(alias, "Developer", 9))
            temp = _l(_STR_INFO_DEVELOPER);
        else if (!strncmp(alias, "Size", 4))
            temp = _l(_STR_SIZE);
        else if (!strncmp(alias, "Description", 11))
            temp = _l(_STR_INFO_DESCRIPTION);
        else
            temp = (char *)alias;

        length = strlen(temp) + 1 + 2;
        mutableText->alias = (char *)calloc(length, sizeof(char));
        if (mutableText->sizingMode == SIZING_WRAP)
            snprintf(mutableText->alias, length, "%s:\n", temp);
        else
            snprintf(mutableText->alias, length, "%s: ", temp);
    } else {
        if (mutableText->sizingMode == SIZING_WRAP)
            fntFitString(elem->font, mutableText->value, elem->width);
    }

    return mutableText;
}

// StaticText ///////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawStaticText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (mutableText->sizingMode == SIZING_NONE)
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->value, elem->color);
    else
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->value, elem->color);
}

static void initStaticText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    const char *value;
    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_value", name);
    configGetStr(themeConfig, elemProp, &value);
    if (value) {
        elem->extended = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, elem, value, NULL, DISPLAY_ALWAYS, SIZING_NONE);
        elem->endElem = &endMutableText;
        elem->drawElem = &drawStaticText;
    } else
        LOG("THEMES StaticText %s: NO value, elem disabled !!\n", name);
}

// GameCountText ////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int getGameCount(void *support)
{
    item_list_t *list = (item_list_t *)support;
    return list->itemGetCount(list);
}

static void drawGameCountText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;

    if (config) {
        if (mutableText->currentConfigId != config->uid) {
            // force refresh
            mutableText->currentConfigId = config->uid;

            int count = getGameCount(menu->item->userdata);
            snprintf(mutableText->value, sizeof(char) * 60, _l(_STR_FILE_COUNT), count);
        }
    }

    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->value, elem->color);
}

static void initGameCountText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    int length = 60;
    char *countStr = (char *)malloc(length * sizeof(char));
    memset(countStr, 0, length * sizeof(char));

    elem->extended = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, elem, countStr, NULL, DISPLAY_ALWAYS, SIZING_NONE);
    elem->endElem = &endMutableText;
    elem->drawElem = &drawGameCountText;
}

// AttributeText ////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawAttributeText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_text_t *mutableText = (mutable_text_t *)elem->extended;
    if (config) {
        if (mutableText->currentConfigId != config->uid) {
            // force refresh
            mutableText->currentConfigId = config->uid;
            mutableText->currentValue = NULL;
            if (configGetStr(config, mutableText->value, (const char **)&mutableText->currentValue)) {
                if (mutableText->sizingMode == SIZING_WRAP)
                    fntFitString(elem->font, mutableText->currentValue, elem->width);
            }
        }
        if (mutableText->currentValue) {
            char result[300];
            if (mutableText->displayMode == DISPLAY_NEVER) {
                if (!strncmp(mutableText->alias, _l(_STR_SIZE), strlen(_l(_STR_SIZE)))) {
                    snprintf(result, sizeof(result), "%s MiB", mutableText->currentValue);
                    if (mutableText->sizingMode == SIZING_NONE)
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, result, elem->color);
                    else
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, result, elem->color);
                } else {
                    if (mutableText->sizingMode == SIZING_NONE)
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->currentValue, elem->color);
                    else
                        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->currentValue, elem->color);
                }
            } else {
                if (!strncmp(mutableText->alias, _l(_STR_SIZE), strlen(_l(_STR_SIZE))))
                    snprintf(result, sizeof(result), "%s%s MiB", mutableText->alias, mutableText->currentValue);
                else
                    snprintf(result, sizeof(result), "%s%s", mutableText->alias, mutableText->currentValue);
                if (mutableText->sizingMode == SIZING_NONE)
                    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, result, elem->color);
                else
                    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, result, elem->color);
            }
            return;
        }
    }
    if (mutableText->displayMode == DISPLAY_ALWAYS) {
        if (mutableText->sizingMode == SIZING_NONE)
            fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, mutableText->alias, elem->color);
        else
            fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, mutableText->alias, elem->color);
    }
}

static void initAttributeText(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    const char *attribute;
    char elemProp[64];

    snprintf(elemProp, sizeof(elemProp), "%s_attribute", name);
    configGetStr(themeConfig, elemProp, &attribute);
    if (attribute) {
        elem->extended = initMutableText(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, elem, attribute, NULL, DISPLAY_ALWAYS, SIZING_NONE);
        elem->endElem = &endMutableText;
        elem->drawElem = &drawAttributeText;
    } else
        LOG("THEMES AttributeText %s: NO attribute, elem disabled !!\n", name);
}

// Common functions for Image ///////////////////////////////////////////////////////////////////////////////////////////////

static void findDuplicate(theme_element_t *first, const char *cachePattern, const char *defaultTexture, const char *overlayTexture, mutable_image_t *target)
{
    theme_element_t *elem = first;
    while (elem) {
        if ((elem->type == ELEM_TYPE_STATIC_IMAGE) || (elem->type == ELEM_TYPE_ATTRIBUTE_IMAGE) || (elem->type == ELEM_TYPE_GAME_IMAGE) || (elem->type == ELEM_TYPE_BACKGROUND)) {
            mutable_image_t *source = (mutable_image_t *)elem->extended;

            if (cachePattern && source->cache && !strcmp(cachePattern, source->cache->suffix)) {
                target->cache = source->cache;
                target->cacheLinked = 1;
                LOG("THEMES Re-using a cache for pattern %s\n", cachePattern);
            }

            if (defaultTexture && source->defaultTexture && !strcmp(defaultTexture, source->defaultTexture->name)) {
                target->defaultTexture = source->defaultTexture;
                target->defaultTextureLinked = 1;
                LOG("THEMES Re-using the default texture for %s\n", defaultTexture);
            }

            if (overlayTexture && source->overlayTexture && !strcmp(overlayTexture, source->overlayTexture->name)) {
                target->overlayTexture = source->overlayTexture;
                target->overlayTextureLinked = 1;
                LOG("THEMES Re-using the overlay texture for %s\n", overlayTexture);
            }
        }

        elem = elem->next;
    }
}

static void freeImageTexture(image_texture_t *texture)
{
    if (texture) {
        if (texture->source.Mem) {
            rmUnloadTexture(&texture->source);
            free(texture->source.Mem);
            texture->source.Mem = NULL;
        }
        if (texture->source.Clut) {
            free(texture->source.Clut);
            texture->source.Clut = NULL;
        }
        if (texture->name) {
            free(texture->name);
            texture->name = NULL;
        }
        free(texture);
    }
}

static image_texture_t *initImageTexture(const char *themePath, config_set_t *themeConfig, const char *name, const char *imgName, int isOverlay)
{
    image_texture_t *texture = (image_texture_t *)malloc(sizeof(image_texture_t));
    texture->name = NULL;

    int texId = -1;
    int result = 0;

    if (themePath) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s", themePath, imgName);
        if (texDiscoverLoad(&texture->source, path, texId) >= 0)
            result = 1;
    } else {
        texId = texLookupInternalTexId(imgName);
        if (texLoadInternal(&texture->source, texId) >= 0)
            result = 1;
    }

    if (result) {
        int length = strlen(imgName) + 1;
        texture->name = (char *)malloc(length * sizeof(char));
        memcpy(texture->name, imgName, length);

        if (isOverlay) {
            int intValue;
            char elemProp[64];
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_ulx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperLeft_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_uly", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperLeft_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_urx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperRight_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_ury", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->upperRight_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_llx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerLeft_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lly", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerLeft_y = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lrx", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerRight_x = intValue;
            snprintf(elemProp, sizeof(elemProp), "%s_overlay_lry", name);
            if (configGetInt(themeConfig, elemProp, &intValue))
                texture->lowerRight_y = intValue;
            return texture;
        }
    } else {
        freeImageTexture(texture);
        texture = NULL;
        return NULL;
    }
}

static image_texture_t *initImageInternalTexture(config_set_t *themeConfig, const char *name)
{
    image_texture_t *texture = (image_texture_t *)malloc(sizeof(image_texture_t));
    texture->name = NULL;
    int result;

    if ((result = texLookupInternalTexId(name)) >= 0) {
        result = texLoadInternal(&texture->source, result);
        int length = strlen(name) + 1;
        texture->name = (char *)malloc(length * sizeof(char));
        memcpy(texture->name, name, length);
    }

    if (result < 0) {
        freeImageTexture(texture);
        texture = NULL;
    }

    return texture;
}

static void endMutableImage(struct theme_element *elem)
{
    mutable_image_t *mutableImage = (mutable_image_t *)elem->extended;
    if (mutableImage) {
        if (mutableImage->cache && !mutableImage->cacheLinked)
            cacheDestroyCache(mutableImage->cache);

        if (mutableImage->defaultTexture && !mutableImage->defaultTextureLinked)
            freeImageTexture(mutableImage->defaultTexture);

        if (mutableImage->overlayTexture && !mutableImage->overlayTextureLinked)
            freeImageTexture(mutableImage->overlayTexture);

        free(mutableImage);
    }

    free(elem);
}

static mutable_image_t *initMutableImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, const char *cachePattern, int cacheCount, const char *defaultTexture, const char *overlayTexture)
{
    mutable_image_t *mutableImage = (mutable_image_t *)malloc(sizeof(mutable_image_t));
    mutableImage->currentUid = -1;
    mutableImage->currentConfigId = 0;
    mutableImage->currentValue = NULL;
    mutableImage->cache = NULL;
    mutableImage->cacheLinked = 0;
    mutableImage->defaultTexture = NULL;
    mutableImage->defaultTextureLinked = 0;
    mutableImage->overlayTexture = NULL;
    mutableImage->overlayTextureLinked = 0;

    char elemProp[64];

    if (type == ELEM_TYPE_ATTRIBUTE_IMAGE) {
        snprintf(elemProp, sizeof(elemProp), "%s_attribute", name);
        configGetStr(themeConfig, elemProp, &cachePattern);
        LOG("THEMES MutableImage %s: type: %s using cache pattern: %s\n", name, elementsType[type], cachePattern);
    } else if ((type == ELEM_TYPE_GAME_IMAGE) || (type == ELEM_TYPE_BACKGROUND)) {
        snprintf(elemProp, sizeof(elemProp), "%s_pattern", name);
        configGetStr(themeConfig, elemProp, &cachePattern);
        snprintf(elemProp, sizeof(elemProp), "%s_count", name);
        configGetInt(themeConfig, elemProp, &cacheCount);
        LOG("THEMES MutableImage %s: type: %s using cache pattern: %s count: %d\n", name, elementsType[type], cachePattern, cacheCount);
    }

    snprintf(elemProp, sizeof(elemProp), "%s_default", name);
    configGetStr(themeConfig, elemProp, &defaultTexture);

    if (type != ELEM_TYPE_BACKGROUND) {
        snprintf(elemProp, sizeof(elemProp), "%s_overlay", name);
        configGetStr(themeConfig, elemProp, &overlayTexture);
    }

    findDuplicate(theme->mainElems.first, cachePattern, defaultTexture, overlayTexture, mutableImage);
    findDuplicate(theme->infoElems.first, cachePattern, defaultTexture, overlayTexture, mutableImage);
    findDuplicate(theme->appsMainElems.first, cachePattern, defaultTexture, overlayTexture, mutableImage);
    findDuplicate(theme->appsInfoElems.first, cachePattern, defaultTexture, overlayTexture, mutableImage);

    if (cachePattern && !mutableImage->cache) {
        if (type == ELEM_TYPE_ATTRIBUTE_IMAGE)
            mutableImage->cache = cacheInitCache(-1, themePath, 0, cachePattern, 1);
        else
            mutableImage->cache = cacheInitCache(theme->gameCacheCount++, "ART", 1, cachePattern, cacheCount);
    }

    if (!themePath)
        if (defaultTexture && !mutableImage->defaultTexture)
            mutableImage->defaultTexture = initImageInternalTexture(themeConfig, defaultTexture);

    if (defaultTexture && !mutableImage->defaultTexture)
        mutableImage->defaultTexture = initImageTexture(themePath, themeConfig, name, defaultTexture, 0);

    if (overlayTexture && !mutableImage->overlayTexture)
        mutableImage->overlayTexture = initImageTexture(themePath, themeConfig, name, overlayTexture, 1);

    return mutableImage;
}

// StaticImage //////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawStaticImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_image_t *staticImage = (mutable_image_t *)elem->extended;
    if (staticImage->overlayTexture) {
        rmDrawOverlayPixmap(&staticImage->overlayTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol,
                            &staticImage->defaultTexture->source, staticImage->overlayTexture->upperLeft_x, staticImage->overlayTexture->upperLeft_y, staticImage->overlayTexture->upperRight_x, staticImage->overlayTexture->upperRight_y,
                            staticImage->overlayTexture->lowerLeft_x, staticImage->overlayTexture->lowerLeft_y, staticImage->overlayTexture->lowerRight_x, staticImage->overlayTexture->lowerRight_y);
    } else
        rmDrawPixmap(&staticImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);
}

static void initStaticImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *imageName)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, NULL, 0, imageName, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->defaultTexture)
        elem->drawElem = &drawStaticImage;
    else
        LOG("THEMES StaticImage %s: NO image name, elem disabled !!\n", name);
}

// GameImage ////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int artEnabledForCache(image_cache_t *cache)
{
    // 三个独立开关只挡背景/封面/光碟，其它 ART 照常读。
    if (!cache || !cache->suffix)
        return 1;
    if (!strncmp(cache->suffix, "BG", 2))
        return gEnableArtBG;
    if (!strncmp(cache->suffix, "COV", 3))
        return gEnableArtCOV;
    if (!strncmp(cache->suffix, "ICO", 3))
        return gEnableArtICO;
    return 1;
}

static int artHideDefaultTemplate(image_cache_t *cache)
{
    if (!cache || !cache->suffix)
        return 0;
    if (!strncmp(cache->suffix, "COV", 3))
        return !gEnableArtCOV;
    if (!strncmp(cache->suffix, "ICO", 3))
        return !gEnableArtICO;
    return 0;
}

static GSTEXTURE *getGameImageTexture(image_cache_t *cache, void *support, struct submenu_item *item)
{
    if (artEnabledForCache(cache)) {
        item_list_t *list = (item_list_t *)support;
        char *startup = list->itemGetStartup(list, item->id);
        return cacheGetTexture(cache, list, &item->cache_id[cache->userId], &item->cache_uid[cache->userId], startup, item->id);
    }

    return NULL;
}

// 与 getGameImageTexture() 相同，但走 Coverflow 专用的 cacheGetTextureQuiet()，
// 后者不依赖"每帧只取一张封面"的全局状态，因此 Coverflow 每帧取多张封面时封面
// 才能正常加载（否则会一直被单封面防抖逻辑挡掉、只显示占位图）。
static GSTEXTURE *getCoverflowTexture(image_cache_t *cache, void *support, struct submenu_item *item)
{
    if (artEnabledForCache(cache)) {
        item_list_t *list = (item_list_t *)support;
        char *startup = list->itemGetStartup(list, item->id);
        return cacheGetTextureQuiet(cache, list, &item->cache_id[cache->userId], &item->cache_uid[cache->userId], startup, item->id);
    }

    return NULL;
}

static void drawGameImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_image_t *gameImage = (mutable_image_t *)elem->extended;
    if (item) {
        GSTEXTURE *texture = getGameImageTexture(gameImage->cache, menu->item->userdata, &item->item);
        if (!texture || !texture->Mem) {
            // 封面/光碟关掉时，连默认模板和卡带框都不画
            if (artHideDefaultTemplate(gameImage->cache))
                return;
            if (gameImage->defaultTexture)
                texture = &gameImage->defaultTexture->source;
            else {
                if (elem->type == ELEM_TYPE_BACKGROUND)
                    guiDrawBGPlasma();
                return;
            }
        }

        if (gameImage->overlayTexture) {
            rmDrawOverlayPixmap(&gameImage->overlayTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol,
                                texture, gameImage->overlayTexture->upperLeft_x, gameImage->overlayTexture->upperLeft_y, gameImage->overlayTexture->upperRight_x, gameImage->overlayTexture->upperRight_y,
                                gameImage->overlayTexture->lowerLeft_x, gameImage->overlayTexture->lowerLeft_y, gameImage->overlayTexture->lowerRight_x, gameImage->overlayTexture->lowerRight_y);
        } else
            rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);

    } else if (elem->type == ELEM_TYPE_BACKGROUND) {
        if (gameImage->defaultTexture)
            rmDrawPixmap(&gameImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);
        else
            guiDrawBGPlasma();
    }
}

static void initGameImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *pattern, int count, const char *texture, const char *overlay)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, pattern, count, texture, overlay);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawGameImage;
    else
        LOG("THEMES GameImage %s: NO pattern, elem disabled !!\n", name);
}

// AttributeImage ///////////////////////////////////////////////////////////////////////////////////////////////////////////

static void drawAttributeImage(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    mutable_image_t *attributeImage = (mutable_image_t *)elem->extended;
    if (config) {
        if (attributeImage->currentConfigId != config->uid) {
            // force refresh
            attributeImage->currentUid = -1;
            attributeImage->currentConfigId = config->uid;
            attributeImage->currentValue = NULL;
            configGetStr(config, attributeImage->cache->suffix, (const char **)&attributeImage->currentValue);
        }
        if (attributeImage->currentValue) {
            // 内置主题（默认/ Coverflow）没有外部路径，属性图使用内置贴图查找。
            if (thmGetGuiValue() < THM_NUM_BUILTIN) {
                int texId;
                char *seppos = strchr(attributeImage->currentValue, '/');
                if (!seppos)
                    texId = texLookupInternalTexId(attributeImage->currentValue);
                else {
                    char imgName[32];
                    snprintf(imgName, sizeof(imgName), "%s_%s", attributeImage->cache->suffix, &seppos[1]);
                    texId = texLookupInternalTexId(&imgName[0]);
                }
                GSTEXTURE *texture = thmGetTexture(texId);
                if (texture && texture->Mem)
                    rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);

                return;
            } else {
                int posZ = 0;
                GSTEXTURE *texture = cacheGetTexture(attributeImage->cache, menu->item->userdata, &posZ, &attributeImage->currentUid, attributeImage->currentValue, -1);
                if (texture && texture->Mem) {
                    if (attributeImage->overlayTexture) {
                        rmDrawOverlayPixmap(&attributeImage->overlayTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol,
                                            texture, attributeImage->overlayTexture->upperLeft_x, attributeImage->overlayTexture->upperLeft_y, attributeImage->overlayTexture->upperRight_x, attributeImage->overlayTexture->upperRight_y,
                                            attributeImage->overlayTexture->lowerLeft_x, attributeImage->overlayTexture->lowerLeft_y, attributeImage->overlayTexture->lowerRight_x, attributeImage->overlayTexture->lowerRight_y);
                    } else
                        rmDrawPixmap(texture, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);

                    return;
                }
            }
        }
    }
    if (attributeImage->defaultTexture)
        rmDrawPixmap(&attributeImage->defaultTexture->source, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);
}

static void initAttributeImage(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, NULL, 1, NULL, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawAttributeImage;
    else
        LOG("THEMES AttributeImage %s: NO attribute, elem disabled !!\n", name);
}

// BasicElement /////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void endBasic(theme_element_t *elem)
{
    if (elem->extended)
        free(elem->extended);

    free(elem);
}

static theme_element_t *initBasic(const char *themePath, config_set_t *themeConfig, theme_t *theme, const char *name, int type, int x, int y, short aligned, int w, int h, short scaled, u64 color, int font)
{
    int intValue;
    unsigned char charColor[3];
    const char *temp;
    char elemProp[64];

    theme_element_t *elem = (theme_element_t *)malloc(sizeof(theme_element_t));

    elem->type = type;
    elem->extended = NULL;
    elem->drawElem = NULL;
    elem->endElem = &endBasic;
    elem->next = NULL;

    snprintf(elemProp, sizeof(elemProp), "%s_x", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "POS_MID", 7))
            x = screenWidth >> 1;
        else
            x = atoi(temp);
    }
    if (x < 0)
        elem->posX = screenWidth + x;
    else
        elem->posX = x;

    snprintf(elemProp, sizeof(elemProp), "%s_y", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "POS_MID", 7))
            y = screenHeight >> 1;
        else
            y = atoi(temp);
    }
    if (y < 0)
        elem->posY = ceil((screenHeight + y) * theme->usedHeight / screenHeight);
    else
        elem->posY = y;

    snprintf(elemProp, sizeof(elemProp), "%s_width", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "DIM_INF", 7))
            elem->width = screenWidth;
        else
            elem->width = atoi(temp);
    } else
        elem->width = w;

    snprintf(elemProp, sizeof(elemProp), "%s_height", name);
    if (configGetStr(themeConfig, elemProp, &temp)) {
        if (!strncmp(temp, "DIM_INF", 7))
            elem->height = screenHeight;
        else
            elem->height = atoi(temp);
    } else
        elem->height = h;

    snprintf(elemProp, sizeof(elemProp), "%s_aligned", name);
    if (configGetInt(themeConfig, elemProp, &intValue)) {
        // 兼容 wOPL/RiptOPL 主题的对齐取值：0=左上(NONE)、1=居中(CENTER)、2=右对齐+垂直居中。
        // 旧代码只区分 0 与非 0（非 0 一律当居中），导致第三方主题写的 aligned=2（本意右对齐）
        // 被误当成居中；配合 x=-8（负值换算成 posX=屏宽-8=右缘附近）会把元素推出屏幕右侧
        // （menuicon / BdmIndex 超屏、与其它元素重叠）。这里补上右对齐分支，取值与 RiptOPL
        // 一致（aligned=2 → ALIGN_VCENTER | ALIGN_RIGHT）。
        if (intValue == 0)
            elem->aligned = ALIGN_NONE;
        else if (intValue == 2)
            elem->aligned = (ALIGN_VCENTER | ALIGN_RIGHT); // 与 RiptOPL 一致：右对齐 + 垂直居中
        else
            elem->aligned = ALIGN_CENTER;
    } else
        elem->aligned = aligned;

    snprintf(elemProp, sizeof(elemProp), "%s_scaled", name);
    if (configGetInt(themeConfig, elemProp, &intValue))
        elem->scaled = (intValue == 0) ? SCALING_NONE : SCALING_RATIO;
    else
        elem->scaled = scaled;

    snprintf(elemProp, sizeof(elemProp), "%s_color", name);
    if (configGetColor(themeConfig, elemProp, charColor))
        elem->color = GS_SETREG_RGBA(charColor[0], charColor[1], charColor[2], 0x80);
    else
        elem->color = color;

    elem->font = font;
    snprintf(elemProp, sizeof(elemProp), "%s_font", name);
    if (configGetInt(themeConfig, elemProp, &intValue)) {
        if (intValue > 0 && intValue < THM_MAX_FONTS)
            elem->font = theme->fonts[intValue];
    }

    elem->reflection = 0;
    snprintf(elemProp, sizeof(elemProp), "%s_reflection", name);
    if (configGetInt(themeConfig, elemProp, &intValue))
        elem->reflection = intValue;

    return elem;
}

// Internal elements ////////////////////////////////////////////////////////////////////////////////////////////////////////
static void drawBackground(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    guiDrawBGPlasma();
}

static void initBackground(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *pattern, int count, const char *texture)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, elem->type, pattern, count, texture, NULL);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawGameImage;
    else if (mutableImage->defaultTexture)
        elem->drawElem = &drawStaticImage;
    else
        elem->drawElem = &drawBackground;
}

static void drawMenuIcon(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    GSTEXTURE *menuIconTex = thmGetTexture(menu->item->icon_id);
    if (menuIconTex && menuIconTex->Mem)
        rmDrawPixmap(menuIconTex, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);
}

static int findMenuNext(struct menu_list *menu)
{
    struct menu_list *next = menu->next;
    while (next != NULL && next->item->visible == 0)
        next = next->next;

    return next == NULL ? 0 : next->item->visible;
}

static int findMenuPrev(struct menu_list *menu)
{
    struct menu_list *prev = menu->prev;
    while (prev != NULL && prev->item->visible == 0)
        prev = prev->prev;

    return prev == NULL ? 0 : prev->item->visible;
}

static void drawMenuText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    GSTEXTURE *leftIconTex = NULL, *rightIconTex = NULL;
    if (findMenuPrev(menu) != 0)
        leftIconTex = thmGetTexture(LEFT_ICON);
    if (findMenuNext(menu) != 0)
        rightIconTex = thmGetTexture(RIGHT_ICON);

    if (elem->aligned) {
        int offset = elem->width >> 1;
        if (leftIconTex && leftIconTex->Mem)
            rmDrawPixmap(leftIconTex, elem->posX - offset, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol);
        if (rightIconTex && rightIconTex->Mem)
            rmDrawPixmap(rightIconTex, elem->posX + offset, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol);
    } else {
        if (leftIconTex && leftIconTex->Mem)
            rmDrawPixmap(leftIconTex, elem->posX - leftIconTex->Width, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol);
        if (rightIconTex && rightIconTex->Mem)
            rmDrawPixmap(rightIconTex, elem->posX + elem->width, elem->posY, elem->aligned, 20, 20, elem->scaled, gDefaultCol);
    }
    fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, menuItemGetText(menu->item), elem->color);
}

static void drawBDMIndex(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    item_list_t *itemList = menu->item->userdata;
    // Only render for bdm modes and if current mode is visible
    if (itemList->mode >= ETH_MODE || menu->item->visible == 0)
        return;

    // Only render if multiple mass devices are connected
    if (itemList->mode == 0 && menu->next->item->visible == 0)
        return;

    char imgName[32];
    snprintf(imgName, sizeof(imgName), "Index_%d", itemList->mode);

    GSTEXTURE *indexTex = thmGetTexture(texLookupInternalTexId(&imgName[0]));
    if (indexTex && indexTex->Mem)
        rmDrawPixmap(indexTex, elem->posX, elem->posY, elem->aligned, elem->width, elem->height, elem->scaled, gDefaultCol);
}

static void drawItemsList(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (item) {
        items_list_t *itemsList = (items_list_t *)elem->extended;

        int posX = elem->posX, posY = elem->posY;
        if (elem->aligned) {
            posX -= elem->width >> 1;
            posY -= elem->height >> 1;
        }

        // Coverflow 模式下让列表从选中项开始，这样那一行可见文字
        // 就充当中心封面的标题，而不是一整页滚动列表。
        submenu_list_t *ps = (gTheme->coverflow != NULL) ? item : menu->item->pagestart;
        int others = 0;
        u64 color;
        while (ps && (others++ < itemsList->displayedItems)) {
            if (ps == item)
                color = gTheme->selTextColor;
            else
                color = elem->color;

            if (itemsList->decoratorImage) {
                GSTEXTURE *itemIconTex = getGameImageTexture(itemsList->decoratorImage->cache, menu->item->userdata, &ps->item);
                if (itemIconTex && itemIconTex->Mem)
                    rmDrawPixmap(itemIconTex, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol);
                else {
                    if (itemsList->decoratorImage->defaultTexture)
                        rmDrawPixmap(&itemsList->decoratorImage->defaultTexture->source, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol);
                }
                fntRenderString(elem->font, elem->posX + DECORATOR_SIZE, posY, elem->aligned, elem->width, elem->height, submenuItemGetText(&ps->item), color);
            } else
                fntRenderString(elem->font, elem->posX, posY, elem->aligned, elem->width, elem->height, submenuItemGetText(&ps->item), color);

            posY += MENU_ITEM_HEIGHT;
            ps = ps->next;
        }
    }
}

static void initItemsList(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, const char *decorator)
{
    char elemProp[64];

    items_list_t *itemsList = (items_list_t *)malloc(sizeof(items_list_t));

    if (elem->width == DIM_UNDEF)
        elem->width = screenWidth;

    if (elem->height == DIM_UNDEF)
        elem->height = theme->usedHeight - (MENU_POS_V + HINT_HEIGHT);

    itemsList->displayedItems = elem->height / MENU_ITEM_HEIGHT;
    LOG("THEMES ItemsList %s: displaying %d elems, item height: %d\n", name, itemsList->displayedItems, elem->height);

    itemsList->decorator = NULL;
    snprintf(elemProp, sizeof(elemProp), "%s_decorator", name);
    configGetStr(themeConfig, elemProp, &decorator);
    if (decorator)
        itemsList->decorator = decorator; // Will be used later (thmValidate)

    itemsList->decoratorImage = NULL;

    elem->extended = itemsList;
    // elem->endElem = &endBasic; does the job

    elem->drawElem = &drawItemsList;
}

static void drawItemText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (item) {
        // 封面关时，封面下方的游戏ID一并隐藏
        if (!gEnableArtCOV)
            return;
        item_list_t *support = menu->item->userdata;
        fntRenderString(elem->font, elem->posX, elem->posY, elem->aligned, 0, 0, support->itemGetStartup(support, item->item.id), elem->color);
    }
}

static void drawHintText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    menu_hint_item_t *hint = menu->item->hints;
    if (hint) {
        int x = elem->posX;

        if (elem->aligned)
            x = guiAlignMenuHints(hint, elem->font, elem->width);

        for (; hint; hint = hint->next) {
            x = guiDrawIconAndText(hint->icon_id, hint->text_id, elem->font, x, elem->posY, elem->color);
            x += elem->width;
        }
    }
}

static void drawInfoHintText(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    int infoHints[2] = {_STR_RUN, _STR_BACK};
    int infoIcons[2] = {CIRCLE_ICON, CROSS_ICON};
    int x = elem->posX;

    if (elem->aligned)
        x = guiAlignSubMenuHints(2, infoHints, infoIcons, elem->font, elem->width, 1);

    x = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? infoIcons[0] : infoIcons[1], infoHints[0], elem->font, x, elem->posY, elem->color);
    x += elem->width;
    x = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? infoIcons[1] : infoIcons[0], infoHints[1], elem->font, x, elem->posY, elem->color);
}

// Coverflow ////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 从 wOPL 移植的游戏列表滚动封面轮播。以当前选中项为中心渲染 N 张封面（默认 3 张），
// 放大中间那张，并在导航时用三次缓出（cubic ease-out）平移整排。
// 倒影通过专用的 rmDraw*Reflect() 辅助函数绘制，从而保持共用的 rmDrawPixmap() 路径原样不动。

static int isAnimating = 0;        // 动画进行中标志
static int animationDirection = 0; // -1 = 下一个（向左滚动），1 = 上一个（向右滚动）
static clock_t animationStartTime = 0;

// Coverflow 可调参数。
// 规则：如果主题 cfg 里写了对应键，则【优先使用主题的定制值】；主题没写才回退到这里的默认值。
// 这些默认值与 RiptOPL 对齐（count=5 本 fork 默认、scale=30px、anim=200ms、dim=0）。
// 默认值定义为宏，供主题解析处“先复位默认、再按主题覆盖”使用。
#define COVERFLOW_MAX 15            // 同屏封面数的硬上限（covers[]/drawOrder[] 数组大小，防越界）
#define COVERFLOW_DEFAULT_COUNT 5   // 同屏显示的封面数默认值
#define COVERFLOW_DEFAULT_SCALE 30  // 中间封面额外放大的像素数默认值
#define COVERFLOW_DEFAULT_ANIM 200  // 滑动时长（毫秒）默认值
#define COVERFLOW_DEFAULT_DIM 0     // 非中心封面是否变暗默认值
#define COVERFLOW_DEFAULT_PRELOAD 2 // 每侧屏幕外预取封面数默认值（左右各 2 张，共 4 张）
static int gCoverflowCount = COVERFLOW_DEFAULT_COUNT;       // 同屏显示的封面数（drawCoverFlow 夹取到 1..COVERFLOW_MAX）
static int gCoverflowCenterScale = COVERFLOW_DEFAULT_SCALE; // 中间封面额外放大的【像素】数
static int gCoverflowAnimSpeed = COVERFLOW_DEFAULT_ANIM;    // 滑动时长（毫秒，<=0 关闭动画）
static int gCoverflowDimCovers = COVERFLOW_DEFAULT_DIM;     // 是否将非中心封面变暗
static int gCoverflowPreload = COVERFLOW_DEFAULT_PRELOAD;   // 每侧屏幕外预取的封面数（无上限，见主题解析处说明）

void thmTriggerCoverflowAnim(int direction)
{
    // 仅当启用了 Coverflow 主题时才生效，因此不影响列表主题下的导航。
    if (!gTheme || gTheme->coverflow == NULL)
        return;

    isAnimating = 1;
    animationDirection = direction;
    animationStartTime = clock();
}

// 绘制一张封面（可选带 case 外壳和/或倒影）。仿照 wOPL 的 thmDrawTexture，但通过
// 选择 reflect / 非 reflect 的 renderman 入口来实现，而不是修改共用函数的签名。
static void coverflowDrawTexture(GSTEXTURE *texture, mutable_image_t *img, int x, int y, short aligned, int w, int h, u64 color, int reflection, int baseW, int baseH)
{
    if (img->overlayTexture) {
        image_texture_t *ov = img->overlayTexture;

        // overlay_* 顶点遵循 wOPL/RiptOPL 约定：相对【元素配置尺寸 width/height】给出，
        // 而不是相对外壳纹理原生像素。例如第三方主题 width=150 / height=212 时，
        // overlay_lry=212、overlay_urx=140 都是落在 0..150 × 0..212 这个元素坐标系里的。
        // 因此这里按【实际绘制尺寸 / 元素配置尺寸】把顶点等比缩放到当前封面大小，
        // 封面才能精确吸附进外壳内框（修复第三方主题封面与顶点未对齐问题）。
        // 中间封面放大时 w/h 已随之增大，顶点自然一起放大，无需再额外加偏移。
        // 说明：内置 Coverflow cfg 的 width/height 恰好等于纹理原生尺寸(256)，故本次改动
        // 不影响内置主题；同时兼容第三方按元素尺寸书写顶点的主题。
        float sx = (baseW > 0) ? (float)w / (float)baseW : 1.0f;
        float sy = (baseH > 0) ? (float)h / (float)baseH : 1.0f;
        // NULL 保护：rmDrawOverlayPixmap 会直接解引用 inlay 指针（无 NULL 检查）。
        // 若既没有封面、也没有占位图（第三方主题未提供 cover.png 且内置 COVER_DEFAULT
        // 也不可用），则不能带 inlay 调用，否则会崩溃。此时【仍然单独把 case 外壳画出来】，
        // 避免整块 case 模块直接消失（修复"缺图时整个 case 都不显示"的问题）。
        if (!texture || !texture->Mem) {
            if (reflection)
                rmDrawPixmapReflect(&ov->source, x, y, aligned, w, h, SCALING_NONE, color);
            else
                rmDrawPixmap(&ov->source, x, y, aligned, w, h, SCALING_NONE, color);
            return;
        }

        int ulx = (int)(ov->upperLeft_x * sx);
        int uly = (int)(ov->upperLeft_y * sy);
        int urx = (int)(ov->upperRight_x * sx);
        int ury = (int)(ov->upperRight_y * sy);
        int blx = (int)(ov->lowerLeft_x * sx);
        int bly = (int)(ov->lowerLeft_y * sy);
        int brx = (int)(ov->lowerRight_x * sx);
        int bry = (int)(ov->lowerRight_y * sy);

        if (reflection)
            rmDrawOverlayPixmapReflect(&ov->source, x, y, aligned, w, h, SCALING_NONE, color, texture, ulx, uly, urx, ury, blx, bly, brx, bry);
        else
            rmDrawOverlayPixmap(&ov->source, x, y, aligned, w, h, SCALING_NONE, color, texture, ulx, uly, urx, ury, blx, bly, brx, bry);
    } else {
        // 无外壳（纯封面）主题：没有可用贴图就直接跳过。
        if (!texture || !texture->Mem)
            return;
        if (reflection)
            rmDrawPixmapReflect(texture, x, y, aligned, w, h, SCALING_NONE, color);
        else
            rmDrawPixmap(texture, x, y, aligned, w, h, SCALING_NONE, color);
    }
}

static void drawCoverFlow(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    if (item == NULL)
        return;

    mutable_image_t *img = (mutable_image_t *)elem->extended;
    item_list_t *sourceList = menu->item->userdata;

    int coverCount = gCoverflowCount;
    if (coverCount < 1)
        coverCount = 1;
    if (coverCount > COVERFLOW_MAX)
        coverCount = COVERFLOW_MAX; // 夹取到数组上限，防止 covers[]/drawOrder[] 越界
    int centerIndex = coverCount / 2;

    int coverHeight = elem->height;
    int coverWidth = gWideScreen ? rmWideScale(elem->width) : elem->width;

    // 限制封面尺寸，使其能全部横向排入屏幕。
    int coverYOffset = 0;
    int maxCoverWidth = (screenWidth - (coverCount - 1) * 10) / coverCount;
    if (coverWidth > maxCoverWidth) {
        int origHeight = coverHeight;
        coverHeight = (coverHeight * maxCoverWidth) / coverWidth;
        coverWidth = maxCoverWidth;
        coverYOffset = (origHeight - coverHeight) / 2;
    }

    int totalCoversWidth = coverCount * coverWidth;
    int totalRemainingSpace = screenWidth - totalCoversWidth;
    int coverSpacing = totalRemainingSpace / (coverCount + 1);
    if (coverSpacing < 0)
        coverSpacing = 0;
    if (gWideScreen)
        coverSpacing = rmWideScale(coverSpacing);

    int coverDistance = coverWidth + coverSpacing;
    int totalGroupWidth = (coverCount - 1) * coverDistance + coverWidth;
    int basePosX = (screenWidth - totalGroupWidth) / 2 + (coverWidth >> 1) + (coverWidth * gTheme->coverflowCoverOffset / 256);

    struct
    {
        submenu_list_t *game;
        GSTEXTURE *texture;
        int renderPosX; // 预计算好的横向绘制坐标（供按层级顺序绘制时取用）
    } covers[COVERFLOW_MAX];

    int ci;
    for (ci = 0; ci < coverCount; ci++) {
        covers[ci].game = NULL;
        covers[ci].texture = NULL;
        covers[ci].renderPosX = 0;
    }
    covers[centerIndex].game = item;

    // 填充左侧：从中心向前回溯。左边缘不做环绕
    //（本分支的 menu_item_t 没有 "last" 指针），所以第一项左侧的槽位保持为空。
    // leftmostVisible / rightmostVisible 记录可见窗口两端的实际游戏节点，供下面预取使用。
    submenu_list_t *leftmostVisible = item;
    submenu_list_t *rightmostVisible = item;
    submenu_list_t *cur = item;
    for (ci = centerIndex - 1; ci >= 0; ci--) {
        submenu_list_t *prev = cur->prev;
        if (prev == NULL || prev == item)
            break;
        covers[ci].game = prev;
        cur = prev;
    }
    leftmostVisible = cur;

    // 填充右侧：向后遍历。右边缘同样【不做环绕】，到列表末尾即停止，让最后一项右侧
    //（centerIndex 之后）的槽位保持为空 —— 与首项左侧留空的规则保持一致，避免末项
    // 右侧又把开头的游戏绕回来显示、造成首/末表现不统一。
    cur = item;
    for (ci = centerIndex + 1; ci < coverCount; ci++) {
        submenu_list_t *next = cur->next;
        if (next == NULL || next == item)
            break;
        covers[ci].game = next;
        cur = next;
    }
    rightmostVisible = cur;

    // 用三次缓出（cubic ease-out）计算滑动偏移。
    float eased = 1.0f;
    float animOffset = 0.0f;
    if (isAnimating) {
        if (gCoverflowAnimSpeed <= 0) {
            isAnimating = 0;
        } else {
            clock_t elapsed = clock() - animationStartTime;
            float t = (float)elapsed / ((float)gCoverflowAnimSpeed * CLOCKS_PER_SEC / 1000);
            if (t >= 1.0f) {
                t = 1.0f;
                isAnimating = 0;
                animationStartTime = 0;
            }
            float inv = 1.0f - t;
            eased = 1.0f - inv * inv * inv;
            animOffset = (float)animationDirection * (float)coverDistance * (eased - 1.0f);
        }
    }

    int posX = basePosX + (int)animOffset;
    int scaling = gCoverflowCenterScale;
    int leavingIndex = (animationDirection > 0) ? (centerIndex + 1) : (centerIndex - 1);

    // 第一遍：预计算每个封面的横向绘制坐标（顺序无关，供下面按层级绘制取用）。
    int i;
    for (i = 0; i < coverCount; i++) {
        covers[i].renderPosX = posX;
        posX += coverDistance;
    }

    // 生成绘制顺序，实现画家算法的正确层级：
    //   先画左侧（i 从 0 递增到 centerIndex-1，越靠近中心越后画，压在外侧之上），
    //   再画右侧（i 从 coverCount-1 递减到 centerIndex+1，同样越靠近中心越后画），
    //   最后画中心封面 —— 保证放大后的中心封面永远在最上层，不被两侧邻居遮挡。
    int drawOrder[COVERFLOW_MAX];
    int drawCount = 0;
    for (i = 0; i < centerIndex; i++)
        drawOrder[drawCount++] = i;
    for (i = coverCount - 1; i > centerIndex; i--)
        drawOrder[drawCount++] = i;
    drawOrder[drawCount++] = centerIndex;

    int oi;
    for (oi = 0; oi < drawCount; oi++) {
        i = drawOrder[oi];

        if (covers[i].game == NULL)
            continue;

        int renderPosX = covers[i].renderPosX;
        int currentCoverWidth = coverWidth;
        int currentCoverHeight = coverHeight;

        // 插值缩放：进入中心的封面逐渐放大，离开中心的封面逐渐缩小。
        // 外壳顶点在 coverflowDrawTexture 里会随 w/h 等比缩放，这里不再需要额外偏移。
        int currentScaling = 0;
        if (i == centerIndex) {
            float growFactor = isAnimating ? eased : 1.0f;
            currentScaling = (int)(scaling * growFactor);
        } else if (isAnimating && i == leavingIndex) {
            currentScaling = (int)(scaling * (1.0f - eased));
        }

        if (currentScaling > 0) {
            currentCoverWidth += currentScaling;
            currentCoverHeight += currentScaling;
        }

        covers[i].texture = getCoverflowTexture(img->cache, sourceList, &covers[i].game->item);
        if (!covers[i].texture || !covers[i].texture->Mem)
            covers[i].texture = img->defaultTexture ? &img->defaultTexture->source : thmGetTexture(COVER_DEFAULT);

        // 不再因缺图而 continue：即使没有封面也没有占位图，coverflowDrawTexture 会在
        // 主题带 overlay(case) 时至少画出空的 case 外壳，避免整块 case 模块消失。

        u64 coverColor = gDefaultCol;
        if (gCoverflowDimCovers && i != centerIndex)
            coverColor = GS_SETREG_RGBA(0x80, 0x80, 0x80, 0x40);

        // 传入元素配置尺寸 elem->width/height 作为顶点基准坐标系（wOPL 约定）。
        coverflowDrawTexture(covers[i].texture, img, renderPosX, elem->posY + coverYOffset, ALIGN_CENTER,
                             currentCoverWidth, currentCoverHeight, coverColor, elem->reflection,
                             elem->width, elem->height);
    }

    // 预取（prefetch）：为可见窗口【两侧当前看不见】的若干封面提前排队加载。这样左右滚动时
    // 这些封面已在缓存里，能直接命中、减少滑动时才临时加载、露出占位图的情况。只【请求】、不绘制。
    //
    // 每侧预取张数 = gCoverflowPreload（优先取自主题 cfg 的 coverflow_preload 键，缺省 2）。
    // 例如填 3 就是左右屏幕外各预读 3 张、共 6 张。此值【不设上限】：主题包填过大会因缓存/内存
    // 过大而出问题，属用户行为，不额外处理。封面缓存槽位数在 initCoverflow 处按
    // (同屏数 + 2*预取数 + 1) 分配，确保这些预取封面都放得下、预取真正生效。
    //
    // 注意：预取【允许环绕】——虽然显示层到列表头/尾就留空（不环绕），但导航是会环绕的
    //（menuNextV 到尾部会跳回首项、menuPrevV 到首部会跳到末项），所以预取要把“另一头”的
    // 封面也提前加载好，环绕跳转时才不会露出占位图。
    if (img->cache && gCoverflowPreload > 0) {
        int preloadPerSide = gCoverflowPreload;

        submenu_list_t *head = menu->item->submenu;

        // 左侧预取：到列表头(prev==NULL)时环绕到列表尾继续。
        submenu_list_t *pcur = leftmostVisible;
        int p;
        for (p = 0; p < preloadPerSide && pcur; p++) {
            submenu_list_t *prev = pcur->prev;
            if (prev == NULL && head) {
                // 环绕到列表尾（本分支无 last 指针，从表头走到末尾；只会在首次触边时走一次）。
                prev = head;
                while (prev->next)
                    prev = prev->next;
            }
            if (prev == NULL || prev == item)
                break; // 空列表，或列表太短已绕回中心项，停止
            getCoverflowTexture(img->cache, sourceList, &prev->item);
            pcur = prev;
        }

        // 右侧预取：到列表尾(next==NULL)时环绕到列表头继续。
        pcur = rightmostVisible;
        for (p = 0; p < preloadPerSide && pcur; p++) {
            submenu_list_t *next = pcur->next;
            if (next == NULL)
                next = head; // 环绕到列表头
            if (next == NULL || next == item)
                break;
            getCoverflowTexture(img->cache, sourceList, &next->item);
            pcur = next;
        }
    }
}

static void initCoverflow(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *elem, const char *name, int count, const char *texture, const char *overlay)
{
    mutable_image_t *mutableImage = initMutableImage(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, "COV", count, texture, overlay);
    elem->extended = mutableImage;
    elem->endElem = &endMutableImage;

    if (mutableImage->cache)
        elem->drawElem = &drawCoverFlow;
    else
        LOG("THEMES Coverflow %s: NO pattern, elem disabled !!\n", name);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void validateBackgroundElems(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_elems_t *mainElems, theme_elems_t *infoElems)
{
    if (!mainElems->first || (mainElems->first->type != ELEM_TYPE_BACKGROUND)) {
        LOG("THEMES No valid background found for main, add default BG_ART\n");
        theme_element_t *backgroundElem = initBasic(themePath, themeConfig, theme, "bg", ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
        initBackground(themePath, themeConfig, theme, backgroundElem, "bg", "BG", 1, NULL);
        backgroundElem->next = mainElems->first;
        mainElems->first = backgroundElem;
    }

    if (infoElems->first) {
        if (infoElems->first->type != ELEM_TYPE_BACKGROUND) {
            LOG("THEMES No valid background found for info, add default BG_ART\n");
            theme_element_t *backgroundElem = initBasic(themePath, themeConfig, theme, "bg", ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
            initBackground(themePath, themeConfig, theme, backgroundElem, "bg", "BG", 1, NULL);
            backgroundElem->next = infoElems->first;
            infoElems->first = backgroundElem;
        }
    }
}

static void validateItemsList(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_element_t *list, theme_elems_t *mainElems)
{
    if (list) {
        items_list_t *itemsList = (items_list_t *)list->extended;
        if (itemsList->decorator) {
            // Second pass to find the decorator
            theme_element_t *decoratorElem = mainElems->first;
            while (decoratorElem) {
                if (decoratorElem->type == ELEM_TYPE_GAME_IMAGE) {
                    mutable_image_t *gameImage = (mutable_image_t *)decoratorElem->extended;
                    if (!strcmp(itemsList->decorator, gameImage->cache->suffix)) {
                        // if user want to cache less than displayed items, then disable itemslist icons, if not would load constantly
                        if (gameImage->cache->count >= itemsList->displayedItems)
                            itemsList->decoratorImage = gameImage;
                        break;
                    }
                }

                decoratorElem = decoratorElem->next;
            }
            itemsList->decorator = NULL;
        }
    } else {
        LOG("THEMES No itemsList found, adding a default one\n");
        list = initBasic(themePath, themeConfig, theme, "il", ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 373, 316, SCALING_RATIO, theme->textColor, theme->fonts[0]);
        initItemsList(themePath, themeConfig, theme, list, "il", NULL);
        list->next = mainElems->first->next; // Position the itemsList as second element (right after the Background)
        mainElems->first->next = list;
    }
}

static void validateGUIElems(const char *themePath, config_set_t *themeConfig, theme_t *theme)
{
    // 1. check we have a valid Background elements
    validateBackgroundElems(themePath, themeConfig, theme, &theme->mainElems, &theme->infoElems);
    validateBackgroundElems(themePath, themeConfig, theme, &theme->appsMainElems, &theme->appsInfoElems);

    // 2. check we have a valid ItemsList element, and link its decorator to the target element
    validateItemsList(themePath, themeConfig, theme, theme->gamesItemsList, &theme->mainElems);
    validateItemsList(themePath, themeConfig, theme, theme->appsItemsList, &theme->appsMainElems);
}

static int addGUIElem(const char *themePath, config_set_t *themeConfig, theme_t *theme, theme_elems_t *elems, const char *type, const char *name)
{
    int enabled = 1;
    char elemProp[64];
    theme_element_t *elem = NULL;

    snprintf(elemProp, sizeof(elemProp), "%s_enabled", name);
    configGetInt(themeConfig, elemProp, &enabled);

    if (enabled) {
        snprintf(elemProp, sizeof(elemProp), "%s_type", name);
        configGetStr(themeConfig, elemProp, &type);
        if (type) {
            if (!strcmp(elementsType[ELEM_TYPE_ATTRIBUTE_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initAttributeText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_STATIC_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initStaticText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_GAME_COUNT_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                initGameCountText(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_ATTRIBUTE_IMAGE], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ATTRIBUTE_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initAttributeImage(themePath, themeConfig, theme, elem, name);
            } else if (!strcmp(elementsType[ELEM_TYPE_GAME_IMAGE], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, NULL, 1, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_STATIC_IMAGE], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_STATIC_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initStaticImage(themePath, themeConfig, theme, elem, name, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_BACKGROUND], type)) {
                if (!elems->first) { // Background elem can only be the first one
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_BACKGROUND, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, theme->fonts[0]);
                    initBackground(themePath, themeConfig, theme, elem, name, NULL, 3, NULL);
                }
            } else if (!strcmp(elementsType[ELEM_TYPE_MENU_ICON], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_MENU_ICON, screenWidth >> 1, 400, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                elem->drawElem = &drawMenuIcon;
            } else if (!strcmp(elementsType[ELEM_TYPE_MENU_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_MENU_TEXT, screenWidth >> 1, 20, ALIGN_CENTER, 200, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawMenuText;
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEMS_LIST], type)) {
                if (!theme->gamesItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 0, 0, ALIGN_NONE, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->gamesItemsList = elem;
                } else if (!theme->appsItemsList) {
                    elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEMS_LIST, 42, 42, ALIGN_NONE, 400, 360, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                    initItemsList(themePath, themeConfig, theme, elem, name, NULL);
                    theme->appsItemsList = elem;
                }
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_ICON], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, 64, 64, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, "ICO", 5, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_COVER], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_GAME_IMAGE, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                initGameImage(themePath, themeConfig, theme, elem, name, "COV", 5, NULL, NULL);
            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEM_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawItemText;
            } else if (!strcmp(elementsType[ELEM_TYPE_HINT_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_HINT_TEXT, 16, -HINT_HEIGHT, ALIGN_NONE, 12, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawHintText;
            } else if (!strcmp(elementsType[ELEM_TYPE_INFO_HINT_TEXT], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_INFO_HINT_TEXT, 16, -HINT_HEIGHT, ALIGN_NONE, 12, 20, SCALING_RATIO, theme->textColor, theme->fonts[0]);
                elem->drawElem = &drawInfoHintText;
            } else if (!strcmp(elementsType[ELEM_TYPE_LOADING_ICON], type)) {
                if (!theme->loadingIcon)
                    theme->loadingIcon = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_LOADING_ICON, -40, -60, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
            } else if (!strcmp(elementsType[ELEM_TYPE_BDM_INDEX], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_BDM_INDEX, screenWidth >> 1, 355, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, gDefaultCol, theme->fonts[0]);
                elem->drawElem = &drawBDMIndex;
            } else if (!strcmp(elementsType[ELEM_TYPE_COVERFLOW], type)) {
                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_COVERFLOW, 0, 0, ALIGN_NONE, DIM_UNDEF, DIM_UNDEF, SCALING_NONE, gDefaultCol, theme->fonts[0]);
                // 封面缓存槽位数按 (同屏数 + 两侧预取数 + 1 余量) 动态分配，确保同屏封面与
                // 左右预取封面都放得下、预取真正生效。gCoverflowCount / gCoverflowPreload 已在
                // 上面的主题级解析里按“优先读主题、缺失用默认”确定。预取数不设上限，故槽位数也
                // 可能很大（主题填过大导致内存不足属用户行为，不额外处理）。
                int coverflowCacheSlots = gCoverflowCount + 2 * gCoverflowPreload + 1;
                initCoverflow(themePath, themeConfig, theme, elem, name, coverflowCacheSlots, NULL, NULL);
                theme->coverflow = elem;
            }

            if (elem) {
                if (!elems->first)
                    elems->first = elem;

                if (!elems->last)
                    elems->last = elem;
                else {
                    elems->last->next = elem;
                    elems->last = elem;
                }
            }
        } else
            return 0; // ends the reading of elements
    }

    return 1;
}

static void freeGUIElems(theme_elems_t *elems)
{
    theme_element_t *elem = elems->first;
    while (elem) {
        elems->first = elem->next;
        elem->endElem(elem);
        elem = elems->first;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

GSTEXTURE *thmGetTexture(unsigned int id)
{
    if (id >= TEXTURES_COUNT)
        return NULL;
    else {
        // see if the texture is valid
        GSTEXTURE *txt = &gTheme->textures[id];

        if (txt->Mem)
            return txt;
        else
            return NULL;
    }
}

static void thmFree(theme_t *theme)
{
    if (theme) {
        // free elements
        freeGUIElems(&theme->mainElems);
        freeGUIElems(&theme->infoElems);
        freeGUIElems(&theme->appsMainElems);
        freeGUIElems(&theme->appsInfoElems);

        // free textures
        GSTEXTURE *texture;
        int id = 0;
        for (; id < TEXTURES_COUNT; id++) {
            texture = &theme->textures[id];
            if (texture->Mem != NULL) {
                rmUnloadTexture(texture);
                texFree(texture);
            }
        }

        // free fonts
        for (id = 0; id < THM_MAX_FONTS; ++id)
            fntRelease(theme->fonts[id]);

        free(theme);
    }
}

static int thmReadEntry(int index, const char *path, const char *separator, const char *name, unsigned char d_type)
{
    if (d_type == DT_DIR && strstr(name, "thm_")) {
        theme_file_t *currTheme = &themes[nThemes + index];

        int length = strlen(name) - 4 + 1;
        currTheme->name = (char *)malloc(length * sizeof(char));
        memcpy(currTheme->name, name + 4, length);
        currTheme->name[length - 1] = '\0';

        length = strlen(path) + 1 + strlen(name) + 1 + 1;
        currTheme->filePath = (char *)malloc(length * sizeof(char));
        sprintf(currTheme->filePath, "%s%s%s%s", path, separator, name, separator);

        LOG("THEMES Theme found: %s\n", currTheme->filePath);

        index++;
    }
    return index;
}

/* themePath must contains the leading separator (as it is dependent of the device, we can't know here) */
static int thmLoadResource(GSTEXTURE *texture, int texId, const char *themePath, short psm, int useDefault)
{
    int success = -1;

    if (themePath != NULL)
        success = texDiscoverLoad(texture, themePath, texId); // only set success here

    if ((success < 0) && useDefault)
        texLoadInternal(texture, texId); // we don't mind the result of "default"

    return success;
}

static void thmApplyTextColor(theme_element_t *elem, u64 color)
{
    while (elem) {
        elem->color = color;
        elem = elem->next;
    }
}

static void thmSetColors(theme_t *theme)
{
    memcpy(theme->bgColor, gDefaultBgColor, 3);
    theme->textColor = GS_SETREG_RGBA(gDefaultTextColor[0], gDefaultTextColor[1], gDefaultTextColor[2], 0x80);
    theme->uiTextColor = GS_SETREG_RGBA(gDefaultUITextColor[0], gDefaultUITextColor[1], gDefaultUITextColor[2], 0x80);
    theme->selTextColor = GS_SETREG_RGBA(gDefaultSelTextColor[0], gDefaultSelTextColor[1], gDefaultSelTextColor[2], 0x80);

    /* APPS 主页/信息页是独立元素链，只改 mainElems 时自定义颜色不会进 APPS。 */
    thmApplyTextColor(theme->mainElems.first, theme->textColor);
    thmApplyTextColor(theme->infoElems.first, theme->textColor);
    thmApplyTextColor(theme->appsMainElems.first, theme->textColor);
    thmApplyTextColor(theme->appsInfoElems.first, theme->textColor);
}

static void thmLoadFonts(config_set_t *themeConfig, const char *themePath, theme_t *theme)
{
    int fntID = 0; // theme side font id, not the fntSys handle
    for (fntID = 0; fntID < THM_MAX_FONTS; ++fntID) {
        int fontSize = 0;
        char sizeKey[64];
        if (fntID == 0) {
            snprintf(sizeKey, sizeof(sizeKey), "default_font_size");
            theme->fonts[0] = FNT_DEFAULT;
        }
        else {
            snprintf(sizeKey, sizeof(sizeKey), "font%d_size", fntID);
            theme->fonts[fntID] = theme->fonts[0];
        }

        if (!configGetInt(themeConfig, sizeKey, &fontSize) || fontSize <= 0) {
            fontSize = FNTSYS_DEFAULT_SIZE;
            continue; // 如果字体大小没变化，就跳过
        }

        // 不要使用主题里的字体，否则出问题，只改变当前字体的文字大小
        int fntHandle = FNT_DEFAULT;
        if (lngGetGuiValue() != 0) {
            char fullPath[128];
            char *fontPath = lngGetFilePath(lngGetGuiValue());
            int len = strlen(fontPath) - strlen(lngGetValue()) - 9; // -4 for extension,  -5 for prefix
            memcpy(fullPath, fontPath, len);
            fullPath[len] = '\0';
            snprintf(fullPath, sizeof(fullPath), "%sfont_%s.ttf", fullPath, lngGetValue());
            fntHandle = fntLoadFile(fullPath, fontSize); // 使用外挂字体
            if (fntHandle == FNT_ERROR) {
                fullPath[len] = '\0';
                snprintf(fullPath, sizeof(fullPath), "%sfont_%s.otf", fullPath, lngGetValue());
                fntHandle = fntLoadFile(fullPath, fontSize); // 使用外挂字体
            }
            //// debug  打印debug信息
            // char debugFileDir[64];
            // strcpy(debugFileDir, "smb:debug-themes.txt");
            // FILE *debugFile = fopen(debugFileDir, "ab+");
            // if (debugFile != NULL) {
            //     fprintf(debugFile, "fntHandle:%d\r\nfullPath:%s\r\n\r\n", fntHandle, fullPath);
            //     fclose(debugFile);
            // }
        } else
            fntHandle = fntLoadFile(NULL, fontSize); // 使用默认字体

        // Do we have a valid font? Assign the font handle to the theme font slot
        if (fntHandle != FNT_ERROR)
            theme->fonts[fntID] = fntHandle;   
    }
}

static void thmLoad(const char *themePath)
{
    LOG("THEMES Load theme path=%s\n", themePath);
    char path[256];
    theme_t *curT = gTheme;
    theme_t *newT = (theme_t *)malloc(sizeof(theme_t));
    memset(newT, 0, sizeof(theme_t));

    newT->useDefault = 1;
    newT->usedHeight = 480;
    thmSetColors(newT);
    newT->mainElems.first = NULL;
    newT->mainElems.last = NULL;
    newT->infoElems.first = NULL;
    newT->infoElems.last = NULL;
    newT->appsMainElems.first = NULL;
    newT->appsMainElems.last = NULL;
    newT->appsInfoElems.first = NULL;
    newT->appsInfoElems.last = NULL;
    newT->gameCacheCount = 0;
    newT->itemsList = NULL;
    newT->gamesItemsList = NULL;
    newT->appsItemsList = NULL;
    newT->loadingIcon = NULL;
    newT->loadingIconCount = LOAD7_ICON - LOAD0_ICON + 1;

    config_set_t *themeConfig = NULL;
    if (!themePath) {
        // 未指定主题路径：加载内置主题模板。根据 builtinThemeID 选择
        // 默认主题还是内置 Coverflow 主题（两者都不依赖外部图片资源）。
        themeConfig = configAlloc(0, NULL, NULL);
        if (builtinThemeID == 1)
            configReadBuffer(themeConfig, &conf_theme_coverflow_cfg, size_conf_theme_coverflow_cfg);
        else
            configReadBuffer(themeConfig, &conf_theme_OPL_cfg, size_conf_theme_OPL_cfg);
    } else {
        snprintf(path, sizeof(path), "%sconf_theme.cfg", themePath);
        themeConfig = configAlloc(0, NULL, path);
        configRead(themeConfig); // try to load the theme config file. If it does not exist, defaults will be used.
    }

    int intValue;
    if (configGetInt(themeConfig, "use_default", &intValue))
        newT->useDefault = intValue;

    if (configGetInt(themeConfig, "use_real_height", &intValue)) {
        if (intValue)
            newT->usedHeight = screenHeight;
    }

    configGetColor(themeConfig, "bg_color", newT->bgColor);

    unsigned char color[3];
    if (configGetColor(themeConfig, "text_color", color))
        newT->textColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    if (configGetColor(themeConfig, "ui_text_color", color))
        newT->uiTextColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    if (configGetColor(themeConfig, "sel_text_color", color))
        newT->selTextColor = GS_SETREG_RGBA(color[0], color[1], color[2], 0x80);

    // Coverflow 整排的可选水平微调（每单位为 1/256 个封面宽度）。
    configGetInt(themeConfig, "coverflow_cover_offset", &newT->coverflowCoverOffset);

    // Coverflow 定制参数：【优先读取主题 cfg 内的定制值；主题没写才回退到引擎默认值】。
    // 每次加载主题都先把这些全局复位为默认，再按当前主题覆盖，避免切换到未定义这些键的
    // 主题时残留上一个主题的设置。本函数在元素定义解析（initCoverflow）之前运行，因此这里
    // 读到的 count/preload 在分配封面缓存时即可用。
    //   coverflow_count            —— 同屏封面数（夹取到 1..COVERFLOW_MAX）
    //   coverflow_center_scale     —— 中间封面额外放大的像素数
    //   coverflow_animation_speed  —— 滑动动画时长（毫秒，<=0 关闭动画）
    //   coverflow_dim_covers       —— 非中心封面是否变暗（0/1）
    //   coverflow_preload          —— 每侧屏幕外预取封面数（如填 3 = 左右各 3、共 6；无上限）
    gCoverflowCount = COVERFLOW_DEFAULT_COUNT;
    gCoverflowCenterScale = COVERFLOW_DEFAULT_SCALE;
    gCoverflowAnimSpeed = COVERFLOW_DEFAULT_ANIM;
    gCoverflowDimCovers = COVERFLOW_DEFAULT_DIM;
    gCoverflowPreload = COVERFLOW_DEFAULT_PRELOAD;
    configGetInt(themeConfig, "coverflow_count", &gCoverflowCount);
    configGetInt(themeConfig, "coverflow_center_scale", &gCoverflowCenterScale);
    configGetInt(themeConfig, "coverflow_animation_speed", &gCoverflowAnimSpeed);
    configGetInt(themeConfig, "coverflow_dim_covers", &gCoverflowDimCovers);
    configGetInt(themeConfig, "coverflow_preload", &gCoverflowPreload);
    // count 夹取到显示数组上限，防止 covers[]/drawOrder[] 越界崩溃；preload 只挡负值、不设上限
    //（主题填过大导致内存/崩溃属用户行为，不额外处理）。
    if (gCoverflowCount < 1)
        gCoverflowCount = 1;
    if (gCoverflowCount > COVERFLOW_MAX)
        gCoverflowCount = COVERFLOW_MAX;
    if (gCoverflowPreload < 0)
        gCoverflowPreload = 0;

    // before loading the element definitions, we have to have the fonts prepared
    // for that, we load the fonts and a translation table
    if (themePath)
        thmLoadFonts(themeConfig, themePath, newT);

    int i = 1, j;
    snprintf(path, sizeof(path), "main0");
    while (addGUIElem(themePath, themeConfig, newT, &newT->mainElems, NULL, path))
        snprintf(path, sizeof(path), "main%d", i++);

    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "appsMain%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->appsMainElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "main%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->appsMainElems, NULL, path);
        }
    }

    i = 1;
    snprintf(path, sizeof(path), "info0");
    while (addGUIElem(themePath, themeConfig, newT, &newT->infoElems, NULL, path))
        snprintf(path, sizeof(path), "info%d", i++);

    for (j = 0; j < i; j++) {
        snprintf(path, sizeof(path), "appsInfo%d", j);

        if (addGUIElem(themePath, themeConfig, newT, &newT->appsInfoElems, NULL, path))
            continue;
        else {
            snprintf(path, sizeof(path), "info%d", j);
            addGUIElem(themePath, themeConfig, newT, &newT->appsInfoElems, NULL, path);
        }
    }

    if (themePath)
        validateGUIElems(themePath, themeConfig, newT);

    newT->itemsList = newT->gamesItemsList;

    configFree(themeConfig);

    LOG("THEMES Number of cache: %d\n", newT->gameCacheCount);
    LOG("THEMES Used height: %d\n", newT->usedHeight);

    // default all to not loaded...
    for (i = 0; i < TEXTURES_COUNT; i++)
        newT->textures[i].Mem = NULL;

    // LOGO, loaded here to avoid flickering during startup with device in AUTO + theme set
    texLoadInternal(&newT->textures[LOGO_PICTURE], LOGO_PICTURE);

    // First start with busy icon
    const char *themePath_temp = themePath;
    int customBusy = 0;
    for (i = LOAD0_ICON; i <= LOAD7_ICON; i++) {
        if (thmLoadResource(&newT->textures[i], i, themePath_temp, GS_PSM_CT32, newT->useDefault) >= 0)
            customBusy = 1;
        else {
            if (customBusy)
                break;
            else
                themePath_temp = NULL;
        }
    }
    newT->loadingIconCount = i;

    // Customizable icons
    for (i = BDM_ICON; i <= START_ICON; i++)
        thmLoadResource(&newT->textures[i], i, themePath, GS_PSM_CT32, newT->useDefault);

    // 内置 Coverflow 主题(builtinThemeID==1, 无外部主题路径)的设备图标改用 wOPL 风格的
    // 单设备大图(带 BDM/SMB/APA/APPS 标签), 而不是默认列表主题那套四合一标签条。
    // 仅覆盖内置 Coverflow 这一套的 textures[] 槽位, 默认列表主题与外部主题都不受影响。
    // 上面的循环已把默认图标载入这些槽, 这里先 texFree 再载入 cf_ 版本以避免内存泄漏。
    if (!themePath && builtinThemeID == 1) {
        static const struct
        {
            int slot;
            int cfTex;
        } cfDevIcons[] = {
            {BDM_ICON, CF_DEV_BDM},
            {USB_ICON, CF_DEV_USB},
            {ILINK_ICON, CF_DEV_ILK},
            {MX4SIO_ICON, CF_DEV_M4S},
            {HDD_BD_ICON, CF_DEV_HDD_BD},
            {HDD_ICON, CF_DEV_HDD},
            {ETH_ICON, CF_DEV_ETH},
            {APP_ICON, CF_DEV_APP}};
        int k;
        for (k = 0; k < (int)(sizeof(cfDevIcons) / sizeof(cfDevIcons[0])); k++) {
            texFree(&newT->textures[cfDevIcons[k].slot]);
            texLoadInternal(&newT->textures[cfDevIcons[k].slot], cfDevIcons[k].cfTex);
        }
    }

    // 缺图占位纹理 COVER_DEFAULT：Coverflow / GameImage 等元素在未显式配置 default= 时，
    // 会退回 thmGetTexture(COVER_DEFAULT) 作为缺图占位。但上面的常规加载区间并不覆盖
    // COVER_DEFAULT（它落在 MAIN_BG..VMODE_PAL 之间，而该段仅在内置主题、且从 ELF_FORMAT
    // 起才加载），导致这张占位纹理对任何主题都从未被加载、恒为 NULL。于是没有在 coverflow
    // 段写 default=cover 的第三方主题，缺图时既无 img->defaultTexture 也无 COVER_DEFAULT，
    // 占位图完全不显示。这里从主题目录加载 cover.png（缺失时回退内置 cover_png，useDefault
    // 恒为 1 以保证始终有占位），使第三方主题即便省略 default= 也能显示缺图占位（wOPL 行为）。
    thmLoadResource(&newT->textures[COVER_DEFAULT], COVER_DEFAULT, themePath, GS_PSM_CT32, 1);

    /* Not customizable icons - currently unused.
    for (i = L1_ICON; i <= R3_ICON; i++)
        thmLoadResource(&newT->textures[i], i, NULL, GS_PSM_CT32, 1); */

    if (!themePath)
        for (i = ELF_FORMAT; i <= VMODE_PAL; i++)
            thmLoadResource(&newT->textures[i], i, NULL, GS_PSM_CT32, 1);

    gTheme = newT;
    thmFree(curT);
}

static void thmRebuildGuiNames(void)
{
    if (guiThemesNames)
        free(guiThemesNames);

    // build the themes name list
    // 列表结构：THM_NUM_BUILTIN 套内置主题 + nThemes 套用户主题 + 1 个 NULL 结束标记
    guiThemesNames = (const char **)malloc((nThemes + THM_NUM_BUILTIN + 1) * sizeof(char **));

    // 两套内置主题固定排在最前
    guiThemesNames[0] = "强化原生主题-支持背景图";
    guiThemesNames[1] = THM_COVERFLOW_NAME;

    // 用户主题接在内置主题之后
    int i = 0;
    for (; i < nThemes; i++) {
        guiThemesNames[i + THM_NUM_BUILTIN] = themes[i].name;
    }

    guiThemesNames[nThemes + THM_NUM_BUILTIN] = NULL;
}

int thmAddElements(char *path, const char *separator, int forceRefresh)
{
    int result, i;

    result = listDir(path, separator, THM_MAX_FILES - nThemes, &thmReadEntry);
    nThemes += result;
    thmRebuildGuiNames();

    const char *temp;
    if (configGetStr(configGetByType(CONFIG_OPL), "theme", &temp)) {
        LOG("THEMES Trying to set again theme: %s\n", temp);
        if (thmSetGuiValue(thmFindGuiID(temp), 0) && forceRefresh) {
            for (i = 0; i < MODE_COUNT; i++)
                moduleUpdateMenu(i, 1, 0);
        }
    }

    return result;
}

void thmInit(void)
{
    LOG("THEMES Init\n");
    gTheme = NULL;

    thmReloadScreenExtents();

    // initialize default internal
    thmLoad(NULL);

    thmAddElements(gBaseMCDir, "/", 0);
}

void thmReinit(const char *path)
{
    builtinThemeID = 0;
    thmLoad(NULL);
    guiThemeID = 0;

    int i = 0;
    while (i < nThemes) {
        if (strncmp(themes[i].filePath, path, strlen(path)) == 0) {
            LOG("THEMES Remove theme: %s\n", themes[i].filePath);
            nThemes--;
            free(themes[i].name);
            themes[i].name = themes[nThemes].name;
            themes[nThemes].name = NULL;
            free(themes[i].filePath);
            themes[i].filePath = themes[nThemes].filePath;
            themes[nThemes].filePath = NULL;
        } else
            i++;
    }

    thmRebuildGuiNames();
}

void thmReloadScreenExtents(void)
{
    rmGetScreenExtents(&screenWidth, &screenHeight);
}

const char *thmGetValue(void)
{
    return guiThemesNames[guiThemeID];
}

int thmSetGuiValue(int themeID, int reload)
{
    if (themeID != -1) {
        if (guiThemeID != themeID || reload) {
            if (themeID < THM_NUM_BUILTIN) {
                // 内置主题（0=默认，1=Coverflow）：无外部路径，走内置模板
                builtinThemeID = themeID;
                thmLoad(NULL);
            } else {
                // 用户主题：GUI 索引需减去内置主题数量才是 themes[] 下标
                thmLoad(themes[themeID - THM_NUM_BUILTIN].filePath);
            }

            guiThemeID = themeID;

            //ForceRefreshPrevTexCache = 1; // 刷新ART缓存，防止死机
            return 1;
        } else if (guiThemeID < THM_NUM_BUILTIN)
            thmSetColors(gTheme);
    }
    return 0;
}

int thmGetGuiValue(void)
{
    return guiThemeID;
}

int thmFindGuiID(const char *theme)
{
    if (theme) {
        // 先匹配内置 Coverflow 主题（GUI 索引 1）
        if (strcasecmp(theme, THM_COVERFLOW_NAME) == 0)
            return 1;
        // 再匹配用户主题（GUI 索引需加上内置主题数量偏移）
        int i = 0;
        for (; i < nThemes; i++) {
            if (strcasecmp(themes[i].name, theme) == 0)
                return i + THM_NUM_BUILTIN;
        }
    }
    // 未匹配到则回退默认主题（GUI 索引 0）
    return 0;
}

const char **thmGetGuiList(void)
{
    return guiThemesNames;
}

char *thmGetFilePath(int themeID)
{
    // 仅用户主题拥有文件路径；GUI 索引需减去内置主题数量才是 themes[] 下标。
    theme_file_t *currTheme = &themes[themeID - THM_NUM_BUILTIN];
    char *path = currTheme->filePath;

    return path;
}

void thmEnd(void)
{
    thmFree(gTheme);

    int i = 0;
    for (; i < nThemes; i++) {
        free(themes[i].name);
        free(themes[i].filePath);
    }

    free(guiThemesNames);
}
