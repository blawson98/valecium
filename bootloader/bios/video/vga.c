// SPDX-License-Identifier: GPL-3.0-only

#include "font.h"
#include "video.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  VGA Mode 0x13 constants                                            */
/* ------------------------------------------------------------------ */

#define VGA_FB ((volatile uint8_t *)0xA0000)
#define VGA_WIDTH 320
#define VGA_HEIGHT 200

/* VGA I/O ports */
#define VGA_MISC_OUT 0x3C2
#define VGA_SEQ_IDX 0x3C4
#define VGA_SEQ_DATA 0x3C5
#define VGA_CRTC_IDX 0x3D4
#define VGA_CRTC_DATA 0x3D5
#define VGA_GC_IDX 0x3CE
#define VGA_GC_DATA 0x3CF
#define VGA_AC_IDX 0x3C0
#define VGA_AC_DATA 0x3C1
#define VGA_INSTAT_1 0x3DA

/* ------------------------------------------------------------------ */
/*  Internal state                                                     */
/* ------------------------------------------------------------------ */

static int s_Initialized = 0;
static int s_CursorX = 0;
static int s_CursorY = 0;

/* ------------------------------------------------------------------ */
/*  VGA register helpers                                               */
/* ------------------------------------------------------------------ */

static inline void seq_w(uint8_t idx, uint8_t val)
{
   outb(VGA_SEQ_IDX, idx);
   outb(VGA_SEQ_DATA, val);
}

static inline void crtc_w(uint8_t idx, uint8_t val)
{
   outb(VGA_CRTC_IDX, idx);
   outb(VGA_CRTC_DATA, val);
}

static inline void gc_w(uint8_t idx, uint8_t val)
{
   outb(VGA_GC_IDX, idx);
   outb(VGA_GC_DATA, val);
}

/* ------------------------------------------------------------------ */
/*  Set VGA mode 0x13 (320×200, 256 colours) via register programming  */
/* ------------------------------------------------------------------ */

