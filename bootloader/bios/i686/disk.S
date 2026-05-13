/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * disk.S — BIOS disk I/O helpers for corefs filesystem drivers.
 *
 * Each function enters real mode via a self-contained GDT switch,
 * calls INT 13h, then returns to 32-bit protected mode.
 */

/* ------------------------------------------------------------------ */
/*  Static GDT + 16-bit thunk (low memory)                             */
/* ------------------------------------------------------------------ */
    .equ THUNK_BASE, 0x500
    .equ THUNK_PARAMS, 0x600
    .equ THUNK_STACK, 0x7c00

    .equ TP_IN_AX, 0x00
    .equ TP_IN_BX, 0x02
    .equ TP_IN_CX, 0x04
    .equ TP_IN_DX, 0x06
    .equ TP_IN_ES, 0x08
    .equ TP_OUT_AX, 0x0A
    .equ TP_OUT_BX, 0x0C
    .equ TP_OUT_CX, 0x0E
    .equ TP_OUT_DX, 0x10
    .equ TP_STATUS, 0x12

    .balign 16
disk_gdt:
    .quad 0x0000000000000000          /* 0x00  null                     */
    .quad 0x00CF9A000000FFFF          /* 0x08  32-bit code, base=0     */
    .quad 0x00CF92000000FFFF          /* 0x10  32-bit data, base=0     */
    .quad 0x00009A000000FFFF          /* 0x18  16-bit code, base=0     */
    .quad 0x000092000000FFFF          /* 0x20  16-bit data, base=0     */
disk_gdt_end:

disk_gdtr:
    .word disk_gdt_end - disk_gdt - 1
    .long disk_gdt

disk_saved_gdtr:
    .word 0                          /* saved GDTR limit               */
    .long 0                          /* saved GDTR base                */

disk_saved_esp:
    .long 0                          /* saved ESP                      */

/* ================================================================== */
/*  x86_Int13_Thunk                                                    */
/* ================================================================== */
.type x86_Int13_Thunk, @function
x86_Int13_Thunk:
    .code32

    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ds
    pushl %es

    cli
    sgdt disk_saved_gdtr
    lgdt disk_gdtr

    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    movl %esp, disk_saved_esp

    cld
    movl $thunk_start, %esi
    movl $THUNK_BASE, %edi
    movl $(thunk_end - thunk_start), %ecx
    rep movsb

    /* Enter 16-bit protected-mode thunk at 0x0000:THUNK_BASE. */
    .byte 0x66, 0xEA
    .word THUNK_BASE
    .word 0x18

pm_return:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl disk_saved_esp, %esp

    lgdt disk_saved_gdtr
    ljmp $0x08, $pm_restore
pm_restore:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es

    popl %es
    popl %ds
    popl %edi
    popl %esi
    popl %ebx
    movl %ebp, %esp
    popl %ebp
    ret

/* ================================================================== */
/*  16-bit real-mode thunk (copied to THUNK_BASE)                      */
/* ================================================================== */
thunk_start:
    .code16

thunk_pm16:
    cli
    movl %cr0, %eax
    andb $0xFE, %al
    movl %eax, %cr0
    /* Far jump to the real-mode entry within the copied thunk. */
    .byte 0xEA
    .word (THUNK_BASE + (thunk_rm - thunk_start))
    .word 0x0000

thunk_rm:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $THUNK_STACK, %sp

    movw $THUNK_PARAMS, %si
    movw TP_IN_AX(%si), %ax
    movw TP_IN_BX(%si), %bx
    movw TP_IN_CX(%si), %cx
    movw TP_IN_DX(%si), %dx
    movw TP_IN_ES(%si), %es

    int $0x13

    movw %ax, TP_OUT_AX(%si)
    movw %bx, TP_OUT_BX(%si)
    movw %cx, TP_OUT_CX(%si)
    movw %dx, TP_OUT_DX(%si)
    movb $0, TP_STATUS(%si)
    jnc 1f
    movb $1, TP_STATUS(%si)
1:
    cli
    movl %cr0, %eax
    orb $1, %al
    movl %eax, %cr0
    .byte 0x66, 0xEA
    .long pm_return
    .word 0x08

