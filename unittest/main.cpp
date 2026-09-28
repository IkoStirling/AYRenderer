#include "AYTest.h"
#include <cstdio>

#ifndef AYRENDERER_TEST_MODULE
#define AYRENDERER_TEST_MODULE "AYRenderer"
#endif

int main(int argc, char* argv[])
{
    // Preserve the last diagnostics if a test process terminates unexpectedly.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return ayt::test::runTests(AYRENDERER_TEST_MODULE, argc, argv);
}
