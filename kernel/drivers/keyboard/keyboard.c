// SPDX-License-Identifier: GPL-3.0-only

#include "keyboard.h"
#include <drivers/tty/tty.h>
#include <std/stdio.h>
#include <stdint.h>

/* modifier state */
static int s_Shift = 0;
static int s_Caps = 0;
static int s_Extended = 0; /* set when 0xE0 prefix received */

/* Minimal set-1 scancode -> ASCII map for printable keys.
   Extend as needed (this is not full; handles letters, digits, space, enter,
   backspace). */
static const char s_ScancodeMap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    '\b', /* 0x00 - 0x0F */
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, /* 0x10 - 0x1F */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z',
    'x', /*0x20-0x2F*/
    'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0,
    ' ', /* 0x30 - 0x3B (space at 0x39) */
    /* rest zeros */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Generic scancode handler (platform-independent).
// Processes PS/2 scancodes and forwards characters into the active TTY.
void Keyboard_HandleScancode(uint8_t scancode)
{
   /* handle key releases and modifier keys */

   /* handle 0xE0 extended prefix */
   if (scancode == 0xE0)
   {
      s_Extended = 1;
      return;
   }

   /* key release */
   if (scancode & 0x80)
   {
      /* key release: clear shift if shift released */
      uint8_t key = scancode & 0x7F;
      if (key == 0x2A || key == 0x36) s_Shift = 0; /* left/right shift */
      /* if this was an extended key release, clear extended state */
      if (s_Extended)
      {
         s_Extended = 0;
         return;
      }
      return;
   }

   /* check for shift press */
   if (scancode == 0x2A || scancode == 0x36)
   {
      s_Shift = 1;
      return;
   }

   /* caps lock toggle (make only) */
   if (scancode == 0x3A)
   {
      s_Caps = !s_Caps;
      return;
   }

   /* Emit ANSI CSI escapes for cursor keys (ESC [ A/B/C/D). */
   if (s_Extended)
   {
      TTY_Device *dev = TTY_GetDevice();
      if (dev)
      {
         switch (scancode)
         {
         case 0x48:
            TTY_InputArrow(dev, 'A');
            break;
         case 0x50:
            TTY_InputArrow(dev, 'B');
            break;
         case 0x4D:
            TTY_InputArrow(dev, 'C');
            break;
         case 0x4B:
            TTY_InputArrow(dev, 'D');
            break;
         default:
            break;
         }
      }

      s_Extended = 0;
      return;
   }

   /* map scancode to character, apply shift/caps */
   if (scancode < sizeof(s_ScancodeMap))
   {
      char base = s_ScancodeMap[scancode];
      if (!base) return;

      char out = base;
      /* simple alphabetic handling for s_Caps/s_Shift */
      if (out >= 'a' && out <= 'z')
      {
         if ((s_Caps && !s_Shift) || (s_Shift && !s_Caps))
         {
            out = out - 'a' + 'A';
         }
      }
      else
      {
         /* rudimentary shifted symbols for digits/punctuation */
         if (s_Shift)
         {
            switch (out)
            {
            case '1':
               out = '!';
               break;
            case '2':
               out = '@';
               break;
            case '3':
               out = '#';
               break;
            case '4':
               out = '$';
               break;
            case '5':
               out = '%';
               break;
            case '6':
               out = '^';
               break;
            case '7':
               out = '&';
               break;
            case '8':
               out = '*';
               break;
            case '9':
               out = '(';
               break;
            case '0':
               out = ')';
               break;
            case '-':
               out = '_';
               break;
            case '=':
               out = '+';
               break;
            case '\\':
               out = '|';
               break;
            case ';':
               out = ':';
               break;
            case '\'':
               out = '"';
               break;
            case ',':
               out = '<';
               break;
            case '.':
               out = '>';
               break;
            case '/':
               out = '?';
               break;
            case '`':
               out = '~';
               break;
            case '[':
               out = '{';
               break;
            case ']':
               out = '}';
               break;
            default:
               break;
            }
         }
      }

      /* Forward translated character to active TTY input stream. */
      TTY_Device *dev = TTY_GetDevice();
      if (dev) TTY_InputChar(dev, out);
   }
}

// Initialize keyboard state.
void Keyboard_Initialize(void)
{
   /* Reset modifier state */
   s_Shift = 0;
   s_Caps = 0;
   s_Extended = 0;

   logfmt(LOG_INFO, "[KEYBOARD] Initialized\n");
}
