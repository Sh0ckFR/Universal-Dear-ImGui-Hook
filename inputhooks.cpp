#include "stdafx.h"

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
		switch (uMsg)
		{
		case WM_LBUTTONDOWN:
			GetIO().MouseDown[0] = true; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_LBUTTONUP:
			GetIO().MouseDown[0] = false; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_RBUTTONDOWN:
			GetIO().MouseDown[1] = true; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_RBUTTONUP:
			GetIO().MouseDown[1] = false; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_MBUTTONDOWN:
			GetIO().MouseDown[2] = true; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_MBUTTONUP:
			GetIO().MouseDown[2] = false; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_MOUSEWHEEL:
			GetIO().MouseWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? +1.0f : -1.0f; return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		case WM_MOUSEMOVE:
			GetIO().MousePos.x = (signed short)(lParam); GetIO().MousePos.y = (signed short)(lParam >> 16); return DefWindowProc(hwnd, uMsg, wParam, lParam);
			break;
		}

		return CallWindowProc(oWndProc, hwnd, uMsg, wParam, lParam);
	}
}