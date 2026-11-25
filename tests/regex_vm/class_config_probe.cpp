#include "xf_data_analytics/text/regexVM_new.hpp"
extern "C" void class_config_probe(ap_uint<4> rule, ap_uint<9> index, ap_uint<64> data, ap_uint<1> read,
                                   ap_uint<256>& result) {
    static ap_uint<256> classes[16][64];
#pragma HLS bind_storage variable = classes type = ram_2p impl = bram
    if (!read) {
        ap_uint<256> previous = classes[rule][index >> 3];
        classes[rule][index >> 3] =
            xf::data_analytics::text::internal_new::replaceClassChunk(previous, index.range(2, 1), data);
    }
    result = read ? classes[rule][index >> 3] : (ap_uint<256>)0;
}
