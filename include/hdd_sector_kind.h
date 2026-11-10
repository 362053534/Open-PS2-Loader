#ifndef HDD_SECTOR_KIND_H
#define HDD_SECTOR_KIND_H

#include <string.h>

/* apaReadHeader 认 LBA0 时核对的 32 字节，不含结尾的 NUL。 */
#define HDD_APA_SONY_MARK     "Sony Computer Entertainment Inc."
#define HDD_APA_SONY_MARK_LEN 32

typedef char hdd_apa_sony_mark_len_check[(sizeof(HDD_APA_SONY_MARK) - 1) == HDD_APA_SONY_MARK_LEN ? 1 : -1];

/* 调用方至少提供 512 字节。真 APA 必须同时有四个字节 APA\0 和 Sony 标记。 */
static inline int hddSectorIsApa(const unsigned char *sector)
{
    return memcmp(sector + 4, "APA", 4) == 0 &&
           memcmp(sector + 0x100, HDD_APA_SONY_MARK, HDD_APA_SONY_MARK_LEN) == 0;
}

/*
 * 调用方至少提供 0x208 字节（LBA0 加 LBA1 的 GPT 签名）。
 * 返回 0：APA。返回 1：MBR、exFAT 或 GPT。返回 -1：认不出，不能当 APA 去装 ps2hdd。
 * APA 身份优先于末尾的 55 AA，因为带保护性 MBR 的 APA 头也会有这个签名。
 */
static inline int hddClassifyBootSectors(const unsigned char *sectors)
{
    if (hddSectorIsApa(sectors))
        return 0;
    if (sectors[0x1FE] == 0x55 && sectors[0x1FF] == 0xAA)
        return 1;
    if (memcmp(sectors + 0x200, "EFI PART", 8) == 0)
        return 1;
    return -1;
}

#endif
