#include "stdafx.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"

namespace hooks {
	void Init() {
		if (kiero::init(kiero::RenderType::D3D12) == kiero::Status::Success) {
			kiero::bind(54, (void**)&d3d12hook::oExecuteCommandListsD3D12, d3d12hook::hookExecuteCommandListsD3D12);
			kiero::bind(58, (void**)&d3d12hook::oSignalD3D12, d3d12hook::hookSignalD3D12);
			kiero::bind(140, (void**)&d3d12hook::oPresentD3D12, d3d12hook::hookPresentD3D12);

			do {
				Sleep(100);
			} while (!(GetAsyncKeyState(globals::uninjectKey) & 0x1));

			d3d12hook::release();

			kiero::shutdown();

			inputhook::Remove(globals::mainWindow);

			Beep(220, 100);

			FreeLibraryAndExitThread(globals::mainModule, 0);
		}
	}
}