static void set_mode_0x13(void)
{
   /* Miscellaneous Output */
   outb(VGA_MISC_OUT, 0x63);

   /* Sequencer */
   seq_w(0x00, 0x01); /* synchronous reset */
   seq_w(0x01, 0x01); /* clocking mode */
   seq_w(0x02, 0x0F); /* map mask — enable all 4 planes */
   seq_w(0x03, 0x00); /* character map select */
   seq_w(0x04, 0x0E); /* memory mode — chain 4, extended */

   /* Unlock CRTC registers (clear protection bit) */
   crtc_w(0x11, 0x0E);

   /* CRT Controller */
   crtc_w(0x00, 0x5F); /* horizontal total */
   crtc_w(0x01, 0x4F); /* horizontal display end */
   crtc_w(0x02, 0x50); /* start horizontal blank */
   crtc_w(0x03, 0x82); /* end horizontal blank */
   crtc_w(0x04, 0x54); /* start horizontal sync */
   crtc_w(0x05, 0x80); /* end horizontal sync */
   crtc_w(0x06, 0xBF); /* vertical total */
   crtc_w(0x07, 0x1F); /* overflow */
   crtc_w(0x08, 0x00); /* preset row scan */
   crtc_w(0x09, 0x41); /* maximum scan line */
   crtc_w(0x0A, 0x00); /* cursor start */
   crtc_w(0x0B, 0x00); /* cursor end */
   crtc_w(0x0C, 0x00); /* start address high */
   crtc_w(0x0D, 0x00); /* start address low */
   crtc_w(0x0E, 0x00); /* cursor location high */
   crtc_w(0x0F, 0x00); /* cursor location low */
   crtc_w(0x10, 0x9C); /* vertical retrace start */
   crtc_w(0x11, 0x8E); /* vertical retrace end + re-protect CR0-7 */
   crtc_w(0x12, 0x8F); /* vertical display end */
   crtc_w(0x13, 0x28); /* offset — logical line width (320/8 * 4) */
   crtc_w(0x14, 0x40); /* underline location */
   crtc_w(0x15, 0x96); /* start vertical blank */
   crtc_w(0x16, 0xB9); /* end vertical blank */
   crtc_w(0x17, 0xA3); /* mode control */
   crtc_w(0x18, 0xFF); /* line compare */

   /* Graphics Controller */
   gc_w(0x00, 0x00); /* set/reset */
   gc_w(0x01, 0x00); /* enable set/reset */
   gc_w(0x02, 0x00); /* colour compare */
   gc_w(0x03, 0x00); /* data rotate */
   gc_w(0x04, 0x00); /* read map select */
   gc_w(0x05, 0x40); /* mode — 256-colour / chain-4 */
   gc_w(0x06, 0x05); /* miscellaneous — graphics mode */
   gc_w(0x07, 0x0F); /* colour don't care */
   gc_w(0x08, 0xFF); /* bit mask */

   /* Attribute Controller */
   /* Reset flip-flop by reading Input Status 1, then write both
    * index and data through the same port 0x3C0 (the flip-flop
    * toggles between index and data on each write). */
   inb(VGA_INSTAT_1);

   outb(VGA_AC_IDX, 0x10); /* index: mode control — graphics */
   outb(VGA_AC_IDX, 0x41); /* data:  256-colour graphics      */
   outb(VGA_AC_IDX, 0x12); /* index: plane enable            */
   outb(VGA_AC_IDX, 0x0F); /* data:  enable all planes       */
   outb(VGA_AC_IDX, 0x13); /* index: horizontal pixel panning */
   outb(VGA_AC_IDX, 0x00); /* data:  no panning              */
   outb(VGA_AC_IDX, 0x14); /* index: colour select            */
   outb(VGA_AC_IDX, 0x00); /* data:  no colour select         */

   /* Enable video output (write index 0x00 with bit 5 set). */
   outb(VGA_AC_IDX, 0x20);

   /* Re-enable sequencer */
   seq_w(0x00, 0x03);
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static inline void put_pixel(int x, int y, uint8_t colour)
{
   if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return;
   VGA_FB[y * VGA_WIDTH + x] = colour;
}

static void draw_glyph(uint8_t c, int x, int y, uint8_t fg)
{
   const uint8_t *glyph;
   int row, col;

   if (c < FONT_FIRST || c > FONT_LAST) c = '?';
   glyph = g_Font8x16[c - FONT_FIRST];

   for (row = 0; row < FONT_HEIGHT; row++)
   {
      uint8_t bits = glyph[row];
      for (col = 0; col < FONT_WIDTH; col++)
      {
         if (bits & (0x80 >> col)) put_pixel(x + col, y + row, fg);
      }
   }
}

static void clear_screen(uint8_t colour)
{
   int i;
   for (i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) VGA_FB[i] = colour;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int VGA_Initialize(void)
{
   set_mode_0x13();
   clear_screen(0); /* black background */
   s_CursorX = 0;
   s_CursorY = 0;
   s_Initialized = 1;
   return SUCCESS;
}

int VGA_PutChar(char c, int x, int y, char color)
{
   if (!s_Initialized) return ENODEV;

   int is_stream = 0;

   /* 1. Resolve Stream Mode Coordinates */
   if (x < 0 && y < 0)
   {
      is_stream = 1;
      x = s_CursorX;
      y = s_CursorY;
   }
   else if ((x < 0) != (y < 0))
      return EINVAL;

   /* 2. Process Control Characters & Line Wrapping */
   switch (c)
   {
   case '\n':
      x = 0;
      y += FONT_HEIGHT;
      break;
   case '\r':
      x = 0;
      break;
   case '\t':
      x = (x / (FONT_WIDTH * 4) + 1) * (FONT_WIDTH * 4);
      if (x >= VGA_WIDTH)
      {
         x = 0;
         y += FONT_HEIGHT;
      }
      break;
   default:
      if (x + FONT_WIDTH > VGA_WIDTH)
      {
         x = 0;
         y += FONT_HEIGHT;
      }
      break;
   }

   /* 3. Perform Scrolling (BEFORE Drawing) */
   while (y + FONT_HEIGHT > VGA_HEIGHT)
   {
      uint32_t scroll_pixels = FONT_HEIGHT;
      uint32_t copy_bytes = VGA_WIDTH * (VGA_HEIGHT - scroll_pixels);

      /* volatile prevents the compiler from optimizing away the memory copy */
      volatile uint8_t *dst = VGA_FB;
      const volatile uint8_t *src = VGA_FB + VGA_WIDTH * scroll_pixels;

      for (uint32_t i = 0; i < copy_bytes; i++) dst[i] = src[i];
      for (uint32_t i = 0; i < VGA_WIDTH * scroll_pixels; i++)
         dst[copy_bytes + i] = 0;

      y -= scroll_pixels;
   }

   /* 4. Draw the Glyph */
   if (c != '\n' && c != '\r' && c != '\t')
   {
      draw_glyph((uint8_t)c, x, y, (uint8_t)color);
      x += FONT_WIDTH;
   }

   /* 5. Update Global Cursor State */
   if (is_stream)
   {
      s_CursorX = x;
      s_CursorY = y;
   }

   return SUCCESS;
}

int VGA_PutPixel(int pixel, int x, int y)
{
   if (!s_Initialized) return ENODEV;

   if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) return EINVAL;

   put_pixel(x, y, (uint8_t)pixel);
   return SUCCESS;
}