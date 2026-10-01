#ifndef MCPCHAT_CHECK_H
#define MCPCHAT_CHECK_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check
{
inline int& failures()
{
    static int count = 0;
    return count;
}

inline void expect(bool condition, const char* expression, const char* file, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s:%d: failed: %s\n", file, line, expression);
        ++failures();
    }
}

struct Test
{
    const char* name;
    void (*run)();
};

inline std::vector<Test>& registry()
{
    static std::vector<Test> tests;
    return tests;
}

struct Register
{
    Register(const char* name, void (*run)())
    {
        registry().push_back({name, run});
    }
};

inline int runAll()
{
    for (const Test& test : registry())
    {
        const int before = failures();
        test.run();
        std::printf("%-40s %s\n", test.name, failures() == before ? "ok" : "FAILED");
    }
    if (failures())
        std::fprintf(stderr, "%d check(s) failed\n", failures());
    return failures() == 0 ? 0 : 1;
}
} // namespace check

#define CHECK(expression) ::check::expect((expression), #expression, __FILE__, __LINE__)
#define TEST(name)                                                                                                     \
    static void name();                                                                                                \
    static ::check::Register register_##name(#name, &name);                                                            \
    static void name()

#endif
