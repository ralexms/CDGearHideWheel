cmake_minimum_required(VERSION 3.24)
project(GearHideWheel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook v1.3.4: vendored in third_party/minhook (else fetched).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook/CMakeLists.txt")
    add_subdirectory(third_party/minhook)
else()
    include(FetchContent)
    FetchContent_Declare(minhook
        GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
        GIT_TAG v1.3.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(minhook)
endif()

set(GHW_SOURCES
    src/main.cpp src/addrs.cpp src/config.cpp src/gear.cpp src/headgear.cpp src/input.cpp src/state.cpp src/stack.cpp
    src/ui.cpp src/vis.cpp src/wheel.cpp src/resolver.cpp src/log.cpp src/game.cpp)

# The plugin: GearHideWheel.asi
add_library(GearHideWheel SHARED ${GHW_SOURCES})
target_include_directories(GearHideWheel PRIVATE src)
target_compile_definitions(GearHideWheel PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(GearHideWheel PRIVATE /W4 /permissive- /utf-8)
set_target_properties(GearHideWheel PROPERTIES SUFFIX ".asi")
target_link_libraries(GearHideWheel PRIVATE minhook hid setupapi)
