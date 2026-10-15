cmake_minimum_required(VERSION 3.21)

project(UniversalImGuiHook VERSION 2.0.0 LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This project builds Windows DLLs and Linux shared objects. On macOS, cross-compile the DLL with -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake (see README).")
endif()

option(UIH_BUILD_EXAMPLE "Build the example library: an injectable DLL on Windows, a preloadable shared object on Linux" ${PROJECT_IS_TOP_LEVEL})
option(UIH_BUILD_TESTS "Build the host applications used to test every backend" OFF)
option(UIH_STATIC_RUNTIME "Link the C and C++ runtimes statically so the library has no redistributable dependency" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(UIH_STATIC_RUNTIME)
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    elseif(MINGW)
        add_link_options(-static -static-libgcc -static-libstdc++)
    else()
        add_link_options(-static-libgcc -static-libstdc++)
    endif()
endif()
if(NOT MSVC)
    add_link_options($<$<NOT:$<CONFIG:Debug>>:-s>)
endif()

set(UIH_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(WIN32 AND NOT TARGET minhook)
    add_library(minhook STATIC
        ${UIH_THIRD_PARTY}/minhook/src/buffer.c
        ${UIH_THIRD_PARTY}/minhook/src/hook.c
        ${UIH_THIRD_PARTY}/minhook/src/trampoline.c
        $<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,${UIH_THIRD_PARTY}/minhook/src/hde/hde64.c,${UIH_THIRD_PARTY}/minhook/src/hde/hde32.c>)
    target_include_directories(minhook PUBLIC ${UIH_THIRD_PARTY}/minhook/include)
endif()

add_library(uih_imgui STATIC
    ${UIH_THIRD_PARTY}/imgui/imgui.cpp
    ${UIH_THIRD_PARTY}/imgui/imgui_demo.cpp
    ${UIH_THIRD_PARTY}/imgui/imgui_draw.cpp
    ${UIH_THIRD_PARTY}/imgui/imgui_tables.cpp
    ${UIH_THIRD_PARTY}/imgui/imgui_widgets.cpp
    ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_vulkan.cpp
    ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(uih_imgui PUBLIC
    ${UIH_THIRD_PARTY}/imgui
    ${UIH_THIRD_PARTY}/imgui/backends
    ${UIH_THIRD_PARTY}/vulkan/include)
target_compile_definitions(uih_imgui PUBLIC
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
if(WIN32)
    target_sources(uih_imgui PRIVATE
        ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_win32.cpp
        ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_dx9.cpp
        ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_dx10.cpp
        ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_dx11.cpp
        ${UIH_THIRD_PARTY}/imgui/backends/imgui_impl_dx12.cpp)
    target_compile_definitions(uih_imgui PUBLIC VK_USE_PLATFORM_WIN32_KHR)
    target_link_libraries(uih_imgui PUBLIC d3dcompiler dxgi dwmapi gdi32 imm32)
else()
    target_link_libraries(uih_imgui PUBLIC ${CMAKE_DL_LIBS})
endif()
add_library(uih::imgui ALIAS uih_imgui)

add_library(uih STATIC src/uih.cpp src/vulkan.cpp)
target_include_directories(uih PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_sources(uih PRIVATE
        src/windows/platform.cpp
        src/windows/input.cpp
        src/windows/dinput.cpp
        src/windows/xinput.cpp
        src/windows/d3d9.cpp
        src/windows/dxgi.cpp
        src/windows/d3d10.cpp
        src/windows/d3d11.cpp
        src/windows/d3d12.cpp
        src/windows/opengl.cpp)
    target_link_libraries(uih PUBLIC uih_imgui PRIVATE minhook)
    target_compile_definitions(uih PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(UIH_XCB REQUIRED IMPORTED_TARGET xcb)
    find_package(Threads REQUIRED)
    target_sources(uih PRIVATE
        src/linux/platform.cpp
        src/linux/interpose.cpp
        src/linux/x11.cpp
        src/linux/opengl.cpp)
    target_link_libraries(uih PUBLIC uih_imgui PRIVATE PkgConfig::UIH_XCB Threads::Threads ${CMAKE_DL_LIBS})
    # Only the functions the hook stands for are visible to the target process.
    target_link_options(uih INTERFACE LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/linux/exports.map)
    # dlsym() has to forward with a tail call, whatever the build type.
    set_source_files_properties(src/linux/interpose.cpp PROPERTIES COMPILE_OPTIONS "-O2;-fno-exceptions;-fno-stack-protector")
    set_target_properties(uih uih_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(MSVC)
    target_compile_options(uih PRIVATE /W4 /permissive-)
else()
    target_compile_options(uih PRIVATE -Wall -Wextra)
endif()
add_library(uih::uih ALIAS uih)

if(UIH_BUILD_EXAMPLE)
    if(WIN32)
        add_library(uih_example SHARED example/dllmain.cpp example/config.cpp example/menu.cpp example/proxy.cpp example/proxy.def)
    else()
        add_library(uih_example SHARED example/preload.cpp example/config.cpp example/menu.cpp)
        target_link_options(uih_example PRIVATE LINKER:--no-undefined)
    endif()
    target_link_libraries(uih_example PRIVATE uih)
    set_target_properties(uih_example PROPERTIES OUTPUT_NAME Universal-ImGui-Hook PREFIX "")
    if(MINGW)
        target_link_options(uih_example PRIVATE -Wl,--enable-stdcall-fixup)
    endif()
endif()

if(UIH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
