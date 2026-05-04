// SPDX-License-Identifier: GPL-3.0-only

#include "video.h"

int VBE_Initialize(void)
{
   return SUCCESS;
}

int VBE_PutChar(char c, int x, int y, char color)
{
   (void)c;
   (void)x;
   (void)y;
   (void)color;
   return SUCCESS;
}