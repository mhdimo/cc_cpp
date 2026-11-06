# Claude Code core library + CLI binary.

file(GLOB_RECURSE CC_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
list(FILTER CC_LIB_SOURCES EXCLUDE REGEX "/main\\.cpp$")

add_library(claude_code STATIC ${CC_LIB_SOURCES})

# The debug information is put in files of their own: the objects stay small
# and the link is quicker. The debugger finds the information by itself, so
# the way of debugging does not change.
option(CC_SPLIT_DWARF "Put the debug information of the objects in files of their own" OFF)
if(CC_SPLIT_DWARF)
    target_compile_options(claude_code PUBLIC -gsplit-dwarf)
endif()

target_include_directories(claude_code PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
)

target_link_libraries(claude_code
    PUBLIC
        ai-sdk-core
        ai-sdk-anthropic
        Boost::headers
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    PRIVATE
        cc_flags
        maya
)

add_executable(claude main.cpp)
target_link_libraries(claude PRIVATE claude_code cc_flags)

# The program is run as `cc++`, the way the port is named. The target keeps
# the name of the reference, so that the rule of the target does not have to
# carry a sign that a shell would read.
set_target_properties(claude PROPERTIES OUTPUT_NAME "cc++")

# The program was called `claude` before. The old file is taken away, so
# that it does not stay behind and answer for a version that is not there.
if(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/claude")
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/claude")
endif()

install(TARGETS claude RENAME cc++ RUNTIME DESTINATION bin)
