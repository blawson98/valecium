// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <stddef.h>
#include <stdint.h>

/* Global error codes (negative errno convention) */
#define SUCCESS   0
#define EINVAL  (-22)
#define ENODEV  (-19)

extern void outb(uint16_t port, uint8_t val);
extern uint8_t inb(uint16_t port);

/* ================================================================== */
/*  VGA text-mode driver                                               */
/* ================================================================== */

/* Default color: light green (0x0A) on black */
#define VGATEXT_DEFAULT_COLOR 0x0A

/* VGA text-mode dimensions */
#define VGATEXT_WIDTH   80
#define VGATEXT_HEIGHT  25

/**
 * VGATEXT_Initialize  –  Set up VGA text mode state.
 *
 * Clears the VGA buffer at 0xB8000, resets the internal cursor to (0,0),
 * and sets the default attribute.  Returns 0 on success or a negative
 * error code.
 */
int VGATEXT_Initialize(void);

/**
 * VGATEXT_PutChar  –  Write one character to the VGA text-mode display.
 *
 * @c      Character to write.
 * @x      Column (0 … VGATEXT_WIDTH-1).  If negative together with @y, write
 *         at the current cursor position and advance it.
 * @y      Row (0 … VGATEXT_HEIGHT-1).   If negative together with @x, write
 *         at the current cursor position and advance it.
 * @color  Foreground/background attribute byte.
 *
 * If exactly one of @x / @y is negative (but not both), returns -EINVAL.
 *
 * Returns 0 on success, or a negative error code.
 */
int VGATEXT_PutChar(char c, int x, int y, char color);

/* ================================================================== */
/*  Serial port driver (serial.c)                                      */
/* ================================================================== */

/**
 * Serial_Initialize  –  Set up COM1 serial port (115200, 8N1).
 *
 * Returns 0 on success or a negative error code.
 */
int Serial_Initialize(void);

/**
 * Serial_PutChar  –  Send one character over the serial port.
 *
 * @c      Character to transmit.
 * @x, @y Ignored (serial is a stream).
 * @color Ignored.
 *
 * Returns 0 on success, or a negative error code.
 */
int Serial_PutChar(char c, int x, int y, char color);

/* ================================================================== */
/*  Character / string / number output (print.c)                       */
/* ================================================================== */

/** Write a single character. */
void putc(char c);

/** Write a null-terminated string. */
void puts(const char *str);

/** Write a signed int as decimal. */
void puti(int val);

/** Write a signed int as decimal (alias for puti). */
void putd(int val);

/** Write a signed long as decimal. */
void putl(long val);

/** Write a signed long long as decimal. */
void putll(long long val);

/** Write an unsigned int as decimal. */
void putu(unsigned val);

/** Write an unsigned long as decimal. */
void putul(unsigned long val);

/** Write an unsigned long long as decimal. */
void putull(unsigned long long val);

/** Write an unsigned long long as lowercase hex. */
void putx(unsigned long long val);

/** Write an unsigned long long as UPPERCASE hex. */
void putX(unsigned long long val);

/** Write an unsigned long long as octal. */
void puto(unsigned long long val);

/** Write an unsigned long long as binary. */
void putb(unsigned long long val);

/** Write a pointer as 0x-prefixed hex. */
void putp(const void *ptr);
