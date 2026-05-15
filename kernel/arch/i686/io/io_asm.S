	# SPDX-License-Identifier: GPL-3.0-only

.global i686_outb
i686_outb:
    movw 4(%esp), %dx
    movb 8(%esp), %al
    outb %al, %dx
    ret

.global i686_outw
i686_outw:
    movw 4(%esp), %dx
    movw 8(%esp), %ax
    outw %ax, %dx
    ret

.global i686_outl
i686_outl:
    movw 4(%esp), %dx
    movl 8(%esp), %eax
    outl %eax, %dx
    ret

.global i686_inb
i686_inb:
    movw 4(%esp), %dx
    xorl %eax, %eax
    inb %dx, %al
    ret

.global i686_inw
i686_inw:
    movw 4(%esp), %dx
    xorl %eax, %eax
    inw %dx, %ax
    ret

.global i686_inl
i686_inl:
    movw 4(%esp), %dx
    xorl %eax, %eax
    inl %dx, %eax
    ret

.global i686_EnableInterrupts
i686_EnableInterrupts:
    pushfl
    popl %eax
    andl $0x200, %eax
    shrl $9, %eax
    sti
    ret

.global i686_DisableInterrupts
i686_DisableInterrupts:
    pushfl
    popl %eax
    andl $0x200, %eax
    shrl $9, %eax
    cli
    ret

.global i686_Panic
i686_Panic:
    cli
    hlt

.global crash_me
crash_me:
	# div by 0
	# movl $0x1337, %ecx
    movl $0, %eax
    divl %eax
    ret

.global i686_Halt
i686_Halt:
    hlt
    ret

.global i686_Reboot
i686_Reboot:
    cli
    movw $0x64, %dx
1:  inb %dx, %al
    testb $2, %al
    jnz 1b
    movb $0xFE, %al
    outb %al, %dx
    jmp 2f
null_idt:
    .word 0
    .long 0
2:  lidt null_idt
    int $0x01
    ret