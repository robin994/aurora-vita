#pragma once
// Host test shim for the production Vita worker protocol. This models threads
// and bounded counting semaphores, not Vita scheduling or device performance.
#include <cstddef>
#include <cstdint>
#include <psp2/kernel/cpu.h>
using SceUID=int;
using SceSize=size_t;
using SceKernelSysClock=uint64_t;
struct SceKernelSystemInfo {
  SceSize size=0;
  uint32_t activeCpuMask=0;
  struct { SceKernelSysClock idleClock=0;uint32_t comesOutOfIdleCount=0,threadSwitchCount=0; } cpuInfo[4];
};
inline constexpr int SCE_KERNEL_CPU_MASK_USER_1=2,SCE_KERNEL_CPU_MASK_USER_2=4;
SceUID sceKernelCreateSema(const char*,int,int,int,void*);
int sceKernelDeleteSema(SceUID);
int sceKernelWaitSema(SceUID,int,void*);
int sceKernelSignalSema(SceUID,int);
SceUID sceKernelCreateThread(const char*,int(*)(SceSize,void*),int,SceSize,int,int,void*);
int sceKernelStartThread(SceUID,SceSize,void*);
int sceKernelWaitThreadEnd(SceUID,void*,void*);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(unsigned);
int sceKernelGetCpuId(void);
SceUID sceKernelGetThreadId(void);
int sceKernelGetThreadCpuAffinityMask(SceUID);
int64_t sceKernelGetSystemTimeWide(void);
int sceKernelGetSystemInfo(SceKernelSystemInfo*);
