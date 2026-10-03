#ifndef EE_CORE_IGS_FORMAT_H_
#define EE_CORE_IGS_FORMAT_H_

#include <tamtypes.h>

/* 截图文本共用转换实现，避免三种整数宽度的重复代码占用常驻区。 */
static void u32todecstr(u32 input, char *output, u8 digits)
{
    u8 i = digits;
    do {
        i--;
        output[i] = "0123456789"[input % 10];
        input = input / 10;
    } while (input > 0);
    while (i > 0) {
        i--;
        output[i] = '0';
    }
    output[digits] = 0;
}

/* 保留旧接口的输入截断及固定宽度补零规则，不能因共用代码而改变截图文件名。 */
#define u8todecstr(input, output, digits)  u32todecstr((u8)(input), (output), (digits))
#define u16todecstr(input, output, digits) u32todecstr((u16)(input), (output), (digits))

#endif
