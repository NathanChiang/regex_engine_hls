#include "xf_data_analytics/text/regexVM_new.hpp"

extern "C" void regex_vm_prepare(ap_uint<4> rule, ap_uint<9> index, ap_uint<64> instruction, ap_uint<9> count,
                                 ap_uint<2> command, ap_uint<512>& result) {
    static ap_uint<64> raw[16][256];
#pragma HLS bind_storage variable = raw type = ram_2p impl = uram
    static ap_uint<512> descriptors[16][256];
#pragma HLS bind_storage variable = descriptors type = ram_2p impl = uram
    if (command == 0 || command == 2)
        raw[rule][index] = instruction;
    if (command == 1 || command == 2)
        xf::data_analytics::text::internal_new::prepareRule<256>(raw[rule], descriptors[rule], count);
    result = command == 3 ? descriptors[rule][index] : (ap_uint<512>)0;
}
