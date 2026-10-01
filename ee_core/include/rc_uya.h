#ifndef EE_CORE_RC_UYA_H_
#define EE_CORE_RC_UYA_H_

#include <string.h>

/* 游戏 ID 只在前端判定，避免把三次字符串比较带进空间紧张的常驻 EE core。 */
static inline int RnC3_IsGameID(const char *game_id)
{
    return game_id != NULL &&
           (strcmp(game_id, "SCUS_973.53") == 0 ||
            strcmp(game_id, "SCES_524.56") == 0 ||
            strcmp(game_id, "SCPS_150.84") == 0);
}

int RnC3_IsMultiplayerElf(const char *path);

#endif
