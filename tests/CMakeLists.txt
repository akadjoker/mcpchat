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

add_executable(mcpchat_e2e TestMain.cpp E2eCocoShape.cpp LoopbackServer.cpp)
target_link_libraries(mcpchat_e2e PRIVATE mcpchat_core)
if(WIN32)
    target_link_libraries(mcpchat_e2e PRIVATE ws2_32)
endif()
add_test(NAME mcpchat_e2e COMMAND mcpchat_e2e)

# The agent, the model client and the MCP client without the window: one message from the command line.
add_executable(mcpchat_headless HeadlessChat.cpp)
target_link_libraries(mcpchat_headless PRIVATE mcpchat_core)
if(WIN32)
    target_link_libraries(mcpchat_headless PRIVATE ws2_32)
endif()
