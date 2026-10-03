#ifndef EE_CORE_IGS_FORMAT_H_
#define EE_CORE_IGS_FORMAT_H_

#include <tamtypes.h>

/* 必须保留单一实现，按常量位宽内联或克隆会让共用代码反而增大常驻区。 */
static void __attribute__((noinline, noclone)) u32todecstr(u32 input, char *output, u8 digits)
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
