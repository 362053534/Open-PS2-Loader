/*
 * Low-level cheat engine
 *
 * Copyright (C) 2009-2010 Mathias Lafeldt <misfire@debugon.org>
 *
 * This file is part of PS2rd, the PS2 remote debugger.
 *
 * PS2rd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PS2rd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PS2rd.  If not, see <http://www.gnu.org/licenses/>.
 *
 * $Id$
 */

#include <tamtypes.h>
#include <kernel.h>
#include <syscallnr.h>
#include "include/cheat_api.h"
#include "coreconfig.h"
#include "util.h"

static const code_t ValkyrieProfile2GameGuardCodes[] = {
    {0x9013A450, 0x0C04E8BC},

    // 每项沿用原补丁的一条条件配一条写入，避免多行跳过语义造成兼容差异。
    {0xD0428B28, 0x00000010}, {0x20428AF4, 0x24160000},
    {0xD0428B28, 0x00000010}, {0x20428D10, 0x24160000},
    {0xD0428B28, 0x00000010}, {0x20428F30, 0x24160000},
    {0xD03897F4, 0x0000FFFA}, {0x203897F4, 0x00000000},
    {0xD03DEB6C, 0x0000FFFA}, {0x203DEB6C, 0x00000000},
    {0xD042D0AC, 0x0000FFFA}, {0x2042D0AC, 0x00000000},

    // 调用点自身最后再改写，确保同一轮中的其他三项仍能通过条件检查。
    {0xD04A424C, 0x00009720}, {0x204A5DEC, 0x00000000},
    {0xD04A424C, 0x00009720}, {0x204A5F54, 0x00000000},
    {0xD04A424C, 0x00009720}, {0x204A60BC, 0x00000000},
    {0xD04A424C, 0x00009720}, {0x204A424C, 0x00000000},
};

int HasBuiltInCheats(void)
{
    USE_LOCAL_EECORE_CONFIG;

    return _strcmp(config->GameID, "SLPM_664.19") == 0;
}

/*---------------------------------*/
/* Setup PS2RD Cheat Engine params */
/*---------------------------------*/
void SetupCheats()
{
    USE_LOCAL_EECORE_CONFIG;
    code_t code;

    int i, j, k, nextCodeCanBeHook;
    i = 0;
    j = 0;
    k = 0;
    nextCodeCanBeHook = 1;

    // 目标代码由游戏运行时展开，必须通过常驻处理器等待其出现后再改写。
    if (HasBuiltInCheats()) {
        hooklist[j++] = ValkyrieProfile2GameGuardCodes[0].addr & 0x01FFFFFC;
        hooklist[j++] = ValkyrieProfile2GameGuardCodes[0].val;

        for (i = 1; i < sizeof(ValkyrieProfile2GameGuardCodes) / sizeof(ValkyrieProfile2GameGuardCodes[0]); i++) {
            codelist[k++] = ValkyrieProfile2GameGuardCodes[i].addr;
            codelist[k++] = ValkyrieProfile2GameGuardCodes[i].val;
        }
    }

    i = 0;
    while (config->gCheatList != NULL && i < MAX_CHEATLIST) {

        code.addr = config->gCheatList[i];
        code.val = config->gCheatList[i + 1];
        i += 2;

        if ((code.addr == 0) && (code.val == 0))
            break;

        if (((code.addr & 0xfe000000) == 0x90000000) && nextCodeCanBeHook == 1) {
            if (j < MAX_HOOKS * 2) {
                hooklist[j] = code.addr & 0x01FFFFFC;
                j++;
                hooklist[j] = code.val;
                j++;
            }
        } else {
            if (k < MAX_CODES * 2) {
                codelist[k] = code.addr;
                k++;
                codelist[k] = code.val;
                k++;
            }
        }
        // Discard any false positives from being possible hooks
        if ((code.addr & 0xf0000000) == 0x40000000 || (code.addr & 0xf0000000) == 0x30000000) {
            nextCodeCanBeHook = 0;
        } else {
            nextCodeCanBeHook = 1;
        }
    }
    numhooks = j / 2;
    numcodes = k / 2;
}

/*-----------------------------------------------------*/
/* Replace SetupThread in kernel. (PS2RD Cheat Engine) */
/*-----------------------------------------------------*/
static inline void Install_HookSetupThread(void)
{
    Old_SetupThread = GetSyscallHandler(__NR_SetupThread);
    SetSyscall(__NR_SetupThread, HookSetupThread);
}

/*----------------------------------------------------*/
/* Restore original SetupThread. (PS2RD Cheat Engine) */
/*----------------------------------------------------*/
static inline void Remove_HookSetupThread(void)
{
    SetSyscall(__NR_SetupThread, Old_SetupThread);
}

/*---------------------------*/
/* Enable PS2RD Cheat Engine */
/*---------------------------*/
void EnableCheats(void)
{
    // Setup Cheats
    SetupCheats();
    // Install Hook SetupThread
    Install_HookSetupThread();
}

/*----------------------------*/
/* Disable PS2RD Cheat Engine */
/*----------------------------*/
void DisableCheats(void)
{
    // Remove Hook SetupThread
    Remove_HookSetupThread();
}
