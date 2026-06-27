// SPDX-License-Identifier: GPL-3.0-only
#pragma once

void *DL_LoadLibrary(void *fileData);
void *DL_LoadSymbol(void *handle, const char *symbol);
