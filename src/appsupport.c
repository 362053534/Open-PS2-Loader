#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/appsupport.h"
#include "include/themes.h"
#include "include/system.h"
#include "include/ioman.h"
#include "include/util.h"
#include "include/textures.h"
#include "include/extern_irx.h"

#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/hddsupport.h"
#include "include/supportbase.h"

#include <elf-loader.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>
#include <io_common.h>

static int appForceUpdate = 1;
static int appItemCount = 0;
static int appAutoListInitialized;
static int appUpdateAllSources;
static u32 appPendingSourceMask;
static u32 appUpdateSourceMask;
static volatile int appInitialScanState;
static int appPOPSPrepareStatus;
static int appPOPSPrepareResult;
static int appPOPSPrepareID;

static config_set_t *configApps;
static app_info_t *appsList;

#define APP_POPS_PREPARE_DRIVERS_FAILED 0x01
#define APP_POPS_PREPARE_EE_READY       0x02

struct app_info_linked
{
    struct app_info_linked *next;
    app_info_t app;
};

typedef struct
{
    struct app_info_linked **appsLinkedList;
    int sourceMode;
} app_scan_context_t;

#define POPS_CACHE_NAME_MAX (APP_BOOT_MAX + 1)
#define POPS_CACHE_ID_MAX   12
#define POPS_CACHE_PATH_MAX 256

typedef struct
{
    char vcdName[POPS_CACHE_NAME_MAX];
    char parsedId[POPS_CACHE_ID_MAX];
} pops_id_cache_entry_t;

typedef struct
{
    pops_id_cache_entry_t *entries;
    int count;
    int capacity;
} pops_id_cache_t;

static pops_id_cache_t popsCacheLoaded;
static pops_id_cache_t popsCacheCurrent;
static char popsCachePrefix[POPS_CACHE_PATH_MAX];
static int popsCacheActive;

#define APP_SOURCE_BIT(mode)       (1U << (mode))
#define APP_AUTO_SOURCE_MASK       ((1U << (APP_SOURCE_MC + 1)) - 1)

enum {
    APP_INITIAL_SCAN_COMPLETE,
    APP_INITIAL_SCAN_PENDING,
    APP_INITIAL_SCAN_QUEUED
};

// forward declaration
static item_list_t appItemList;

static void appFreeList(void);
static void appFreeLegacyConfig(void);

#define POPS_BDM_ELF_PREFIX "XX."
#define POPS_SMB_ELF_PREFIX "SB."

#define POPS_BOOT_MAILBOX_MAGIC    0x53504F50
#define POPS_BOOT_MAILBOX_VERSION  1
#define POPS_BOOT_MAILBOX_PATH_MAX 256
#define POPS_BOOT_VCD_PREFIX       "0:/POPS/"

#define POPS_EE_RESIDENT_SIZE             0x1000
#define POPS_EE_TRAMPOLINE_OFFSET         0x0200
#define POPS_EE_HDD_PATCH_HELPER_OFFSET   0x0400
#define POPS_EE_HDD_PATH_HELPER_OFFSET    0x0600
#define POPS_EE_HDD_NATIVE_HELPER_OFFSET  0x0680
#define POPS_EE_HDD_MOUNT_HELPER_OFFSET   0x0780
#define POPS_EE_HDD_PFS_MOUNT_HELPER_OFFSET 0x07C0
#define POPS_EE_HDD_PARTITION_OFFSET      0x0800
#define POPS_EE_HDD_PARTITION_CAPACITY    0x0040
#define POPS_EE_HDD_RESOURCE_FORMAT_OFFSET 0x0840
#define POPS_EE_HDD_RESOURCE_FORMAT_CAPACITY 0x0040
#define POPS_EE_HDD_VCD_PREFIX_OFFSET     0x0880
#define POPS_EE_HDD_VCD_PREFIX_CAPACITY   0x0040
#define POPS_EE_HDD_PFS_FORMAT_OFFSET     0x08C0
#define POPS_EE_HDD_PFS_FORMAT_CAPACITY   0x0040
#define POPS_EE_HDD_VMC_PATH_HELPER_OFFSET 0x0900
#define POPS_EE_COPY_TABLE_OFFSET         ELF_LOADER_RESIDENT_COPY_TABLE_OFFSET
#define POPS_EE_USBD_ADDRESS              0x00140000
#define POPS_EE_DRIVER_ADDRESS            0x00180000
#define POPS_EE_IRX_SIZE_OFFSET           0x007C
#define POPS_EE_SMB_PACKAGE_ADDRESS       0x00140000
#define POPS_EE_SMB_PACKAGE_CAPACITY      0x00040000
#define POPS_EE_SMB_PACKAGE_MAGIC         0x534D4256
#define POPS_EE_SMB_PACKAGE_VERSION       1
#define POPS_EE_SMB_FILE_COUNT            8
#define POPS_EE_SMB_CODE_OFFSET           0x0100

static u8 appPOPSEEResident[POPS_EE_RESIDENT_SIZE] __attribute__((aligned(64)));

extern const u8 appPOPSSMBVFSStart[];
extern const u8 appPOPSSMBVFSOpen[];
extern const u8 appPOPSSMBVFSClose[];
extern const u8 appPOPSSMBVFSRead[];
extern const u8 appPOPSSMBVFSLseek[];
extern const u8 appPOPSSMBVFSEnd[];

/* 该跳板在POPStarter完成解压后修正其固定地址IRX分支中的目标指针。 */
static const u32 appPOPSEETrampoline[] = {
    0x3C080087,
    0x3C0B0088,
    0x3C093C04,
    0x35290014,
    0xAD091D50,
    0xAD0920B8,
    0xAD69AC90,
    0xAD69AD04,
    0x3C093C04,
    0x35290018,
    0xAD091F04,
    0xAD092154,
    0xAD69AD3C,
    0xAD69ADB0,
    0x0821C000,
    0x00000000,
};

/* 主跳板必须停在复制表之前，较大的目录式APA修补逻辑放到复制表之后。 */
static const u32 appPOPSHDDOPLTrampoline[] = {
    0x0C025100, // jal 0x00094400
    0x00000000,
    0x0000000F, // sync
    0x3C080087, // lui t0,0x0087
    0x01000008, // jr t0
    0x00000000,
};

/* 目录式APA链路复用pfs1，并绕过只适用于独立__.POPS分区的VCD枚举流程。 */
static const u32 appPOPSHDDOPLPatchHelper[] = {
    /* 原始__common缓冲区过短，改由常驻包装函数直接提供实际APA设备名。 */
    0x3C08008E, // lui t0,0x008E
    0x3C090C02, // lui t1,0x0C02
    0x352951F0, // ori t1,t1,0x51F0，jal 0x000947C0
    0xAD098B6C, // sw t1,-0x7494(t0)，修补0x008D8B6C
    /* 环境页面和后续资源路径必须与实际挂载点保持一致。 */
    0x3C08009B, // lui t0,0x009B
    0x25080AE0, // addiu t0,t0,0x0AE0
    0x3C090009, // lui t1,0x0009
    0x252948C0, // addiu t1,t1,0x48C0
    0x912A0000, // lbu t2,0(t1)
    0xA10A0000, // sb t2,0(t0)
    0x25080001, // addiu t0,t0,1
    0x25290001, // addiu t1,t1,1
    0x1540FFFB, // bnez t2,复制PFS资源格式串
    0x00000000,
    /* 两个资源格式串都使用同一个实际APA分区名。 */
    0x3C08009B, // lui t0,0x009B
    0x25080B0C, // addiu t0,t0,0x0B0C
    0x3C090009, // lui t1,0x0009
    0x25294840, // addiu t1,t1,0x4840
    0x912A0000, // lbu t2,0(t1)
    0xA10A0000, // sb t2,0(t0)
    0x25080001, // addiu t0,t0,1
    0x25290001, // addiu t1,t1,1
    0x1540FFFB, // bnez t2,复制资源格式串
    0x00000000,
    0x3C08009B, // lui t0,0x009B
    0x25080B3C, // addiu t0,t0,0x0B3C
    0x3C090009, // lui t1,0x0009
    0x25294840, // addiu t1,t1,0x4840
    0x912A0000, // lbu t2,0(t1)
    0xA10A0000, // sb t2,0(t0)
    0x25080001, // addiu t0,t0,1
    0x25290001, // addiu t1,t1,1
    0x1540FFFB, // bnez t2,复制资源格式串
    0x00000000,
    0x3C080087, // lui t0,0x0087
    /* 用常驻辅助函数构造当前目录对应的pfs1 VCD路径。 */
    0x3C090C02, // lui t1,0x0C02
    0x35295180, // ori t1,t1,0x5180，jal 0x00094600
    0xAD09596C, // sw t1,0x596C(t0)
    0xAD005970, // sw zero,0x5970(t0)
    0x3C091000, // lui t1,0x1000
    0x35290008, // ori t1,t1,8
    0xAD095974, // sw t1,0x5974(t0)，跳过原有pfs0:前缀构造
    0xAD005978, // sw zero,0x5978(t0)
    0x3C092784, // lui t1,0x2784
    0x3529347B, // ori t1,t1,0x347B
    0xAD0959D0, // sw t1,0x59D0(t0)，把游戏名追加到/POPS/之后
    0xAD095A24, // sw t1,0x5A24(t0)
    /* pfs1已经就绪，不能再枚举和挂载__.POPS、__.POPS0至__.POPS9。 */
    0x3C091000, // lui t1,0x1000
    0x3529013C, // ori t1,t1,0x013C
    0xAD095AB0, // sw t1,0x5AB0(t0)，跳过独立分区枚举并进入后续加载流程
    0xAD005AB4, // sw zero,0x5AB4(t0)
    /* 目录式APA使用目录内VCD，不应套用独立APA分区的PP./__.名称规则。 */
    0x3C080088, // lui t0,0x0088，0x8D5C会按有符号偏移解释
    0x3C091000, // lui t1,0x1000
    0x3529004F, // ori t1,t1,0x004F
    0xAD098D5C, // sw t1,0x8D5C(t0)，直接进入通过分支
    0xAD008D60, // sw zero,0x8D60(t0)
    /* POPStarter完成原生POPS修补后，再补入HDD模式和实际分区名。 */
    0x3C080087, // lui t0,0x0087
    0x3C090C02, // lui t1,0x0C02
    0x352951A0, // ori t1,t1,0x51A0，jal 0x00094680
    0xAD0944B0, // sw t1,0x44B0(t0)
    0x0000000F, // sync
    0x03E00008, // jr ra
    0x00000000,
};

/* 固定放在0x00094600，避免修改修补辅助函数时改变被调用地址。 */
static const u32 appPOPSHDDOPLPathHelper[] = {
    0x3C08009B, // lui t0,0x009B
    0x25083470, // addiu t0,t0,0x3470
    0x3C090009, // lui t1,0x0009
    0x25294880, // addiu t1,t1,0x4880
    0x912A0000, // lbu t2,0(t1)
    0xA10A0000, // sb t2,0(t0)
    0x25080001, // addiu t0,t0,1
    0x25290001, // addiu t1,t1,1
    0x1540FFFB, // bnez t2,复制VCD路径前缀
    0x00000000,
    0x03E00008, // jr ra
    0x00000000,
};

/* 原生POPS会在入口重新解析argv，因此必须修改它最终采用的HDD分支和挂载调用。 */
static const u32 appPOPSHDDOPLNativeHelper[] = {
    0x27BDFFF0, // addiu sp,sp,-16
    0xAFBF0000, // sw ra,0(sp)
    0x0C236DF9, // jal 0x008DB7E4
    0x00000000,
    /* POPStarter已经完成VMC路径构造，此时只把运行期挂载点切到pfs0。 */
    0x0C025240, // jal 0x00094900
    0x00000000,
    /* main_Initialize前部也会读取同一字段，必须选择最后一个HDD分支判断。 */
    0x3C080020, // lui t0,0x0020
    0x250804F0, // addiu t0,t0,0x04F0
    0x3C090020, // lui t1,0x0020
    0x25290940, // addiu t1,t1,0x0940
    0x3C0A8262, // lui t2,0x8262
    0x354A30C8, // ori t2,t2,0x30C8，lb v0,0x30C8(s3)
    0x240E0000, // addiu t6,zero,0
    0x8D0B0000, // lw t3,0(t0)
    0x156A0007, // bne t3,t2,继续扫描
    0x00000000,
    0x8D0C0004, // lw t4,4(t0)
    0x000C6402, // srl t4,t4,16
    0x240D1040, // addiu t5,zero,0x1040，beqz v0
    0x158D0002, // bne t4,t5,继续扫描
    0x00000000,
    0x250E0000, // addiu t6,t0,0，保留最后一个匹配位置
    0x25080004, // addiu t0,t0,4
    0x0109582B, // sltu t3,t0,t1
    0x1560FFF4, // bnez t3,继续扫描
    0x00000000,
    0x11C00017, // beqz t6,版本不匹配等待
    0x00000000,
    0xADC00004, // sw zero,4(t6)，禁止跳入非HDD分支
    /* 将SPU_MountHDD调用改到常驻包装函数，由它传入真实分区名。 */
    0x3C080020, // lui t0,0x0020
    0x250804F0, // addiu t0,t0,0x04F0
    0x3C0A0C08, // lui t2,0x0C08
    0x354A524E, // ori t2,t2,0x524E，jal 0x00214938
    0x8D0B0000, // lw t3,0(t0)
    0x116A0007, // beq t3,t2,找到挂载调用
    0x00000000,
    0x25080004, // addiu t0,t0,4
    0x0109582B, // sltu t3,t0,t1
    0x1560FFFA, // bnez t3,继续扫描
    0x00000000,
    0x10000009, // b 版本不匹配等待
    0x00000000,
    0x3C0B0C02, // lui t3,0x0C02
    0x356B51E0, // ori t3,t3,0x51E0，jal 0x00094780
    0xAD0B0000, // sw t3,0(t0)
    0x0000000F, // sync
    0x8FBF0000, // lw ra,0(sp)
    0x27BD0010, // addiu sp,sp,16
    0x03E00008, // jr ra
    0x00000000,
    0x1000FFFF, // b 版本不匹配等待
    0x00000000,
};

