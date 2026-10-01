add_executable(fake_mcp_server FakeMcpServer.cpp)
target_link_libraries(fake_mcp_server PRIVATE mcpchat_core)

add_executable(mcpchat_tests
    TestMain.cpp
    CoreTests.cpp
    AgentTests.cpp
    StdioTests.cpp
    HttpTests.cpp
    LoopbackServer.cpp
)
target_link_libraries(mcpchat_tests PRIVATE mcpchat_core)
if(WIN32)
    target_link_libraries(mcpchat_tests PRIVATE ws2_32)
endif()
target_compile_definitions(mcpchat_tests PRIVATE FAKE_MCP_SERVER="$<TARGET_FILE:fake_mcp_server>")
add_dependencies(mcpchat_tests fake_mcp_server)
add_test(NAME mcpchat COMMAND mcpchat_tests)
