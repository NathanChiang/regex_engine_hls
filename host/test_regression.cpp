// Dependency-free regression using the actual Vitis ap_uint and kernel code.
// No Oniguruma, OpenCL, or generated regex compiler library is needed.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include "re_engine_kernel.hpp"
#include "re_engine_iova_table.hpp"
#include "re_engine_rule_table.hpp"
#include "xf_data_analytics/text/enums.hpp"

#define CHECK(c) do { if (!(c)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); \
} } while (0)

#ifndef RE_HLS_COSIM
static cfg_input tc = {};
static re_iova_cfg_status is;
static re_iova_lookup_status il;
static void itick(uint64_t address = 0, bool valid = false) {
    re_engine_iova_table(tc, address, valid, is, il);
}
static bool icmd(unsigned cmd, unsigned job = 0, uint64_t base = 0, uint64_t limit = 0) {
    tc.cmd = cmd; tc.valid = 1; tc.job_id = job; tc.arg0 = base; tc.arg1 = limit;
    bool ack = false;
    for (int n = 0; n < 4096; ++n) {
        itick();
        if (is.ack || is.full) { ack = is.ack; break; }
        CHECK(n < 4095);
    }
    tc.valid = 0; itick(); tc.cmd = RE_CFG_NONE; itick();
    return ack;
}
static int find_iova(uint64_t address) {
    tc.cmd = RE_CFG_NONE; tc.valid = 0; itick(address, true);
    for (int n = 0; n < 8; ++n) {
        itick(address, true);
        if (il.ack) {
            int result = il.hit ? il.job_id.to_uint() : -1;
            for (int hold = 0; hold < 3; ++hold) { itick(address, true); CHECK(!il.ack); }
            itick(); itick();
            return result;
        }
    }
    CHECK(false); return -1;
}
static void test_iova() {
    CHECK(icmd(RE_CFG_CLEAR));
    CHECK(find_iova(0) == -1);
    CHECK(icmd(RE_CFG_ADDR_WRITE, 3, 4095, 8193));
    CHECK(find_iova(4094) == -1);
    CHECK(find_iova(4095) == 3);
    CHECK(find_iova(8192) == 3);
    CHECK(find_iova(8193) == -1);
    CHECK(icmd(RE_CFG_ADDR_WRITE, 4, 4096, 8192));
    CHECK(find_iova(5000) == 3);
    CHECK(icmd(RE_CFG_ADDR_DELETE, 3, 4095, 8193));
    CHECK(find_iova(5000) == 4);
    CHECK(icmd(RE_CFG_ADDR_WRITE, 5, 4096, 8192)); // reuse index 0: higher priority
    CHECK(find_iova(5000) == 5);
    CHECK(icmd(RE_CFG_ADDR_DELETE, 99, 1, 2));
    CHECK(icmd(RE_CFG_CLEAR));
    // Same hash bucket, disjoint physical pages. The ninth insert must fail.
    for (unsigned i = 0; i < 8; ++i)
        CHECK(icmd(RE_CFG_ADDR_WRITE, i, (uint64_t(i) << 22) + 4096, (uint64_t(i) << 22) + 8192));
    // Bucket 0 has room but bucket 1 is full: precheck must be atomic.
    CHECK(!icmd(RE_CFG_ADDR_WRITE, 30, 0, 8192));
    CHECK(find_iova(0) == -1);
    for (unsigned i = 0; i < 8; ++i) CHECK(find_iova((uint64_t(i) << 22) + 4096) == int(i));
    CHECK(icmd(RE_CFG_ADDR_DELETE, 3, (3ULL << 22) + 4096, (3ULL << 22) + 8192));
    CHECK(icmd(RE_CFG_ADDR_WRITE, 30, 0, 8192));
    CHECK(find_iova(0) == 30);
    CHECK(icmd(RE_CFG_CLEAR));
    CHECK(icmd(RE_CFG_ADDR_WRITE, 9, 0x3fffff, 0xc00001)); // >1024 pages, wraps hash
    CHECK(find_iova(0x3ffffe) == -1);
    CHECK(find_iova(0x800000) == 9);
    CHECK(find_iova(0xc00000) == 9);
    CHECK(find_iova(0xc00001) == -1);
    CHECK(icmd(RE_CFG_ADDR_DELETE, 9, 0x3fffff, 0xc00001));
    CHECK(find_iova(0x800000) == -1);
    CHECK(icmd(RE_CFG_ADDR_WRITE, 10, UINT64_MAX - 5000, UINT64_MAX));
    CHECK(find_iova(UINT64_MAX - 1) == 10);
    CHECK(find_iova(UINT64_MAX) == -1);
    CHECK(icmd(RE_CFG_CLEAR));
    // Exercise the full logical directory and its last index.
    for (unsigned i = 0; i < 1024; ++i)
        CHECK(icmd(RE_CFG_ADDR_WRITE, i, uint64_t(i) * 4096, uint64_t(i + 1) * 4096));
    CHECK(!icmd(RE_CFG_ADDR_WRITE, 0, 1ULL << 32, (1ULL << 32) + 1));
    CHECK(find_iova(1023 * 4096) == 1023);
    CHECK(icmd(RE_CFG_ADDR_DELETE, 1023, 1023 * 4096, 1024 * 4096));
    CHECK(icmd(RE_CFG_ADDR_WRITE, 31, 1ULL << 32, (1ULL << 32) + 1));
    CHECK(find_iova(1ULL << 32) == 31);
    CHECK(icmd(RE_CFG_CLEAR));
    // Seeded reference comparison, including overlaps and directory-slot reuse.
    struct Mapping { bool valid; unsigned job; uint64_t base, limit; } model[64] = {};
    uint32_t seed = 0x1957;
    auto random = [&]() -> uint32_t { seed = seed * 1664525u + 1013904223u; return seed; };
    for (int step = 0; step < 500; ++step) {
        unsigned slot = random() % 64;
        if (model[slot].valid) {
            Mapping m = model[slot];
            CHECK(icmd(RE_CFG_ADDR_DELETE, m.job, m.base, m.limit));
            model[slot].valid = false;
        } else {
            unsigned free = 0;
            while (free < 64 && model[free].valid) ++free;
            if (free < 64) {
                uint64_t base = random() % (128 * 4096);
                uint64_t limit = base + 1 + random() % 8192;
                unsigned job = random() % 32;
                if (icmd(RE_CFG_ADDR_WRITE, job, base, limit)) model[free] = {true, job, base, limit};
            }
        }
        for (int q = 0; q < 8; ++q) {
            uint64_t addr = random() % (130 * 4096);
            int want = -1;
            for (unsigned i = 0; i < 64; ++i)
                if (model[i].valid && addr >= model[i].base && addr < model[i].limit) { want = model[i].job; break; }
            CHECK(find_iova(addr) == want);
        }
    }
    std::puts("PASS IOVA boundaries, priority, overflow atomicity, capacity, 4000 reference lookups");
}

