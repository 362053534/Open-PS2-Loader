#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rc_uya.h"
#include "coreconfig.h"

struct EECoreConfig_t g_ee_core_config;

static void selectGame(const char *game_id)
{
    g_ee_core_config.EnableRnC3UyaPatch = RnC3_IsGameID(game_id);
    g_ee_core_config.RnC3UyaMultiplayer = 0;
}

static void setCurrentElf(const char *path)
{
    g_ee_core_config.RnC3UyaMultiplayer = g_ee_core_config.EnableRnC3UyaPatch && RnC3_IsMultiplayerElf(path);
}

/* 包含生产实现，仅重命名入口；IOP 静态状态可据此模拟重启清零。 */
#define _start rcuyapatch_start
#include "../../modules/iopcore/patches/rcuyapatch/main.c"
#undef _start

#define RAM_SIZE   0x200000
#define PAGE_SIZE  256
#define PAGE_COUNT (RAM_SIZE / PAGE_SIZE)
#define FIRST_PAGE (0x10000 / PAGE_SIZE)

struct block
{
    unsigned int first;
    unsigned int pages;
};

static unsigned char allocated[PAGE_COUNT];
static struct block blocks[32];
static iop_library_t sysmem_lib;
static lc_internals_t loadcore;
static struct irx_import_table callers[2];
static unsigned int used_bytes;
static int alloc_calls;
static int free_calls;
static int last_mode;
static int last_size;
static void *last_ptr;
static int interrupts_enabled;
static int suspend_calls;
static int resume_calls;
static int icache_flushes;
static int diagnostic_calls;
static int interleave_on_resume;
static void *interleaved_result;
static int invalidate_export_on_alloc;
static int invalidate_export_on_suspend;

static int isFree(unsigned int first, unsigned int pages)
{
    unsigned int i;

    if (first < FIRST_PAGE || first >= PAGE_COUNT || pages > PAGE_COUNT - first)
        return 0;
    for (i = first; i < first + pages; i++) {
        if (allocated[i])
            return 0;
    }
    return 1;
}

