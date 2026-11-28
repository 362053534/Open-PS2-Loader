#include <stdio.h>
#include <sysclib.h>
#include <loadcore.h>
#include <ioman.h>
#include <sysmem.h>

/* IOP 的 stdio 一般已带 O_RDONLY，这里兜底避免依赖具体 ps2sdk 版本。 */
#ifndef O_RDONLY
#define O_RDONLY 0x0001
#endif

#define MODNAME "imgdrv"
IRX_ID(MODNAME, 1, 2);

/* IOPRP 镜像的 ROMDIR 项，与 EE 侧 src/ioprp.c 的定义保持一致。 */
struct romdir_entry
{
    char fileName[10];
    unsigned short int extinfo_size;
    unsigned int fileSize;
};

#define MAX_ROMDIR_ENTRIES 32
#define MAX_PATCH_ENTRIES  8

/* EE 在加载本模块前会把这些哨兵改写成真实地址/数值。 */
unsigned int ioprpimg = 0xDEC1DEC1; /* OPL 补丁镜像，只含 CDVDMAN/CDVDFSV/EESYNC */
int ioprpsiz = 0xDEC2DEC2;
/* 方案 B：游戏自带 IOPRP 在设备上的完整路径（"<设备>/<游戏.iso>/MODULES/IOPRPxxx.IMG"）。
 * 仍是哨兵值表示未提供，此时保持旧行为（直接把补丁镜像交给 UDNL）。 */
unsigned int ioprpsrc = 0xDEC3DEC3;

const char name[] = "host";

static int read_pos;
static unsigned char *image;
static int image_size;
/* 合并镜像是本模块分配的，需要在下次加载时释放，避免每次重启都漏一份。 */
static int image_owned;

static struct romdir_entry game_dir[MAX_ROMDIR_ENTRIES];
static struct romdir_entry patch_dir[MAX_PATCH_ENTRIES];
static unsigned char dir_buffer[MAX_ROMDIR_ENTRIES * sizeof(struct romdir_entry)];

static int dummy_fs()
{
    return 0;
}

static int open_fs(iop_file_t *fd, const char *filename, int flags)
{
    read_pos = 0;
    return 0;
}

static int lseek_fs(iop_file_t *fd, int offset, int whence)
{
    switch (whence) {
        case SEEK_SET:
            read_pos = offset;
            break;
        case SEEK_CUR:
            read_pos += offset;
            break;
        default:
            read_pos = image_size + offset;
            break;
    }

    if (read_pos < 0)
        read_pos = 0;
    else if (read_pos > image_size)
        read_pos = image_size;

    return read_pos;
}

static int read_fs(iop_file_t *fd, void *buffer, int size)
{
    int avail = image_size - read_pos;

    if (size > avail)
        size = avail;
    if (size <= 0)
        return 0;

    memcpy(buffer, image + read_pos, size);
    read_pos += size;

    return size;
}

static int close_fs()
{
    DelDrv(name);
    return 0;
}

typedef struct _iop_device_tm
{
    const char *name;
    unsigned int type;
    unsigned int version; /* Not so sure about this one.  */
    const char *desc;
    struct _iop_device_ops_tm *ops;
} iop_device_tm_t;

typedef struct _iop_device_ops_tm
{
    int (*init)(iop_device_t *);
    int (*deinit)(iop_device_t *);
    int (*format)(iop_file_t *);
    int (*open)(iop_file_t *, const char *, int);
    int (*close)(iop_file_t *);
    int (*read)(iop_file_t *, void *, int);
    int (*write)(iop_file_t *, void *, int);
    int (*lseek)(iop_file_t *, unsigned long, int);
} iop_device_ops_tm_t;

iop_device_ops_t my_device_ops =
    {
        dummy_fs, // init
        dummy_fs, // deinit
        NULL,     // dummy_fs,// format
        open_fs,  // open
        close_fs, // close
        read_fs,  // read
        NULL,     // dummy_fs,// write
        lseek_fs, // lseek
};

iop_device_t my_device = {
    name,
    IOP_DT_FS,
    1,
    name,
    &my_device_ops};

/* 解析 ROMDIR。返回条目个数（不含结尾的全零项），失败返回负数。 */
static int romdir_parse(const unsigned char *base, int limit, struct romdir_entry *out, int max)
{
    const struct romdir_entry *entry = (const struct romdir_entry *)base;
    int i;

    for (i = 0; i < max; i++) {
        if ((i + 1) * (int)sizeof(struct romdir_entry) > limit)
            return -1;
        if (entry[i].fileName[0] == '\0')
            return i;
        out[i] = entry[i];
    }

    return -1;
}

