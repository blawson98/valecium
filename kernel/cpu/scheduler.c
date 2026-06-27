// SPDX-License-Identifier: GPL-3.0-only

#include "scheduler.h"
#include <std/stdio.h>

#define SCHED_MAX_PROCESSES 128

static Process *s_Processes[SCHED_MAX_PROCESSES];
static uint32_t s_ProcessCount = 0;
static uint32_t s_LastIndex = 0;
static Process *s_NextRunnable = NULL;

void Scheduler_Initialize(void)
{
   for (uint32_t i = 0; i < SCHED_MAX_PROCESSES; ++i)
   {
      s_Processes[i] = NULL;
   }

   s_ProcessCount = 0;
   s_LastIndex = 0;
   s_NextRunnable = NULL;
}

void Scheduler_RegisterProcess(Process *process)
{
   if (!process) return;

   for (uint32_t i = 0; i < s_ProcessCount; ++i)
   {
      if (s_Processes[i] == process)
      {
         return;
      }
   }

   if (s_ProcessCount >= SCHED_MAX_PROCESSES)
   {
      logfmt(LOG_WARNING, "[SCHED] process list full, pid=%u not queued\n",
             process->pid);
      return;
   }

   s_Processes[s_ProcessCount++] = process;
}

void Scheduler_UnregisterProcess(Process *process)
{
   if (!process || s_ProcessCount == 0) return;

   for (uint32_t i = 0; i < s_ProcessCount; ++i)
   {
      if (s_Processes[i] != process) continue;

      for (uint32_t j = i; j + 1 < s_ProcessCount; ++j)
      {
         s_Processes[j] = s_Processes[j + 1];
      }

      s_Processes[s_ProcessCount - 1] = NULL;
      --s_ProcessCount;

      if (s_ProcessCount == 0)
      {
         s_LastIndex = 0;
      }
      else if (s_LastIndex >= s_ProcessCount)
      {
         s_LastIndex = 0;
      }

      return;
   }
}

void Scheduler_GetNextRunnableProcess(void)
{
   s_NextRunnable = NULL;
   if (s_ProcessCount == 0) return;

   for (uint32_t n = 0; n < s_ProcessCount; ++n)
   {
      uint32_t idx = (s_LastIndex + n) % s_ProcessCount;
      Process *candidate = s_Processes[idx];

      if (!candidate) continue;
      if (candidate->state == STATE_BLOCKED) continue;
      if (candidate->state == STATE_ZOMBIE) continue;
      if (candidate->state == STATE_WAITING) continue;

      s_NextRunnable = candidate;
      s_LastIndex = (idx + 1) % s_ProcessCount;
      return;
   }
}

void Scheduler_SetProcessState(void)
{
   Process *current = Process_GetCurrent();
   if (!current) return;

   if (current->state == STATE_RUNNING)
   {
      current->state = STATE_READY;
   }
}

void Scheduler_Schedule(void)
{
   Scheduler_SetProcessState();
   Scheduler_GetNextRunnableProcess();

   Process *next = s_NextRunnable;
   if (!next) return;

   next->state = STATE_RUNNING;
   Process_SetCurrent(next);
}

uint32_t Scheduler_GetProcessCount(void) { return s_ProcessCount; }

Process *Scheduler_GetProcessAt(uint32_t index)
{
   if (index >= s_ProcessCount) return NULL;
   return s_Processes[index];
}