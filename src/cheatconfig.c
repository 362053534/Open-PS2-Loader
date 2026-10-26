#include "cheatconfig.h"
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CHEAT_SELECTION_KEY_MAX CONFIG_KEY_NAME_LEN

// 校验值把第一行代码的值循环移位后与地址异或，一个整数里同时保留地址和值的信息
#define CHEAT_CHECK_VALUE_ROTATE_BITS 16

static void normalizeCheatName(const char *source, char *destination, size_t destinationSize)
{
    size_t output = 0;
    int pendingSpace = 0;

    if (destinationSize == 0)
        return;

    while (*source != NUL && isspace((unsigned char)*source))
        source++;

    while (*source != NUL && output + 1 < destinationSize) {
        unsigned char character = (unsigned char)*source++;

        if (isspace(character)) {
            pendingSpace = output > 0;
            continue;
        }

        if (pendingSpace) {
            destination[output++] = SPACE;
            pendingSpace = 0;
        }

        destination[output++] = tolower(character);
    }

    destination[output] = NUL;
}

// 用第一行代码（地址和值）算出一个校验值，不含标题，用来区分同名金手指
static u32 cheatCodeHash(const cheat_entry_t *cheat)
{
    u32 addr = cheat->codes[0].addr;
    u32 val = cheat->codes[0].val;

    return addr ^ ((val << CHEAT_CHECK_VALUE_ROTATE_BITS) | (val >> (32 - CHEAT_CHECK_VALUE_ROTATE_BITS)));
}

int cheatConfigIsMasterCode(const char *name)
{
    char normalized[CHEAT_NAME_MAX + 1];

    normalizeCheatName(name, normalized, sizeof(normalized));

    /* 魔改金手指常在主代码名后追加后缀，因此按前缀匹配。 */
    return strncmp(normalized, "mastercode", 10) == 0 ||
           strncmp(normalized, "master code", 11) == 0 ||
           strncmp(normalized, "enable code", 11) == 0;
}

// 第一条有代码的条目，标题以 m 或 M 加一个空格开头，或者任意位置含“码”字，也当作主代码
#define CHEAT_FIRST_MASTER_PREFIX_SPACE_INDEX 1
#define CHEAT_FIRST_MASTER_KEYWORD "码"

int cheatConfigIsFirstEntryMasterTitle(const char *name)
{
    if ((name[0] == 'm' || name[0] == 'M') && name[CHEAT_FIRST_MASTER_PREFIX_SPACE_INDEX] == SPACE)
        return 1;

    return strstr(name, CHEAT_FIRST_MASTER_KEYWORD) != NULL;
}

// 同名金手指的标题按原样逐字节比较，不忽略空格，也不统一大小写
static int cheatNamesMatch(const char *first, const char *second)
{
    return strcmp(first, second) == 0;
}

void cheatConfigLoadSelections(config_set_t *configSet)
{
    int cheatIndex;
    int selectionIndex;
    int selectionCount = 0;
    int pass;
    char key[CHEAT_SELECTION_KEY_MAX];
    // 数组较大，放静态区，避免占用线程栈
    static char savedName[MAX_CODES][CONFIG_KEY_VALUE_LEN];
    static int savedHash[MAX_CODES];
    static int savedHasHash[MAX_CODES];
    static char savedMatched[MAX_CODES];

    /* First use is safe: required code on, every optional code off. */
    for (cheatIndex = 0; cheatIndex < MAX_CODES && gCheats[cheatIndex].name[0] != NUL; cheatIndex++)
        gCheats[cheatIndex].enabled = !cheatIsInfoEntry(&gCheats[cheatIndex]) && gCheats[cheatIndex].isMaster;

    if (configSet == NULL)
        return;

    // 先读出全部已保存的选择，后面分两轮匹配
    for (selectionIndex = 0; selectionIndex < MAX_CODES; selectionIndex++) {
        snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_PREFIX, selectionIndex);

        if (!configGetStrCopy(configSet, key, savedName[selectionIndex], sizeof(savedName[selectionIndex])))
            break;

        snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_ID_PREFIX, selectionIndex);
        savedHasHash[selectionIndex] = configGetInt(configSet, key, &savedHash[selectionIndex]);
        savedMatched[selectionIndex] = 0;
        selectionCount++;
    }

    /* 第一轮：标题和第一行代码都一致的才算精确匹配；第二轮：没有精确匹配上的（旧配置或第一行代码被编辑过）按标题匹配第一个尚未勾选的同名金手指。
     * 每个金手指最多被一条选择占用，所以同名金手指不会都落到第一个上。 */
    for (pass = 0; pass < 2; pass++) {
        for (selectionIndex = 0; selectionIndex < selectionCount; selectionIndex++) {
            if (savedMatched[selectionIndex])
                continue;

            if (pass == 0 && !savedHasHash[selectionIndex])
                continue;

            for (cheatIndex = 0; cheatIndex < MAX_CODES && gCheats[cheatIndex].name[0] != NUL; cheatIndex++) {
                if (gCheats[cheatIndex].enabled || cheatIsInfoEntry(&gCheats[cheatIndex]) || gCheats[cheatIndex].isMaster)
                    continue;

                if (!cheatNamesMatch(gCheats[cheatIndex].name, savedName[selectionIndex]))
                    continue;

                if (pass == 0 && (u32)savedHash[selectionIndex] != cheatCodeHash(&gCheats[cheatIndex]))
                    continue;

                gCheats[cheatIndex].enabled = 1;
                savedMatched[selectionIndex] = 1;
                break;
            }
        }
    }

    /* Database changes must never leave the required code disabled. */
    for (cheatIndex = 0; cheatIndex < MAX_CODES && gCheats[cheatIndex].name[0] != NUL; cheatIndex++) {
        if (!cheatIsInfoEntry(&gCheats[cheatIndex]) && gCheats[cheatIndex].isMaster)
            gCheats[cheatIndex].enabled = 1;
    }
}

void cheatConfigSaveSelections(config_set_t *configSet)
{
    int cheatIndex;
    int selectionIndex = 0;
    char key[CHEAT_SELECTION_KEY_MAX];

    if (configSet == NULL)
        return;

    /* Clear stale numbered values from earlier selections. */
    for (cheatIndex = 0; cheatIndex < MAX_CODES; cheatIndex++) {
        snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_PREFIX, cheatIndex);
        configRemoveKey(configSet, key);
        snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_ID_PREFIX, cheatIndex);
        configRemoveKey(configSet, key);
    }

    for (cheatIndex = 0; cheatIndex < MAX_CODES && gCheats[cheatIndex].name[0] != NUL; cheatIndex++) {
        if (gCheats[cheatIndex].enabled && !gCheats[cheatIndex].isMaster) {
            snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_PREFIX, selectionIndex);
            configSetStr(configSet, key, gCheats[cheatIndex].name);
            snprintf(key, sizeof(key), "%s%03d", CONFIG_ITEM_CHEAT_SELECTION_ID_PREFIX, selectionIndex);
            configSetInt(configSet, key, (int)cheatCodeHash(&gCheats[cheatIndex]));
            selectionIndex++;
        }
    }
}
