/* SPDX-License-Identifier: GPL-3.0-only */

.code16
.section .entry, "ax"
.global _start

_start:
	/* Save the BIOS boot-drive number (passed in DL) immediately. */
	movb	%dl, boot_drive

	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es

	/* Detect hardware and build the Multiboot2 Boot Information
	 * structure while BIOS services are still available. */
	movb	boot_drive, %dl
	call	detect_hardware

	movw	$stage2_message, %si

print_loop:
	lodsb
	testb %al, %al
	jz done
	movb $0x0e, %ah
	movb $0x00, %bh
	movb $0x0a, %bl
	int $0x10
	jmp print_loop

done:
	cli

	/* Enable A20 through System Control Port A (0x92). */
	inb $0x92, %al
	orb $0x02, %al
	andb $0xfe, %al
	outb %al, $0x92

	lgdtl gdt_descriptor

	movl %cr0, %eax
	orl $0x1, %eax
	movl %eax, %cr0

	ljmp $0x08, $protected_mode_entry

.code32
protected_mode_entry:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	movw %ax, %ss
	movl $0x9fc00, %esp

	/* Initialise VGA text mode and display MBI info via C print functions. */
	call	VGATEXT_Initialize
	call    Serial_Initialize
	call	display_mbi_info

hang:
	hlt
	jmp hang

.section .text, "ax"

/* ================================================================== */
/*  display_mbi_info  —  Walk MBI tags and print their contents.      */
/*  EBX = tag walk pointer.  EDX = mmap entry pointer.                */
/* ================================================================== */
display_mbi_info:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	pushl	%edx
	pushl	%esi
	pushl	%ebp

	movl	$MBI_BASE + 8, %ebx

.Ltag_loop:
	movl	(%ebx), %eax
	cmpl	$0, %eax
	je	.Ltag_end

	/* Sanity check tag size to avoid runaway or invalid tags. */
	movl	4(%ebx), %ecx
	cmpl	$8, %ecx
	jl	.Ltag_end
	cmpl	$0x1000, %ecx
	jg	.Ltag_end

	/* Dispatch (only print mmap entries). */
	movl	(%ebx), %eax
	cmpl	$6, %eax
	je	.Ltag_memory_map
	jmp	.Ltag_next

.Ltag_memory_map:
	movl	4(%ebx), %eax
	subl	$16, %eax
	movl	8(%ebx), %ecx
	/* Sanity check entry_size (E820 is 20 or 24 bytes). */
	cmpl	$20, %ecx
	jl	.Ltag_next
	cmpl	$64, %ecx
	jg	.Ltag_next
	testl	%ecx, %ecx
	jz	.Ltag_next
	xorl	%edx, %edx
	divl	%ecx
	movl	%eax, %ebp              /* total entry count */
	movl	%ebp, %eax
	xorl	%ecx, %ecx
	pushl	8(%ebx)                /* stack[0] = entry_size      */
	leal	16(%ebx), %edx          /* EDX = first entry          */
.Lmmap_entry_loop:
	cmpl	%eax, %ecx
	jge	.Lmmap_done

	/* Print entry fields at EDX — use C print functions */
	pushl	%ebx
	pushl	%eax
	pushl	%ecx
	pushl	%edx

	/* base = */
	pushl	$msg_mmap_base
	call	puts
	addl	$4, %esp
	/* base high 32 bits */
	pushl	4(%edx)
	pushl	(%edx)
	call	putx
	addl	$8, %esp
	/* newline */
	pushl	$10
	call	putc
	addl	$4, %esp

	/* len = */
	pushl	$msg_mmap_len
	call	puts
	addl	$4, %esp
	/* len high 32 bits, then low 32 bits */
	pushl	12(%edx)
	pushl	8(%edx)
	call	putx
	addl	$8, %esp
	/* newline */
	pushl	$10
	call	putc
	addl	$4, %esp

	/* type = */
	pushl	$msg_mmap_type
	call	puts
	addl	$4, %esp
	/* type (32-bit) */
	pushl	16(%edx)
	call	puti
	addl	$4, %esp
	/* newline */
	pushl	$10
	call	putc
	addl	$4, %esp

	popl	%edx
	popl	%ecx
	popl	%eax
	popl	%ebx

	/* EDX += entry_size (saved at top of stack) */
	addl	(%esp), %edx
	incl	%ecx
	jmp	.Lmmap_entry_loop

.Lmmap_done:
	addl	$4, %esp                /* pop saved entry_size       */
	jmp	.Ltag_next

.Ltag_next:
	movl	4(%ebx), %ecx
	leal	(%ebx, %ecx), %ebx
	addl	$7, %ebx
	andl	$0xFFFFFFF8, %ebx
	jmp	.Ltag_loop

.Ltag_end:
	popl	%ebp
	popl	%esi
	popl	%edx
	popl	%ecx
	popl	%ebx
	popl	%eax
	ret

.align 8
gdt_start:
	.quad 0x0000000000000000
	.quad 0x00cf9a000000ffff
	.quad 0x00cf92000000ffff
gdt_end:

gdt_descriptor:
	.word gdt_end - gdt_start - 1
	.long gdt_start

/* ================================================================== */
/*  Data section                                                       */
/* ================================================================== */

	.balign 4
boot_drive:
	.byte 0

	/* Multiboot2 Boot Information structure base (matches detect.S). */
	.set MBI_BASE,     0x6000
	.set MBI_PTR_SAVE, 0x5000

	/* ---- String constants ---- */
	stage2_message:
		.asciz "Valecium stage2 bootloader loaded"

msg_mmap_base:
	.asciz "base="

msg_mmap_len:
	.asciz "len ="

msg_mmap_type:
	.asciz "type="
