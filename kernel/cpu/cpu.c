// SPDX-License-Identifier: GPL-3.0-only

#include <cpu/process.h>
#include <cpu/scheduler.h>

void CPU_Initialize(void)
{
   Scheduler_Initialize();
}
