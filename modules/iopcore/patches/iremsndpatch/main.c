#include <intrman.h>
#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <thbase.h>
#include <thsemap.h>

#define MODNAME "iremsndpatch"
IRX_ID(MODNAME, 1, 1);

#define JAL(addr) (0x0c000000 | (((addr)&0x03ffffff) >> 2))
#define JMP(addr) (0x08000000 | (0x3ffffff & ((addr) >> 2)))

/* There was a memo dated July 2000, which went along the lines of saying that the atick functions of modmidi and modhsyn should be called,
   if related sequence data is stopped and removed.

   The explanation was that these modules only process sequence data when their atick functions are called.
   If data is removed and their atick functions are not called before further calls to the library are made,
   then the correct operation of the library cannot be guaranteed because the data would be dequeued.

   By logging the commands issued to the RPC server of IREMSND, it can be established that the game eventually calls sndIopSesqCmd_STOPSE.
   But sndIopSesqCmd_STOPSE does not ever call the modseseq and/or modhsyn atick functions!

   So to correct this, we wake up the tick thread, after the sndIopSesqCmd_STOPSE, sndIopSesqCmd_STOPSEID and sndIopCmd_STOPALL RPC functions are called.
   While Sony also documented that this could cause a change in tempo, it is a small price to pay for such a simple fix.

   The inter-task protection mechanism was a simple one that involved a flag, which ran under the assumption that the atick thread will always run where the RPC thread does not.
   However, if the atick thread does get changed to WAIT, then the RPC thread can RUN (which allows the critical section to be violated). */

extern void _lock_RpcFunc(void);

static int *tickThreadId;
static int lockSema;

static int postStopSeseqTick(void)
{ // Even if the atick thread waits on the locking semaphore, it will wake up immediately when the RPC thread unlocks the critical section.
    WakeupThread(*tickThreadId);
    return 1;
}

int _lock(void)
{
    return WaitSema(lockSema);
}

int _unlock(void)
{
    return SignalSema(lockSema);
}


static int isSupportedIremSnd(const ModuleInfo_t *m)
{
    static const struct {
        u16 offset;
        u16 opcode;
    } checks[] = {
        {0x0418, 0x8c22}, {0x0ac0, 0x3c01}, {0x0ac4, 0xac22},
        {0x0d58, 0x3c01}, {0x0d64, 0xac20}, {0x0d70, 0x3c01},
        {0x0d80, 0xac23}, {0x0f54, 0x3c01}, {0x0f58, 0xac20},
        {0x18f4, 0x03e0}, {0x1f4c, 0x03e0}, {0x1ff8, 0x03e0},
        {0x0c30, 0x03e0}
    };
    const u32 *text = (const u32 *)m->text_start;
    unsigned int i;

    /* 先确认整套补丁适用，避免未知版本或重复加载时只改写部分指令。 */
    if (m->text_start == 0 || m->text_size < 0x2000)
        return 0;
    for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        u32 instruction = text[checks[i].offset / 4];

        if ((instruction >> 16) != checks[i].opcode)
            return 0;
        if (checks[i].opcode == 0x03e0 && instruction != 0x03e00008)
            return 0;
    }
    /* 新锁入口仍依赖原延迟槽保存参数，迁移地址对之间不能夹入对 at 的修改。 */
    return text[0x0d5c / 4] == 0x00809021 &&
           text[0x0d74 / 4] == 0x24020ff0 &&
           text[0x0d78 / 4] == 0x00a08821 &&
           text[0x0d7c / 4] == 0x00c08021;
}

int _start(int argc, char **argv)
{
    lc_internals_t *lc;
    ModuleInfo_t *m;
    u16 hi16;
    s16 lo16;
    iop_sema_t sema;
    int modId;
    u32 retHi, retClear;

    if (argc != 2) {
        printf("Missing module ID arg.\n");
        return MODULE_NO_RESIDENT_END;
    }

    modId = strtol(argv[1], NULL, 16);

    lc = GetLoadcoreInternalData();

    // Locate the specified module.
    m = lc->image_info;
    while (m != NULL) {
        if (modId == m->id)
            break;

        m = m->next;
    }

    if (m != NULL) {
        if (!isSupportedIremSnd(m)) {
            printf("IREMSND patch: unsupported module layout.\n");
            return MODULE_NO_RESIDENT_END;
        }

        /* 保存加载器已重定位的完整地址对，不能在覆盖原位置后再读取。 */
        retHi = *(vu32 *)(m->text_start + 0x00000d58);
        retClear = *(vu32 *)(m->text_start + 0x00000d64);

        sema.initial = 1;
        sema.max = 1;
        sema.option = 0;
        sema.attr = 0;

        lockSema = CreateSema(&sema);
        if (lockSema < 0) {
            printf("IREMSND patch: semaphore creation failed.\n");
            return MODULE_NO_RESIDENT_END;
        }

        // Generate pointer to the threadId variable.
        hi16 = *(vu16 *)(m->text_start + 0x00000ac0);
        lo16 = *(volatile s16 *)(m->text_start + 0x00000ac4);
        tickThreadId = (int *)(((u32)hi16 << 16) + lo16);

        // Apply patch on module. The module has not been initialized yet and has no running threads.
        // sndIopAtick
        *(vu32 *)(m->text_start + 0x00000418) = JAL((u32)&_lock);
        *(vu32 *)(m->text_start + 0x000018f4) = JMP((u32)&_unlock);

        // sndIopRpcFunc
        *(vu32 *)(m->text_start + 0x00000d58) = JAL((u32)&_lock_RpcFunc);
        *(vu32 *)(m->text_start + 0x00000d64) = 0x00000000; // nop
        /* 信号量已替代 cmdExec 标志，在原标志更新位置清零本次 RPC 返回值。 */
        *(vu32 *)(m->text_start + 0x00000d70) = retHi;
        *(vu32 *)(m->text_start + 0x00000d80) = retClear;
        *(vu32 *)(m->text_start + 0x00000f54) = JAL((u32)&_unlock);
        *(vu32 *)(m->text_start + 0x00000f58) = 0x00000000; // nop

        // sndIopSesqCmd_STOPSE
        *(vu32 *)(m->text_start + 0x00001f4c) = JMP((u32)&postStopSeseqTick);
        // sndIopSesqCmd_STOPSEID
        *(vu32 *)(m->text_start + 0x00001ff8) = JMP((u32)&postStopSeseqTick);
        // sndIopCmd_STOPALL
        *(vu32 *)(m->text_start + 0x00000c30) = JMP((u32)&postStopSeseqTick);

        FlushIcache(); // Flush instruction cache as instructions were modified.

        return MODULE_RESIDENT_END;
    }

    printf("Could not find module %d.\n", modId);

    return MODULE_NO_RESIDENT_END;
}
