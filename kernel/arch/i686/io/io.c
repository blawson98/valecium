// SPDX-License-Identifier: GPL-3.0-only

#include "io.h"

#define UNUSED_PORT 0x80

void i686_iowait(void) { i686_outb(UNUSED_PORT, 0); }