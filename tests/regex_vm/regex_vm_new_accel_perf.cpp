#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

#ifdef XF_REGEX_VM_NEW_PREDECODED
#define RE_VM_INSTRUCTION_ROWS 256
#else
#define RE_VM_INSTRUCTION_ROWS 32
#endif

extern "C" unsigned int regex_vm_new_accel_perf(ap_uint<256> bitset[128],
                                                ap_uint<512> instructions[RE_VM_INSTRUCTION_ROWS],
                                                ap_uint<512> message[64], unsigned int length,
                                                ap_uint<16> offsets[512]) {
#pragma HLS interface ap_ctrl_hs port = return
#pragma HLS interface ap_memory port = bitset storage_type = ram_2p
#pragma HLS interface ap_memory port = instructions storage_type = ram_2p
#pragma HLS interface ap_memory port = message storage_type = ram_2p
#pragma HLS interface ap_memory port = offsets storage_type = ram_t2p
    ap_uint<2> match = 0;
#ifdef XF_REGEX_VM_NEW_PREDECODED
    xf::data_analytics::text::regexVM_new_predecoded<8192, 256>(
#else
    xf::data_analytics::text::regexVM_new_wide<8192, 256>(
#endif
        bitset, instructions, message, length, match, offsets);
    return (unsigned int)match;
}
