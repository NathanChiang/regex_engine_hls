// Reuse the repository's complete regex corpus without changing its patterns
// or reference engine. This is a host-only C regression, not an HLS top.
extern "C" {
#include "oniguruma.h"
#include "xf_data_analytics/text/xf_re_compile.h"
}
#include "xf_data_analytics/text/regexVM_new.hpp"
#include <cstdio>

static unsigned corpus_instructions = 0;
static unsigned corpus_classes = 0;
static unsigned corpus_calls = 0, corpus_slot_fit = 0, corpus_max_words = 0;
static int corpus_compile(const char* pattern, unsigned* bitset, uint64_t* instructions, unsigned* instruction_count,
                          unsigned* class_count, unsigned* group_count, uint8_t* names, uint32_t* name_offsets) {
    int result =
        xf_re_compile(pattern, bitset, instructions, instruction_count, class_count, group_count, names, name_offsets);
    corpus_instructions = *instruction_count;
    corpus_classes = *class_count;
    return result;
}

namespace xf {
namespace data_analytics {
namespace text {
template <int STACK_SIZE>
void corpus_vm(ap_uint<32>* bitset, ap_uint<64>* instructions, ap_uint<32>* message, unsigned length, ap_uint<2>& match,
               ap_uint<16>* offsets) {
    ++corpus_calls;
    unsigned words = 2 + corpus_instructions + 4 * corpus_classes;
    if (words <= 256)
        ++corpus_slot_fit;
    if (words > corpus_max_words)
        corpus_max_words = words;
    // test.cpp casts native compiler arrays to SDK pointer types. Materialize
    // real SDK objects before interpreting them so this host regression does
    // not depend on class layout or strict-aliasing behavior.
    static ap_uint<64> copied_instructions[32768];
    static ap_uint<32> copied_bitset[1024];
    const uint64_t* native_instructions = reinterpret_cast<const uint64_t*>(instructions);
    const unsigned* native_bitset = reinterpret_cast<const unsigned*>(bitset);
    for (unsigned i = 0; i < corpus_instructions; ++i)
        copied_instructions[i] = native_instructions[i];
    for (unsigned i = 0; i < corpus_classes * 8; ++i)
        copied_bitset[i] = native_bitset[i];
    instructions = copied_instructions;
    bitset = copied_bitset;
#ifdef REGEX_VM_CORPUS_BASELINE
    // Ignored speculative reads in regexVM_opt use full 16-bit operand values.
    static ap_uint<16> shadow[65537];
    for (unsigned i = 0; i < 512; ++i)
        shadow[i] = 0xffff;
    regexVM_opt<STACK_SIZE>(bitset, instructions, message, length, match, shadow);
    for (unsigned i = 0; i < 512; ++i)
        offsets[i] = shadow[i];
#else
    static ap_uint<512> packed[4097], descriptors[32768];
    static ap_uint<256> classes[128];
    ap_uint<512> wide_message[16];
    ap_uint<16> narrow_offsets[512], wide_offsets[512];
    for (unsigned i = 0; i < 512; ++i)
        narrow_offsets[i] = wide_offsets[i] = offsets[i] = 0xffff;
    for (unsigned i = 0; i < 128; ++i) {
        classes[i] = 0;
        for (unsigned k = 0; k < 8; ++k)
            if (i < corpus_classes)
                classes[i].range(32 * k + 31, 32 * k) = bitset[8 * i + k];
    }
    for (unsigned i = 0; i < (corpus_instructions + 7) / 8 + 1; ++i) {
        packed[i] = 0;
        for (unsigned k = 0; k < 8; ++k)
            packed[i].range(64 * k + 63, 64 * k) =
                8 * i + k < corpus_instructions ? instructions[8 * i + k] : (ap_uint<64>)OP_END << 56;
    }
    for (unsigned i = 0; i < 16; ++i) {
        wide_message[i] = 0;
        for (unsigned k = 0; k < 16; ++k)
            if (64 * i + 4 * k < length)
                wide_message[i].range(32 * k + 31, 32 * k) = message[16 * i + k];
    }
    internal_new::prepareRule<32768>(instructions, descriptors, corpus_instructions);
    ap_uint<2> narrow_match = 0, wide_match = 0;
    regexVM_new<STACK_SIZE, 32768>(bitset, instructions, message, length, narrow_match, narrow_offsets);
    regexVM_new_wide<STACK_SIZE, 32768>(classes, packed, wide_message, length, wide_match, wide_offsets);
    regexVM_new_predecoded<STACK_SIZE, 32768>(classes, descriptors, wide_message, length, match, offsets);
    bool equal = match == narrow_match && match == wide_match;
    for (unsigned i = 0; i < 512; ++i)
        equal = equal && offsets[i] == narrow_offsets[i] && offsets[i] == wide_offsets[i];
    if (!equal) {
        std::fprintf(stderr, "VM layout mismatch: narrow=%u wide=%u decoded=%u\n", (unsigned)narrow_match,
                     (unsigned)wide_match, (unsigned)match);
        match = 2;
    }
#endif
}
} // namespace text
} // namespace data_analytics
} // namespace xf

// Headers have already been included, so these hooks affect only the corpus's
// compile call and its VM call. Its test list and Oniguruma checks stay intact.
#define xf_re_compile corpus_compile
#define regexVM_opt corpus_vm
#define main corpus_main
#include "test.cpp"
#undef main
#undef regexVM_opt
#undef xf_re_compile

int main(int argc, char** argv) {
    int result = corpus_main(argc, argv);
    std::printf("VM calls=%u, fitting 2048-byte rule slots=%u, max rule words=%u\n", corpus_calls, corpus_slot_fit,
                corpus_max_words);
    return result;
}
