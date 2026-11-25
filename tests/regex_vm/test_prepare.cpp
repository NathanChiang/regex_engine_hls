#include "xf_data_analytics/text/regexVM_new.hpp"
#include <cstdio>
using namespace xf::data_analytics::text;
extern "C" void regex_vm_prepare(ap_uint<4>, ap_uint<9>, ap_uint<64>, ap_uint<9>, ap_uint<2>, ap_uint<512>&);
static ap_uint<512> invoke(ap_uint<4> rule, ap_uint<9> index, ap_uint<64> data, ap_uint<9> count, ap_uint<2> command) {
    ap_uint<512> result;
    regex_vm_prepare(rule, index, data, count, command, result);
    return result;
}
int main() {
    ap_uint<64> raw[256];
    ap_uint<512> expected[256];
    int failures = 0;
    for (unsigned count : {2u, 7u, 8u, 9u, 95u, 254u}) {
        for (unsigned pc = 0; pc < 256; ++pc)
            raw[pc] = (ap_uint<64>)OP_END << 56;
        if (count >= 7) {
            raw[0] = 0x2300000000000000ULL;
            raw[1] = 0x3b10000400000000ULL;
            raw[2] = 0x0e01000000000000ULL;
            raw[3] = 0x3a00000100000000ULL;
            raw[4] = 0x0701630000000000ULL;
            raw[5] = 0x2400000000000000ULL;
        }
        for (unsigned k = 0; k < count; ++k) {
            unsigned pc = count - 1 - k;
            invoke(1, pc, raw[pc], count, 0);
        }
        invoke(1, 0, 0, count, 1);
        for (unsigned stage = 0; stage < (count >= 7 ? 2u : 1u); ++stage) {
            if (stage) {
                raw[1] = 0x3b10000600000000ULL;
                invoke(1, 1, raw[1], count, 2);
            }
            internal_new::prepareRule<256>(raw, expected, count);
            for (unsigned pc = 0; pc < count; ++pc) {
                ap_uint<512> actual = invoke(1, pc, 0, count, 3);
                if (actual != expected[pc]) {
                    ++failures;
                    printf("FAIL prepare count=%u stage=%u pc=%u head=%s expected=%s flags=%u/%u\n", count, stage, pc,
                           ((ap_uint<64>)actual.range(63, 0)).to_string(16).c_str(),
                           ((ap_uint<64>)expected[pc].range(63, 0)).to_string(16).c_str(), (unsigned)actual[128],
                           (unsigned)expected[pc][128]);
                }
            }
        }
    }
    printf("prepare differential failures=%d\n", failures);
    return failures;
}
