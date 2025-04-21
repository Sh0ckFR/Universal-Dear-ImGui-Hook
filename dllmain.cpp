#include "stdafx.h"

unsigned long __stdcall onAttach()
{
	hooks::Init();
	return 0;
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
		globals::mainModule = hModule;
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)onAttach, hModule, 0, 0);
	}
	return 1;
}