#include "stdafx.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
using namespace ImGui;

namespace inputhook {
	WNDPROC	oWndProc;

	void Init(HWND hWindow)
	{
		oWndProc = (WNDPROC)SetWindowLongPtr(hWindow, GWLP_WNDPROC, (__int3264)(LONG_PTR)WndProc);
	}

	void Remove(HWND hWindow)
	{
		SetWindowLongPtr(hWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
	}

	LRESULT APIENTRY WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (menu::isOpen) {
			ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);
			return true;
		}
		return CallWindowProc(oWndProc, hwnd, uMsg, wParam, lParam);
	}
}