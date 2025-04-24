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
	typedef HRESULT(STDMETHODCALLTYPE* PresentD3D12)(
		IDXGISwapChain3 * pSwapChain, UINT SyncInterval, UINT Flags);
	extern PresentD3D12 oPresentD3D12;

	typedef void(STDMETHODCALLTYPE* ExecuteCommandListsFn)(
		ID3D12CommandQueue * _this, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
	extern ExecuteCommandListsFn oExecuteCommandListsD3D12;

	typedef HRESULT(STDMETHODCALLTYPE* SignalFn)(
		ID3D12CommandQueue * _this, ID3D12Fence* pFence, UINT64 Value);
	extern SignalFn oSignalD3D12;

	extern long __fastcall hookPresentD3D12(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags);
	extern void STDMETHODCALLTYPE hookExecuteCommandListsD3D12(
		ID3D12CommandQueue* _this,
		UINT                          NumCommandLists,
		ID3D12CommandList* const* ppCommandLists);

	extern HRESULT STDMETHODCALLTYPE hookSignalD3D12(
		ID3D12CommandQueue* _this,
		ID3D12Fence* pFence,
		UINT64              Value);

	extern void release();
}

namespace menu {
	extern bool isOpen;
	extern void Init();
}