/* 数据区偏移按“前面所有项大小累加并 16 字节对齐”计算，与 src/ioprp.c 里的布局一致。 */
static int romdir_find(const struct romdir_entry *dir, int count, const char *fileName, unsigned int *data_offset, unsigned int *file_size)
{
    unsigned int offset = 0;
    int i;

    for (i = 0; i < count; i++) {
        if (!strcmp(dir[i].fileName, fileName)) {
            if (data_offset != NULL)
                *data_offset = offset;
            if (file_size != NULL)
                *file_size = dir[i].fileSize;
            return 1;
        }

        offset += (dir[i].fileSize + 0xF) & ~0xF;
    }

    return 0;
}

/*
 * 方案 B 的核心：从设备上读出游戏自带的 IOPRP，把 OPL 补丁镜像里的同名模块
 * （CDVDMAN/CDVDFSV/EESYNC）顶替进去，重建一份完整镜像供 UDNL 加载。
 * 这样 IOP 里只存在一套基础模块；替换后大小变化，所以不能原地改，要重排数据区。
 * 任何一步失败都保留原有行为（只交补丁镜像），不破坏无人值守时的可用性。
 */
static void load_merged_image(void)
{
    struct romdir_entry out_dir[MAX_ROMDIR_ENTRIES];
    unsigned char *merged;
    unsigned int repl_off, repl_size, new_size, old_size;
    int fd, count, patch_count, i, n, src_off, dst_off, total;

    if (ioprpsrc == 0xDEC3DEC3)
        return;

    if ((fd = open((char *)ioprpsrc, O_RDONLY)) < 0)
        return;

    if ((n = read(fd, dir_buffer, sizeof(dir_buffer))) < (int)sizeof(struct romdir_entry) ||
        (count = romdir_parse(dir_buffer, n, game_dir, MAX_ROMDIR_ENTRIES)) <= 1 ||
        (patch_count = romdir_parse((const unsigned char *)ioprpimg, ioprpsiz, patch_dir, MAX_PATCH_ENTRIES)) <= 0) {
        close(fd);
        return;
    }

    total = 0;
    for (i = 0; i < count; i++) {
        new_size = romdir_find(patch_dir, patch_count, game_dir[i].fileName, NULL, &repl_size) ? repl_size : game_dir[i].fileSize;
        total += (new_size + 0xF) & ~0xF;
    }
    if (total <= 0) {
        close(fd);
        return;
    }

    if ((merged = (unsigned char *)AllocSysMemory(0, total, NULL)) == NULL) {
        close(fd);
        return;
    }
    memset(merged, 0, total);

    src_off = 0;
    dst_off = 0;
    for (i = 0; i < count; i++) {
        old_size = game_dir[i].fileSize;

        if (romdir_find(patch_dir, patch_count, game_dir[i].fileName, &repl_off, &repl_size)) {
            memcpy(&merged[dst_off], &((unsigned char *)ioprpimg)[repl_off], repl_size);
            new_size = repl_size;
        } else {
            new_size = old_size;
            if (old_size > 0 &&
                (lseek(fd, src_off, SEEK_SET) != src_off || read(fd, &merged[dst_off], (int)old_size) != (int)old_size)) {
                FreeSysMemory(merged);
                close(fd);
                return;
            }
        }

        out_dir[i] = game_dir[i];
        out_dir[i].fileSize = new_size;
        src_off += (old_size + 0xF) & ~0xF;
        dst_off += (new_size + 0xF) & ~0xF;
    }

    /* 目录最后写，避免被上面按源镜像拷进来的旧目录覆盖。 */
    memset(&out_dir[count], 0, sizeof(struct romdir_entry));
    memcpy(merged, out_dir, (count + 1) * sizeof(struct romdir_entry));

    close(fd);

    if (image_owned && image != NULL)
        FreeSysMemory(image);

    image = merged;
    image_size = total;
    image_owned = 1;
}

int _start(int argc, char **argv)
{
    if (image_owned && image != NULL) {
        FreeSysMemory(image);
        image_owned = 0;
    }

    image = (unsigned char *)ioprpimg;
    image_size = ioprpsiz;
    read_pos = 0;

    load_merged_image();

    DelDrv(name);
    AddDrv((iop_device_t *)&my_device);

    return MODULE_RESIDENT_END;
}
