#ifndef RCUYA_TEST_OSD_CONFIG_H_
#define RCUYA_TEST_OSD_CONFIG_H_

#include <tamtypes.h>

/* 只校验配置前缀的填充布局，不模拟 OSD 位域或主机与 EE 的完整 ABI。 */
typedef struct
{
    u32 words[2];
} ConfigParam;

#endif
