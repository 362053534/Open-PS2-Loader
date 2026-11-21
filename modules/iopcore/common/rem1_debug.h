#ifndef __REM1_DEBUG_H
#define __REM1_DEBUG_H

#define REM1_DEBUG_MAGIC_0 0x314D4552u
#define REM1_DEBUG_MAGIC_1 0x59414C44u
/* 低 32 位时钟约 116 秒回绕，调试下限限制在 60 秒内。 */
#define REM1_DEBUG_MAX_MS 60000u

struct cdvdfsv_rem1_debug
{
    unsigned int magic[2];
    unsigned int min_us;
};

#endif
