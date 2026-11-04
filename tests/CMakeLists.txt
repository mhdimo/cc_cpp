include(Catch)

file(GLOB_RECURSE CC_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(claude_code_tests ${CC_TEST_SOURCES})

target_link_libraries(claude_code_tests
    PRIVATE
        claude_code
        Catch2::Catch2WithMain
        cc_flags
)

# The tests find their fixtures — the little MCP server, for one — through
# this.
target_compile_definitions(claude_code_tests PRIVATE
    CC_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

catch_discover_tests(claude_code_tests
    TEST_PREFIX "cc."
)
