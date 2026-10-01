#include "FakeLogic.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// The fake MCP server over stdio. Besides the shared tools it knows "crash" (exits with code 3 after a line on
// stderr), "ask" (sends the client a ping and answers once the pong is back) and "sleep" (answers after 10 s).
int main()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::cerr << "fake server ready" << std::endl;
    std::string line;
    while (std::getline(std::cin, line))
    {
        const mcpchat::Json request = mcpchat::Json::parse(line, nullptr, false);
        if (request.is_discarded() || !request.is_object())
            continue;
        const mcpchat::Json params = request.value("params", mcpchat::Json::object());
        if (request.value("method", "") == "tools/call" && params.value("name", "") == "crash")
        {
            std::cerr << "about to crash" << std::endl;
            std::exit(3);
        }
        if (request.value("method", "") == "tools/call" && params.value("name", "") == "sleep")
            std::this_thread::sleep_for(std::chrono::seconds(10));
        if (request.value("method", "") == "tools/call" && params.value("name", "") == "ask")
        {
            std::cout << mcpchat::dump({{"jsonrpc", "2.0"}, {"id", "server-1"}, {"method", "ping"}}) << "\n" << std::flush;
            std::string pong;
            if (!std::getline(std::cin, pong))
                return 1;
            const mcpchat::Json reply = mcpchat::Json::parse(pong, nullptr, false);
            const bool answered = !reply.is_discarded() && reply.value("id", mcpchat::Json()) == "server-1" &&
                                  reply.contains("result");
            std::cout << mcpchat::dump({{"jsonrpc", "2.0"},
                                         {"id", request["id"]},
                                         {"result",
                                          {{"content", {{{"type", "text"}, {"text", answered ? "pong received" : "no pong"}}}}}}})
                      << "\n"
                      << std::flush;
            continue;
        }
        const auto reply = fake::answer(request);
        if (reply)
            std::cout << mcpchat::dump(*reply) << "\n" << std::flush;
    }
    return 0;
}
