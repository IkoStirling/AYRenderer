#include "AYTest.h"

int main(int argc, char* argv[])
{
    if (argc > 1) {
        return ayt::test::runSuite(argv[1]);
    }
    return ayt::test::runAllTests("AYRenderer");
}
