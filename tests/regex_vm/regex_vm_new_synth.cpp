#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

extern "C" void regex_vm_new_synth(ap_uint<32> bitset[1024],
                                    ap_uint<512> instructions[32],
                                    ap_uint<512> message[64],
                                    unsigned int length,
                                    ap_uint<2>& match,
                                    ap_uint<16> offsets[512]) {
#pragma HLS interface ap_memory port = bitset storage_type = ram_2p
#pragma HLS interface ap_memory port = instructions storage_type = ram_2p
#pragma HLS interface ap_memory port = message storage_type = ram_2p
#pragma HLS interface ap_memory port = offsets storage_type = ram_2p
    xf::data_analytics::text::regexVM_new_wide<8192>(bitset, instructions, message, length, match, offsets);
}
