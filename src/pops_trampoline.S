#include <syscallnr.h>

    .set noreorder
    .text
    .align 4

    .extern appPOPSDirectSource
    .extern appPOPSDirectSize

    .globl appPOPSDirectTrampoline
    .ent appPOPSDirectTrampoline
appPOPSDirectTrampoline:
    lui     $t0, %hi(appPOPSDirectSource)
    addiu   $t0, $t0, %lo(appPOPSDirectSource)
    lw      $t1, 0($t0)
    lui     $t0, %hi(appPOPSDirectSize)
    addiu   $t0, $t0, %lo(appPOPSDirectSize)
    lw      $t2, 0($t0)
    lui     $t3, 0x0010
1:
    beq     $t2, $zero, 2f
    nop
    lbu     $t4, 0($t1)
    sb      $t4, 0($t3)
    addiu   $t1, $t1, 1
    addiu   $t3, $t3, 1
    b       1b
    addiu   $t2, $t2, -1
2:
    daddu   $a0, $zero, $zero
    addiu   $v1, $zero, __NR_FlushCache
    syscall
    addiu   $a0, $zero, 2
    addiu   $v1, $zero, __NR_FlushCache
    syscall
    lui     $t0, 0x0010
    jr      $t0
    nop
    .end appPOPSDirectTrampoline

    .globl appPOPSDirectTrampolineEnd
appPOPSDirectTrampolineEnd:
