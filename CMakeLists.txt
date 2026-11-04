cmake_minimum_required(VERSION 3.24)
project(claude_code_cpp
    VERSION 0.1.0
    DESCRIPTION "Claude Code CLI — C++20 rewrite built on ai-sdk-cpp"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(CC_BUILD_TESTS "Build the Claude Code test suite" ON)
option(CC_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# The compiler is run through ccache when it is there: the objects of the
# files that did not change are taken from the cache, so the second build
# of a change is a link and not a compile.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

set(AI_SDK_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(AI_SDK_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(AI_SDK_BUILD_CLI        OFF CACHE BOOL "" FORCE)
set(AI_SDK_BUILD_BINDINGS   OFF CACHE BOOL "" FORCE)
set(AI_SDK_BUILD_SHARED     OFF CACHE BOOL "" FORCE)
set(AI_SDK_BUILD_STATIC     ON  CACHE BOOL "" FORCE)

# Only the providers Claude Code actually talks to.
set(AI_SDK_PROVIDER_ANTHROPIC          ON  CACHE BOOL "" FORCE)
# The OpenAI-compatible provider reuses the OpenAI provider's headers.
set(AI_SDK_PROVIDER_OPENAI             ON  CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_OPENAI_COMPATIBLE ON  CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_GOOGLE             OFF CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_DEEPSEEK           OFF CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_MOONSHOTAI         OFF CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_ZAI                OFF CACHE BOOL "" FORCE)
set(AI_SDK_PROVIDER_BEDROCK            OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ai_sdk_cpp
    GIT_REPOSITORY https://github.com/mhdimo/ai-sdk-cpp.git
    GIT_TAG        v1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ai_sdk_cpp)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

# ---------------------------------------------------------------------------
# FTXUI — the terminal UI toolkit the renderer, the prompt box and the
# dialogs are built on. We use its screen/dom layout engine and its terminal
# input parser (for key decoding), but drive the frame loop ourselves so the
# transcript can be committed to the terminal scrollback the way Claude Code
# does it.
# ---------------------------------------------------------------------------
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET          ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v5.0.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

find_package(Boost 1.82 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning configuration shared by our own targets
# ---------------------------------------------------------------------------
add_library(cc_flags INTERFACE)
target_compile_options(cc_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    $<$<BOOL:${CC_WARNINGS_AS_ERRORS}>:-Werror>
)
target_compile_features(cc_flags INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(CC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
