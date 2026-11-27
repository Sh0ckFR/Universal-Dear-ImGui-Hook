# A library built against the C++ SDK that `engine dump all` writes of the
# fake game of tests/fake_unreal.cpp, as a trainer is built against the SDK of
# a real game: tests/CMakeLists.txt builds it with ctest --build-and-test once
# the remote test copied the SDK to UIH_SDK_DIR/uih_sdk.hpp, then uih_host
# --sdk-test loads it and runs its uih_sdk_test(). The SDK needs nothing else:
# neither Unreal's headers nor this repository.
cmake_minimum_required(VERSION 3.21)
project(uih_sdk_test LANGUAGES CXX)

if(NOT UIH_SDK_DIR OR NOT UIH_SDK_OUTPUT)
    message(FATAL_ERROR "give -DUIH_SDK_DIR=<directory of uih_sdk.hpp> -DUIH_SDK_OUTPUT=<directory of the library>")
endif()

add_library(uih_sdk_test SHARED sdk_test.cpp)
target_include_directories(uih_sdk_test PRIVATE ${UIH_SDK_DIR})
target_compile_features(uih_sdk_test PRIVATE cxx_std_17)
set_target_properties(uih_sdk_test PROPERTIES PREFIX "" CXX_EXTENSIONS OFF
                      RUNTIME_OUTPUT_DIRECTORY $<1:${UIH_SDK_OUTPUT}>
                      LIBRARY_OUTPUT_DIRECTORY $<1:${UIH_SDK_OUTPUT}>)
# The header must compile without warnings, as a trainer's code would.
if(MSVC)
    target_compile_options(uih_sdk_test PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(uih_sdk_test PRIVATE -Wall -Wextra -Werror)
endif()
