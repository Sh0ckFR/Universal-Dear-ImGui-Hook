#include "stdafx.h"

unsigned long __stdcall onAttach()
{
	hooks::Init();
	return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
		globals::mainWindow = (HWND)FindWindow(0, "Renderer: [DirectX12], Input: [Raw input], 64 bits"); // Main window of the GFXTest Application: https://bitbucket.org/learn_more/gfxtest/src/master/ 
		globals::mainModule = hModule;
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)onAttach, hModule, 0, 0);
	}
	return 1;
}