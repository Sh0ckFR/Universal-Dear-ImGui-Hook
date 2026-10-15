// imgui.png, embedded in the library and decoded with stb_image, so the
// menu can show a picture without a file next to the game.
#include "imgui.h"
#include "uih.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <cstddef>

#ifdef _WIN32
// Embedded by image.rc.
static bool EmbeddedPng(const unsigned char*& data, size_t& size) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EmbeddedPng), &module);
    HRSRC resource = FindResourceW(module, L"IMGUI_PNG", MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    if (!loaded)
        return false;
    data = static_cast<const unsigned char*>(LockResource(loaded));
    size = SizeofResource(module, resource);
    return data != nullptr;
}
#else
// UIH_IMAGE is the path of imgui.png, set by CMake.
__asm__(".section .rodata\n"
        ".balign 16\n"
        "uih_example_png:\n"
        ".incbin \"" UIH_IMAGE "\"\n"
        "uih_example_png_end:\n"
        ".previous\n");
extern "C" const unsigned char uih_example_png[];
extern "C" const unsigned char uih_example_png_end[];

static bool EmbeddedPng(const unsigned char*& data, size_t& size) {
    data = uih_example_png;
    size = static_cast<size_t>(uih_example_png_end - uih_example_png);
    return size > 0;
}
#endif

ImTextureData* ExampleImage(ImVec2& size) {
    static ImTextureData* texture = nullptr;
    static ImVec2 imageSize;
    if (!texture) {
        const unsigned char* png = nullptr;
        size_t length = 0;
        int width = 0;
        int height = 0;
        int channels = 0;
        if (!EmbeddedPng(png, length))
            return nullptr;
        unsigned char* pixels = stbi_load_from_memory(png, static_cast<int>(length), &width, &height, &channels, 4);
        if (!pixels)
            return nullptr;
        texture = uih::CreateTexture(pixels, width, height);
        stbi_image_free(pixels);
        imageSize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    }
    size = imageSize;
    return texture;
}
