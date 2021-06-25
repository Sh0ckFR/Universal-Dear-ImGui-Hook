#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdint>

#include <dxgi.h>
#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")

#if defined _M_X64
typedef uint64_t uintx_t;
#elif defined _M_IX86
typedef uint32_t uintx_t;
#endif

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <d3d12.h>
#include <dxgi1_4.h>

#include "d3d12hook.h"

#include "globals.h"
#include "kiero.h"
#include "inputhooks.h"
#include "hooks.h"
#include "menu.h"