/* 原生POPS进入HDD分支后只从这里取得APA设备名，避免其argv解析覆盖来源。 */
static const u32 appPOPSHDDOPLMountHelper[] = {
    0x27BDFFF0, // addiu sp,sp,-16
    0xAFBF0000, // sw ra,0(sp)
    0x3C040009, // lui a0,0x0009
    0x24844800, // addiu a0,a0,0x4800
    0x0C08524E, // jal 0x00214938
    0x00000000,
    0x8FBF0000, // lw ra,0(sp)
    0x27BD0010, // addiu sp,sp,16
    0x03E00008, // jr ra
    0x00000000,
};

/* 只修正POPStarter生成的两个VMC路径，避免影响启动阶段仍需使用的pfs1。 */
static const u32 appPOPSHDDOPLVMCPathHelper[] = {
    0x3C080050, // lui t0,0x0050
    0x25088080, // addiu t0,t0,0x8080，扫描起点0x004F8080
    0x3C090050, // lui t1,0x0050
    0x2529817D, // addiu t1,t1,0x817D，最后一个完整前缀之后
    0x240A0070, // addiu t2,zero,'p'
    0x240C0066, // addiu t4,zero,'f'
    0x240D0073, // addiu t5,zero,'s'
    0x240E0031, // addiu t6,zero,'1'
    0x240F0030, // addiu t7,zero,'0'
    0x910B0000, // lbu t3,0(t0)
    0x156A000B, // bne t3,t2,检查下一字节
    0x00000000,
    0x910B0001, // lbu t3,1(t0)
    0x156C0008, // bne t3,t4,检查下一字节
    0x00000000,
    0x910B0002, // lbu t3,2(t0)
    0x156D0005, // bne t3,t5,检查下一字节
    0x00000000,
    0x910B0003, // lbu t3,3(t0)
    0x156E0002, // bne t3,t6,检查下一字节
    0x00000000,
    0xA10F0003, // sb t7,3(t0)，pfs1改为pfs0
    0x25080001, // addiu t0,t0,1
    0x0109582B, // sltu t3,t0,t1
    0x1560FFF0, // bnez t3,继续扫描
    0x00000000,
    0x03E00008, // jr ra
    0x00000000,
};

/* POPStarter挂载pfs1时直接使用EE常驻区中的完整APA设备名。 */
static const u32 appPOPSHDDOPLPFSMountHelper[] = {
    0x27BDFFF0, // addiu sp,sp,-16
    0xAFBF0000, // sw ra,0(sp)
    0x3C050009, // lui a1,0x0009
    0x24A54800, // addiu a1,a1,0x4800
    0x0C245794, // jal 0x00915E50
    0x00000000,
    0x8FBF0000, // lw ra,0(sp)
    0x27BD0010, // addiu sp,sp,16
    0x03E00008, // jr ra
    0x00000000,
};

typedef struct
{
    u32 magic;
    u32 version;
    s32 deviceType;
    char vcdPath[POPS_BOOT_MAILBOX_PATH_MAX];
    u32 checksum;
} pops_boot_mailbox_t;

typedef struct
{
    u32 nameOffset;
    u32 dataOffset;
    u32 size;
    u32 cursor;
} pops_smb_vfs_entry_t;

typedef struct
{
    u32 magic;
    u32 version;
    u32 fileCount;
    u32 totalSize;
    pops_smb_vfs_entry_t entries[POPS_EE_SMB_FILE_COUNT];
} pops_smb_vfs_header_t;

