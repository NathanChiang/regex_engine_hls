#include "re_engine_iova_table.hpp"
#include "re_engine_kernel.hpp"
// Exercise the streaming configuration FSM in both C and RTL. The SDK types
// above retain their host implementation during C simulation.
#ifndef __SYNTHESIS__
#define __SYNTHESIS__
#define REGEX_PROBE_HOST
#endif
#include "re_engine_rule_table.hpp"
#ifdef REGEX_PROBE_HOST
#undef __SYNTHESIS__
#endif

extern "C" void rule_config_probe(unsigned command, unsigned valid, unsigned index, ap_uint<64> data,
                                  ap_uint<32>& event, ap_uint<32>& flags) {
    static hls::stream<ReRuleUpdate> updates;
    static hls::stream<bool> done;
#pragma HLS stream variable = updates depth = 2
#pragma HLS stream variable = done depth = 2
    cfg_input cfg;
    cfg.cmd = command;
    cfg.valid = valid;
    cfg.job_id = 1;
    cfg.arg0 = index * 8;
    cfg.arg1 = data;
    re_iova_lookup_status iova = {};
    re_rule_cfg_status status;
    re_rule_lookup_status lookup;
    re_engine_rule_table(cfg, iova, updates, done, status, lookup);
    event = 0;
    ReRuleUpdate update;
    if (updates.read_nb(update)) {
        event[31] = 1;
        event[30] = update.finalize;
        event.range(29, 21) = update.instruction_count;
        event.range(20, 12) = update.index;
        event.range(11, 10) = update.bitset;
        event.range(9, 6) = update.rule;
        done.write(true);
    }
    flags = 0;
    flags[0] = status.ack;
    flags[1] = status.cmd_transition_allowed;
    flags[2] = status.table_ready;
}