static re_rule_cfg_status rs;
static re_rule_lookup_status rl;
static void rtick(const re_iova_lookup_status& in) {
    re_engine_rule_table(tc, in, rs, rl);
}
static void rtick() {
    re_iova_lookup_status idle;
    idle.ack = 0; idle.hit = 0; idle.job_id = 0;
    rtick(idle);
}
static bool rcmd(unsigned cmd, unsigned job = 0, unsigned offset = 0, uint64_t word = 0) {
    tc = {}; tc.cmd = cmd; tc.valid = 1; tc.job_id = job; tc.arg0 = offset; tc.arg1 = word;
    rtick(); bool ack = rs.ack;
    for (int n = 0; n < 3; ++n) { rtick(); CHECK(!rs.ack); }
    tc.valid = 0; rtick(); tc.cmd = RE_CFG_NONE; rtick();
    return ack;
}
static void find_rule(unsigned job, bool iova_hit = true) {
    tc.cmd = RE_CFG_NONE; tc.valid = 0;
    re_iova_lookup_status in = {}; in.ack = 1; in.hit = iova_hit; in.job_id = job;
    rtick(in);
    for (int n = 0; n < 40 && !rl.ack; ++n) rtick();
    CHECK(rl.ack);
}
static void test_rule() {
    const unsigned last = RE_RULE_TABLE_ENTRIES - 1;
    CHECK(rcmd(RE_CFG_CLEAR));
    find_rule(0); CHECK(!rl.hit);
    CHECK(!rcmd(RE_CFG_RULE_WRITE, RE_RULE_TABLE_ENTRIES));
    CHECK(!rcmd(RE_CFG_RULE_WRITE, 0, 1));
    CHECK(!rcmd(RE_CFG_RULE_WRITE, 0, 2048));
    CHECK(rcmd(RE_CFG_RULE_WRITE, last, 2040, UINT64_MAX));
    find_rule(last); CHECK(rl.hit && rl.job_id == last);
    for (unsigned job = 0; job < RE_RULE_TABLE_ENTRIES; ++job)
        for (unsigned w = 0; w < 256; ++w)
            CHECK(rcmd(RE_CFG_RULE_WRITE, job, w * 8, (uint64_t(job + 1) << 32) | w));
    CHECK(rs.full);
    for (unsigned job = 0; job < RE_RULE_TABLE_ENTRIES; ++job) {
        find_rule(job); CHECK(rl.hit);
    }
    CHECK(rcmd(RE_CFG_RULE_DELETE, last)); CHECK(!rs.full);
    CHECK(rcmd(RE_CFG_RULE_WRITE, last, 8, 42));
    find_rule(last); CHECK(rl.hit);
    find_rule(RE_RULE_TABLE_ENTRIES); CHECK(!rl.hit);
    find_rule(last, false); CHECK(!rl.hit);
    CHECK(rcmd(RE_CFG_CLEAR)); find_rule(last); CHECK(!rl.hit);
    std::puts("PASS rule entries: partial write, delete/reallocate, bounds");
}
#ifdef RE_HLS_QUICK_CSIM
static void test_rule_replicas() {
    const uint64_t header = (uint64_t(1) << 48) | (uint64_t(1) << 32) | 1;
    CHECK(rcmd(RE_CFG_CLEAR));
    CHECK(rcmd(RE_CFG_RULE_WRITE, 0, 0, 7));
    CHECK(rcmd(RE_CFG_RULE_WRITE, 0, 8, header));
    CHECK(rcmd(RE_CFG_RULE_WRITE, 0, 16, 0x0123456789abcdefULL));
    for (unsigned w = 0; w < 4; ++w)
        CHECK(rcmd(RE_CFG_RULE_WRITE, 0, 24 + 8 * w,
                   (uint64_t(2 * w + 2) << 32) | (2 * w + 1)));
    find_rule(0);
    CHECK(rl.hit && rl.config_valid && rl.cpgp_nm == 1);
    for (int p = 0; p < RE_PU_NM; ++p) {
        CHECK(re_rule_instr_mem[p][0][0] == 0x0123456789abcdefULL);
        CHECK(re_rule_bitset_mem[p][0][0] == 1);
        CHECK(re_rule_bitset_mem[p][0][7] == 8);
    }
    CHECK(!rcmd(RE_CFG_RULE_WRITE, RE_RULE_TABLE_ENTRIES));
    // The inline table model is also called by reEngineKernel in this CSim
    // executable. Do not leak a valid entry into the following kernel test.
    CHECK(rcmd(RE_CFG_CLEAR));
    std::puts("PASS rule storage: all 16 PU replicas and bounds");
}
#endif
#endif