thunk_end:
    .code32

/* ================================================================== */
/*  DISK_GetDriveParams                                                */
/* ================================================================== */
.globl DISK_GetDriveParams
DISK_GetDriveParams:
    .code32

    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %edi

    movb $0x08, %ah
    movb $0x00, %al
    movw %ax, (THUNK_PARAMS + TP_IN_AX)

    movw $0x0000, (THUNK_PARAMS + TP_IN_BX)
    movw $0x0000, (THUNK_PARAMS + TP_IN_CX)

    movb 8(%ebp), %dl               /* dl = drive                      */
    movb $0x00, %dh
    movw %dx, (THUNK_PARAMS + TP_IN_DX)
    movw $0x0000, (THUNK_PARAMS + TP_IN_ES)

    call x86_Int13_Thunk

    movzwl (THUNK_PARAMS + TP_OUT_BX), %ebx
    movzwl (THUNK_PARAMS + TP_OUT_CX), %ecx
    movzwl (THUNK_PARAMS + TP_OUT_DX), %edx

    movl 12(%ebp), %edi
    movb %bl, (%edi)

    movzbl %ch, %eax
    movzbl %cl, %ebx
    shr $6, %ebx
    shl $8, %ebx
    or %eax, %ebx
    inc %ebx

    movl 16(%ebp), %edi
    movw %bx, (%edi)

    movzbl %cl, %ebx
    and $0x3F, %ebx
    movl 20(%ebp), %edi
    movw %bx, (%edi)

    movzbl %dh, %ebx
    inc %ebx
    movl 24(%ebp), %edi
    movw %bx, (%edi)

    movzbl (THUNK_PARAMS + TP_STATUS), %eax

    popl %edi
    popl %ebx
    movl %ebp, %esp
    popl %ebp
    ret

/* ================================================================== */
/*  DISK_Reset                                                         */
/* ================================================================== */
.globl DISK_Reset
DISK_Reset:
    .code32

    pushl %ebp
    movl %esp, %ebp
    movw $0x0000, (THUNK_PARAMS + TP_IN_AX)
    movw $0x0000, (THUNK_PARAMS + TP_IN_BX)
    movw $0x0000, (THUNK_PARAMS + TP_IN_CX)

    movb 8(%ebp), %dl               /* dl = drive                      */
    movb $0x00, %dh
    movw %dx, (THUNK_PARAMS + TP_IN_DX)
    movw $0x0000, (THUNK_PARAMS + TP_IN_ES)

    call x86_Int13_Thunk

    movzbl (THUNK_PARAMS + TP_STATUS), %eax

    movl %ebp, %esp
    popl %ebp
    ret

/* ================================================================== */
/*  DISK_Read                                                          */
/* ================================================================== */
.globl DISK_Read
DISK_Read:
    .code32

    pushl %ebp
    movl %esp, %ebp
    pushl %ebx

    movb 24(%ebp), %al             /* al = count                      */
    movb $0x02, %ah
    movw %ax, (THUNK_PARAMS + TP_IN_AX)

    xorl %ecx, %ecx
    movw 12(%ebp), %ax             /* cylinder                        */
    movb %al, %ch
    movb 16(%ebp), %cl             /* sector                          */
    andb $0x3F, %cl
    movb %ah, %bl
    andb $0x03, %bl
    shlb $6, %bl
    orb %bl, %cl
    movw %cx, (THUNK_PARAMS + TP_IN_CX)

    movb 8(%ebp), %dl              /* dl = drive                      */
    movb 20(%ebp), %dh             /* dh = head                       */
    movw %dx, (THUNK_PARAMS + TP_IN_DX)

    movl 28(%ebp), %eax            /* buffer linear addr              */
    movl %eax, %ebx
    andl $0xF, %ebx                /* offset                          */
    shrl $4, %eax                  /* segment                         */
    movw %bx, (THUNK_PARAMS + TP_IN_BX)
    movw %ax, (THUNK_PARAMS + TP_IN_ES)

    call x86_Int13_Thunk

    movzbl (THUNK_PARAMS + TP_STATUS), %eax

    popl %ebx
    movl %ebp, %esp
    popl %ebp
    ret