static void *testAlloc(int mode, int size, void *ptr)
{
    unsigned int first, pages, slot;

    alloc_calls++;
    last_mode = mode;
    last_size = size;
    last_ptr = ptr;
    if (mode < ALLOC_FIRST || mode > ALLOC_ADDRESS || size <= 0 || size > RAM_SIZE)
        return NULL;
    pages = ((unsigned int)size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (mode == ALLOC_ADDRESS) {
        uintptr_t address = (uintptr_t)ptr;
        if (address >= RAM_SIZE || address % PAGE_SIZE != 0)
            return NULL;
        first = address / PAGE_SIZE;
        if (!isFree(first, pages))
            return NULL;
    } else if (mode == ALLOC_LAST) {
        first = PAGE_COUNT - pages;
        while (first >= FIRST_PAGE && !isFree(first, pages))
            first--;
        if (first < FIRST_PAGE)
            return NULL;
    } else {
        first = FIRST_PAGE;
        while (first < PAGE_COUNT && !isFree(first, pages))
            first++;
        if (first == PAGE_COUNT)
            return NULL;
    }

    for (slot = 0; slot < sizeof(blocks) / sizeof(blocks[0]); slot++) {
        if (blocks[slot].pages == 0)
            break;
    }
    assert(slot < sizeof(blocks) / sizeof(blocks[0]));
    blocks[slot].first = first;
    blocks[slot].pages = pages;
    memset(&allocated[first], 1, pages);
    used_bytes += pages * PAGE_SIZE;

    /* 注入导出表变化，检查安装失败时是否释放已预留的内存。 */
    if (invalidate_export_on_alloc)
        sysmem_lib.exports[4] = NULL;
    return (void *)(uintptr_t)(first * PAGE_SIZE);
}

static int testFree(void *ptr)
{
    unsigned int slot;

    free_calls++;
    for (slot = 0; slot < sizeof(blocks) / sizeof(blocks[0]); slot++) {
        if (blocks[slot].pages != 0 &&
            ptr == (void *)(uintptr_t)(blocks[slot].first * PAGE_SIZE)) {
            memset(&allocated[blocks[slot].first], 0, blocks[slot].pages);
            used_bytes -= blocks[slot].pages * PAGE_SIZE;
            blocks[slot].pages = 0;
            return 0;
        }
    }
    return -1;
}

lc_internals_t *GetLoadcoreInternalData(void)
{
    return &loadcore;
}

int CpuSuspendIntr(int *state)
{
    *state = interrupts_enabled;
    interrupts_enabled = 0;
    suspend_calls++;
    if (invalidate_export_on_suspend) {
        invalidate_export_on_suspend = 0;
        sysmem_lib.exports[4] = NULL;
    }
    return 0;
}

int CpuResumeIntr(int state)
{
    interrupts_enabled = state;
    resume_calls++;

    /* 确定性地插入第二个申请，模拟移交预留块时的线程切换。 */
    if (state && interleave_on_resume) {
        fp_AllocSysMemory alloc = sysmem_lib.exports[4];
        interleave_on_resume = 0;
        interleaved_result = alloc(ALLOC_FIRST, UYA_BUF_SIZE, NULL);
    }
    return 0;
}

void FlushIcache(void)
{
    assert(!interrupts_enabled);
    icache_flushes++;
}

int Kprintf(const char *format, ...)
{
    assert(strstr(format, "cannot reserve") != NULL);
    diagnostic_calls++;
    return 0;
}

static void unusedExport(void) {}

static void resetIopFixture(void)
{
    unsigned int i;

    memset(allocated, 0, sizeof(allocated));
    memset(blocks, 0, sizeof(blocks));
    memset(&sysmem_lib, 0, sizeof(sysmem_lib));
    memset(callers, 0, sizeof(callers));
    memcpy(sysmem_lib.name, "sysmem", sizeof("sysmem"));
    for (i = 0; i < 4; i++)
        sysmem_lib.exports[i] = unusedExport;
    sysmem_lib.exports[4] = testAlloc;
    sysmem_lib.exports[5] = testFree;
    sysmem_lib.caller = &callers[0];
    callers[0].next = &callers[1];
    callers[0].stubs[0].jump = callers[0].stubs[1].jump = 0x08000000;
    callers[0].stubs[0].fno = 4;
    callers[0].stubs[1].fno = 5;
    callers[1].stubs[0].jump = 0x08000000;
    callers[1].stubs[0].fno = 4;
    loadcore.let_next = &sysmem_lib;

    used_bytes = 0;
    alloc_calls = free_calls = 0;
    suspend_calls = resume_calls = icache_flushes = diagnostic_calls = 0;
    last_mode = last_size = 0;
    last_ptr = NULL;
    interrupts_enabled = 1;
    interleave_on_resume = invalidate_export_on_alloc = invalidate_export_on_suspend = 0;
    interleaved_result = NULL;
    org_AllocSysMemory = NULL;
    uya_hole = NULL;
}

static void startPatch(void)
{
    resetIopFixture();
    assert(rcuyapatch_start(0, NULL) == MODULE_RESIDENT_END);
    assert(used_bytes == UYA_BUF_SIZE);
    assert(alloc_calls == 1 && free_calls == 0);
    assert(sysmem_lib.exports[4] == (void *)hooked_AllocSysMemory);
    assert(sysmem_lib.exports[5] == (void *)testFree);
    assert(icache_flushes == 1);
    assert(interrupts_enabled && suspend_calls == resume_calls);
}

static void testConfigPadding(void)
{
    struct legacy_prefix
    {
        u32 magic[2];
        char GameMode;
        char GameModeDesc[CORE_GAME_MODE_DESC_MAX_LEN];
        int EnableDebug;
    };

    assert(offsetof(struct EECoreConfig_t, EnableDebug) == offsetof(struct legacy_prefix, EnableDebug));
    assert(offsetof(struct EECoreConfig_t, ExitPath) == sizeof(struct legacy_prefix));
    assert(offsetof(struct EECoreConfig_t, HDDSpindown) == sizeof(struct legacy_prefix) + CORE_EXIT_PATH_MAX_LEN);
    assert(offsetof(struct EECoreConfig_t, EnableRnC3UyaPatch) == 25);
    assert(offsetof(struct EECoreConfig_t, RnC3UyaMultiplayer) == 26);
    assert(offsetof(struct EECoreConfig_t, EnableDebug) == 28);
    puts("PASS: UYA flags fit in existing config padding without shifting subsequent fields");
}

static void testElfScope(void)
{
    static const char *games[] = {"SCUS_973.53", "SCES_524.56", "SCPS_150.84"};
    static const char *paths[] = {
        "cdrom0:\\I5BOOTN.ELF;1", "cdrom0:/I5BOOTN.ELF;1",
        "cdrom0:I5BOOTN.ELF", "I5BOOTN.ELF", "cdrom0:\\net\\i5bootn.elf;1"};
    static const char *non_multiplayer[] = {
        "", "I", "I5BOOTN", "I5BOOTN.EL", "I5BOOTN.ELF.bak", "I5BOOTN.ELF;10",
        "I5BOOTN.ELF;", "I5BOOTN.ELF;2", "I5BOOTN.ELF;1\\other.elf", "rom0:PS2LOGO"};
    unsigned int g, p;

    assert(!g_ee_core_config.RnC3UyaMultiplayer);
    assert(!RnC3_IsMultiplayerElf(NULL));
    for (g = 0; g < sizeof(games) / sizeof(games[0]); g++) {
        selectGame(games[g]);
        assert(g_ee_core_config.EnableRnC3UyaPatch);
        setCurrentElf(games[g]);
        assert(!g_ee_core_config.RnC3UyaMultiplayer);
        for (p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
            setCurrentElf(paths[p]);
            assert(g_ee_core_config.RnC3UyaMultiplayer);
        }
        for (p = 0; p < sizeof(non_multiplayer) / sizeof(non_multiplayer[0]); p++) {
            setCurrentElf(non_multiplayer[p]);
            assert(!g_ee_core_config.RnC3UyaMultiplayer);
        }
    }
    selectGame("SCUS_974.65");
    setCurrentElf(paths[0]);
    assert(!g_ee_core_config.RnC3UyaMultiplayer);
    assert(!RnC3_IsGameID("SCUS_973.53.backup"));
    assert(!RnC3_IsGameID("SCUS_973.5"));
    selectGame(NULL);
    setCurrentElf(paths[0]);
    assert(!g_ee_core_config.RnC3UyaMultiplayer);
    selectGame(games[0]);
    setCurrentElf(NULL);
    assert(!g_ee_core_config.RnC3UyaMultiplayer);
    puts("PASS: only known UYA multiplayer ELFs enable the patch; cold single-player does not");
}

static void testModeTransitions(void)
{
    selectGame("SCUS_973.53");
    setCurrentElf("cdrom0:\\SCUS_973.53;1");
    resetIopFixture();
    assert(!g_ee_core_config.RnC3UyaMultiplayer && used_bytes == 0);

    setCurrentElf("cdrom0:\\I5BOOTN.ELF;1");
    startPatch();
    assert(g_ee_core_config.RnC3UyaMultiplayer);
    startPatch();
    assert(g_ee_core_config.RnC3UyaMultiplayer && used_bytes == UYA_BUF_SIZE);

    setCurrentElf("cdrom0:\\SCUS_973.53;1");
    resetIopFixture();
    assert(!g_ee_core_config.RnC3UyaMultiplayer && used_bytes == 0);
    puts("PASS: simulated IOP resets preserve multiplayer state; returning to single-player clears it");
}

static void testHandoffAndRelease(void)
{
    void *a, *b;
    fp_AllocSysMemory alloc;
    fp_FreeSysMemory release;

    startPatch();
    alloc = sysmem_lib.exports[4];
    release = sysmem_lib.exports[5];
    a = alloc(ALLOC_FIRST, UYA_BUF_SIZE, NULL);
    assert(a == UYA_BUF_ADDR && uya_hole == NULL && alloc_calls == 1);
    b = alloc(ALLOC_FIRST, UYA_BUF_SIZE, NULL);
    assert(b == NULL && alloc_calls == 2 && used_bytes == UYA_BUF_SIZE);
    assert(release(a) == 0 && free_calls == 1 && used_bytes == 0);
    b = alloc(ALLOC_FIRST, UYA_BUF_SIZE, NULL);
    assert(b == UYA_BUF_ADDR && last_mode == ALLOC_ADDRESS);
    assert(release(b) == 0 && used_bytes == 0);
    assert(release(b) == -1);
    puts("PASS: one-time handoff, no overlapping alias, real free, and valid reallocation");
}

static void testPassthrough(void)
{
    void *p;
    fp_AllocSysMemory alloc;
    fp_FreeSysMemory release;

    startPatch();
    alloc = sysmem_lib.exports[4];
    release = sysmem_lib.exports[5];
    p = alloc(ALLOC_LAST, 4096, NULL);
    assert(p != NULL && last_mode == ALLOC_LAST && last_size == 4096 && last_ptr == NULL);
    assert(release(p) == 0);
    p = alloc(ALLOC_ADDRESS, UYA_BUF_SIZE, (void *)(uintptr_t)0x120000);
    assert(p == (void *)(uintptr_t)0x120000 && last_ptr == p);
    assert(release(p) == 0 && uya_hole == UYA_BUF_ADDR);
    assert(alloc(-1, UYA_BUF_SIZE, NULL) == NULL && last_mode == -1);
    assert(alloc(3, UYA_BUF_SIZE, NULL) == NULL && last_mode == 3);
    assert(alloc(ALLOC_FIRST, -1, NULL) == NULL);
    assert(alloc(ALLOC_FIRST, 0, NULL) == NULL);
    p = alloc(ALLOC_ADDRESS, UYA_BUF_SIZE, UYA_BUF_ADDR);
    assert(p == UYA_BUF_ADDR && uya_hole == NULL);
    assert(release(p) == 0 && used_bytes == 0);

    startPatch();
    alloc = sysmem_lib.exports[4];
    p = alloc(ALLOC_LAST, UYA_BUF_SIZE, NULL);
    assert(p == UYA_BUF_ADDR && uya_hole == NULL);
    assert(testFree(p) == 0 && used_bytes == 0);
    puts("PASS: unrelated allocations, explicit addresses, and invalid requests preserve allocator semantics");
}

static void testInterleaving(void)
{
    void *p;
    fp_AllocSysMemory alloc;

    startPatch();
    alloc = sysmem_lib.exports[4];
    interleave_on_resume = 1;
    p = alloc(ALLOC_FIRST, UYA_BUF_SIZE, NULL);
    assert(p == UYA_BUF_ADDR && interleaved_result == NULL);
    assert(alloc_calls == 2 && used_bytes == UYA_BUF_SIZE);
    assert(interrupts_enabled && suspend_calls == resume_calls);
    assert(testFree(p) == 0 && used_bytes == 0);
    puts("PASS: a second request inserted at handoff cannot acquire the same live buffer");
}

static void testFailureCleanup(void)
{
    void *occupant;

    resetIopFixture();
    occupant = testAlloc(ALLOC_ADDRESS, UYA_BUF_SIZE, UYA_BUF_ADDR);
    assert(occupant == UYA_BUF_ADDR);
    assert(rcuyapatch_start(0, NULL) == MODULE_NO_RESIDENT_END);
    assert(sysmem_lib.exports[4] == (void *)testAlloc && sysmem_lib.exports[5] == (void *)testFree);
    assert(used_bytes == UYA_BUF_SIZE && diagnostic_calls == 1 && icache_flushes == 0);
    assert(testFree(occupant) == 0);

    resetIopFixture();
    loadcore.let_next = NULL;
    assert(rcuyapatch_start(0, NULL) == MODULE_NO_RESIDENT_END && alloc_calls == 0);

    resetIopFixture();
    sysmem_lib.exports[5] = NULL;
    assert(rcuyapatch_start(0, NULL) == MODULE_NO_RESIDENT_END && alloc_calls == 0);

    resetIopFixture();
    invalidate_export_on_alloc = 1;
    assert(rcuyapatch_start(0, NULL) == MODULE_NO_RESIDENT_END);
    assert(used_bytes == 0 && free_calls == 1 && uya_hole == NULL);
    assert(icache_flushes == 0 && interrupts_enabled);

    resetIopFixture();
    invalidate_export_on_suspend = 1;
    assert(rcuyapatch_start(0, NULL) == MODULE_NO_RESIDENT_END);
    assert(sysmem_lib.exports[4] == NULL && used_bytes == 0 && uya_hole == NULL);
    assert(free_calls == 1 && icache_flushes == 0 && interrupts_enabled);
    puts("PASS: failed reservation or hook installation leaves no active patch or leaked reservation");
}

static u32 expectedJump(void *function)
{
    return 0x08000000 | ((((u32)(uintptr_t)function) << 4) >> 6);
}

static void testLibraryHooks(void)
{
    resetIopFixture();
    assert(ioplib_getByName("sysmem") == &sysmem_lib);
    assert(ioplib_getByName("sys") == NULL);
    assert(ioplib_getByName("") == NULL);
    assert(ioplib_getTableSize(NULL) == 0);
    assert(ioplib_hookExportEntry(&sysmem_lib, 6, unusedExport) == NULL);

    memcpy(sysmem_lib.name, "12345678", 8);
    assert(ioplib_getByName("12345678") == &sysmem_lib);
    assert(ioplib_getByName("1234567") == NULL);
    startPatch();
    assert(callers[0].stubs[0].jump == expectedJump(sysmem_lib.exports[4]));
    assert(callers[1].stubs[0].jump == expectedJump(sysmem_lib.exports[4]));
    assert(callers[0].stubs[1].jump == expectedJump(sysmem_lib.exports[5]));
    assert(callers[0].stubs[2].jump == 0 && callers[1].stubs[1].jump == 0);
    puts("PASS: bounded library-name lookup and import relinking with I-cache flush");
}

int main(void)
{
    testConfigPadding();
    testElfScope();
    testModeTransitions();
    testHandoffAndRelease();
    testPassthrough();
    testInterleaving();
    testFailureCleanup();
    testLibraryHooks();
    puts("All host regression tests passed (not a PS2 gameplay test).");
    return 0;
}
