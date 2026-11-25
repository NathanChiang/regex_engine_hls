#include "ap_int.h"
#include <cstdio>
extern "C" void class_config_probe(ap_uint<4>, ap_uint<9>, ap_uint<64>, ap_uint<1>, ap_uint<256>&);
int main() {
    int failures = 0;
    ap_uint<256> actual;
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        for (unsigned k = 0; k < 4; ++k) {
            unsigned index = reverse ? 6 - 2 * k : 2 * k;
            ap_uint<64> data = index == 2 ? 0x0000000600000000ULL : 0;
            class_config_probe(1, index, data, 0, actual);
        }
        class_config_probe(1, 0, 0, 1, actual);
        ap_uint<256> expected = 0;
        expected[97] = 1;
        expected[98] = 1;
        if (actual != expected) {
            ++failures;
            printf("FAIL class packing reverse=%u actual=%s expected=%s\n", reverse, actual.to_string(16).c_str(),
                   expected.to_string(16).c_str());
        }
    }
    printf("class configuration failures=%d\n", failures);
    return failures;
}
