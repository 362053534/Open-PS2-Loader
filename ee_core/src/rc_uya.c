#include "rc_uya.h"

int RnC3_IsMultiplayerElf(const char *path)
{
    static const char multiplayer_elf[] = "I5BOOTN.ELF";
    const char *name;
    const char *p;
    unsigned int i;

    if (path == NULL)
        return 0;
    name = path;
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

    /* 直接检查版本号可保持为叶函数，省去 strcmp 调用及保存寄存器的栈帧。 */
    name += i;
    return name[0] == '\0' || (name[0] == ';' && name[1] == '1' && name[2] == '\0');
}
