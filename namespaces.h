#pragma once

namespace globals {
	extern HMODULE mainModule;
	extern HWND mainWindow;
	extern int uninjectKey;
	extern int openMenuKey;
}

namespace hooks {
	extern void Init();
}

namespace inputhook {
	extern void Init(HWND hWindow);
	extern void Remove(HWND hWindow);
	static LRESULT APIENTRY WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
}

namespace d3d12hook {
	typedef long(__fastcall* PresentD3D12) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
	extern PresentD3D12 oPresentD3D12;

	extern void(*oExecuteCommandListsD3D12)(ID3D12CommandQueue*, UINT, ID3D12CommandList*);
	extern HRESULT(*oSignalD3D12)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

	extern long __fastcall hookPresentD3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags);
	extern void hookExecuteCommandListsD3D12(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* ppCommandLists);
	extern HRESULT hookSignalD3D12(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);
	extern void release();
}

namespace menu {
	extern bool isOpen;
	extern void Init();
}
