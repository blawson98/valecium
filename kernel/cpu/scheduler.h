// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <cpu/process.h>

void Scheduler_Initialize(void);

void Scheduler_RegisterProcess(Process *process);
void Scheduler_UnregisterProcess(Process *process);

void Scheduler_Schedule(void);

void Scheduler_SetProcessState(void);
void Scheduler_GetNextRunnableProcess(void);
uint32_t Scheduler_GetProcessCount(void);
Process *Scheduler_GetProcessAt(uint32_t index);

#endif