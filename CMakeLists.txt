cmake_minimum_required(VERSION 3.24)
project(claude_code_cpp
    VERSION 0.1.0
    DESCRIPTION "Claude Code CLI — C++20 rewrite built on ai-sdk-cpp"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
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
# maya — the terminal UI toolkit. Its input decoder reads the keys, the
# mouse and the pastes of the terminal, and its platform layer knows the
# size of the screen. The screen itself is drawn by our own terminal layer,
# so that the transcript can be committed to the terminal scrollback the way
# Claude Code does it.
#
# The runtime of maya (jaal) is not used: the session drives its own loop.
# ---------------------------------------------------------------------------
set(MAYA_BUILD_APP      OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(MAYA_BUILD_FUZZERS  OFF CACHE BOOL "" FORCE)
# A binary that runs on another machine cannot be built for the CPU of this
# one.
set(MAYA_NATIVE_TUNING  OFF CACHE BOOL "" FORCE)
set(MAYA_WERROR         OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    maya
    GIT_REPOSITORY https://github.com/agentty-org/maya.git
    GIT_TAG        661aa60cdec0b1cf5c934eacd3bba9f40ca2b7da
    GIT_SHALLOW    TRUE
    # jaal is the runtime of `maya::app`, which the port does not use.
    GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(maya)

# maya asks for C++26 where the compiler has it. The port stays on C++23:
# nothing of it needs the newer standard, and the compilers of the older
# systems are left alone.
set_property(TARGET maya PROPERTY INTERFACE_COMPILE_FEATURES "cxx_std_23")

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
