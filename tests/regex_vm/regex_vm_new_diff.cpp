#define XF_REGEX_VM_NEW_PROFILE
#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

#include <cstdio>
#include <cstring>

using namespace xf::data_analytics::text;

static ap_uint<64> op(unsigned code,
                      unsigned mode = 0,
                      unsigned len = 0,
                      unsigned a = 0,
                      unsigned b = 0,
                      unsigned c = 0) {
    ap_uint<64> v = 0;
    v.range(63, 56) = code;
    v.range(55, 52) = mode;
    v.range(51, 48) = len;
    v.range(47, 32) = a;
    v.range(31, 16) = b;
    v.range(15, 0) = c;
    return v;
}

static ap_uint<64> literal(const char* s, unsigned len) {
    ap_uint<64> v = op(OP_STR_N, 0, len);
    for (unsigned k = 0; k < len; ++k) v.range(47 - 8 * k, 40 - 8 * k) = (unsigned char)s[k];
    return v;
}

static bool run_case(const char* name, const ap_uint<64>* program, const char* text, ap_uint<32>* bitset) {
    ap_uint<64> narrow_instr[256];
    ap_uint<512> wide_instr[32];
    ap_uint<32> narrow_msg[128];
    ap_uint<512> wide_msg[8];
    ap_uint<16> old_offsets[32], narrow_offsets[32], wide_offsets[32];
    for (int i = 0; i < 256; ++i) narrow_instr[i] = op(OP_END);
    for (int i = 0; i < 32; ++i) narrow_instr[i] = program[i];
    for (int i = 0; i < 32; ++i) {
        wide_instr[i] = 0;
        for (int k = 0; k < 8; ++k)
            wide_instr[i].range(64 * k + 63, 64 * k) = narrow_instr[8 * i + k];
    }
    for (int i = 0; i < 128; ++i) narrow_msg[i] = 0;
    for (int i = 0; i < 8; ++i) wide_msg[i] = 0;
    unsigned len = std::strlen(text);
    for (unsigned i = 0; i < len; ++i) {
        narrow_msg[i >> 2].range(8 * (i & 3) + 7, 8 * (i & 3)) = (unsigned char)text[i];
        wide_msg[i >> 6].range(8 * (i & 63) + 7, 8 * (i & 63)) = (unsigned char)text[i];
    }
    for (int i = 0; i < 32; ++i) old_offsets[i] = narrow_offsets[i] = wide_offsets[i] = 0xffff;

    ap_uint<2> old_match = 3, narrow_match = 3, wide_match = 3;
    regexVM_opt<128>(bitset, narrow_instr, narrow_msg, len, old_match, old_offsets);
    regexVM_new<128, 256>(bitset, narrow_instr, narrow_msg, len, narrow_match, narrow_offsets);
    regexVM_new_wide<128, 256>(bitset, wide_instr, wide_msg, len, wide_match, wide_offsets);

    bool okay = old_match == narrow_match && old_match == wide_match;
    for (int i = 0; i < 8; ++i)
        okay = okay && old_offsets[i] == narrow_offsets[i] && old_offsets[i] == wide_offsets[i];
    std::printf("%s: %s old=%u narrow=%u wide=%u range=%u..%u\n", name, okay ? "PASS" : "FAIL",
                (unsigned)old_match, (unsigned)narrow_match, (unsigned)wide_match,
                (unsigned)old_offsets[0], (unsigned)old_offsets[1]);
    return okay;
}

int main() {
    std::setbuf(stdout, NULL);
    ap_uint<32> bitset[1024];
    for (int i = 0; i < 1024; ++i) bitset[i] = 0;
    bitset[((unsigned)'a') >> 5][((unsigned)'a') & 31] = 1;

    int failures = 0;
    ap_uint<64> p[32];

    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    p[0] = literal("abcd", 4); p[1] = literal("efgh", 4); p[2] = op(OP_END);
    failures += !run_case("literal-8-match", p, "abcdefgh", bitset);
    failures += !run_case("literal-8-fail", p, "abcdefgX", bitset);

    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    p[0] = op(OP_ANYCHAR_STAR); p[1] = literal("ab", 2); p[2] = op(OP_END);
    failures += !run_case("star-backtrack-match", p, "012345678901234567890123456789ab", bitset);
    failures += !run_case("star-backtrack-fail", p, "012345678901234567890123456789ac", bitset);

    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    p[0] = op(OP_CCLASS, 0, 1, 0); p[1] = op(OP_END);
    failures += !run_case("class-match", p, "a", bitset);
    failures += !run_case("class-fail", p, "b", bitset);

    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    p[0] = op(OP_PUSH, 1, 0, 3); p[1] = literal("a", 1); p[2] = op(OP_END);
    p[3] = literal("b", 1); p[4] = op(OP_END);
    failures += !run_case("alternate-first", p, "a", bitset);
    failures += !run_case("alternate-second", p, "b", bitset);
    failures += !run_case("alternate-fail", p, "c", bitset);

    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    p[0] = op(OP_BEGIN_BUF); p[1] = literal("xy", 2); p[2] = op(OP_END_BUF); p[3] = op(OP_END);
    failures += !run_case("anchors-match", p, "xy", bitset);
    failures += !run_case("anchors-fail", p, "xyz", bitset);

    std::printf("differential failures=%d\n", failures);
    return failures;
}
