/* SPDX-License-Identifier: GPL-3.0-only */

.macro x86_EnterRealMode
    .code32
    .byte 0x66                 /* 16-bit operand size prefix for far jump */
    ljmp $0x18, $(1f - CoreFsBase)  /* 1 - jump to 16-bit protected mode segment */

1:
    .code16
    /* 2 - disable protected mode bit in cr0 */
    movl %cr0, %eax
    andb $0xFE, %al
    movl %eax, %cr0

    /* 3 - jump to real mode */
    ljmp $0x00, $(2f - CoreFsBase)

2:
    /* 4 - setup segments */
    movw $0, %ax
    movw %ax, %ds
    movw %ax, %ss

    /* 5 - enable interrupts */
    sti
.endm


.macro x86_EnterProtectedMode
    cli

    /* 4 - set protection enable flag in CR0 */
    movl %cr0, %eax
    orb $1, %al
    movl %eax, %cr0

    /* 5 - far jump into protected mode */
    ljmp $0x08, $(1f - CoreFsBase)


1:
    /* we are now in protected mode! */
    .code32
    
    /* 6 - setup segment registers */
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %ss
.endm

/* Convert linear address to segment:offset address */
/* Args: */
/*    1 - linear address */
/*    2 - (out) target segment (e.g. es) */
/*    3 - target 32-bit register to use (e.g. eax) */
/*    4 - target lower 16-bit half of #3 (e.g. ax) */
.macro LinearToSegOffset src seg reg reg16
    mov \src, \reg             /* linear address to register */
    shr $4, \reg
    mov \reg16, \seg
    mov \src, \reg             /* linear address to register */
    and $0xf, \reg
.endm

    .balign 16
CoreFsBase = .

.globl x86_Disk_GetDriveParams
x86_Disk_GetDriveParams:
    .code32

    /* make new call frame */
    pushl %ebp                 /* save old call frame */
    movl %esp, %ebp            /* initialize new call frame */

    x86_EnterRealMode

    .code16

    /* save regs */
    push %es
    push %bx
    push %esi
    push %di

    /* call int13h */
    mov 8(%bp), %dl            /* dl - disk drive */
    movb $0x08, %ah
    movw $0, %di
    movw %di, %es
    stc
    int $0x13

    /* out params */
    movl $1, %eax
    sbbl $0, %eax

    /* drive type from bl */
    LinearToSegOffset 12(%bp), %es, %esi, %si
    movb %bl, %es:(%si)

    /* cylinders */
    movb %ch, %bl              /* cylinders - lower bits in ch */
    movb %cl, %bh              /* cylinders - upper bits in cl (6-7) */
    shrb $6, %bh
    incw %bx

    LinearToSegOffset 16(%bp), %es, %esi, %si
    movw %bx, %es:(%si)

    /* sectors */
    xorb %ch, %ch              /* sectors - lower 6 bits in cl */
    andb $0x3F, %cl
    
    LinearToSegOffset 20(%bp), %es, %esi, %si
    movw %cx, %es:(%si)

    /* heads */
    movb %dh, %cl              /* heads - dh */
    incw %cx

    LinearToSegOffset 24(%bp), %es, %esi, %si
    movw %cx, %es:(%si)

    /* restore regs */
    pop %di
    pop %esi
    pop %bx
    pop %es

    /* return */
    push %eax

    x86_EnterProtectedMode

    .code32

    popl %eax

    /* restore old call frame */
    movl %ebp, %esp
    popl %ebp
    ret


.globl x86_Disk_Reset
x86_Disk_Reset:
    .code32

    /* make new call frame */
    pushl %ebp                 /* save old call frame */
    movl %esp, %ebp            /* initialize new call frame */

    x86_EnterRealMode

    movb $0, %ah
    mov 8(%bp), %dl            /* dl - drive */
    stc
    int $0x13

    movl $1, %eax
    sbbl $0, %eax              /* 1 on success, 0 on fail */

    push %eax

    x86_EnterProtectedMode

    popl %eax

    /* restore old call frame */
    movl %ebp, %esp
    popl %ebp
    ret


.globl x86_Disk_Read
x86_Disk_Read:
    .code32

    /* make new call frame */
    pushl %ebp                 /* save old call frame */
    movl %esp, %ebp            /* initialize new call frame */

    x86_EnterRealMode

    .code16

    /* save modified regs */
    push %bx
    push %es

    /* setup args */
    mov 8(%bp), %dl            /* dl - drive */

    mov 12(%bp), %ch           /* ch - cylinder (lower 8 bits) */
    mov 13(%bp), %cl           /* cl - cylinder to bits 6-7 */
    shlb $6, %cl
    
    mov 16(%bp), %al           /* al - sector to bits 0-5 */
    andb $0x3F, %al
    orb %al, %cl

    mov 20(%bp), %dh           /* dh - head */

    mov 24(%bp), %al           /* al - count */

    LinearToSegOffset 28(%bp), %es, %ebx, %bx

    /* call int13h */
    movb $0x02, %ah
    stc
    int $0x13

    /* set return value */
    movl $1, %eax
    sbbl $0, %eax              /* 1 on success, 0 on fail */

    /* restore regs */
    pop %es
    pop %bx

    push %eax

    x86_EnterProtectedMode

    popl %eax

    /* restore old call frame */
    movl %ebp, %esp
    popl %ebp
    ret
