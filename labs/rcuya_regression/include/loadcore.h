#ifndef RCUYA_TEST_LOADCORE_H_
#define RCUYA_TEST_LOADCORE_H_

#include <stddef.h>
#include <tamtypes.h>

struct irx_import_stub
{
    u32 jump;
    unsigned int fno;
};

struct irx_import_table
{
    struct irx_import_table *next;
    struct irx_import_stub stubs[3];
};

typedef struct _iop_library
{
    struct _iop_library *prev;
    struct irx_import_table *caller;
    char name[8];
    void *exports[16];
} iop_library_t;

typedef struct
{
    iop_library_t *let_next;
} lc_internals_t;

#define IRX_ID(name, major, minor)
#define MODULE_RESIDENT_END    0
#define MODULE_NO_RESIDENT_END 1

lc_internals_t *GetLoadcoreInternalData(void);
void FlushIcache(void);

#endif
