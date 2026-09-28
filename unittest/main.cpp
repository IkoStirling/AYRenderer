#include "AYTest.h"

#ifndef AYRENDERER_TEST_MODULE
#define AYRENDERER_TEST_MODULE "AYRenderer"
#endif

int main(int argc, char* argv[])
{
    return ayt::test::runTests(AYRENDERER_TEST_MODULE, argc, argv);
}
