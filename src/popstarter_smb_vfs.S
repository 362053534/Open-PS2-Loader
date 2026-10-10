    .set noreorder
    .set noat

    .section .text.app_pops_smb_vfs, "ax", @progbits
    .align 4

    .global appPOPSSMBVFSStart
    .global appPOPSSMBVFSOpen
    .global appPOPSSMBVFSClose
    .global appPOPSSMBVFSRead
    .global appPOPSSMBVFSLseek
    .global appPOPSSMBVFSEnd

/*
 * 这段代码会从OPL当前位置复制到0x00140000附近执行。
 * 内部不得引用OPL的gp或绝对符号，只允许使用固定包地址和POPStarter入口。
 * n32中的$t0至$t3就是物理寄存器$12至$15，额外临时值必须使用$8至$11。
 */
appPOPSSMBVFSStart:

appPOPSSMBVFSOpen:
    beqz    $a0, appPOPSSMBVFSOpenDelegate
    nop
    lui     $t0, 0x0014
    lw      $t1, 0($t0)
    lui     $t3, 0x534d
    ori     $t3, $t3, 0x4256
    bne     $t1, $t3, appPOPSSMBVFSOpenDelegate
    nop
    lw      $t1, 4($t0)
    addiu   $t3, $zero, 1
    bne     $t1, $t3, appPOPSSMBVFSOpenDelegate
    nop
    lw      $t1, 8($t0)
    addiu   $t3, $zero, 8
    bne     $t1, $t3, appPOPSSMBVFSOpenDelegate
    nop

    /* 只比较最后一个路径分量，使mc0和mc1共用同一份内存文件。 */
    move    $t2, $a0
    move    $8, $a0
appPOPSSMBVFSOpenScanPath:
    lbu     $9, 0($8)
    beqz    $9, appPOPSSMBVFSOpenFind
    nop
    addiu   $10, $zero, 0x2f
    beq     $9, $10, appPOPSSMBVFSOpenSeparator
    nop
    addiu   $10, $zero, 0x5c
    beq     $9, $10, appPOPSSMBVFSOpenSeparator
    nop
    addiu   $10, $zero, 0x3a
    bne     $9, $10, appPOPSSMBVFSOpenScanNext
    nop
appPOPSSMBVFSOpenSeparator:
    addiu   $t2, $8, 1
appPOPSSMBVFSOpenScanNext:
    b       appPOPSSMBVFSOpenScanPath
    addiu   $8, $8, 1

appPOPSSMBVFSOpenFind:
    addiu   $t3, $t0, 16
    move    $t9, $zero
appPOPSSMBVFSOpenNextEntry:
    lw      $9, 0($t3)
    addu    $9, $9, $t0
    move    $8, $t2
appPOPSSMBVFSOpenCompare:
    lbu     $10, 0($8)
    lbu     $11, 0($9)
    bne     $10, $11, appPOPSSMBVFSOpenMismatch
    nop
    beqz    $10, appPOPSSMBVFSOpenMatch
    nop
    addiu   $8, $8, 1
    b       appPOPSSMBVFSOpenCompare
    addiu   $9, $9, 1

appPOPSSMBVFSOpenMismatch:
    addiu   $t9, $t9, 1
    sltiu   $t8, $t9, 8
    beqz    $t8, appPOPSSMBVFSOpenDelegate
    addiu   $t3, $t3, 16
    b       appPOPSSMBVFSOpenNextEntry
    nop

appPOPSSMBVFSOpenMatch:
    sw      $zero, 12($t3)
    addiu   $v0, $t9, 0x7001
    jr      $ra
    nop

appPOPSSMBVFSOpenDelegate:
    addiu   $sp, $sp, -128
    sd      $ra, 32($sp)
    j       0x0087c398
    nop

appPOPSSMBVFSClose:
    addiu   $t0, $a0, -0x7001
    sltiu   $t1, $t0, 8
    beqz    $t1, appPOPSSMBVFSCloseDelegate
    nop
    move    $v0, $zero
    jr      $ra
    nop

appPOPSSMBVFSCloseDelegate:
    addiu   $sp, $sp, -48
    sd      $ra, 32($sp)
    j       0x0087c3f8
    nop

appPOPSSMBVFSRead:
    addiu   $t0, $a0, -0x7001
    sltiu   $t1, $t0, 8
    beqz    $t1, appPOPSSMBVFSReadDelegate
    nop
    sll     $t1, $t0, 4
    lui     $t0, 0x0014
    addiu   $t1, $t1, 16
    addu    $t1, $t1, $t0
    lw      $t2, 12($t1)
    lw      $t3, 8($t1)
    sltu    $8, $t3, $t2
    bnez    $8, appPOPSSMBVFSReadEmpty
    nop
    beq     $t2, $t3, appPOPSSMBVFSReadEmpty
    nop
    subu    $8, $t3, $t2
    sltu    $9, $a2, $8
    beqz    $9, appPOPSSMBVFSReadSizeReady
    move    $11, $8
    move    $11, $a2
appPOPSSMBVFSReadSizeReady:
    beqz    $11, appPOPSSMBVFSReadEmpty
    nop
    lw      $9, 4($t1)
    addu    $9, $9, $t0
    addu    $9, $9, $t2
    move    $8, $a1
    move    $t3, $11
appPOPSSMBVFSReadCopy:
    lbu     $10, 0($9)
    sb      $10, 0($8)
    addiu   $9, $9, 1
    addiu   $8, $8, 1
    addiu   $t3, $t3, -1
    bnez    $t3, appPOPSSMBVFSReadCopy
    nop
    addu    $t2, $t2, $11
    sw      $t2, 12($t1)
    move    $v0, $11
    jr      $ra
    nop

appPOPSSMBVFSReadEmpty:
    move    $v0, $zero
    jr      $ra
    nop

appPOPSSMBVFSReadDelegate:
    addiu   $sp, $sp, -48
    sd      $ra, 32($sp)
    j       0x0087c428
    nop

appPOPSSMBVFSLseek:
    addiu   $t0, $a0, -0x7001
    sltiu   $t1, $t0, 8
    beqz    $t1, appPOPSSMBVFSLseekDelegate
    nop
    sll     $t1, $t0, 4
    lui     $t0, 0x0014
    addiu   $t1, $t1, 16
    addu    $t1, $t1, $t0
    beqz    $a2, appPOPSSMBVFSLseekSet
    move    $9, $a1
    addiu   $t2, $zero, 1
    beq     $a2, $t2, appPOPSSMBVFSLseekCurrent
    nop
    addiu   $t2, $zero, 2
    bne     $a2, $t2, appPOPSSMBVFSLseekError
    nop
    lw      $9, 8($t1)
    b       appPOPSSMBVFSLseekSet
    addu    $9, $9, $a1

appPOPSSMBVFSLseekCurrent:
    lw      $9, 12($t1)
    addu    $9, $9, $a1
appPOPSSMBVFSLseekSet:
    bltz    $9, appPOPSSMBVFSLseekError
    nop
    sw      $9, 12($t1)
    move    $v0, $9
    jr      $ra
    nop

appPOPSSMBVFSLseekError:
    addiu   $v0, $zero, -1
    jr      $ra
    nop

appPOPSSMBVFSLseekDelegate:
    addiu   $sp, $sp, -64
    sd      $ra, 48($sp)
    j       0x0087c468
    nop

appPOPSSMBVFSEnd:
    nop
