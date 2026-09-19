#pragma once
// Host test shim for the production Vita worker protocol. This models threads
// and bounded counting semaphores, not Vita scheduling or device performance.
#include <cstddef>
using SceUID=int;
using SceSize=size_t;
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
