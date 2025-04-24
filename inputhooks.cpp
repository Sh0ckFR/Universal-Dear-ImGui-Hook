#include "stdafx.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace inputhook {
    static WNDPROC sOriginalWndProc = nullptr;

    void Init(HWND hWindow)
    {
        DebugLog("[inputhook] Initializing input hook for window %p\n", hWindow);
        sOriginalWndProc = (WNDPROC)SetWindowLongPtr(hWindow, GWLP_WNDPROC, (LONG_PTR)WndProc);
        if (!sOriginalWndProc) {
            DebugLog("[inputhook] Failed to set WndProc: %d\n", GetLastError());
        }
        else {
            DebugLog("[inputhook] WndProc hook set. Original WndProc=%p\n", sOriginalWndProc);
        }
    }

    void Remove(HWND hWindow)
    {
        DebugLog("[inputhook] Removing input hook for window %p\n", hWindow);
        if (SetWindowLongPtr(hWindow, GWLP_WNDPROC, (LONG_PTR)sOriginalWndProc) == 0) {
            DebugLog("[inputhook] Failed to restore WndProc: %d\n", GetLastError());
        }
        else {
            DebugLog("[inputhook] WndProc restored to %p\n", sOriginalWndProc);
        }
    }

    LRESULT APIENTRY WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        ImGuiIO& io = ImGui::GetIO();

        ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam);

        if (menu::isOpen && (io.WantCaptureMouse || io.WantCaptureKeyboard))
        {
            //DebugLog("[inputhook] Swallow msg=0x%X (WantCapture M=%d K=%d)\n", uMsg, io.WantCaptureMouse, io.WantCaptureKeyboard);
            return TRUE;
        }

        return CallWindowProc(sOriginalWndProc, hwnd, uMsg, wParam, lParam);
    }
}
