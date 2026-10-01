#include "ioplib.h"

#include <intrman.h>
#include <tamtypes.h>

iop_library_t *ioplib_getByName(const char *name)
{
    iop_library_t *libptr;
    int i;

    libptr = GetLoadcoreInternalData()->let_next;
    while (libptr != NULL) {
        for (i = 0; i < 8; i++) {
            if (libptr->name[i] != name[i])
                break;
            /* 短名称的 NUL 已完成匹配，不能再读字符串末尾之外的填充字节。 */
            if (name[i] == '\0')
                return libptr;
        }

        if (i == 8)
            return libptr;

        libptr = libptr->prev;
    }

    return NULL;
}

unsigned int ioplib_getTableSize(iop_library_t *lib)
{
    void **exp;
    unsigned int size;

    exp = NULL;
    if (lib != NULL)
        exp = lib->exports;

    size = 0;
    if (exp != NULL)
        while (*exp++ != NULL)
            size++;

    return size;
}

void *ioplib_hookExportEntry(iop_library_t *lib, unsigned int entry, void *func)
{
    if (entry < ioplib_getTableSize(lib)) {
        int oldstate;
        void **exp, *temp;

        exp = &lib->exports[entry];

        CpuSuspendIntr(&oldstate);
        temp = *exp;
        /* 空导出不能安装钩子，避免调用方失败卸载后留下悬空函数指针。 */
        if (temp != NULL)
            *exp = func;
        func = temp;
        CpuResumeIntr(oldstate);

        return func;
    }

    return NULL;
}

void ioplib_relinkExports(iop_library_t *lib)
{
    struct irx_import_table *table;
    struct irx_import_stub *stub;
    int oldstate;

    /* 导入桩属于指令，重连后必须刷新 I-cache，且不能让线程看到半更新的状态。 */
    CpuSuspendIntr(&oldstate);
    for (table = lib->caller; table != NULL; table = table->next) {
        for (stub = (struct irx_import_stub *)table->stubs; stub->jump != 0; stub++)
            stub->jump = 0x08000000 | (((u32)lib->exports[stub->fno] << 4) >> 6);
    }
    FlushIcache();
    CpuResumeIntr(oldstate);
}
