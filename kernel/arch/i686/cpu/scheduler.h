// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifndef I686_SCHEDULER_H
#define I686_SCHEDULER_H

#include <arch/i686/cpu/isr.h>

void i686_Scheduler_SaveCpuState(void);
void i686_Scheduler_RestoreCpuState(void);
Registers *i686_Scheduler_ContextSwitch_Impl(void);

void __attribute__((cdecl)) i686_Scheduler_ContextSwitch();

#endif