typedef char pops_boot_mailbox_size_must_be_272[(sizeof(pops_boot_mailbox_t) == 272) ? 1 : -1];
typedef char pops_ee_mailbox_must_fit[(sizeof(pops_boot_mailbox_t) <= POPS_EE_TRAMPOLINE_OFFSET) ? 1 : -1];
typedef char pops_ee_trampoline_must_fit[(POPS_EE_TRAMPOLINE_OFFSET + sizeof(appPOPSEETrampoline) <= POPS_EE_COPY_TABLE_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_opl_trampoline_must_fit[(POPS_EE_TRAMPOLINE_OFFSET + sizeof(appPOPSHDDOPLTrampoline) <= POPS_EE_COPY_TABLE_OFFSET) ? 1 : -1];
typedef char pops_ee_copy_table_must_fit[(POPS_EE_COPY_TABLE_OFFSET + sizeof(elf_loader_resident_copy_table_t) <= POPS_EE_RESIDENT_SIZE) ? 1 : -1];
typedef char pops_ee_hdd_patch_helper_after_copy_table[(POPS_EE_COPY_TABLE_OFFSET + sizeof(elf_loader_resident_copy_table_t) <= POPS_EE_HDD_PATCH_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_patch_helper_must_fit[(POPS_EE_HDD_PATCH_HELPER_OFFSET + sizeof(appPOPSHDDOPLPatchHelper) <= POPS_EE_HDD_PATH_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_path_helper_must_fit[(POPS_EE_HDD_PATH_HELPER_OFFSET + sizeof(appPOPSHDDOPLPathHelper) <= POPS_EE_HDD_NATIVE_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_native_helper_must_fit[(POPS_EE_HDD_NATIVE_HELPER_OFFSET + sizeof(appPOPSHDDOPLNativeHelper) <= POPS_EE_HDD_MOUNT_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_mount_helper_must_fit[(POPS_EE_HDD_MOUNT_HELPER_OFFSET + sizeof(appPOPSHDDOPLMountHelper) <= POPS_EE_HDD_PFS_MOUNT_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_pfs_mount_helper_must_fit[(POPS_EE_HDD_PFS_MOUNT_HELPER_OFFSET + sizeof(appPOPSHDDOPLPFSMountHelper) <= POPS_EE_HDD_PARTITION_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_partition_must_fit[(POPS_EE_HDD_PARTITION_OFFSET + POPS_EE_HDD_PARTITION_CAPACITY <= POPS_EE_HDD_RESOURCE_FORMAT_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_resource_format_must_fit[(POPS_EE_HDD_RESOURCE_FORMAT_OFFSET + POPS_EE_HDD_RESOURCE_FORMAT_CAPACITY <= POPS_EE_HDD_VCD_PREFIX_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_vcd_prefix_must_fit[(POPS_EE_HDD_VCD_PREFIX_OFFSET + POPS_EE_HDD_VCD_PREFIX_CAPACITY <= POPS_EE_HDD_PFS_FORMAT_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_pfs_format_must_fit[(POPS_EE_HDD_PFS_FORMAT_OFFSET + POPS_EE_HDD_PFS_FORMAT_CAPACITY <= POPS_EE_HDD_VMC_PATH_HELPER_OFFSET) ? 1 : -1];
typedef char pops_ee_hdd_vmc_path_helper_must_fit[(POPS_EE_HDD_VMC_PATH_HELPER_OFFSET + sizeof(appPOPSHDDOPLVMCPathHelper) <= POPS_EE_RESIDENT_SIZE) ? 1 : -1];
typedef char pops_smb_vfs_header_must_fit[(sizeof(pops_smb_vfs_header_t) <= POPS_EE_SMB_CODE_OFFSET) ? 1 : -1];

static int appIsPOPSLauncher(const app_info_t *app)
{
    return app->popstarter || strstr(app->path, "APPS") == NULL;
}

static int appResolveMode(const app_info_t *app)
{
    /* pfs0和pfs1只是可变挂载点，HDD POPS必须使用扫描时保存的稳定来源。 */
    if (app->popstarter &&
        (app->popsHddSource == OPL_HDD_POPS_SOURCE_LEGACY ||
         app->popsHddSource == OPL_HDD_POPS_SOURCE_OPL))
        return HDD_MODE;

    return oplPath2Mode(app->path);
}

static int appGetPOPSBDMDeviceType(const app_info_t *app)
{
    int mode = appResolveMode(app);

    if (mode >= BDM_MODE && mode <= BDM_MODE4)
        return bdmGetDeviceType(mode);

    return BDM_TYPE_UNKNOWN;
}

static u32 appPOPSBootMailboxChecksum(const pops_boot_mailbox_t *mailbox)
{
    const u8 *data = (const u8 *)mailbox;
    u32 checksum = 2166136261u;
    unsigned int i;

    for (i = 0; i < sizeof(*mailbox) - sizeof(mailbox->checksum); i++) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }

    return checksum;
}

static u32 appPOPSChecksum(const void *buffer, unsigned int size)
{
    const u8 *data = (const u8 *)buffer;
    u32 checksum = 2166136261u;
    unsigned int i;

    for (i = 0; i < size; i++) {
        checksum ^= data[i];
        checksum *= 16777619u;
    }

    return checksum;
}

static void appPOPSWriteU32(u8 *buffer, unsigned int offset, u32 value)
{
    memcpy(buffer + offset, &value, sizeof(value));
}

static unsigned int appPOPSAlign16(unsigned int value)
{
    return (value + 15) & ~15;
}

static u32 appPOPSMIPSJump(unsigned int address)
{
    return 0x08000000 | ((address >> 2) & 0x03FFFFFF);
}

static int appPOPSBuildSMBTrampoline(void)
{
    static const unsigned int patchOffsets[] = {0x0000, 0x0060, 0x0090, 0x00D0};
    const unsigned int outputBranchAddress = 0x0087162C;
    const unsigned int outputStoreAddress = 0x00871634;
    const u32 outputStoreInstruction = 0xAF8238B0;
    const u8 *handlers[] = {
        appPOPSSMBVFSOpen,
        appPOPSSMBVFSClose,
        appPOPSSMBVFSRead,
        appPOPSSMBVFSLseek};
    const unsigned int codeSize = appPOPSSMBVFSEnd - appPOPSSMBVFSStart;
    u32 *code = (u32 *)(appPOPSEEResident + POPS_EE_TRAMPOLINE_OFFSET);
    unsigned int target, jump;
    int i, count;

    count = 0;
    code[count++] = 0x3C080088; // lui t0,0x0088
    code[count++] = 0x2508C390; // addiu t0,t0,-15472

    for (i = 0; i < 4; i++) {
        unsigned int handlerOffset = handlers[i] - appPOPSSMBVFSStart;

        if (handlerOffset >= codeSize)
            return -1;

        target = POPS_EE_SMB_PACKAGE_ADDRESS + POPS_EE_SMB_CODE_OFFSET + handlerOffset;
        jump = appPOPSMIPSJump(target);
        code[count++] = 0x3C090000 | (jump >> 16);                         // lui t1,高16位
        code[count++] = 0x35290000 | (jump & 0xFFFF);                     // ori t1,t1,低16位
        code[count++] = 0xAD090000 | (patchOffsets[i] & 0xFFFF);           // sw t1,偏移(t0)
        code[count++] = 0xAD000000 | ((patchOffsets[i] + 4) & 0xFFFF);     // sw zero,偏移+4(t0)
    }

    /* POPStarter识别SMB参数后会重新开启输出，这里强制改为关闭普通输出。 */
    code[count++] = 0x3C080000 | (outputBranchAddress >> 16);             // 加载补丁地址高位
    code[count++] = 0x3C090000 | (outputStoreInstruction >> 16);          // 加载替换指令高位
    code[count++] = 0x35290000 | (outputStoreInstruction & 0xFFFF);       // 加载替换指令低位
    code[count++] = 0xAD000000 | (outputBranchAddress & 0xFFFF);          // 取消跳过输出设置的分支
    code[count++] = 0xAD090000 | (outputStoreAddress & 0xFFFF);           // 将普通输出标志设为关闭
    code[count++] = 0x0000000F; // sync
    code[count++] = 0x3C080087; // lui t0,0x0087
    code[count++] = 0x01000008; // jr t0
    code[count++] = 0x00000000;

    return count * sizeof(*code) <= POPS_EE_COPY_TABLE_OFFSET - POPS_EE_TRAMPOLINE_OFFSET ? 0 : -1;
}

static void *appPOPSBuildSMBPackage(unsigned int *packageSize)
{
    static const char *names[POPS_EE_SMB_FILE_COUNT] = {
        "IPCONFIG.DAT",
        "SMBCONFIG.DAT",
        "poweroff.irx",
        "ps2dev9.irx",
        "smsutils.irx",
        "ps2ip.irx",
        "ps2smap.irx",
        "smbman.irx"};
    char ipconfig[48], smbconfig[144];
    const void *buffers[POPS_EE_SMB_FILE_COUNT];
    int sizes[POPS_EE_SMB_FILE_COUNT];
    pops_smb_vfs_header_t *header;
    unsigned int codeSize, offset, dataOffset, totalSize;
    u8 *package;
    int ipconfigSize, smbconfigSize;
    int i;

    if (buildPopstarterSMBConfigs(ipconfig, sizeof(ipconfig), &ipconfigSize,
                                  smbconfig, sizeof(smbconfig), &smbconfigSize) < 0)
        return NULL;

    buffers[0] = ipconfig;
    buffers[1] = smbconfig;
    buffers[2] = popstarter_smb_poweroff_irx;
    buffers[3] = popstarter_smb_ps2dev9_irx;
    buffers[4] = popstarter_smb_smsutils_irx;
    buffers[5] = popstarter_smb_ps2ip_irx;
    buffers[6] = popstarter_smb_ps2smap_irx;
    buffers[7] = popstarter_smb_smbman_irx;
    sizes[0] = ipconfigSize;
    sizes[1] = smbconfigSize;
    sizes[2] = size_popstarter_smb_poweroff_irx;
    sizes[3] = size_popstarter_smb_ps2dev9_irx;
    sizes[4] = size_popstarter_smb_smsutils_irx;
    sizes[5] = size_popstarter_smb_ps2ip_irx;
    sizes[6] = size_popstarter_smb_ps2smap_irx;
    sizes[7] = size_popstarter_smb_smbman_irx;

    codeSize = appPOPSSMBVFSEnd - appPOPSSMBVFSStart;
    if (codeSize == 0 || POPS_EE_SMB_CODE_OFFSET + codeSize > POPS_EE_SMB_PACKAGE_CAPACITY)
        return NULL;

    offset = appPOPSAlign16(POPS_EE_SMB_CODE_OFFSET + codeSize);
    for (i = 0; i < POPS_EE_SMB_FILE_COUNT; i++)
        offset += strlen(names[i]) + 1;

    dataOffset = appPOPSAlign16(offset);
    totalSize = dataOffset;
    for (i = 0; i < POPS_EE_SMB_FILE_COUNT; i++) {
        totalSize = appPOPSAlign16(totalSize);
        totalSize += sizes[i];
    }
    totalSize = appPOPSAlign16(totalSize);
    if (totalSize > POPS_EE_SMB_PACKAGE_CAPACITY)
        return NULL;

    package = memalign(64, totalSize);
    if (!package)
        return NULL;
    memset(package, 0, totalSize);

    header = (pops_smb_vfs_header_t *)package;
    header->magic = POPS_EE_SMB_PACKAGE_MAGIC;
    header->version = POPS_EE_SMB_PACKAGE_VERSION;
    header->fileCount = POPS_EE_SMB_FILE_COUNT;
    header->totalSize = totalSize;
    memcpy(package + POPS_EE_SMB_CODE_OFFSET, appPOPSSMBVFSStart, codeSize);

    offset = appPOPSAlign16(POPS_EE_SMB_CODE_OFFSET + codeSize);
    for (i = 0; i < POPS_EE_SMB_FILE_COUNT; i++) {
        unsigned int nameSize = strlen(names[i]) + 1;

        header->entries[i].nameOffset = offset;
        memcpy(package + offset, names[i], nameSize);
        offset += nameSize;
    }

    offset = dataOffset;
    for (i = 0; i < POPS_EE_SMB_FILE_COUNT; i++) {
        offset = appPOPSAlign16(offset);
        header->entries[i].dataOffset = offset;
        header->entries[i].size = sizes[i];
        memcpy(package + offset, buffers[i], sizes[i]);
        offset += sizes[i];
    }

    *packageSize = totalSize;
    return package;
}

static void *appPOPSStageIRX(const void *irx, int size)
{
    u8 *staged;

    staged = malloc(size);
    if (!staged)
        return NULL;

    memcpy(staged, irx, size);
    appPOPSWriteU32(staged, POPS_EE_IRX_SIZE_OFFSET, (u32)size);
    return staged;
}

static const void *appPOPSGetBDMDriver(int deviceType, int *size)
{
    if (deviceType == BDM_TYPE_ATA) {
        *size = size_popstarter_bdmhdd_irx;
        return popstarter_bdmhdd_irx;
    }
    if (deviceType == BDM_TYPE_SDC) {
        *size = size_popstarter_mx4sio_irx;
        return popstarter_mx4sio_irx;
    }
    if (deviceType == BDM_TYPE_USB || deviceType == BDM_TYPE_ILINK) {
        *size = size_popstarter_usbhdfsd_irx;
        return popstarter_usbhdfsd_irx;
    }

    *size = 0;
    return NULL;
}

static void appPOPSPatchOuterELF(u8 *patchedELF)
{
    /* POPStarter外层解压后不得清除EE常驻包所在的两个槽位。 */
    appPOPSWriteU32(patchedELF, 0x0434, 0x3C04001C);
    appPOPSWriteU32(patchedELF, 0x0438, 0x3C0601E3);
    appPOPSWriteU32(patchedELF, 0x0444, 0x34840000);
    appPOPSWriteU32(patchedELF, 0x0450, 0x34C63190);

    /* 解压完成后先进入低端常驻跳板，再执行内层入口。 */
    appPOPSWriteU32(patchedELF, 0x28B54, 0x08025080);
}

static void *appPreparePOPSBDMEEInjection(const pops_boot_mailbox_t *mailbox)
{
    elf_loader_resident_copy_table_t *copyTable;
    const void *driver;
    u8 *patchedELF;
    void *stagedUSBD;
    void *stagedDriver;
    int driverSize;

    driver = appPOPSGetBDMDriver(mailbox->deviceType, &driverSize);
    if (!driver)
        return NULL;

    patchedELF = malloc(size_popstarter_elf);
    stagedUSBD = appPOPSStageIRX(popstarter_usbd_irx, size_popstarter_usbd_irx);
    stagedDriver = appPOPSStageIRX(driver, driverSize);
    if (!patchedELF || !stagedUSBD || !stagedDriver) {
        free(patchedELF);
        free(stagedUSBD);
        free(stagedDriver);
        return NULL;
    }

    /* 内嵌资源与补丁版本由构建保证，运行时只处理内存分配失败。 */
    memcpy(patchedELF, popstarter_elf, size_popstarter_elf);

    appPOPSPatchOuterELF(patchedELF);

    memset(appPOPSEEResident, 0, sizeof(appPOPSEEResident));
    memcpy(appPOPSEEResident, mailbox, sizeof(*mailbox));
    memcpy(appPOPSEEResident + POPS_EE_TRAMPOLINE_OFFSET,
           appPOPSEETrampoline, sizeof(appPOPSEETrampoline));

    copyTable = (elf_loader_resident_copy_table_t *)(appPOPSEEResident + POPS_EE_COPY_TABLE_OFFSET);
    copyTable->magic = ELF_LOADER_RESIDENT_COPY_MAGIC;
    copyTable->version = ELF_LOADER_RESIDENT_COPY_VERSION;
    copyTable->count = 2;
    copyTable->entries[0].source = stagedUSBD;
    copyTable->entries[0].destination = (void *)POPS_EE_USBD_ADDRESS;
    copyTable->entries[0].size = size_popstarter_usbd_irx;
    copyTable->entries[1].source = stagedDriver;
    copyTable->entries[1].destination = (void *)POPS_EE_DRIVER_ADDRESS;
    copyTable->entries[1].size = driverSize;
    copyTable->checksum = appPOPSChecksum(copyTable, sizeof(*copyTable) - sizeof(copyTable->checksum));

    return patchedELF;
}

static void *appPreparePOPSSMBEEInjection(const pops_boot_mailbox_t *mailbox)
{
    elf_loader_resident_copy_table_t *copyTable;
    unsigned int packageSize;
    u8 *patchedELF;
    void *package;

    package = appPOPSBuildSMBPackage(&packageSize);
    patchedELF = malloc(size_popstarter_elf);
    if (!package || !patchedELF) {
        free(package);
        free(patchedELF);
        return NULL;
    }

    memcpy(patchedELF, popstarter_elf, size_popstarter_elf);
    appPOPSPatchOuterELF(patchedELF);

    memset(appPOPSEEResident, 0, sizeof(appPOPSEEResident));
    memcpy(appPOPSEEResident, mailbox, sizeof(*mailbox));
    if (appPOPSBuildSMBTrampoline() < 0) {
        free(package);
        free(patchedELF);
        return NULL;
    }

    copyTable = (elf_loader_resident_copy_table_t *)(appPOPSEEResident + POPS_EE_COPY_TABLE_OFFSET);
    copyTable->magic = ELF_LOADER_RESIDENT_COPY_MAGIC;
    copyTable->version = ELF_LOADER_RESIDENT_COPY_VERSION;
    copyTable->count = 1;
    copyTable->entries[0].source = package;
    copyTable->entries[0].destination = (void *)POPS_EE_SMB_PACKAGE_ADDRESS;
    copyTable->entries[0].size = packageSize;
    copyTable->checksum = appPOPSChecksum(copyTable, sizeof(*copyTable) - sizeof(copyTable->checksum));

    return patchedELF;
}

static void *appPreparePOPSHDDOPLEEInjection(const char *partition, const char *pfsPath)
{
    char resourceFormat[POPS_EE_HDD_RESOURCE_FORMAT_CAPACITY];
    char vcdPrefix[POPS_EE_HDD_VCD_PREFIX_CAPACITY];
    char pfsFormat[POPS_EE_HDD_PFS_FORMAT_CAPACITY];
    const char *relativePath;
    u8 *patchedELF;

    if (partition == NULL || strncmp(partition, "hdd0:", 5) != 0 || partition[5] == '\0' ||
        strlen(partition) >= POPS_EE_HDD_PARTITION_CAPACITY ||
        snprintf(resourceFormat, sizeof(resourceFormat), ":%s:%%s\n", partition + 5) >= (int)sizeof(resourceFormat))
        return NULL;

    if (pfsPath == NULL || strncmp(pfsPath, "pfs0:", 5) != 0)
        return NULL;
    relativePath = pfsPath + 5;
    while (*relativePath == '/')
        relativePath++;
    if (*relativePath == '\0' ||
        snprintf(vcdPrefix, sizeof(vcdPrefix), "pfs1:/%s/", relativePath) >= (int)sizeof(vcdPrefix) ||
        snprintf(pfsFormat, sizeof(pfsFormat), "pfs1:/%s/%%s\n", relativePath) >= 0x2C)
        return NULL;

    patchedELF = malloc(size_popstarter_elf);
    if (!patchedELF)
        return NULL;

    memcpy(patchedELF, popstarter_elf, size_popstarter_elf);
    appPOPSPatchOuterELF(patchedELF);

    memset(appPOPSEEResident, 0, sizeof(appPOPSEEResident));
    memcpy(appPOPSEEResident + POPS_EE_TRAMPOLINE_OFFSET,
           appPOPSHDDOPLTrampoline, sizeof(appPOPSHDDOPLTrampoline));
    memcpy(appPOPSEEResident + POPS_EE_HDD_PATCH_HELPER_OFFSET,
           appPOPSHDDOPLPatchHelper, sizeof(appPOPSHDDOPLPatchHelper));
    memcpy(appPOPSEEResident + POPS_EE_HDD_PATH_HELPER_OFFSET,
           appPOPSHDDOPLPathHelper, sizeof(appPOPSHDDOPLPathHelper));
    memcpy(appPOPSEEResident + POPS_EE_HDD_NATIVE_HELPER_OFFSET,
           appPOPSHDDOPLNativeHelper, sizeof(appPOPSHDDOPLNativeHelper));
    memcpy(appPOPSEEResident + POPS_EE_HDD_MOUNT_HELPER_OFFSET,
           appPOPSHDDOPLMountHelper, sizeof(appPOPSHDDOPLMountHelper));
    memcpy(appPOPSEEResident + POPS_EE_HDD_PFS_MOUNT_HELPER_OFFSET,
           appPOPSHDDOPLPFSMountHelper, sizeof(appPOPSHDDOPLPFSMountHelper));
    memcpy(appPOPSEEResident + POPS_EE_HDD_VMC_PATH_HELPER_OFFSET,
           appPOPSHDDOPLVMCPathHelper, sizeof(appPOPSHDDOPLVMCPathHelper));
    strcpy((char *)(appPOPSEEResident + POPS_EE_HDD_PARTITION_OFFSET), partition);
    strcpy((char *)(appPOPSEEResident + POPS_EE_HDD_RESOURCE_FORMAT_OFFSET), resourceFormat);
    strcpy((char *)(appPOPSEEResident + POPS_EE_HDD_VCD_PREFIX_OFFSET), vcdPrefix);
    strcpy((char *)(appPOPSEEResident + POPS_EE_HDD_PFS_FORMAT_OFFSET), pfsFormat);

    return patchedELF;
}

static void *appPreparePOPSEEInjection(const pops_boot_mailbox_t *mailbox, int mode, int hddSource,
                                       const char *hddPartition, const char *hddPath)
{
    if (mode == ETH_MODE)
        return appPreparePOPSSMBEEInjection(mailbox);
    if (mode == HDD_MODE && hddSource == OPL_HDD_POPS_SOURCE_OPL)
        return appPreparePOPSHDDOPLEEInjection(hddPartition, hddPath);

    return appPreparePOPSBDMEEInjection(mailbox);
}

static int appBuildPOPSBootMailbox(const app_info_t *app, pops_boot_mailbox_t *mailbox)
{
    int deviceType;

    memset(mailbox, 0, sizeof(*mailbox));

    deviceType = appGetPOPSBDMDeviceType(app);
    if (deviceType < BDM_TYPE_USB || deviceType > BDM_TYPE_ATA)
        return -1;

    if (app->vcdName[0] == '\0')
        return -1;

    if (snprintf(mailbox->vcdPath, sizeof(mailbox->vcdPath), "%s%s",
                 POPS_BOOT_VCD_PREFIX, app->vcdName) >= (int)sizeof(mailbox->vcdPath))
        return -1;

    mailbox->magic = POPS_BOOT_MAILBOX_MAGIC;
    mailbox->version = POPS_BOOT_MAILBOX_VERSION;
    mailbox->deviceType = deviceType;
    mailbox->checksum = appPOPSBootMailboxChecksum(mailbox);
    return 0;
}

static int appGetPOPSDirectoryEntryMode(const char *directory, const char *name, unsigned int *mode)
{
    iox_dirent_t dirent;
    int fd, result;

    fd = fileXioDopen(directory);
    if (fd < 0)
        return -1;

    while ((result = fileXioDread(fd, &dirent)) > 0) {
        if (strcmp(dirent.name, name) == 0) {
            *mode = dirent.stat.mode;
            fileXioDclose(fd);
            return 1;
        }
    }

    fileXioDclose(fd);
    return result < 0 ? -1 : 0;
}

static int appPreparePOPSHDDOPLVCDLink(const app_info_t *app)
{
    static const char linkDirectory[] = "pfs0:/";
    char sourcePath[APP_PATH_MAX + APP_BOOT_MAX + 2];
    char linkPath[APP_BOOT_MAX + sizeof("pfs0:/")];
    char temporaryLinkPath[APP_BOOT_MAX + sizeof("pfs0:/.new")];
    char temporaryLinkName[APP_BOOT_MAX + sizeof(".new")];
    const char *relativePath;
    unsigned int mode;
    int fd, result;

    if (strncmp(app->path, "pfs0:", 5) != 0)
        return -1;
    relativePath = app->path + 5;
    while (*relativePath == '/')
        relativePath++;
    if (*relativePath == '\0' ||
        snprintf(sourcePath, sizeof(sourcePath), "pfs0:/%s/%s", relativePath, app->vcdName) >= (int)sizeof(sourcePath) ||
        snprintf(linkPath, sizeof(linkPath), "pfs0:/%s", app->vcdName) >= (int)sizeof(linkPath) ||
        snprintf(temporaryLinkPath, sizeof(temporaryLinkPath), "pfs0:/%s.new", app->vcdName) >= (int)sizeof(temporaryLinkPath) ||
        snprintf(temporaryLinkName, sizeof(temporaryLinkName), "%s.new", app->vcdName) >= (int)sizeof(temporaryLinkName))
        return -1;

    /* 先确认目标可读，避免把有效映射替换成指向不存在文件的链接。 */
    fd = openFile(sourcePath, O_RDONLY);
    if (fd < 0)
        return -1;
    close(fd);

    result = appGetPOPSDirectoryEntryMode(linkDirectory, app->vcdName, &mode);
    if (result < 0 || (result > 0 && !FIO_S_ISLNK(mode))) {
        /* 目录项模式不会跟随链接，可避免覆盖分区根目录中已有的普通文件。 */
        return -1;
    }

    result = appGetPOPSDirectoryEntryMode(linkDirectory, temporaryLinkName, &mode);
    if (result < 0 || (result > 0 && !FIO_S_ISLNK(mode)))
        return -1;
    if (result > 0) {
        if (fileXioRemove(temporaryLinkPath) < 0)
            return -1;
    }

    /* 先落盘临时链接，再由PFS原子替换正式入口，避免留下空窗。 */
    /* PFS收到的链接内容会去掉设备前缀，因此这里必须保留根目录斜杠。 */
    if (fileXioSymlink(sourcePath, temporaryLinkPath) < 0 ||
        fileXioRename(temporaryLinkPath, linkPath) < 0 ||
        appGetPOPSDirectoryEntryMode(linkDirectory, app->vcdName, &mode) != 1 ||
        !FIO_S_ISLNK(mode)) {
        fileXioRemove(temporaryLinkPath);
        return -1;
    }

    /* 普通打开会由PFS解析链接，能同时验证链接内容及目标VCD。 */
    fd = openFile(linkPath, O_RDONLY);
    if (fd < 0)
        return -1;
    close(fd);

    return 0;
}

static struct config_value_t *appGetConfigValue(int id)
{
    struct config_value_t *cur = configApps->head;

    while (id--) {
        cur = cur->next;
    }

    return cur;
}

static char *appGetELFName(char *name)
{
    // Looking for the ELF name
    char *pos = strrchr(name, '/');
    if (!pos)
        pos = strrchr(name, ':');
    if (pos) {
        return pos + 1;
    }

    return name;
}

static float appGetELFSize(char *path)
{
    int fd, size;
    float bytesInMiB = 1048576.0f;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("Failed to open APP %s\n", path);
        return 0.0f;
    }

    size = getFileSize(fd);
    close(fd);

    // Return size in MiB
    return (size / bytesInMiB);
}

static char *appGetBoot(char *device, int max, char *path)
{
    char *pos, *filenamesep;

    // Looking for the boot device & filename from the path
    pos = strrchr(path, ':');
    if (pos != NULL) {
        int len = (int)(pos + 1 - path);
        if (len + 1 > max)
            len = max - 1;
        strncpy(device, path, len);
        device[len] = '\0';
    }

    filenamesep = strchr(path, '/');
    if (filenamesep != NULL)
        return filenamesep + 1;

    if (pos) {
        return pos + 1;
    }

    return path;
}

void appInit(item_list_t *itemList)
{
    LOG("APPSUPPORT Init\n");
    appForceUpdate = 1;
    appAutoListInitialized = 0;
    appUpdateAllSources = 0;
    appInitialScanState = gAutoDetectPS1Apps && gAPPStartMode == START_MODE_AUTO ? APP_INITIAL_SCAN_PENDING : APP_INITIAL_SCAN_COMPLETE;
    // ETH和HDD可能先于APPS初始化完成，必须保留它们已经登记的来源更新。
    appUpdateSourceMask = 0;
    configGetInt(configGetByType(CONFIG_OPL), "app_frames_delay", &appItemList.delay);
    appFreeLegacyConfig();
    if (!gAutoDetectPS1Apps)
        configApps = oplGetLegacyAppsConfig();
    appsList = NULL;
    appItemList.enabled = 1;
}

item_list_t *appGetObject(int initOnly)
{
    if (initOnly && !appItemList.enabled)
        return NULL;
    return &appItemList;
}

void appForceRefresh(void)
{
    appForceUpdate = 1;
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &appItemList.mode);
}

void appRequestSourceRefresh(int mode)
{
    if (!gAutoDetectPS1Apps || mode < BDM_MODE || mode > HDD_MODE)
        return;

    appPendingSourceMask |= APP_SOURCE_BIT(mode);
    // 首次扫描统一放到BDM列表阶段之后，初始化期间只累计来源变化。
    if (appInitialScanState == APP_INITIAL_SCAN_COMPLETE && gAPPStartMode != START_MODE_DISABLED && appItemList.enabled)
        ioPutRequestUnique(IO_MENU_UPDATE_DEFFERED, &appItemList.mode);
}

int appStartInitialScan(void)
{
    if (appInitialScanState == APP_INITIAL_SCAN_PENDING) {
        appInitialScanState = APP_INITIAL_SCAN_QUEUED;
        if (ioPutRequestUnique(IO_MENU_UPDATE_DEFFERED, &appItemList.mode) != IO_OK)
            appInitialScanState = APP_INITIAL_SCAN_PENDING;
    }

    return appInitialScanState == APP_INITIAL_SCAN_COMPLETE;
}

void appPostUpdateCallback(int mode)
{
    if (mode == APP_MODE && appInitialScanState != APP_INITIAL_SCAN_COMPLETE)
        appInitialScanState = APP_INITIAL_SCAN_COMPLETE;
}

static int appNeedsUpdate(item_list_t *itemList)
{
    int update;

    update = 0;
    appUpdateAllSources = 0;
    appUpdateSourceMask = 0;

    if (gAutoDetectPS1Apps) {
        // 自动识别使用来源掩码，旧的全局标记只在传统CFG模式消费。
        oplShouldAppsUpdate();
        if (appForceUpdate) {
            appForceUpdate = 0;
            appPendingSourceMask = 0;
            appUpdateAllSources = 1;
            update = 1;
        } else if (appPendingSourceMask) {
            if (appAutoListInitialized)
                appUpdateSourceMask = appPendingSourceMask;
            else
                appUpdateAllSources = 1;
            appPendingSourceMask = 0;
            update = 1;
        }

        return update;
    }

    if (appForceUpdate) {
        appForceUpdate = 0;
        update = 1;
    }
    if (oplShouldAppsUpdate())
        update = 1;

    if (update) {
        appFreeLegacyConfig();
        if (!gAutoDetectPS1Apps)
            configApps = oplGetLegacyAppsConfig();
    }

    return update;
}

static int addAppsLegacyList(struct app_info_linked **appsLinkedList)
{
    struct config_value_t *cur;
    struct app_info_linked *app;
    int count;

    configClear(configApps);
    configRead(configApps);

    count = 0;
    cur = configApps->head;
    while (cur != NULL) {
        if (*appsLinkedList == NULL) {
            *appsLinkedList = malloc(sizeof(struct app_info_linked));
            app = *appsLinkedList;
            app->next = NULL;
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            break;
        }

        strncpy(app->app.title, cur->key, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';

        // Split the boot filename from the path.
        const char *elfname = appGetELFName(cur->val);
        if (elfname != cur->val) {
            strncpy(app->app.boot, elfname, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';

            int pathlen = (int)(elfname - cur->val) - 1;
            if (cur->val[pathlen] == ':') // Discard only '/'.
                pathlen++;
            if (pathlen > APP_PATH_MAX)
                pathlen = APP_PATH_MAX;
            strncpy(app->app.path, cur->val, pathlen);
            app->app.path[pathlen] = '\0';
        } else {
            // Cannot split boot filename from the path, somehow.
            strncpy(app->app.boot, cur->val, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';
            strncpy(app->app.path, cur->val, APP_PATH_MAX + 1);
            app->app.path[APP_PATH_MAX] = '\0';
        }
        strncpy(app->app.startup, app->app.boot, APP_BOOT_MAX + 1);
        app->app.startup[APP_BOOT_MAX] = '\0';
        app->app.vcdName[0] = '\0';

        app->app.legacy = 1;
        app->app.generated = 0;
        app->app.popstarter = 0;
        app->app.popsHddSource = OPL_HDD_POPS_SOURCE_NONE;
        app->app.popsHddPartition[0] = '\0';
        app->app.sourceMode = APP_SOURCE_NONE;
        count++;
        cur = cur->next;
    }

    return count;
}

static int appScanCallback(const char *path, config_set_t *appConfig, void *arg)
{
    struct app_info_linked **appsLinkedList = (struct app_info_linked **)arg;
    struct app_info_linked *app;
    const char *title, *boot, *argv1;

    if (configGetStr(appConfig, APP_CONFIG_TITLE, &title) != 0 && configGetStr(appConfig, APP_CONFIG_BOOT, &boot) != 0) {
        if (*appsLinkedList == NULL) {
            *appsLinkedList = malloc(sizeof(struct app_info_linked));
            app = *appsLinkedList;
            app->next = NULL;
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            return -1;
        }

        strncpy(app->app.title, title, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';
        strncpy(app->app.boot, boot, APP_BOOT_MAX + 1);
        app->app.boot[APP_BOOT_MAX] = '\0';
        strncpy(app->app.startup, app->app.boot, APP_BOOT_MAX + 1);
        app->app.startup[APP_BOOT_MAX] = '\0';
        strncpy(app->app.path, path, APP_PATH_MAX + 1);
        app->app.path[APP_PATH_MAX] = '\0';
        app->app.vcdName[0] = '\0';
        if (configGetStr(appConfig, APP_CONFIG_ARGV1, &argv1) != 0) {
            strncpy(app->app.argv1, argv1, APP_ARGV1_MAX + 1);
            app->app.argv1[APP_ARGV1_MAX] = '\0';
        } else
            app->app.argv1[0] = '\0';
        app->app.legacy = 0;
        app->app.generated = 0;
        app->app.popstarter = 0;
        app->app.popsHddSource = OPL_HDD_POPS_SOURCE_NONE;
        app->app.popsHddPartition[0] = '\0';
        app->app.sourceMode = APP_SOURCE_NONE;
        return 0;
    } else {
        LOG("APPSUPPORT item has no boot/title.\n");
        return 1;
    }

    return -1;
}

static void appPOPSCacheFree(pops_id_cache_t *cache)
{
    if (cache->entries != NULL)
        free(cache->entries);
    cache->entries = NULL;
    cache->count = 0;
    cache->capacity = 0;
}

static int appPOPSCacheReserve(pops_id_cache_t *cache, int count)
{
    pops_id_cache_entry_t *entries;
    int capacity;

    if (count <= cache->capacity)
        return 0;

    capacity = cache->capacity > 0 ? cache->capacity : 32;
    while (capacity < count) {
        if (capacity > 0x3fffffff)
            return -1;
        capacity *= 2;
    }

    entries = realloc(cache->entries, capacity * sizeof(pops_id_cache_entry_t));
    if (entries == NULL)
        return -1;

    cache->entries = entries;
    cache->capacity = capacity;
    return 0;
}

static int appPOPSCacheAppendEntry(pops_id_cache_t *cache, const char *vcdName, const char *parsedId)
{
    pops_id_cache_entry_t *entry;

    if (appPOPSCacheReserve(cache, cache->count + 1) < 0)
        return -1;

    entry = &cache->entries[cache->count++];
    strcpy(entry->vcdName, vcdName);
    strcpy(entry->parsedId, parsedId);
    return 0;
}

static int appPOPSCacheCompare(const void *left, const void *right)
{
    const pops_id_cache_entry_t *a = (const pops_id_cache_entry_t *)left;
    const pops_id_cache_entry_t *b = (const pops_id_cache_entry_t *)right;

    return strcmp(a->vcdName, b->vcdName);
}

static const pops_id_cache_entry_t *appPOPSCacheFindLoaded(const char *vcdName)
{
    pops_id_cache_entry_t key;

    if (popsCacheLoaded.count == 0)
        return NULL;

    memset(&key, 0, sizeof(key));
    strncpy(key.vcdName, vcdName, sizeof(key.vcdName) - 1);
    return (const pops_id_cache_entry_t *)bsearch(&key, popsCacheLoaded.entries, popsCacheLoaded.count,
                                                   sizeof(pops_id_cache_entry_t), appPOPSCacheCompare);
}

static int appPOPSCacheBuildFilename(char *filename, const char *prefix)
{
    size_t length;

    if (prefix == NULL || prefix[0] == '\0')
        return -1;

    length = strlen(prefix);
    if (length > 0 && (prefix[length - 1] == '/' || prefix[length - 1] == ':'))
        return snprintf(filename, POPS_CACHE_PATH_MAX, "%sCACHE/Cache_POPS.bin", prefix) >= POPS_CACHE_PATH_MAX ? -1 : 0;

    return snprintf(filename, POPS_CACHE_PATH_MAX, "%s/CACHE/Cache_POPS.bin", prefix) >= POPS_CACHE_PATH_MAX ? -1 : 0;
}

static void appPOPSCacheLoad(void)
{
    char filename[POPS_CACHE_PATH_MAX];
    FILE *file;
    long size;
    int count, i, validCount;

    if (appPOPSCacheBuildFilename(filename, popsCachePrefix) < 0)
        return;
    file = fopen(filename, "rb");
    if (file == NULL)
        return;

    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }

    count = size / sizeof(pops_id_cache_entry_t);
    if (count <= 0 || appPOPSCacheReserve(&popsCacheLoaded, count) < 0 ||
        (int)fread(popsCacheLoaded.entries, sizeof(pops_id_cache_entry_t), count, file) != count) {
        fclose(file);
        appPOPSCacheFree(&popsCacheLoaded);
        return;
    }
    fclose(file);

    validCount = 0;
    for (i = 0; i < count; i++) {
        pops_id_cache_entry_t *entry = &popsCacheLoaded.entries[i];

        if (memchr(entry->vcdName, '\0', sizeof(entry->vcdName)) == NULL ||
            memchr(entry->parsedId, '\0', sizeof(entry->parsedId)) == NULL || entry->vcdName[0] == '\0' ||
            sbIsValidStartupExecName(entry->parsedId) != 0)
            continue;

        if (validCount != i)
            memcpy(&popsCacheLoaded.entries[validCount], entry, sizeof(*entry));
        validCount++;
    }
    popsCacheLoaded.count = validCount;
    if (popsCacheLoaded.count > 1)
        qsort(popsCacheLoaded.entries, popsCacheLoaded.count, sizeof(pops_id_cache_entry_t), appPOPSCacheCompare);
}

static void appPOPSCacheBegin(const char *cachePrefix)
{
    appPOPSCacheFree(&popsCacheLoaded);
    appPOPSCacheFree(&popsCacheCurrent);
    popsCachePrefix[0] = '\0';
    popsCacheActive = 0;

    if (cachePrefix == NULL || cachePrefix[0] == '\0' || strlen(cachePrefix) >= sizeof(popsCachePrefix))
        return;

    strcpy(popsCachePrefix, cachePrefix);
    popsCacheActive = 1;
    appPOPSCacheLoad();
}

static void appPOPSCacheCommit(void)
{
    char filename[POPS_CACHE_PATH_MAX];
    FILE *file;
    int i, writeCount;
    int unchanged;

    if (!popsCacheActive)
        return;

    if (popsCacheCurrent.count > 1)
        qsort(popsCacheCurrent.entries, popsCacheCurrent.count, sizeof(pops_id_cache_entry_t), appPOPSCacheCompare);
    writeCount = 0;
    for (i = 0; i < popsCacheCurrent.count; i++) {
        if (writeCount == 0 || strcmp(popsCacheCurrent.entries[i].vcdName, popsCacheCurrent.entries[writeCount - 1].vcdName) != 0) {
            if (writeCount != i)
                memcpy(&popsCacheCurrent.entries[writeCount], &popsCacheCurrent.entries[i], sizeof(pops_id_cache_entry_t));
            writeCount++;
        }
    }
    popsCacheCurrent.count = writeCount;

    unchanged = popsCacheCurrent.count == popsCacheLoaded.count &&
                (writeCount == 0 || memcmp(popsCacheCurrent.entries, popsCacheLoaded.entries,
                                           writeCount * sizeof(pops_id_cache_entry_t)) == 0);
    if (unchanged)
        return;

    if (appPOPSCacheBuildFilename(filename, popsCachePrefix) < 0)
        return;
    if (popsCacheCurrent.count == 0) {
        remove(filename);
        return;
    }

    file = fopen(filename, "wb");
    if (file == NULL) {
        LOG("APPSUPPORT unable to write POPS ID cache: %s\n", filename);
        return;
    }
    if ((int)fwrite(popsCacheCurrent.entries, sizeof(pops_id_cache_entry_t), popsCacheCurrent.count, file) != popsCacheCurrent.count)
        LOG("APPSUPPORT POPS ID cache write failed: %s\n", filename);
    fclose(file);
}

static void appPOPSCacheEnd(void)
{
    appPOPSCacheFree(&popsCacheLoaded);
    appPOPSCacheFree(&popsCacheCurrent);
    popsCachePrefix[0] = '\0';
    popsCacheActive = 0;
}

static int appIsNumberedVcdName(const char *vcdName, int nameLength)
{
    char startup[POPS_CACHE_ID_MAX];

    if (nameLength < 17 || vcdName[11] != '.' || strcasecmp(&vcdName[nameLength - 4], ".VCD") != 0)
        return 0;

    memcpy(startup, vcdName, POPS_CACHE_ID_MAX - 1);
    startup[POPS_CACHE_ID_MAX - 1] = '\0';
    return sbIsValidStartupExecName(startup) == 0;
}

static int appAddPOPSItem(const char *path, const char *vcdName, void *arg, const char *elfPrefix, int hddSource, const char *hddPartition)
{
    app_scan_context_t *context = (app_scan_context_t *)arg;
    struct app_info_linked **appsLinkedList = context->appsLinkedList;
    struct app_info_linked *app;
    char title[APP_TITLE_MAX + 1];
    char boot[APP_BOOT_MAX + 1];
    char startup[APP_BOOT_MAX + 1];
    char vcdPath[APP_PATH_MAX + POPS_CACHE_NAME_MAX + 2];
    const pops_id_cache_entry_t *cachedId;
    int nameLength, titleLength, pathLength;

    if (hddPartition != NULL && strlen(hddPartition) > APP_HDD_PARTITION_MAX) {
        LOG("APPSUPPORT POPS APA partition name is too long: %s\n", hddPartition);
        return 1;
    }

    nameLength = strlen(vcdName);
    if (appIsNumberedVcdName(vcdName, nameLength)) {
        // 编号前缀：boot芯=前11字符，显示title=其后至.VCD前
        titleLength = nameLength - 16; // 11编号 + 1点 + 4(.VCD)
        if (titleLength <= 0 || titleLength > APP_TITLE_MAX) {
            LOG("APPSUPPORT POPS numbered VCD title is invalid: %s\n", vcdName);
            return 1;
        }

        memcpy(startup, vcdName, 11);
        startup[11] = '\0';
        memcpy(title, &vcdName[12], titleLength);
        title[titleLength] = '\0';

        // boot = {elfPrefix}{boot芯}.{显示title}.ELF；封面下/ART KEY = boot芯
        if (snprintf(boot, sizeof(boot), "%s%s.%s.ELF", elfPrefix, startup, title) >= sizeof(boot)) {
            LOG("APPSUPPORT POPS ELF filename is too long: %s\n", vcdName);
            return 1;
        }
    } else {
        titleLength = nameLength - 4; // Remove the .VCD extension.
        if (titleLength <= 0 || titleLength > APP_TITLE_MAX) {
            LOG("APPSUPPORT POPS VCD filename is too long: %s\n", vcdName);
            return 1;
        }

        memcpy(title, vcdName, titleLength);
        title[titleLength] = '\0';

        if (snprintf(boot, sizeof(boot), "%s%s.ELF", elfPrefix, title) >= sizeof(boot)) {
            LOG("APPSUPPORT POPS ELF filename is too long: %s\n", vcdName);
            return 1;
        }

        cachedId = popsCacheActive ? appPOPSCacheFindLoaded(vcdName) : NULL;
        if (cachedId != NULL) {
            strcpy(startup, cachedId->parsedId);
            if (appPOPSCacheAppendEntry(&popsCacheCurrent, vcdName, startup) < 0)
                return -1;
        } else {
            if (snprintf(vcdPath, sizeof(vcdPath), "%s/%s", path, vcdName) >= (int)sizeof(vcdPath))
                return 1;

            if (sbGetPOPSStartupExecName(vcdPath, startup, APP_BOOT_MAX) == 0) {
                if (popsCacheActive && appPOPSCacheAppendEntry(&popsCacheCurrent, vcdName, startup) < 0)
                    return -1;
            } else {
                // 无法解析时保留旧链路，确保游戏仍可启动。
                strncpy(startup, boot, sizeof(startup));
                startup[sizeof(startup) - 1] = '\0';
            }
        }
    }

    app = malloc(sizeof(struct app_info_linked));
    if (!app) {
        LOG("APPSUPPORT unable to allocate memory.\n");
        return -1;
    }
    app->next = *appsLinkedList;
    *appsLinkedList = app;

    strcpy(app->app.title, title);
    strcpy(app->app.boot, boot);
    strcpy(app->app.vcdName, vcdName);
    strcpy(app->app.startup, startup);
    strcpy(app->app.path, path);
    pathLength = strlen(app->app.path);
    if (pathLength > 0 && app->app.path[pathLength - 1] == '/')
        app->app.path[pathLength - 1] = '\0';
    app->app.argv1[0] = '\0';
    app->app.legacy = 0;
    app->app.generated = 1;
    app->app.popstarter = 1;
    app->app.popsHddSource = hddSource;
    app->app.sourceMode = context->sourceMode;
    if (hddPartition != NULL) {
        strncpy(app->app.popsHddPartition, hddPartition, APP_HDD_PARTITION_MAX);
        app->app.popsHddPartition[APP_HDD_PARTITION_MAX] = '\0';
    } else {
        app->app.popsHddPartition[0] = '\0';
    }

    return 0;
}

static int appScanBDMPOPSCallback(const char *path, const char *vcdName, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, POPS_BDM_ELF_PREFIX, OPL_HDD_POPS_SOURCE_NONE, NULL);
}

static int appScanSMBPOPSCallback(const char *path, const char *vcdName, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, POPS_SMB_ELF_PREFIX, OPL_HDD_POPS_SOURCE_NONE, NULL);
}

static int appScanHDDPOPSCallback(const char *path, const char *vcdName, int source, const char *partition, void *arg)
{
    return appAddPOPSItem(path, vcdName, arg, "", source, partition);
}

static int appScanELFCallback(const char *path, const char *elfName, void *arg)
{
    app_scan_context_t *context = (app_scan_context_t *)arg;
    struct app_info_linked **appsLinkedList = context->appsLinkedList;
    struct app_info_linked *app;
    char title[APP_TITLE_MAX + 1];
    int titleLength;

    if (strlen(elfName) > APP_TITLE_MAX + 4) {
        LOG("APPSUPPORT APP ELF filename is too long: %s\n", elfName);
        return 1;
    }

    app = malloc(sizeof(struct app_info_linked));
    if (!app) {
        LOG("APPSUPPORT unable to allocate memory.\n");
        return -1;
    }
    app->next = *appsLinkedList;
    *appsLinkedList = app;

    titleLength = strlen(elfName) - 4; // Remove the .ELF extension.
    memcpy(title, elfName, titleLength);
    title[titleLength] = '\0';
    strcpy(app->app.title, title);
    strcpy(app->app.boot, elfName);
    app->app.vcdName[0] = '\0';
    strcpy(app->app.startup, elfName);
    strcpy(app->app.path, path);
    app->app.argv1[0] = '\0';
    app->app.legacy = 0;
    app->app.generated = 1;
    app->app.popstarter = 0;
    app->app.popsHddSource = OPL_HDD_POPS_SOURCE_NONE;
    app->app.popsHddPartition[0] = '\0';
    app->app.sourceMode = context->sourceMode;

    return 0;
}

static void appFreeLinkedList(struct app_info_linked **appsLinkedList)
{
    struct app_info_linked *appNext;

    while (*appsLinkedList) {
        appNext = (*appsLinkedList)->next;
        free(*appsLinkedList);
        *appsLinkedList = appNext;
    }
}

static void appMoveLinkedList(app_info_t *target, int *targetCount, struct app_info_linked **appsLinkedList)
{
    struct app_info_linked *app, *appNext, *reversedList;

    reversedList = NULL;
    app = *appsLinkedList;
    while (app) {
        appNext = app->next;
        app->next = reversedList;
        reversedList = app;
        app = appNext;
    }

    while (reversedList) {
        memcpy(&target[*targetCount], &reversedList->app, sizeof(app_info_t));
        (*targetCount)++;
        appNext = reversedList->next;
        free(reversedList);
        reversedList = appNext;
    }
    *appsLinkedList = NULL;
}

static void appCopyCachedSource(app_info_t *target, int *targetCount, int sourceMode, int popstarter)
{
    int i;

    for (i = 0; i < appItemCount; i++) {
        if (appsList[i].sourceMode == sourceMode && !!appsList[i].popstarter == popstarter) {
            memcpy(&target[*targetCount], &appsList[i], sizeof(app_info_t));
            (*targetCount)++;
        }
    }
}

static int appScanAutoAppsSource(int sourceMode, struct app_info_linked **appsLinkedList)
{
    app_scan_context_t context;

    context.appsLinkedList = appsLinkedList;
    context.sourceMode = sourceMode;

    if (sourceMode == APP_SOURCE_MC)
        return oplScanMCApps(&appScanELFCallback, &context);
    if (sourceMode >= BDM_MODE && sourceMode <= BDM_MODE4)
        return oplScanBDMAppsByMode(sourceMode, &appScanELFCallback, &context);
    if (sourceMode == ETH_MODE)
        return oplScanSMBApps(&appScanELFCallback, &context);
    if (sourceMode == HDD_MODE)
        return oplScanHDDApps(&appScanELFCallback, &context);

    return 0;
}

static int appScanAutoPOPSSource(int sourceMode, struct app_info_linked **appsLinkedList)
{
    app_scan_context_t context;
    const char *cachePrefix;
    int result;

    context.appsLinkedList = appsLinkedList;
    context.sourceMode = sourceMode;

    cachePrefix = oplGetPOPSCachePrefix(sourceMode);
    appPOPSCacheBegin(cachePrefix);

    if (sourceMode >= BDM_MODE && sourceMode <= BDM_MODE4)
        result = oplScanBDMPOPSByMode(sourceMode, &appScanBDMPOPSCallback, &context);
    else if (sourceMode == ETH_MODE)
        result = oplScanSMBPOPS(&appScanSMBPOPSCallback, &context);
    else if (sourceMode == HDD_MODE)
        result = oplScanHDDPOPS(&appScanHDDPOPSCallback, &context);
    else
        result = 0;

    if (result >= 0)
        appPOPSCacheCommit();
    appPOPSCacheEnd();

    return result;
}

static void appRestoreAutoRefresh(u32 sourceMask, int fullUpdate)
{
    if (fullUpdate)
        appForceUpdate = 1;
    else
        appPendingSourceMask |= sourceMask;
}

static int appUpdateAutoItemList(void)
{
    struct app_info_linked *newAppsLists[MODE_COUNT];
    struct app_info_linked *newPOPSLists[MODE_COUNT];
    int newAppsCounts[MODE_COUNT];
    int newPOPSCounts[MODE_COUNT];
    app_info_t *newAppsList;
    u32 sourceMask;
    int fullUpdate, newItemCount, outputCount;
    int sourceMode, i;

    memset(newAppsLists, 0, sizeof(newAppsLists));
    memset(newPOPSLists, 0, sizeof(newPOPSLists));
    memset(newAppsCounts, 0, sizeof(newAppsCounts));
    memset(newPOPSCounts, 0, sizeof(newPOPSCounts));

    fullUpdate = appUpdateAllSources || !appAutoListInitialized;
    sourceMask = fullUpdate ? APP_AUTO_SOURCE_MASK : appUpdateSourceMask & APP_AUTO_SOURCE_MASK;
    appUpdateAllSources = 0;
    appUpdateSourceMask = 0;
    if (!sourceMask)
        return appItemCount;

    for (sourceMode = BDM_MODE; sourceMode <= APP_SOURCE_MC; sourceMode++) {
        if (!(sourceMask & APP_SOURCE_BIT(sourceMode)))
            continue;

        newAppsCounts[sourceMode] = appScanAutoAppsSource(sourceMode, &newAppsLists[sourceMode]);
        if (newAppsCounts[sourceMode] < 0)
            break;

        newPOPSCounts[sourceMode] = appScanAutoPOPSSource(sourceMode, &newPOPSLists[sourceMode]);
        if (newPOPSCounts[sourceMode] < 0)
            break;
    }

    if (sourceMode <= APP_SOURCE_MC) {
        for (i = BDM_MODE; i <= APP_SOURCE_MC; i++) {
            appFreeLinkedList(&newAppsLists[i]);
            appFreeLinkedList(&newPOPSLists[i]);
        }
        appRestoreAutoRefresh(sourceMask, fullUpdate);
        return appItemCount;
    }

    newItemCount = 0;
    for (sourceMode = BDM_MODE; sourceMode <= APP_SOURCE_MC; sourceMode++)
        newItemCount += newAppsCounts[sourceMode] + newPOPSCounts[sourceMode];
    if (!fullUpdate) {
        for (i = 0; i < appItemCount; i++) {
            sourceMode = appsList[i].sourceMode;
            if (sourceMode < BDM_MODE || sourceMode > APP_SOURCE_MC || !(sourceMask & APP_SOURCE_BIT(sourceMode)))
                newItemCount++;
        }
    }

    newAppsList = NULL;
    if (newItemCount > 0) {
        newAppsList = malloc(newItemCount * sizeof(app_info_t));
        if (!newAppsList) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            for (i = BDM_MODE; i <= APP_SOURCE_MC; i++) {
                appFreeLinkedList(&newAppsLists[i]);
                appFreeLinkedList(&newPOPSLists[i]);
            }
            appRestoreAutoRefresh(sourceMask, fullUpdate);
            return appItemCount;
        }
    }

    outputCount = 0;
    if (sourceMask & APP_SOURCE_BIT(APP_SOURCE_MC))
        appMoveLinkedList(newAppsList, &outputCount, &newAppsLists[APP_SOURCE_MC]);
    else
        appCopyCachedSource(newAppsList, &outputCount, APP_SOURCE_MC, 0);

    for (sourceMode = BDM_MODE; sourceMode <= HDD_MODE; sourceMode++) {
        if (sourceMask & APP_SOURCE_BIT(sourceMode))
            appMoveLinkedList(newAppsList, &outputCount, &newAppsLists[sourceMode]);
        else
            appCopyCachedSource(newAppsList, &outputCount, sourceMode, 0);
    }
    for (sourceMode = BDM_MODE; sourceMode <= HDD_MODE; sourceMode++) {
        if (sourceMask & APP_SOURCE_BIT(sourceMode))
            appMoveLinkedList(newAppsList, &outputCount, &newPOPSLists[sourceMode]);
        else
            appCopyCachedSource(newAppsList, &outputCount, sourceMode, 1);
    }

    // 未带来源的旧条目只在局部更新时保留，完整更新会统一重建来源标记。
    if (!fullUpdate) {
        for (i = 0; i < appItemCount; i++) {
            sourceMode = appsList[i].sourceMode;
            if (sourceMode < BDM_MODE || sourceMode > APP_SOURCE_MC) {
                memcpy(&newAppsList[outputCount], &appsList[i], sizeof(app_info_t));
                outputCount++;
            }
        }
    }

    if (appsList)
        free(appsList);
    appsList = newAppsList;
    appItemCount = outputCount;
    appAutoListInitialized = 1;

    // 更新期间若又收到来源变化，必须追加一轮，不能被当前在途请求吞掉。
    if (appPendingSourceMask)
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &appItemList.mode);

    LOG("APPSUPPORT %d apps loaded\n", appItemCount);
    return appItemCount;
}

static int appUpdateItemList(item_list_t *itemList)
{
    struct app_info_linked *appsLinkedList, *appNext;

    if (gAutoDetectPS1Apps)
        return appUpdateAutoItemList();

    appFreeList();

    appsLinkedList = NULL;

    // Get legacy apps list first, so it is possible to use appGetConfigValue(id).
    appItemCount += addAppsLegacyList(&appsLinkedList);

    // Scan title.cfg files on devices.
    appItemCount += oplScanApps(&appScanCallback, &appsLinkedList);

    // Generate apps list
    if (appItemCount > 0) {
        appsList = malloc(appItemCount * sizeof(app_info_t));

        if (appsList != NULL) {
            int i;
            for (i = 0; appsLinkedList != NULL; i++) { // appsLinkedList contains items in reverse order.
                memcpy(&appsList[appItemCount - i - 1], &appsLinkedList->app, sizeof(app_info_t));

                appNext = appsLinkedList->next;
                free(appsLinkedList);
                appsLinkedList = appNext;
            }
        } else {
            LOG("APPSUPPORT unable to allocate memory.\n");
            appItemCount = 0;
        }
    }

    appAutoListInitialized = 0;
    LOG("APPSUPPORT %d apps loaded\n", appItemCount);

    return appItemCount;
}

static void appFreeList(void)
{
    if (appsList != NULL) {
        free(appsList);
        appsList = NULL;
    }

    appItemCount = 0;
}

static void appFreeLegacyConfig(void)
{
    if (configApps != NULL) {
        configFree(configApps);
        configApps = NULL;
    }
}

static int appGetItemCount(item_list_t *itemList)
{
    return appItemCount;
}

static char *appGetItemName(item_list_t *itemList, int id)
{
    return appsList[id].title;
}

static int appGetItemNameLength(item_list_t *itemList, int id)
{
    return CONFIG_KEY_NAME_LEN;
}

/* appGetItemStartup()：封面下方文字，同时作为 ART KEY。 */
static char *appGetItemStartup(item_list_t *itemList, int id)
{
    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        return appGetELFName(cur->val);
    } else {
        return appsList[id].startup;
    }
}

static void appDeleteItem(item_list_t *itemList, int id)
{
    if (appsList[id].generated)
        return;

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        unlink(cur->val);
        cur->key[0] = '\0';
        configApps->modified = 1;
        configWrite(configApps);
    } else {
        sysDeleteFolder(appsList[id].path);
    }

    appForceUpdate = 1;
}

static void appRenameItem(item_list_t *itemList, int id, char *newName)
{
    char value[256];

    if (appsList[id].generated)
        return;

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);

        strncpy(value, cur->val, sizeof(value));
        configRemoveKey(configApps, cur->key);
        configSetStr(configApps, newName, value);
        configWrite(configApps);
    } else {
        config_set_t *appConfig;

        snprintf(value, sizeof(value), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);

        appConfig = configAlloc(0, NULL, value);
        if (appConfig != NULL) {
            configRead(appConfig);
            configSetStr(appConfig, APP_CONFIG_TITLE, newName);
            configWrite(appConfig);

            configFree(appConfig);
        }
    }

    appForceUpdate = 1;
}

static void appPreparePOPSLauncher(void)
{
    pops_boot_mailbox_t bootMailbox;
    const void *driver;
    int driverSize;
    int useEEInjection;
    int mode;

    appPOPSPrepareResult = 0;
    mode = appResolveMode(&appsList[appPOPSPrepareID]);
    driver = NULL;
    driverSize = 0;
    useEEInjection = mode == ETH_MODE ||
                     (mode == HDD_MODE && appsList[appPOPSPrepareID].popsHddSource == OPL_HDD_POPS_SOURCE_OPL);
    if (!useEEInjection && mode >= BDM_MODE && mode <= BDM_MODE4 &&
        appBuildPOPSBootMailbox(&appsList[appPOPSPrepareID], &bootMailbox) == 0)
        driver = appPOPSGetBDMDriver(bootMailbox.deviceType, &driverSize);
    if (!useEEInjection)
        useEEInjection = driver != NULL;

    /* 只有EE注入无法使用时才写入记忆卡，避免正常BDM启动继续依赖外部文件。 */
    if (useEEInjection)
        appPOPSPrepareResult |= APP_POPS_PREPARE_EE_READY;
    else if (gAutoDetectPS1Apps && mode != HDD_MODE &&
             installPopstarterDrivers(mode, appGetPOPSBDMDeviceType(&appsList[appPOPSPrepareID])) < 0)
        appPOPSPrepareResult |= APP_POPS_PREPARE_DRIVERS_FAILED;

    appPOPSPrepareStatus = 0;
}

static void appLaunchItem(item_list_t *itemList, int id, config_set_t *configSet)
{
    int fd;
    char filename[256];
    const char *argv1;

    if (gAutoDetectPS1Apps && appIsPOPSLauncher(&appsList[id])) {
        char cheatPath[APP_PATH_MAX + 14];
        char cheatPrompt[APP_HDD_PARTITION_MAX + APP_PATH_MAX + 64];
        char popstarterArg[APP_HDD_PARTITION_MAX + APP_PATH_MAX + APP_BOOT_MAX + sizeof(":pfs://") + 1];
        char *argv[1];
        pops_boot_mailbox_t bootMailbox;
        const void *launchELF;
        const void *residentData;
        unsigned int residentSize;
        int useEEInjection;
        const char *cheats =
            "HDTVFIX / 强制使用480i输出，解决部分液晶电视不支持240p导致的黑屏或绿屏。\r\n"
            "480p / 强制使用480p输出，部分游戏可能不兼容。\r\n"
            "\r\n"
            "COMPATIBILITY_0x01 / 修复部分游戏缺少音乐或语音的问题。\r\n"
            "COMPATIBILITY_0x02 / 模式0x01的变体，并避免破坏部分游戏的MDEC过场动画。\r\n"
            "COMPATIBILITY_0x03 / 模式0x01无效时可尝试的另一种CD状态修复。\r\n"
            "COMPATIBILITY_0x04 / 修复部分游戏的卡顿、闪烁及其他兼容性问题。\r\n"
            "COMPATIBILITY_0x05 / 主要用于修复PAL版《生化危机：导演剪辑版》的过场动画。\r\n"
            "COMPATIBILITY_0x06 / 跳过BIOS OSD外壳及部分光盘检查，修复部分游戏启动时卡死。\r\n"
            "COMPATIBILITY_0x07 / 修复部分游戏纹理缺失的问题。\r\n"
            "\r\n"
            "CODECACHE_ADDON_0 / 调整代码缓存机制，可修复部分游戏的卡顿或随机死机。\r\n"
            "SUBCDSTATUS / 调整光驱子状态，作用类似兼容模式0x05。\r\n"
            "FAKELC / 使用伪造的LibCrypt数据，绕过部分游戏的LibCrypt保护问题。\r\n"
            "\r\n"
            "NOPAL / 禁用POPStarter的自动PAL补丁，按POPS原生NTSC方式输出。\r\n"
            "FORCEPAL / 强制启用PAL补丁和欧洲BIOS区域设置。\r\n"
            "SMOOTH / 默认启用POPS纹理平滑。\r\n"
            "\r\n"
            "SCANLINES / 启用扫描线显示效果。\r\n"
            "WIDESCREEN / 启用宽屏显示补丁，不会修正2D界面和背景。\r\n"
            "ULTRA_WIDESCREEN / 启用比普通宽屏更宽的显示比例。\r\n"
            "EYEFINITY / 启用超宽多屏风格显示比例。\r\n"
            "\r\n"
            "XPOS_0640 / 设置画面的水平位置，0640为参数示例。\r\n"
            "YPOS_10 / 设置画面的垂直位置，10为参数示例。\r\n"
            "DWSTRETCH_#### / 调整画面的水平拉伸宽度。\r\n"
            "DWCROP_#### / 调整画面的水平裁切范围。\r\n"
            "\r\n"
            "MUTE_CDDA / 禁用游戏的CDDA音轨音乐。\r\n"
            "MUTE_VAB / 禁用VAB、VAG等采样音频或音乐。\r\n"
            "\r\n"
            "SAFEMODE / 延迟启用RAW十六进制代码，降低游戏启动时崩溃的概率。\r\n"
            "00507028 00000001 / 让手柄1始终启用震动。\r\n"
            "005070B8 00000001 / 让手柄2始终启用震动。\r\n"
            "\r\n"
            "D2LS / 使用左摇杆模拟数字方向键，并保持数字控制模式。\r\n"
            "D2LS_ALT / 左摇杆模拟数字方向键的替代模式，并保持模拟控制模式。\r\n"
            "\r\n"
            "NOVMC0 / 禁用第一个虚拟记忆卡插槽。\r\n"
            "NOVMC1 / 禁用第二个虚拟记忆卡插槽。\r\n"
            "\r\n"
            "IGR0 / 按L1+L2+R1+R2+×+下打开IGR菜单。\r\n"
            "IGR1 / 按START+SELECT打开IGR菜单。\r\n"
            "IGR2 / 按L1+L2+R1+R2+START+SELECT打开IGR菜单。\r\n"
            "IGR3 / 按L1+L2+R1+R2+×+下直接退出游戏，不显示IGR菜单。\r\n"
            "IGR4 / 按START+SELECT直接退出游戏，不显示IGR菜单。\r\n"
            "IGR5 / 按L1+L2+R1+R2+START+SELECT直接退出游戏，不显示IGR菜单。\r\n"
            "NOIGR / 完全禁用游戏内重启和退出功能。\r\n"
            "\r\n"
            "CACHE1 / 将POPS读取缓存从16个扇区缩减为1个扇区，可修复部分动画卡死。\r\n"
            "USBDELAY_# / 调整POPS的USB读取延迟；用于数据传输兼容，不影响设备识别。";
        int enableHDTVFix;
        int createCheats = 0;
        int requiresHDDOPLInjection;
        int restoreHDDPOPSPartition = 0;
        int mode;

        snprintf(cheatPrompt, sizeof(cheatPrompt), "若黑屏，请删除 POPS/CHEATS.TXT 即可重新选择分辨率");

        // 初次启动时写入玩家选择的 POPStarter 分辨率配置。
        mode = appResolveMode(&appsList[id]);
        requiresHDDOPLInjection = mode == HDD_MODE && appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_OPL;

        if (mode == HDD_MODE) {
            const char *resourcePartition = requiresHDDOPLInjection ? appsList[id].popsHddPartition : "hdd0:__common";
            const char *resourcePath = requiresHDDOPLInjection ? appsList[id].path : OPL_HDD_POPS_SCRATCH_MOUNTPOINT "POPS";
            const char *resourceRelativePath;
            char resourceLocation[APP_HDD_PARTITION_MAX + APP_PATH_MAX + 2];
            iox_stat_t resourceStat;
            int resourceMounted;

            if (requiresHDDOPLInjection &&
                (strncmp(resourcePartition, "hdd0:", 5) != 0 || resourcePartition[5] == '\0' ||
                 strncmp(appsList[id].path, "pfs0:", 5) != 0 || appsList[id].path[5] == '\0')) {
                guiMsgBox("目录式APA游戏缺少来源分区信息", 0, NULL);
                return;
            }
            if (requiresHDDOPLInjection && strcmp(resourcePartition, gOPLPart) != 0) {
                guiMsgBox("目录式APA游戏的来源分区已经变化，请刷新游戏列表", 0, NULL);
                return;
            }

            resourceRelativePath = resourcePath + 5;
            while (*resourceRelativePath == '/')
                resourceRelativePath++;
            if (requiresHDDOPLInjection && *resourceRelativePath == '\0') {
                guiMsgBox("目录式APA游戏缺少来源分区信息", 0, NULL);
                return;
            }

            snprintf(resourceLocation, sizeof(resourceLocation), "%s/%s",
                     strncmp(resourcePartition, "hdd0:", 5) == 0 ? resourcePartition + 5 : resourcePartition,
                     resourceRelativePath);

            /* 目录式APA只使用固定的pfs0；pfs1仅供独立APA分区临时切换。 */
            if (requiresHDDOPLInjection) {
                resourceMounted = fileXioGetStat(resourcePath, &resourceStat) >= 0;
            } else {
                restoreHDDPOPSPartition = 1;
                resourceMounted = oplEnsureHDDPOPSScratchPartition(resourcePartition) == 0;
            }

            if (resourceMounted) {
                char missingFiles[32];

                missingFiles[0] = '\0';
                snprintf(filename, sizeof(filename), "%s/IOPRP252.IMG", resourcePath);
                fd = openFile(filename, O_RDONLY);
                if (fd >= 0)
                    close(fd);
                else
                    strcpy(missingFiles, "IOPRP252.IMG");

                snprintf(filename, sizeof(filename), "%s/POPS.ELF", resourcePath);
                fd = openFile(filename, O_RDONLY);
                if (fd >= 0)
                    close(fd);
                else {
                    if (missingFiles[0] != '\0')
                        strcat(missingFiles, " 和 ");
                    strcat(missingFiles, "POPS.ELF");
                }

                if (missingFiles[0] != '\0') {
                    char message[96];

                    snprintf(message, sizeof(message), "%s 目录下缺少 %s", resourceLocation, missingFiles);
                    guiMsgBox(message, 0, NULL);
                    if (restoreHDDPOPSPartition)
                        oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
                    return;
                }

                if (requiresHDDOPLInjection && appPreparePOPSHDDOPLVCDLink(&appsList[id]) < 0) {
                    guiMsgBox("无法为目录式APA分区准备原生VCD入口，请检查POPS文件和分区根目录", 0, NULL);
                    if (restoreHDDPOPSPartition)
                        oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
                    return;
                }

                snprintf(cheatPath, sizeof(cheatPath), "%s/CHEATS.TXT", resourcePath);
                snprintf(cheatPrompt, sizeof(cheatPrompt),
                         "若黑屏，请删除 %s/POPS/CHEATS.TXT 即可重新选择分辨率",
                         requiresHDDOPLInjection ? resourcePartition + 5 : "__common");
            } else {
                if (requiresHDDOPLInjection)
                    guiMsgBox("无法挂载目录式APA分区的POPS资源目录", 0, NULL);
                else
                    cheatPath[0] = '\0';
                if (restoreHDDPOPSPartition)
                    oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
                if (requiresHDDOPLInjection)
                    return;
            }
        } else {
            snprintf(cheatPath, sizeof(cheatPath), "%s/POPS_IOX.PAK", appsList[id].path);
            fd = openFile(cheatPath, O_RDONLY);
            if (fd < 0) {
                guiMsgBox("POPS 目录下缺少 POPS_IOX.PAK", 0, NULL);
                return;
            }
            close(fd);

            snprintf(cheatPath, sizeof(cheatPath), "%s/CHEATS.TXT", appsList[id].path);
        }

        if (cheatPath[0] != '\0') {
            fd = openFile(cheatPath, O_RDONLY);
            if (fd >= 0)
                close(fd);
            else {
                enableHDTVFix = guiMsgBoxCustom("初次启动，请选择适合的分辨率，避免黑屏！", "480i (液晶)", "240p (CRT)", NULL);

                guiMsgBox(cheatPrompt, 0, NULL);
                createCheats = 1;
            }
        }

        appPOPSPrepareStatus = 1;
        appPOPSPrepareID = id;
        guiHandleDeferedIO(&appPOPSPrepareStatus, _l(_STR_PLEASE_WAIT), IO_CUSTOM_SIMPLEACTION, &appPreparePOPSLauncher);

        if (requiresHDDOPLInjection && !(appPOPSPrepareResult & APP_POPS_PREPARE_EE_READY)) {
            guiMsgBox("无法启用目录式APA的POPS内存补丁", 0, NULL);
            if (restoreHDDPOPSPartition)
                oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
            return;
        }

        // 目录式APA来源需要携带真实分区和目录；其它链路继续使用uLE假ELF名。
        if (requiresHDDOPLInjection) {
            const char *relativePath = appsList[id].path + 5;

            while (*relativePath == '/')
                relativePath++;
            if (snprintf(popstarterArg, sizeof(popstarterArg), "%s:pfs:/%s/%s",
                         appsList[id].popsHddPartition, relativePath, appsList[id].boot) >= (int)sizeof(popstarterArg)) {
                guiMsgBox("POPSTARTER启动参数过长", 0, NULL);
                if (restoreHDDPOPSPartition)
                    oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
                return;
            }
        } else if (snprintf(popstarterArg, sizeof(popstarterArg), "uLE:%s",
                            appsList[id].boot) >= (int)sizeof(popstarterArg)) {
            guiMsgBox("POPSTARTER启动参数过长", 0, NULL);
            if (restoreHDDPOPSPartition)
                oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
            return;
        }

        argv[0] = popstarterArg;

        launchELF = popstarter_elf;
        residentData = &bootMailbox;
        residentSize = sizeof(bootMailbox);
        useEEInjection = 0;
        if (appBuildPOPSBootMailbox(&appsList[id], &bootMailbox) < 0) {
            memset(&bootMailbox, 0, sizeof(bootMailbox));
        }

        /* SMB与目录式APA链路不依赖BDM邮箱，邮箱不可用时仍须执行各自的EE补丁。 */
        if ((mode == ETH_MODE || requiresHDDOPLInjection || bootMailbox.magic == POPS_BOOT_MAILBOX_MAGIC) &&
            (appPOPSPrepareResult & APP_POPS_PREPARE_EE_READY)) {
            launchELF = appPreparePOPSEEInjection(&bootMailbox, mode, appsList[id].popsHddSource,
                                                  appsList[id].popsHddPartition, appsList[id].path);
            if (launchELF) {
                residentData = appPOPSEEResident;
                residentSize = sizeof(appPOPSEEResident);
                useEEInjection = 1;
            } else {
                if (requiresHDDOPLInjection) {
                    guiMsgBox("无法准备目录式APA的POPS内存补丁", 0, NULL);
                    if (restoreHDDPOPSPartition)
                        oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
                    return;
                }
                launchELF = popstarter_elf;
            }
        }

        /* 预检后若EE内存准备仍失败，必须在OPL退出前补装记忆卡驱动。 */
        if (mode != HDD_MODE &&
            (appPOPSPrepareResult & APP_POPS_PREPARE_EE_READY) && !useEEInjection &&
            installPopstarterDrivers(mode, appGetPOPSBDMDeviceType(&appsList[id])) < 0)
            appPOPSPrepareResult |= APP_POPS_PREPARE_DRIVERS_FAILED;

        if ((appPOPSPrepareResult & APP_POPS_PREPARE_DRIVERS_FAILED) &&
            !guiMsgBox("无法注入驱动，请检查记忆卡！是否强行启动？", 1, NULL)) {
            return;
        }

        if (mode < 0)
            mode = APP_MODE;
        if (appGetPOPSBDMDeviceType(&appsList[id]) == BDM_TYPE_SDC) {
            int enabled = 0;

            fileXioDevctl("mass:", USBMASS_DEVCTL_SET_MX4SIO_PROBE, &enabled, sizeof(enabled), NULL, 0);
        }
        if (createCheats) {
            fd = openFile(cheatPath, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd >= 0) {
                const char *line = cheats;

                if (enableHDTVFix)
                    write(fd, "$", 1);
                while (*line) {
                    const char *lineEnd = strstr(line, "\r\n");
                    int lineLength = lineEnd ? lineEnd - line + 2 : strlen(line);

                    write(fd, line, lineLength);
                    line += lineLength;
                }
                close(fd);
            }
        }
        if (gRememberLastPlayed) {
            configSetStr(configGetByType(CONFIG_LAST), "last_played", appsList[id].startup);
            saveConfig(CONFIG_LAST, 0);
        }
        deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call appCleanUp, so configApps/cur will be freed
        LoadELFFromMemoryNoResetWithResidentData(launchELF, residentData, residentSize, 1, argv);
        if (restoreHDDPOPSPartition)
            oplEnsureHDDPOPSScratchPartition(OPL_HDD_POPS_PARTITION);
        return;
    }

    // Retrieve configuration set by appGetConfig()
    configGetStrCopy(configSet, CONFIG_ITEM_STARTUP, filename, sizeof(filename));

    // If no device number is specified use mass? to auto find device number
    const char *oldPrefix = "mass:";
    const char *newPrefix = "mass?:";

    if (strncmp(filename, oldPrefix, strlen(oldPrefix)) == 0) {
        size_t oldPrefixLen = strlen(oldPrefix);
        size_t newPrefixLen = strlen(newPrefix);
        memmove(filename + newPrefixLen, filename + oldPrefixLen, strlen(filename) - oldPrefixLen + 1);

        memcpy(filename, newPrefix, newPrefixLen);
    }

    // If legacy apps state mass? find the first connected mass device with the corresponding filename and set the unit number for launch.
    if (!strncmp("mass?", filename, 5)) {
        for (int i = 0; i < BDM_MODE4; i++) {
            filename[4] = i + '0';
            fd = open(filename, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                break;
            }
        }
    }

    //// 修复SMB加载APP卡死问题
    //const char *smbOldPrefix = "smb:";
    //const char *smbNewPrefix = "smb0:";
    //if (strncmp(filename, smbOldPrefix, strlen(smbOldPrefix)) == 0) {
    //    size_t oldPrefixLen = strlen(smbOldPrefix);
    //    size_t newPrefixLen = strlen(smbNewPrefix);
    //    memmove(filename + newPrefixLen, filename + oldPrefixLen, strlen(filename) - oldPrefixLen + 1);

    //    memcpy(filename, smbNewPrefix, newPrefixLen);
    //}
    fd = open(filename, O_RDONLY);
    if (fd >= 0) {
        int mode, argc = 0;
        char partition[128];
        char *argv[1];
        close(fd);

        strcpy(partition, "");

        // To keep the necessary device accessible, we will assume the mode that owns the device which contains the file to boot.
        mode = oplPath2Mode(filename);
        if (mode < 0)
            mode = APP_MODE; // Legacy apps mode on memory card (mc?:/*)

        if (mode == HDD_MODE)
            snprintf(partition, sizeof(partition), "%s:", gOPLPart);

        if (configGetStr(configSet, CONFIG_ITEM_ALTSTARTUP, &argv1) != 0) {
            argv[0] = (char *)argv1;
            argc = 1;
        }
        if (gRememberLastPlayed) {
            configSetStr(configGetByType(CONFIG_LAST), "last_played", appsList[id].startup);
            saveConfig(CONFIG_LAST, 0);
        }
        deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call appCleanUp, so configApps/cur will be freed
        LoadELFFromFileWithPartition(filename, partition, argc, argv);
    } else {
        guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
    }
}

static config_set_t *appGetConfig(item_list_t *itemList, int id)
{
    config_set_t *config;
    char tmp[8];

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        config = oplGetLegacyAppsInfo(appGetELFName(cur->val));
        configRead(config);

        configSetStr(config, CONFIG_ITEM_NAME, appGetELFName(cur->val));
        configSetStr(config, CONFIG_ITEM_LONGNAME, cur->key);
        configSetStr(config, CONFIG_ITEM_STARTUP, cur->val);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(cur->val));
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    } else {
        char path[256];
        if (appsList[id].generated) {
            config = configAlloc(0, NULL, NULL);
        } else {
            snprintf(path, sizeof(path), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);
            config = configAlloc(0, NULL, path);
            configRead(config); // Does not matter if the config file could be loaded or not.
        }

        configSetStr(config, CONFIG_ITEM_NAME, appsList[id].boot);
        configSetStr(config, CONFIG_ITEM_LONGNAME, appsList[id].title);
        configSetStr(config, CONFIG_ITEM_ALTSTARTUP, appsList[id].argv1); // reuse AltStartup for argument 1
        if (appsList[id].popstarter &&
            (appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_LEGACY ||
             appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_OPL) &&
            appsList[id].popsHddPartition[0] != '\0' && appsList[id].vcdName[0] != '\0') {
            const char *partition = appsList[id].popsHddPartition;
            const char *relativePath = appsList[id].path;
            const char *separator;

            /* 信息页显示真实APA位置，不暴露pfs0/pfs1临时挂载槽位。 */
            if (strncmp(partition, "hdd0:", 5) == 0)
                partition += 5;

            if (appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_LEGACY) {
                snprintf(path, sizeof(path), "%s:%s", partition, appsList[id].vcdName);
            } else {
                separator = strchr(relativePath, ':');
                if (separator != NULL)
                    relativePath = separator + 1;
                while (*relativePath == '/')
                    relativePath++;

                snprintf(path, sizeof(path), "%s:%s/%s", partition, relativePath, appsList[id].vcdName);
            }
        } else {
            snprintf(path, sizeof(path), "%s/%s", appsList[id].path, appsList[id].boot);
        }
        configSetStr(config, CONFIG_ITEM_STARTUP, path);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        if (appsList[id].popstarter)
            snprintf(tmp, sizeof(tmp), "%.2f", size_popstarter_elf / 1048576.0f);
        else
            snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(path));
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    }
    return config;
}

static int appGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char device[8] = "", *startup;
    int id;

    for (id = 0; id < appItemCount; id++) {
        if (appsList[id].legacy) {
            struct config_value_t *cur = appGetConfigValue(id);
            if (value == appGetELFName(cur->val)) {
                appGetBoot(device, sizeof(device), cur->val);
                break;
            }
        } else if (value == appsList[id].startup) {
            if (appsList[id].popstarter &&
                (appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_LEGACY ||
                 appsList[id].popsHddSource == OPL_HDD_POPS_SOURCE_OPL)) {
                /* HDD POPS的ART固定来自工作分区，不能按pfs1临时槽位判断设备。 */
                appGetBoot(device, sizeof(device), gHDDPrefix);
            } else {
                appGetBoot(device, sizeof(device), appsList[id].path);
            }
            break;
        }
    }

    startup = appGetBoot(device, sizeof(device), value);

    if (!strcmp(folder, "ART")) {
        // 记忆卡ELF：只从卡根 ART/ 读取，不扫其它设备
        if (!strncmp(device, "mc", 2)) {
            char path[128];

            snprintf(path, sizeof(path), "%sART/%s_%s", device, startup, suffix);
            return texDiscoverLoad(resultTex, path, -1);
        }
        return oplGetAppImage(device, folder, isRelative, startup, suffix, resultTex, psm);
    } else
        return oplGetAppImage(device, folder, isRelative, value, suffix, resultTex, psm);
}

static int appGetTextId(item_list_t *itemList)
{
    return _STR_APPS;
}

static int appGetIconId(item_list_t *itemList)
{
    return APP_ICON;
}

// This may be called, even if appInit() was not.
static void appCleanUp(item_list_t *itemList, int exception)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT CleanUp\n");

        appFreeList();
        appFreeLegacyConfig();
        appAutoListInitialized = 0;
        appPendingSourceMask = 0;
        appInitialScanState = APP_INITIAL_SCAN_COMPLETE;
    }
}

// This may be called, even if appInit() was not.
static void appShutdown(item_list_t *itemList)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT Shutdown\n");

        appFreeList();
        appFreeLegacyConfig();
        appAutoListInitialized = 0;
        appPendingSourceMask = 0;
        appInitialScanState = APP_INITIAL_SCAN_COMPLETE;
    }
}

static item_list_t appItemList = {
    APP_MODE, -1, 0, MODE_FLAG_NO_COMPAT | MODE_FLAG_NO_UPDATE, MENU_MIN_INACTIVE_FRAMES, APP_MODE_UPDATE_DELAY, NULL, NULL, &appGetTextId, NULL, &appInit, &appNeedsUpdate, &appUpdateItemList,
    &appGetItemCount, NULL, &appGetItemName, &appGetItemNameLength, &appGetItemStartup, &appDeleteItem, &appRenameItem, &appLaunchItem,
    &appGetConfig, &appGetImage, &appCleanUp, &appShutdown, NULL, &appGetIconId};
