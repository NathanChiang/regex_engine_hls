#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

// Match the optimized VM benchmark's narrow memories and capacities.
extern "C" unsigned int regex_vm_new_perf(ap_uint<32> bitset[1024],
                                          ap_uint<64> instructions[4096],
                                          ap_uint<32> message[1024], unsigned int length,
                                          ap_uint<16> offsets[512]) {
#pragma HLS interface ap_ctrl_hs port=return
#pragma HLS interface ap_memory port=bitset storage_type=ram_2p
#pragma HLS interface ap_memory port=instructions storage_type=ram_2p
#pragma HLS interface ap_memory port=message storage_type=ram_2p
#pragma HLS interface ap_memory port=offsets storage_type=ram_t2p
    ap_uint<2> match = 0;
    xf::data_analytics::text::regexVM_new<8192, 4096>(
        bitset, instructions, message, length, match, offsets);
    return (unsigned int)match;
}
