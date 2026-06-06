// SPDX-License-Identifier: GPL-3.0-only

#include "logo_gen.h"

void LOGO_GetValecium(uint32_t *width, uint32_t *height,
                       const uint8_t **palette, uint32_t *palette_size,
                       const uint8_t **data)
{
   *width = VALECIUM_LOGO_W;
   *height = VALECIUM_LOGO_H;
   *palette = g_ValeciumLogo_PaletteRGB;
   *palette_size = VALECIUM_LOGO_PALETTE_SIZE;
   *data = g_ValeciumLogo_Data4bpp;
}
