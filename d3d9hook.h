#pragma once

#include <d3d9.h>

namespace d3d9hook {
    using EndSceneFn = HRESULT(__stdcall*)(IDirect3DDevice9*);
    using ResetFn    = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    extern EndSceneFn oEndScene;
    extern ResetFn    oReset;

    HRESULT __stdcall hookEndScene(IDirect3DDevice9* device);
    HRESULT __stdcall hookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);

    void Init();
    void release();
}
