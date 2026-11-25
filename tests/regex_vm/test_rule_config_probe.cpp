#include "ap_int.h"
#include <cstdio>
extern "C" void rule_config_probe(unsigned, unsigned, unsigned, ap_uint<64>, ap_uint<32>&, ap_uint<32>&);
static unsigned finalizations = 0;
static bool command(unsigned cmd, unsigned index, ap_uint<64> data, bool expected_finalize, unsigned count) {
    bool observed = false, ack = false;
    ap_uint<32> event, flags;
    for (unsigned tick = 0; tick < 64; ++tick) {
        rule_config_probe(cmd, 1, index, data, event, flags);
        if (event[31] && event[30]) {
            observed = true;
            ++finalizations;
            if (event.range(29, 21) != count)
                return false;
        }
        if (flags[0]) {
            ack = true;
            break;
        }
    }
    if (!ack || observed != expected_finalize) {
        printf("FAIL cfg index=%u expected finalize=%u observed=%u ack=%u\n", index, expected_finalize, observed, ack);
        return false;
    }
    for (unsigned tick = 0; tick < 64; ++tick) {
        rule_config_probe(cmd, 0, index, data, event, flags);
        if (flags[1])
            return true;
    }
    return false;
}
int main() {
    int failures = 0;
    failures += !command(5, 0, 0, false, 0);
    failures += !command(3, 0, 4, false, 0);
    failures += !command(3, 1, 0x0014000000000002ULL, false, 0);
    failures += !command(3, 2, 0x0100000000000000ULL, false, 2);
    failures += !command(3, 3, 0x0100000000000000ULL, true, 2);
    const unsigned order[] = {0, 1, 8, 7, 6, 5, 4, 3, 2, 12, 11, 10, 9};
    for (unsigned step = 0; step < 13; ++step) {
        unsigned index = order[step];
        ap_uint<64> data = index == 0 ? 13 : index == 1 ? 0x0001000100000007ULL : 0x0100000000000000ULL;
        failures += !command(3, index, data, step == 12, 7);
    }
    failures += !command(3, 3, 0x3b10000600000000ULL, true, 7);
    printf("rule configuration failures=%d finalizations=%u\n", failures, finalizations);
    return failures;
}
