#define XF_REGEX_VM_NEW_PROFILE
#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace xf::data_analytics::text;

static FILE *case_report = NULL;
static unsigned case_id = 0;

#ifdef COSIM_ACCEL
extern "C" unsigned int regex_vm_new_accel_perf(ap_uint<256> bitset[128], ap_uint<512> instructions[32],
                                                ap_uint<512> message[64], unsigned int length,
                                                ap_uint<16> offsets[512]);
#endif

static ap_uint<64> op(unsigned code, unsigned mode = 0, unsigned len = 0, unsigned a = 0, unsigned b = 0,
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

static ap_uint<64> literal(const char *s, unsigned len) {
    ap_uint<64> v = op(OP_STR_N, 0, len);
    for (unsigned k = 0; k < len; ++k)
        v.range(47 - 8 * k, 40 - 8 * k) = (unsigned char)s[k];
    return v;
}

static bool run_case(const char *name, const ap_uint<64> *program, const char *text, ap_uint<32> *bitset,
                     unsigned explicit_length = ~0u, int expected_match = -1) {
    ap_uint<64> narrow_instr[256];
    ap_uint<512> wide_instr[32];
    ap_uint<512> decoded_instr[256];
    ap_uint<64> configured_instr[256];
    ap_uint<32> narrow_msg[1024];
    ap_uint<512> wide_msg[64];
    ap_uint<256> wide_bitset[128];
    // The legacy scalar oracle speculatively reads capture slots even for
    // literal operands. Give those ignored prefetches a full 16-bit address
    // space so native C/RTL post-checking has no out-of-bounds C++ reads.
    static ap_uint<16> old_offsets[65537];
    ap_uint<16> narrow_offsets[512], wide_offsets[512], accel_offsets[512], decoded_offsets[512];
    for (int i = 0; i < 256; ++i)
        narrow_instr[i] = op(OP_END);
    for (int i = 0; i < 32; ++i)
        narrow_instr[i] = program[i];
    for (int i = 0; i < 32; ++i) {
        wide_instr[i] = 0;
        for (int k = 0; k < 8; ++k)
            wide_instr[i].range(64 * k + 63, 64 * k) = narrow_instr[8 * i + k];
    }
    ap_uint<512> empty_descriptor = (ap_uint<512>)op(OP_END) | ((ap_uint<512>)op(OP_END) << 64);
    for (int pc = 0; pc < 256; ++pc) {
        configured_instr[pc] = op(OP_END);
        decoded_instr[pc] = empty_descriptor;
    }
    // Configuration may deliver payload words in either order after headers.
    for (int k = 0; k < 32; ++k) {
        int pc = (unsigned)program[0].range(63, 56) & 1 ? 31 - k : k;
        configured_instr[pc] = narrow_instr[pc];
    }
    internal_new::prepareRule<256>(configured_instr, decoded_instr, 32);
    for (int i = 0; i < 128; ++i) {
        wide_bitset[i] = 0;
        for (int k = 0; k < 8; ++k)
            wide_bitset[i].range(32 * k + 31, 32 * k) = bitset[8 * i + k];
    }
    for (int i = 0; i < 1024; ++i)
        narrow_msg[i] = 0;
    for (int i = 0; i < 64; ++i)
        wide_msg[i] = 0;
    unsigned len = explicit_length == ~0u ? std::strlen(text) : explicit_length;
    for (unsigned i = 0; i < len; ++i) {
        narrow_msg[i >> 2].range(8 * (i & 3) + 7, 8 * (i & 3)) = (unsigned char)text[i];
        wide_msg[i >> 6].range(8 * (i & 63) + 7, 8 * (i & 63)) = (unsigned char)text[i];
    }
    for (int i = 0; i < 512; ++i)
        old_offsets[i] = narrow_offsets[i] = wide_offsets[i] = accel_offsets[i] = decoded_offsets[i] = 0xffff;

    ap_uint<2> old_match = 3, narrow_match = 3, wide_match = 3;
    ap_uint<2> accel_match = 3;
    // Use the scalar interpreter as the semantic oracle. regexVM_opt's
    // speculative second lane does not implement every synthetic POP/branch
    // combination correctly. The Apache fixture separately compares opt.
    regexVM<8192>(bitset, narrow_instr, narrow_msg, len, old_match, old_offsets);
    // The old interpreter overwrites its cached previous word when two
    // non-consuming instructions execute at a 32-bit boundary. Anchor cases
    // therefore use an explicit semantic expectation as their reference.
    if (expected_match >= 0) {
        old_match = expected_match;
        if (expected_match == 1) {
            old_offsets[0] = 0;
            old_offsets[1] = len;
        }
    }
    regexVM_new<8192, 256>(bitset, narrow_instr, narrow_msg, len, narrow_match, narrow_offsets);
    regexVM_new_wide<8192, 256>(bitset, wide_instr, wide_msg, len, wide_match, wide_offsets);
#ifdef COSIM_ACCEL
    accel_match = regex_vm_new_accel_perf(wide_bitset,
#ifdef XF_REGEX_VM_NEW_PREDECODED
                                          decoded_instr,
#else
                                          wide_instr,
#endif
                                          wide_msg, len, accel_offsets);
#else
    regexVM_new_wide<8192, 256>(wide_bitset, wide_instr, wide_msg, len, accel_match, accel_offsets);
#endif
    ap_uint<2> decoded_match = 3;
    regexVM_new_predecoded<8192, 256>(wide_bitset, decoded_instr, wide_msg, len, decoded_match, decoded_offsets);

    bool okay =
        old_match == narrow_match && old_match == wide_match && old_match == accel_match && old_match == decoded_match;
    for (int i = 0; i < 32; ++i)
        okay = okay && old_offsets[i] == narrow_offsets[i] && old_offsets[i] == wide_offsets[i] &&
               old_offsets[i] == accel_offsets[i] && old_offsets[i] == decoded_offsets[i];
    if (case_report)
        std::fprintf(case_report, "%u,%s,%u,%u,%u\n", case_id, name, len, (unsigned)old_match, okay);
    ++case_id;
    std::printf("%s: %s old=%u narrow=%u wide=%u range=%u..%u bytes=%u\n", name, okay ? "PASS" : "FAIL",
                (unsigned)old_match, (unsigned)narrow_match, (unsigned)wide_match, (unsigned)old_offsets[0],
                (unsigned)old_offsets[1], len);
    return okay;
}

int main() {
    std::setbuf(stdout, NULL);
    if (const char *path = std::getenv("RE_VM_CASE_REPORT")) {
        case_report = std::fopen(path, "w");
        if (!case_report)
            return 1;
        std::fprintf(case_report, "case_id,family,bytes,expected_match,passed\n");
    }
    ap_uint<32> bitset[1024];
    for (int i = 0; i < 1024; ++i)
        bitset[i] = 0;
    bitset[((unsigned)'a') >> 5][((unsigned)'a') & 31] = 1;

    int failures = 0;
    ap_uint<64> p[32];

    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = literal("abcd", 4);
    p[1] = literal("efgh", 4);
    p[2] = op(OP_END);
    failures += !run_case("literal-8-match", p, "abcdefgh", bitset);
    failures += !run_case("literal-8-fail", p, "abcdefgX", bitset);

    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_ANYCHAR_STAR);
    p[1] = literal("ab", 2);
    p[2] = op(OP_END);
    failures += !run_case("star-backtrack-match", p, "012345678901234567890123456789ab", bitset);
    failures += !run_case("star-backtrack-fail", p, "012345678901234567890123456789ac", bitset);

    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_CCLASS, 0, 1, 0);
    p[1] = op(OP_END);
    failures += !run_case("class-match", p, "a", bitset);
    failures += !run_case("class-fail", p, "b", bitset);

    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_PUSH, 1, 0, 3);
    p[1] = literal("a", 1);
    p[2] = op(OP_END);
    p[3] = literal("b", 1);
    p[4] = op(OP_END);
    failures += !run_case("alternate-first", p, "a", bitset);
    failures += !run_case("alternate-second", p, "b", bitset);
    failures += !run_case("alternate-fail", p, "c", bitset);

    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_BEGIN_BUF);
    p[1] = literal("xy", 2);
    p[2] = op(OP_END_BUF);
    p[3] = op(OP_END);
    failures += !run_case("anchors-match", p, "xy", bitset);
    failures += !run_case("anchors-fail", p, "xyz", bitset);

    // The suffix forces a greedy class loop to give back bytes, including at
    // a vector-window boundary. A possessive loop must never give them back.
    for (int possessive = 0; possessive < 2; ++possessive) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        unsigned exit = possessive ? 5 : 4;
        p[0] = op(OP_MEM_START, 0, 0, 2);
        p[1] = op(OP_PUSH, 1, 0, exit);
        p[2] = op(OP_CCLASS, 0, 1, 0);
        p[3] = possessive ? op(OP_POP) : op(OP_JUMP, 0, 0, 1);
        if (possessive)
            p[4] = op(OP_JUMP, 0, 0, 1);
        p[exit] = op(OP_MEM_END, 0, 0, 3);
        p[exit + 1] = literal("ab", 2);
        p[exit + 2] = op(OP_END_BUF);
        for (unsigned n : {0u, 1u, 7u, 8u, 23u, 24u, 25u, 63u, 64u, 65u, 127u, 4086u}) {
            std::string text(n, 'a');
            text += "ab";
            failures += !run_case(possessive ? "class-possessive" : "class-greedy", p, text.c_str(), bitset);
        }
    }

    // Retry filtering must retain suffixes straddling an unread window edge.
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_ANYCHAR_STAR);
    p[1] = literal("abcd", 4);
    p[2] = op(OP_END_BUF);
    for (unsigned n = 0; n < 80; ++n) {
        std::string text(n, 'x');
        text += "abcd";
        failures += !run_case("star-window-edge", p, text.c_str(), bitset);
    }

    // Seeded loops cross all message alignments and class bitmap word lanes.
    std::mt19937 random(1902);
    for (unsigned trial = 0; trial < 160; ++trial) {
        unsigned base = 8 * (trial % 128);
        for (int k = 0; k < 8; ++k)
            bitset[base + k] = random();
        // Use a known member so long runs exercise more than just loop exit.
        bitset[base + ('a' >> 5)]['a' & 31] = 1;
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        unsigned prefix = trial % 4;
        for (unsigned k = 0; k < prefix; ++k)
            p[k] = op(OP_ANYCHAR);
        p[prefix] = op(OP_PUSH, 1, 0, prefix + 3);
        p[prefix + 1] = op(OP_CCLASS, 0, 1, base);
        p[prefix + 2] = op(OP_JUMP, 0, 0, prefix);
        p[prefix + 3] = literal("ab", 2);
        p[prefix + 4] = op(OP_END_BUF);
        std::string text(prefix, 'x');
        text.append(random() % 200, 'a');
        text += trial & 1 ? "ab" : "ac";
        failures += !run_case("random-class-loop", p, text.c_str(), bitset);
    }

    // Lazy CCLASS/PUSH backedges must retain the earliest successful suffix,
    // including after a promising prefix fails and a later alternative wins.
    for (int variant = 0; variant < 3; ++variant) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        for (int i = 0; i < 8; ++i)
            bitset[8 + i] = ~0u;
        bitset[8][10] = 0;
        p[0] = op(OP_JUMP, 0, 0, 2);
        p[1] = op(OP_CCLASS, 0, 1, 8);
        p[2] = op(OP_PUSH, 1, 0, 1);
        p[3] = variant == 2 ? op(OP_MEM_START, 0, 0, 2) : op(OP_MEM_END, 0, 0, 3);
        p[4] = variant == 1 ? op(OP_PUSH, 1, 0, 8) : literal("END", 3);
        if (variant == 1) {
            p[5] = literal("END", 3);
            p[6] = op(OP_JUMP, 0, 0, 9);
            p[8] = literal("FIN", 3);
            p[9] = op(OP_END_BUF);
        } else {
            p[5] = op(OP_END_BUF);
        }
        for (unsigned n : {0u, 1u, 2u, 7u, 23u, 24u, 25u, 63u, 64u, 65u, 180u, 4080u}) {
            std::string text(n, 'a');
            text += variant == 1 ? "ENDxFIN" : "END";
            failures += !run_case("lazy-class-suffix", p, text.c_str(), bitset);
            text.back() = 'X';
            failures += !run_case("lazy-class-no-suffix", p, text.c_str(), bitset);
        }
    }

    // Fuse a delimiter, capture, mandatory first class byte, and repetition.
    // The first and repeated classes deliberately differ.
    for (int i = 0; i < 8; ++i) {
        bitset[i] = 0;
        bitset[16 + i] = 0;
    }
    bitset['a' >> 5]['a' & 31] = 1;
    bitset[16 + ('b' >> 5)]['b' & 31] = 1;
    for (unsigned prefix : {0u, 1u, 4u}) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        unsigned c = prefix ? 1 : 0;
        if (prefix)
            p[0] = literal("::::", prefix);
        p[c] = op(OP_MEM_START, 0, 0, 2);
        p[c + 1] = op(OP_CCLASS, 0, 1, 0);
        p[c + 2] = op(OP_PUSH, 1, 0, c + 6);
        p[c + 3] = op(OP_CCLASS, 0, 1, 16);
        p[c + 4] = op(OP_POP);
        p[c + 5] = op(OP_JUMP, 0, 0, c + 2);
        p[c + 6] = op(OP_MEM_END, 0, 0, 3);
        p[c + 7] = literal("END", 3);
        p[c + 8] = op(OP_END_BUF);
        for (unsigned n : {0u, 1u, 20u, 24u, 25u, 63u, 120u}) {
            std::string text(prefix, ':');
            text += 'a';
            text.append(n, 'b');
            text += "END";
            failures += !run_case("prefixed-plus-class", p, text.c_str(), bitset);
            text[prefix] = 'b';
            failures += !run_case("prefixed-plus-class-fail", p, text.c_str(), bitset);
        }
    }
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = literal(" ", 1);
    p[1] = op(OP_PUSH_OR_JUMP_EXACT1, 1, 0, 4, 0x2000);
    p[2] = literal(" ", 1);
    p[3] = op(OP_JUMP, 0, 0, 1);
    p[4] = literal(" END", 4);
    p[5] = op(OP_END_BUF);
    for (unsigned n : {0u, 1u, 2u, 24u, 25u, 64u, 128u}) {
        std::string text(n, ' ');
        text += "END";
        failures += !run_case("peek-loop-give-back", p, text.c_str(), bitset);
    }

    // Nested capture snapshots must see forwarded writes and restore older
    // values when failure crosses several capture and branch stack entries.
    for (unsigned groups = 1; groups <= 4; ++groups) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        unsigned pc = 0;
        for (unsigned g = 0; g < groups; ++g) {
            p[pc++] = op(OP_MEM_START, 0, 0, 2 + 2 * g);
            p[pc++] = op(OP_MEM_END, 0, 0, 3 + 2 * g);
        }
        unsigned branch = pc++;
        for (unsigned g = 0; g < groups; ++g) {
            p[pc++] = op(OP_MEM_START_PUSH, 0, 0, 2 + 2 * g);
            p[pc++] = literal("a", 1);
            p[pc++] = op(OP_MEM_END, 0, 0, 3 + 2 * g);
        }
        p[pc++] = literal("Z", 1);
        p[pc++] = op(OP_END_BUF);
        p[pc++] = op(OP_END);
        p[branch] = op(OP_PUSH, 1, 0, pc);
        p[pc++] = literal("aaaa", groups);
        p[pc++] = op(OP_END_BUF);
        std::string text(groups, 'a');
        failures += !run_case("nested-capture-restore", p, text.c_str(), bitset);
        text += 'Z';
        failures += !run_case("nested-capture-no-restore", p, text.c_str(), bitset);
    }

    // Exercise all eight bitmap words, including NUL and bytes above 127.
    for (unsigned ch = 0; ch < 256; ++ch) {
        for (int i = 0; i < 8; ++i)
            bitset[i] = 0;
        bitset[ch >> 5][ch & 31] = 1;
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        p[0] = op(OP_PUSH, 1, 0, 3);
        p[1] = op(OP_CCLASS, 0, 1, 0);
        p[2] = op(OP_JUMP, 0, 0, 0);
        p[3] = op(OP_END_BUF);
        std::string text(65, (char)ch);
        failures += !run_case("class-all-byte-values", p, text.data(), bitset, text.size());
        text.back() = (char)(ch ^ 1);
        failures += !run_case("class-all-byte-values-fail", p, text.data(), bitset, text.size());
    }
    // BEGIN_LINE at a nonzero pointer needs the previous byte. Test both
    // narrow-word and wide-word boundaries, and the adjacent alignments.
    for (unsigned n : {3u, 4u, 5u, 63u, 64u, 65u, 127u, 128u}) {
        for (int i = 0; i < 8; ++i)
            bitset[i] = ~0u;
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        p[0] = op(OP_REPEAT, 0, 0, 1, n, n);
        p[1] = op(OP_CCLASS, 0, 1, 0);
        p[2] = op(OP_REPEAT_INC, 0, 0, 1, n, n);
        p[3] = op(OP_BEGIN_LINE);
        p[4] = literal("Z", 1);
        p[5] = op(OP_END_BUF);
        std::string text(n, 'x');
        text += 'Z';
        text[n - 1] = '\n';
        failures += !run_case("begin-line-boundary", p, text.c_str(), bitset, text.size(), 1);
        text[n - 1] = 'x';
        failures += !run_case("begin-line-boundary-fail", p, text.c_str(), bitset, text.size(), 0);
    }

    // Exercise scalar-heavy paths as well as accelerated repetition shapes.
    for (int i = 0; i < 8; ++i)
        bitset[i] = ~0u;
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_BEGIN_BUF);
    for (unsigned i = 1; i <= 28; ++i)
        p[i] = op(OP_CCLASS, 0, 1, 0);
    p[29] = op(OP_END_BUF);
    for (unsigned n : {0u, 27u, 28u, 29u}) {
        std::string text(n, 'x');
        failures += !run_case("scalar-class-chain", p, text.c_str(), bitset);
    }
    for (unsigned atoms = 1; atoms <= 29; ++atoms) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        std::string text;
        for (unsigned i = 0; i < atoms; ++i) {
            unsigned n = 1 + i % 4;
            p[i] = literal("abcd", n);
            text.append("abcd", n);
        }
        p[atoms] = op(OP_END_BUF);
        failures += !run_case("literal-chain", p, text.c_str(), bitset);
        text.back() ^= 1;
        failures += !run_case("literal-chain-fail", p, text.c_str(), bitset);
    }
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    bitset[0] = 0;
    bitset['a' >> 5] = 1u << ('a' & 31);
    p[0] = op(OP_REPEAT, 0, 0, 1, 2, 5);
    p[1] = op(OP_CCLASS, 0, 1, 0);
    p[2] = op(OP_REPEAT_INC, 0, 0, 1, 2, 5);
    p[3] = literal("b", 1);
    p[4] = op(OP_END_BUF);
    for (unsigned n = 0; n <= 7; ++n) {
        std::string text(n, 'a');
        text += 'b';
        failures += !run_case("bounded-repeat", p, text.c_str(), bitset);
    }
    for (unsigned depth = 1; depth <= 2; ++depth) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        unsigned pc = 0;
        p[pc++] = op(OP_MARK, 0, 0, 7);
        if (depth == 2)
            p[pc++] = op(OP_MARK, 0, 0, 8);
        unsigned branch = pc++;
        p[pc++] = op(OP_MEM_START_PUSH, 0, 0, 2);
        p[pc++] = literal("a", 1);
        p[pc++] = op(OP_MEM_END, 0, 0, 3);
        p[pc++] = op(OP_POP_TO_MARK, 0, 0, 7);
        p[branch] = op(OP_PUSH, 1, 0, pc);
        p[pc++] = literal("b", 1);
        p[pc++] = op(OP_END_BUF);
        failures += !run_case("mark-pop-capture-restore", p, "ab", bitset);
        failures += !run_case("mark-pop-fail", p, "aX", bitset);
    }
    for (unsigned branch = 0; branch <= 1; ++branch) {
        for (int i = 0; i < 32; ++i)
            p[i] = op(OP_END);
        p[0] = literal("abc", 3);
        p[1] = op(OP_STEP_BACK_START, 0, 0, 5, 2, branch);
        p[2] = literal("bc", 2);
        p[3] = op(OP_END_BUF);
        p[5] = literal("bc", 2);
        p[6] = op(OP_END_BUF);
        failures += !run_case("step-back-match", p, "abc", bitset);
        p[1] = op(OP_STEP_BACK_START, 0, 0, 5, 4, branch);
        failures += !run_case("step-back-underflow", p, "abc", bitset);
    }
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_ANYCHAR_STAR);
    p[1] = op(OP_MEM_START, 0, 0, 2);
    p[2] = op(OP_CCLASS, 0, 1, 0);
    p[3] = op(OP_MEM_END, 0, 0, 3);
    p[4] = op(OP_END_BUF);
    for (unsigned n : {0u, 1u, 23u, 24u, 25u, 63u, 64u, 65u, 255u, 1023u, 4088u}) {
        std::string text(n, 'a');
        failures += !run_case("star-control-suffix", p, text.c_str(), bitset);
    }
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_ANYCHAR_STAR);
    p[1] = op(OP_END_LINE);
    p[2] = literal("\n", 1);
    p[3] = op(OP_END_BUF);
    for (unsigned n : {0u, 1u, 23u, 24u, 63u, 64u, 65u, 255u}) {
        std::string text(n, 'a');
        text += '\n';
        failures += !run_case("star-line-boundary", p, text.c_str(), bitset);
    }
    // A nullable suffix must retain all lazy alternatives; its first byte
    // cannot be used as a rejection filter.
    for (int i = 0; i < 32; ++i)
        p[i] = op(OP_END);
    p[0] = op(OP_JUMP, 0, 0, 2);
    p[1] = op(OP_CCLASS, 0, 1, 0);
    p[2] = op(OP_PUSH, 1, 0, 1);
    p[3] = op(OP_PUSH, 1, 0, 6);
    p[4] = literal("b", 1);
    p[5] = op(OP_JUMP, 0, 0, 7);
    p[6] = literal("", 0);
    p[7] = op(OP_END_BUF);
    for (unsigned n : {0u, 1u, 23u, 24u, 25u, 64u}) {
        std::string text(n, 'a');
        failures += !run_case("lazy-nullable-suffix", p, text.c_str(), bitset);
    }

    // Valid, finite backtracking can exceed the former fixed instruction
    // budget without overflowing the stack. Do not silently reject it.
    for (int i = 0; i < 32; ++i) p[i] = op(OP_END);
    for (unsigned loop = 0; loop < 3; ++loop) {
        p[3 * loop] = op(OP_PUSH, 1, 0, 3 * loop + 3);
        p[3 * loop + 1] = op(OP_CCLASS, 0, 1, 0);
        p[3 * loop + 2] = op(OP_JUMP, 0, 0, 3 * loop);
    }
    p[9] = literal("b", 1); p[10] = op(OP_END_BUF);
    for (unsigned n : {32u, 96u}) {
        std::string text(n, 'a');
        failures += !run_case("overlapping-repetition-fail", p, text.c_str(), bitset);
    }

    std::printf("differential failures=%d cases=%u\n", failures, case_id);
    if (case_report)
        std::fclose(case_report);
    return failures;
}
