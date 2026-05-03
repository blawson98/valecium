/* SPDX-License-Identifier: GPL-3.0-only */

.code16
.section .text, "ax"
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

	/* Display the Multiboot2 Boot Information we collected. */
	movl	$0xb8000, %edi          /* VGA text-mode buffer start  */
	call	clear_vga_buffer
	call	display_mbi_info

hang:
	hlt
	jmp hang

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

	/* Print entry fields at EDX */
	pushl	%ebx
	pushl	%eax
	pushl	%ecx
	pushl	%edx

	movl	$msg_mmap_base, %esi
	call	print_string_vga
	movl	4(%edx), %eax
	call	print_hex32_vga
	movl	(%edx), %eax
	call	print_hex32_vga
	call	print_newline_vga

	movl	$msg_mmap_len, %esi
	call	print_string_vga
	movl	12(%edx), %eax
	call	print_hex32_vga
	movl	8(%edx), %eax
	call	print_hex32_vga
	call	print_newline_vga

	movl	$msg_mmap_type, %esi
	call	print_string_vga
	movl	16(%edx), %eax
	call	print_hex32_vga
	call	print_newline_vga

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

/* ================================================================== */
/*  VGA helper routines                                                */
/* ================================================================== */

/* Clear the VGA text-mode buffer (80 × 25 = 2000 cells = 1000 dwords). */
clear_vga_buffer:
	pushal
	movl	$1000, %ecx
	xorl	%eax, %eax
	cld
	rep	stosl                   /* write 0 to each 4-byte cell   */
	popal
	movl	$0xb8000, %edi          /* reset cursor to start         */
	ret

/* Print a null-terminated string pointed to by ESI at VGA cursor EDI. */
print_string_vga:
	pushl	%eax
.Lps_loop:
	lodsb
	testb	%al, %al
	jz	.Lps_done
	movb	%al, (%edi)
	movb	$0x0a, 1(%edi)
	addl	$2, %edi
	jmp	.Lps_loop
.Lps_done:
	popl	%eax
	ret

/* Print EAX as 8 hex digits at VGA cursor EDI. */
print_hex32_vga:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	movl	$8, %ecx
.Lhex_loop:
	movl	%eax, %ebx
	shrl	$28, %ebx
	andb	$0x0F, %bl
	cmpb	$10, %bl
	jl	.Lhex_digit
	addb	$('A' - 10), %bl
	jmp	.Lhex_store
.Lhex_digit:
	addb	$'0', %bl
.Lhex_store:
	movb	%bl, (%edi)
	movb	$0x0a, 1(%edi)
	addl	$2, %edi
	shll	$4, %eax
	decl	%ecx
	jnz	.Lhex_loop
	popl	%ecx
	popl	%ebx
	popl	%eax
	ret

/* Print EAX as an unsigned decimal number at VGA cursor EDI. */
print_dec32_vga:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	pushl	%edx
	movl	$10, %ebx
	xorl	%ecx, %ecx              /* digit count     */

	/* Special case for 0 */
	testl	%eax, %eax
	jnz	.Ldec_divide
	movb	$'0', (%edi)
	movb	$0x0a, 1(%edi)
	addl	$2, %edi
	jmp	.Ldec_done

.Ldec_divide:
	/* Divide repeatedly, push remainders onto the stack. */
.Ldec_loop:
	xorl	%edx, %edx
	divl	%ebx
	pushl	%edx                    /* remainder       */
	incl	%ecx
	testl	%eax, %eax
	jnz	.Ldec_loop

	/* Pop remainders and write to VGA. */
.Ldec_write:
	popl	%eax
	addb	$'0', %al
	movb	%al, (%edi)
	movb	$0x0a, 1(%edi)
	addl	$2, %edi
	decl	%ecx
	jnz	.Ldec_write

.Ldec_done:
	popl	%edx
	popl	%ecx
	popl	%ebx
	popl	%eax
	ret

/* Advance EDI to the start of the next VGA text-mode line. */
print_newline_vga:
	pushl	%eax
	pushl	%ebx
	pushl	%edx
	subl	$0xb8000, %edi
	movl	$160, %ebx
	movl	%edi, %eax
	xorl	%edx, %edx
	divl	%ebx                    /* EAX = current line */
	incl	%eax                    /* next line          */
	mull	%ebx                    /* EAX = byte offset  */
	addl	$0xb8000, %eax
	movl	%eax, %edi
	popl	%edx
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
