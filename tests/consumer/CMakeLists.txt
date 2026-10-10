# Minimal project consuming the hook through FetchContent. Build it with:
#   cmake -S tests/consumer -B build-consumer -A x64
#   cmake --build build-consumer --config Release
cmake_minimum_required(VERSION 3.21)
project(uih_consumer LANGUAGES C CXX)

set(UIH_GIT_REPOSITORY "https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook.git" CACHE STRING "Repository to fetch the hook from")
set(UIH_GIT_TAG "master" CACHE STRING "Commit, branch or tag to fetch")

include(FetchContent)
FetchContent_Declare(uih GIT_REPOSITORY ${UIH_GIT_REPOSITORY} GIT_TAG ${UIH_GIT_TAG})
FetchContent_MakeAvailable(uih)

add_library(my_overlay SHARED overlay.cpp)
target_link_libraries(my_overlay PRIVATE uih::uih)
