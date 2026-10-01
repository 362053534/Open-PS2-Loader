#include <stddef.h>

#include "rc_uya.h"
#include "util.h"

/* EE core 的 BSS 会跨 LoadExecPS2 保留；每次换 ELF 必须覆盖，IOP 重启则沿用。 */
static int rnc_uya_multiplayer;

static int isMultiplayerElf(const char *path)
{
    static const char multiplayer_elf[] = "I5BOOTN.ELF";
    const char *name = path;
    const char *p;
    unsigned int i;

    for (p = path; *p != '\0'; p++) {
        if (*p == ':' || *p == '/' || *p == '\\')
            name = p + 1;
    }

    for (i = 0; multiplayer_elf[i] != '\0'; i++) {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
        if (ch != multiplayer_elf[i])
            return 0;
    }

    /* 只接受完整文件名及 ISO9660 版本号，防止相似文件名误启用固定地址补丁。 */
    name += i;
    return *name == '\0' || _strcmp(name, ";1") == 0;
}

void RnC3_SetCurrentElf(const char *game_id, const char *path)
{
    rnc_uya_multiplayer = 0;
    if (game_id == NULL || path == NULL)
        return;

    /* 其他游戏也可能使用同名 ELF，不能仅凭文件名拦截其 sysmem。 */
    if (_strcmp(game_id, "SCUS_973.53") != 0 &&
        _strcmp(game_id, "SCES_524.56") != 0 &&
        _strcmp(game_id, "SCPS_150.84") != 0)
        return;

    rnc_uya_multiplayer = isMultiplayerElf(path);
}

int RnC3_NeedsIopPatch(void)
{
    return rnc_uya_multiplayer;
}
