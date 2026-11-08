#include <cstdio>

#include "testfw.hpp"

void run_payload_tests();
void run_config_tests();
void run_diag_tests();

int main()
{
    std::printf("MiniLoRa host tests\n===================\n");
    run_payload_tests();
    run_config_tests();
    run_diag_tests();
    std::printf("\nTOTAL: %d checks, %d failures\n",
                testfw::g_checks, testfw::g_failures);
    return testfw::g_failures == 0 ? 0 : 1;
}
