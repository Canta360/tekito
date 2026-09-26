#pragma once

#include <windows.h>
#include <atomic>

extern HINSTANCE g_moduleInstance;
extern std::atomic<long> g_objectCount;
extern std::atomic<long> g_serverLockCount;