static hls::stream<DmaAxisBeat> dma;
static hls::stream<RePerfRecord> perf;
static cfg_input cfg = {};
static cfg_output status;
static match_out result;
static ap_uint<1> result_ack = 0;
static void tick() { reEngineKernel(dma, perf, cfg, status, result_ack, result); }
static bool command(unsigned cmd, unsigned job = 0, uint64_t a = 0, uint64_t b = 0) {
    cfg.valid = 0; cfg.cmd = cmd; tick(); tick();
    cfg.job_id = job; cfg.arg0 = a; cfg.arg1 = b; cfg.valid = 1;
    bool done = false;
    for (int n = 0; n < 4096; ++n) {
        tick();
        if (status.ack || status.full) { done = status.ack; break; }
        CHECK(n < 4095);
    }
    // Simulate a slow MMIO poller holding valid after completion.
    for (int i = 0; i < 4; ++i) { tick(); CHECK(bool(status.ack) == done); }
    cfg.valid = 0; tick(); tick();
    CHECK(!status.ack);
    cfg.cmd = RE_CFG_NONE; tick(); tick();
    return done;
}
static uint64_t op(unsigned code) { return uint64_t(code) << 56; }
static void load_rule(bool literal, unsigned groups = 1, unsigned job = 1) {
    using namespace xf::data_analytics::text::enums;
    std::vector<uint64_t> instructions;
    if (literal) {
        instructions.push_back(op(OP_BEGIN_BUF));
        instructions.push_back(op(OP_STR_N) | (3ULL << 48) | (0x61626300ULL << 16));
        instructions.push_back(op(OP_END_BUF));
    }
    instructions.push_back(op(OP_END));
    instructions.push_back(op(OP_END)); // speculative instruction read guard
    CHECK(command(RE_CFG_RULE_DELETE, job));
    CHECK(command(RE_CFG_RULE_WRITE, job, 0, instructions.size() + 2));
    CHECK(command(RE_CFG_RULE_WRITE, job, 8, (uint64_t(groups) << 48) | instructions.size()));
    for (unsigned i = 0; i < instructions.size(); ++i)
        CHECK(command(RE_CFG_RULE_WRITE, job, (i + 2) * 8, instructions[i]));
}
static void packet(unsigned length, uint64_t addr = 0x1001, bool sparse = false, bool sop = true) {
    unsigned beats = (length + 63) / 64;
    for (unsigned b = 0; b < beats; ++b) {
        DmaAxisBeat beat = {};
        beat.data = 0;
        if (b == 0) beat.data.range(23, 0) = 0x636261;
        unsigned bytes = length - b * 64; if (bytes > 64) bytes = 64;
        beat.keep = bytes == 64 ? UINT64_MAX : ((1ULL << bytes) - 1);
        if (sparse && b == 0) beat.keep = 5;
        beat.strb = beat.keep;
        beat.user[0] = (b == 0 && sop); beat.user.range(64, 1) = addr;
        beat.last = b + 1 == beats; dma.write(beat);
    }
}
static std::vector<uint32_t> collect(unsigned words, uint64_t address = 0x1001) {
    std::vector<uint32_t> out;
    for (int n = 0; n < 256 && !result.valid; ++n) tick();
    CHECK(result.valid && result.address == address && result.halt);
    while (out.size() < words) {
        CHECK(result.valid);
        ap_uint<512> held = result.output_buf;
        for (int n = 0; n < 3; ++n) { tick(); CHECK(result.valid && result.output_buf == held); }
        for (unsigned i = 0; i < 16 && out.size() < words; ++i)
            out.push_back(result.output_buf.range(32 * i + 31, 32 * i).to_uint());
        result_ack = 1; tick(); // exactly one chunk per rising edge
        CHECK(result.ack);
        bool next_valid = result.valid; ap_uint<512> next = result.output_buf;
        for (int n = 0; n < 3; ++n) { tick(); CHECK(bool(result.valid) == next_valid && result.output_buf == next); }
        result_ack = 0; tick();
        CHECK(!result.ack);
    }
    CHECK(!result.valid); CHECK(dma.empty());
    return out;
}
static void dropped_packet() {
    for (int n = 0; n < 160; ++n) {
        tick(); CHECK(!result.valid);
        if (dma.empty() && status.cmd_transition_allowed) return;
    }
    CHECK(false);
}
static void test_kernel() {
#if defined(RE_HLS_QUICK_CSIM) || \
    (!defined(RE_HLS_EXHAUSTIVE_CSIM) && !defined(RE_HLS_COSIM))
    // C simulation zero-initializes static table memories; avoid the RTL
    // CLEAR sweep, which intentionally invalidates one full bucket per step.
    tick(); tick();
    CHECK(!status.engine_ready);
    CHECK(command(RE_CFG_ADDR_WRITE, 1, 0x1000, 0x2000));
    load_rule(true); CHECK(status.engine_ready);
    packet(3, 0x2000); // end-exclusive mapping boundary is a clean miss
    dropped_packet();
    packet(3, 0x1001, true); // mapped packet with malformed TKEEP
    dropped_packet();
    CHECK(command(RE_CFG_RULE_DELETE, 1));
    CHECK(!status.engine_ready);
    std::puts("PASS kernel smoke: configuration, lookup miss, and malformed DMA drop");
#else
#ifdef RE_HLS_COSIM
    // Cold-start RAM contents are initialized by the generated RTL. CLEAR and
    // warm-reset behavior are tested on production RTL in tb_mmio.sv; avoid
    // 2050 redundant AXI-Lite transactions in the automatic cosim driver.
    tick(); tick(); CHECK(!status.engine_ready);
#else
    CHECK(command(RE_CFG_CLEAR)); CHECK(!status.engine_ready);
#endif
    CHECK(command(RE_CFG_ADDR_WRITE, 1, 0x1000, 0x2000));
    load_rule(true); CHECK(status.engine_ready);
    packet(3); std::vector<uint32_t> out = collect(2);
    CHECK(out[0] == 1 && out[1] == (3u << 16));
    packet(4); out = collect(2); CHECK(out[0] == 0);
    packet(3, 0x2000); dropped_packet();
    packet(3, 0x1001, true); dropped_packet();
    packet(65, 0x1001, false, false); dropped_packet();
    load_rule(false, 20);
    packet(65); out = collect(21); CHECK(out[0] == 1 && out[1] == 0);
    for (unsigned i = 2; i < out.size(); ++i) CHECK(out[i] == UINT32_MAX);
    load_rule(false, RE_CPGP_NM);
    packet(RE_MAX_PACKET_BYTES); out = collect(RE_CPGP_NM + 1); CHECK(out[0] == 1);
    packet(RE_MAX_PACKET_BYTES + 1); dropped_packet();
    packet(4160); dropped_packet();
    // A bad word count used to deadlock the config DATAFLOW reader.
    CHECK(command(RE_CFG_RULE_WRITE, 1, 0, 255));
    packet(3); dropped_packet();
    load_rule(true);
    packet(3); out = collect(2); CHECK(out[0] == 1);
    CHECK(command(RE_CFG_RULE_DELETE, 1)); packet(3); dropped_packet();
#ifndef RE_HLS_COSIM
    CHECK(command(RE_CFG_CLEAR));
#endif
    CHECK(!status.engine_ready);
    std::puts("PASS exhaustive kernel: regex match/miss, DMA framing/length, malformed header, MMIO holds, 33 result chunks");
#endif
}
int main() {
#if defined(RE_HLS_EXHAUSTIVE_CSIM) && !defined(RE_HLS_COSIM) && !defined(RE_HLS_QUICK_CSIM)
    std::puts("RUN exhaustive IOVA/rule-table regression");
    std::fflush(stdout);
    test_iova(); test_rule();
#elif defined(RE_HLS_QUICK_CSIM)
    std::puts("RUN reduced-dimension rule-storage smoke test");
    std::fflush(stdout);
    test_rule_replicas();
#endif
    std::puts("RUN end-to-end kernel smoke test");
    std::fflush(stdout);
    test_kernel();
    std::puts("PASS all regressions");
    return 0;
}
