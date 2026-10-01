cmake_minimum_required(VERSION 3.16)

project(mcpchat VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Like zen_platform: no runtime DLL next to the executable.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8)
endif()

option(MCPCHAT_BUILD_GUI "Build the window (zen_platform + iGUI)" ON)
option(MCPCHAT_BUILD_TESTS "Build the tests" ON)

find_package(Threads REQUIRED)
include(cmake/Dependencies.cmake)

add_library(mcpchat_core STATIC
    src/util/Base64.cpp
    src/util/Strings.cpp
    src/util/Paths.cpp
    src/net/Url.cpp
    src/net/Sse.cpp
    src/mcp/Process.cpp
    src/mcp/McpClient.cpp
    src/mcp/HttpTransport.cpp
    src/mcp/StdioTransport.cpp
    src/mcp/ServerHub.cpp
    src/llm/Wire.cpp
    src/llm/OpenAiProvider.cpp
    src/agent/Tools.cpp
    src/agent/History.cpp
    src/agent/Agent.cpp
    src/config/Config.cpp
    src/ui/TextWrap.cpp
)
if(WIN32)
    target_sources(mcpchat_core PRIVATE src/net/HttpWinHttp.cpp)
    target_link_libraries(mcpchat_core PUBLIC winhttp)
else()
    find_package(CURL REQUIRED)
    target_sources(mcpchat_core PRIVATE src/net/HttpCurl.cpp)
    target_link_libraries(mcpchat_core PUBLIC CURL::libcurl)
endif()
target_include_directories(mcpchat_core PUBLIC src third_party)
target_link_libraries(mcpchat_core PUBLIC Threads::Threads)
target_compile_definitions(mcpchat_core PUBLIC MCPCHAT_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mcpchat_core PRIVATE -Wall -Wextra)
endif()
if(MINGW)
    target_link_options(mcpchat_core PUBLIC -static)
endif()

if(MCPCHAT_BUILD_GUI)
    add_executable(mcpchat WIN32
        src/app/main.cpp
        src/render/GlFunctions.cpp
        src/render/GlBackend.cpp
        src/render/Image.cpp
        src/ui/ChatSession.cpp
        src/ui/SettingsDialog.cpp
        src/ui/ChatWindow.cpp
    )
    target_link_libraries(mcpchat PRIVATE mcpchat_core platform igui::igui igui::font)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mcpchat PRIVATE -Wall -Wextra)
    endif()
    if(MSVC)
        # A window program that still starts at main().
        target_link_options(mcpchat PRIVATE /ENTRY:mainCRTStartup)
    endif()
    install(TARGETS mcpchat RUNTIME DESTINATION .)
endif()

if(MCPCHAT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
