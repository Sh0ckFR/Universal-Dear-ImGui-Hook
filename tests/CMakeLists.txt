function(uih_add_host_test name)
    add_test(NAME ${name} COMMAND uih_host ${ARGN})
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL TRUE)
endfunction()

if(WIN32)
    add_executable(uih_host host.cpp host_vulkan.cpp)
    target_link_libraries(uih_host PRIVATE uih d3d9 d3d10 d3d11 d3d12 dxgi opengl32 dinput8 dxguid)
    target_compile_definitions(uih_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

    # A controller the tests drive, in its own directory so that only the
    # gamepad scenario loads it in place of the system xinput1_4.dll.
    add_library(uih_fake_xinput SHARED fake_xinput.cpp fake_xinput.def)
    set_target_properties(uih_fake_xinput PROPERTIES OUTPUT_NAME xinput1_4 PREFIX ""
                          RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_CURRENT_BINARY_DIR}/fake_xinput>)
    if(MINGW)
        target_link_options(uih_fake_xinput PRIVATE -Wl,--enable-stdcall-fixup)
    endif()

    uih_add_host_test(d3d9 --api d3d9)
    uih_add_host_test(d3d9ex --api d3d9ex)
    uih_add_host_test(d3d10 --api d3d10)
    uih_add_host_test(d3d11 --api d3d11)
    uih_add_host_test(d3d11_early --api d3d11 --inject-at 0)
    uih_add_host_test(d3d11_scale --api d3d11 --scale 2)
    uih_add_host_test(d3d12 --api d3d12)
    uih_add_host_test(d3d12_decoy --api d3d12 --decoy-queue)
    uih_add_host_test(d3d12_decoy_early --api d3d12 --decoy-queue --inject-at 0)
    uih_add_host_test(vulkan --api vulkan)
    uih_add_host_test(vulkan_early --api vulkan --inject-at 0)
    uih_add_host_test(opengl --api opengl)
    uih_add_host_test(key_file --parse-key-file ${CMAKE_CURRENT_SOURCE_DIR}/keys.ini)
    uih_add_host_test(input_raw --api d3d11 --input raw)
    uih_add_host_test(input_directinput --api d3d11 --input directinput)
    uih_add_host_test(gamepad --api d3d11 --gamepad $<TARGET_FILE:uih_fake_xinput>)

    # Back buffers storing linear values: the overlay colors must come out unchanged.
    uih_add_host_test(d3d9_srgb --api d3d9 --format srgb --capture)
    uih_add_host_test(d3d10_srgb --api d3d10 --format srgb --capture)
    uih_add_host_test(d3d11_srgb --api d3d11 --format srgb --capture)
    uih_add_host_test(d3d11_float --api d3d11 --format float --capture)
    uih_add_host_test(d3d12_float --api d3d12 --format float --capture)
    uih_add_host_test(vulkan_srgb --api vulkan --format srgb --capture)
    uih_add_host_test(vulkan_srgb_early --api vulkan --format srgb --capture --inject-at 0 --settle 3000)
    uih_add_host_test(opengl_srgb --api opengl --format srgb --capture)

    if(TARGET uih_example)
        uih_add_host_test(dll_d3d11 --api d3d11 --dll $<TARGET_FILE:uih_example>)
        uih_add_host_test(dll_vulkan --api vulkan --dll $<TARGET_FILE:uih_example>)
        uih_add_host_test(dll_opengl --api opengl --dll $<TARGET_FILE:uih_example>)
        uih_add_host_test(dll_d3d9 --api d3d9 --dll $<TARGET_FILE:uih_example>)
        uih_add_host_test(dll_proxy --proxy $<TARGET_FILE:uih_example>)
        # The DLL reads tests/keys.ini through UIH_CONFIG: F1 opens the menu, END unloads it.
        uih_add_host_test(dll_key_file --api d3d11 --dll $<TARGET_FILE:uih_example> --menu-key 0x70 --unload-key 0x23)
        set_tests_properties(dll_key_file PROPERTIES ENVIRONMENT "UIH_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/keys.ini")
    endif()
else()
    pkg_check_modules(UIH_HOST REQUIRED IMPORTED_TARGET x11 xtst gl egl)

    # The host does not link the hook: the tests preload the example library.
    add_executable(uih_host host_linux.cpp host_vulkan.cpp)
    target_include_directories(uih_host PRIVATE ${UIH_THIRD_PARTY}/vulkan/include)
    target_compile_definitions(uih_host PRIVATE VK_NO_PROTOTYPES)
    target_link_libraries(uih_host PRIVATE PkgConfig::UIH_HOST ${CMAKE_DL_LIBS})

    if(TARGET uih_example)
        function(uih_add_preload_test name)
            uih_add_host_test(${name} ${ARGN})
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:uih_example>;UIH_DEBUG=1")
        endfunction()

        uih_add_preload_test(preload_glx --api opengl)
        uih_add_preload_test(preload_glx_lookup --api opengl --lookup)
        uih_add_preload_test(preload_egl --api opengl --egl)
        uih_add_preload_test(preload_egl_lookup --api opengl --egl --lookup)
        uih_add_preload_test(preload_vulkan --api vulkan)
        uih_add_preload_test(preload_vulkan_device --api vulkan --device-proc-addr)
        # tests/keys.ini through UIH_CONFIG: F1 opens the menu, END removes the overlay.
        uih_add_host_test(preload_key_file --api opengl --menu-key 0xffbe --unload-key 0xff57)
        set_tests_properties(preload_key_file PROPERTIES
            ENVIRONMENT "LD_PRELOAD=$<TARGET_FILE:uih_example>;UIH_DEBUG=1;UIH_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/keys.ini")
    endif()
endif()
