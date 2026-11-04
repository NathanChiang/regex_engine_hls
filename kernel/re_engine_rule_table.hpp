#ifndef RE_ENGINE_RULE_TABLE_HPP
#define RE_ENGINE_RULE_TABLE_HPP

#include <ap_int.h>
#include "re_engine_kernel.hpp"
#include "re_engine_iova_table.hpp"

// Logical rule table: sixteen entries, one preprocessed rule per job ID.
#ifndef RE_RULE_TABLE_ENTRIES
#define RE_RULE_TABLE_ENTRIES RE_MAX_RULES
#endif

#ifndef RE_MAX_RULE_BYTES
#define RE_MAX_RULE_BYTES 2048
#endif

#define RE_RULE_WORD_BYTES      8
#define RE_RULE_WORDS_PER_ENTRY (RE_MAX_RULE_BYTES / RE_RULE_WORD_BYTES)
#define RE_RULE_INSTR_DEPTH     RE_RULE_WORDS_PER_ENTRY
#define RE_RULE_BITSET_DEPTH    (RE_RULE_WORDS_PER_ENTRY * 2)

static_assert(RE_RULE_TABLE_ENTRIES == 16 && RE_MAX_RULE_BYTES == 2048,
              "Rule table addressing requires 16 entries of 2048 bytes");

struct re_rule_cfg_status {
    ap_uint<1> ack;
    ap_uint<1> full;
    ap_uint<1> cmd_transition_allowed;
    ap_uint<1> table_ready;
};

struct re_rule_lookup_status {
    // Pulses when the chained IOVA -> rule lookup has completed.
    ap_uint<1> ack;
    // 1 when the job ID exists in the rule table.
    ap_uint<1> hit;
    // 1 when the rule header is valid and all required configuration words
    // have been written. The kernel only launches reEngine for this case.
    ap_uint<1> config_valid;
    ap_uint<16> job_id;
    ap_uint<16> cpgp_nm;
};

// C simulation keeps the replicas here so the step-model regression can inspect
// them directly. In synthesized hardware each processing unit owns its replica;
// ingress sends decoded updates over FIFOs instead of sharing an array across
// non-terminating dataflow processes.
#ifndef __SYNTHESIS__
static ap_uint<64> re_rule_instr_mem[RE_PU_NM][RE_RULE_TABLE_ENTRIES][RE_RULE_INSTR_DEPTH];
static ap_uint<32> re_rule_bitset_mem[RE_PU_NM][RE_RULE_TABLE_ENTRIES][RE_RULE_BITSET_DEPTH];
#endif

namespace re_rule_table_internal {

// Small metadata memories used to validate partial writes and provide the
// capturing-group count to reEngine.
static ap_uint<1> re_rule_entry_valid[RE_RULE_TABLE_ENTRIES];
static ap_uint<RE_RULE_WORDS_PER_ENTRY> re_rule_word_valid[RE_RULE_TABLE_ENTRIES];
static ap_uint<64> re_rule_word_count[RE_RULE_TABLE_ENTRIES];
static ap_uint<32> re_rule_instr_count[RE_RULE_TABLE_ENTRIES];
static ap_uint<16> re_rule_cclass_count[RE_RULE_TABLE_ENTRIES];
static ap_uint<16> re_rule_cpgp_count[RE_RULE_TABLE_ENTRIES];

// Decode one accepted payload word. Word 0 is the total word count and word 1
// is cpgp_nm | cclass_nm | instr_nm; payload words follow in that order.
//
// The configuration protocol writes words 0 and 1 before payload words. With
// no raw CFG image retained, payload written before the header is ignored and
// the entry remains invalid until it is rewritten in the proper order.
inline bool decodeCFGbuf(ap_uint<4> entry,
                         ap_uint<8> word_index,
                         ap_uint<64> word,
                         ReRuleUpdate& update) {
#pragma HLS inline
    if (word_index < 2 || !re_rule_word_valid[entry][1])
        return false;

    const ap_uint<32> instr_nm = re_rule_instr_count[entry];
    const ap_uint<16> cclass_nm = re_rule_cclass_count[entry];

    if (word_index < (2 + instr_nm)) {
        const ap_uint<32> instr_index = word_index - 2;
        if (instr_index < RE_RULE_INSTR_DEPTH) {
            update.rule = entry;
            update.bitset = 0;
            update.index = instr_index;
            update.data = word;
            return true;
        }
    } else {
        const ap_uint<32> bitset_word_index = word_index - 2 - instr_nm;
        if (bitset_word_index < ((ap_uint<32>)cclass_nm * 4)) {
            const ap_uint<32> bitset_index = bitset_word_index * 2;
            if (bitset_index + 1 < RE_RULE_BITSET_DEPTH) {
                update.rule = entry;
                update.bitset = 1;
                update.index = bitset_index;
                update.data = word;
                return true;
            }
        }
    }
    return false;
}

#ifndef __SYNTHESIS__
inline void readCFGbuf(ap_uint<4> entry, ap_uint<8> word_index, ap_uint<64> word) {
#pragma HLS inline
    ReRuleUpdate update;
    if (!decodeCFGbuf(entry, word_index, word, update))
        return;
    for (int p = 0; p < RE_PU_NM; ++p) {
#pragma HLS unroll
        if (update.bitset) {
            re_rule_bitset_mem[p][update.rule][update.index] = update.data.range(31, 0);
            re_rule_bitset_mem[p][update.rule][update.index + 1] = update.data.range(63, 32);
        } else {
            re_rule_instr_mem[p][update.rule][update.index] = update.data;
        }
    }
}
#endif

inline bool config_valid(ap_uint<4> entry) {
#pragma HLS inline

    const ap_uint<64> words = re_rule_word_count[entry];
    const ap_uint<32> instr_nm = re_rule_instr_count[entry];
    const ap_uint<16> cclass_nm = re_rule_cclass_count[entry];
    const ap_uint<16> cpgp_nm = re_rule_cpgp_count[entry];

    const ap_uint<64> expected_words =
        2 + ap_uint<64>(instr_nm) + ap_uint<64>(cclass_nm) * 4;

    bool header_valid =
        re_rule_entry_valid[entry] &&
        re_rule_word_valid[entry][0] &&
        re_rule_word_valid[entry][1] &&
        instr_nm != 0 &&
        instr_nm <= RE_INSTR_DEPTH &&
        instr_nm <= RE_RULE_INSTR_DEPTH &&
        cclass_nm <= RE_CCLASS_NM &&
        ap_uint<32>(cclass_nm) * 8 <= RE_RULE_BITSET_DEPTH &&
        cpgp_nm != 0 &&
        cpgp_nm <= RE_CPGP_NM &&
        words == expected_words &&
        words <= RE_RULE_WORDS_PER_ENTRY;

    // The lowest 'words' bits must all be 1.
    const ap_uint<RE_RULE_WORDS_PER_ENTRY> required_mask =
        (ap_uint<RE_RULE_WORDS_PER_ENTRY>(1) << words) - 1;

    bool all_words_written =
        (re_rule_word_valid[entry] & required_mask) == required_mask;

    return header_valid && all_words_written;
}

} // namespace re_rule_table_internal

/*
 * Configuration writes are preprocessed here. Runtime lookup only returns the
 * direct job ID and metadata; it does not fetch or assemble a raw pattern.
 * The kernel uses that job ID to pass the selected instruction and bitset
 * entry pointers directly into reEngine.
 */
inline void re_engine_rule_table(
    const cfg_input& cfg_in,
    const re_iova_lookup_status& iova_lookup,
#ifdef __SYNTHESIS__
    hls::stream<ReRuleUpdate>& rule_updates,
    hls::stream<bool>& rule_update_done,
#endif
    re_rule_cfg_status& status,
    re_rule_lookup_status& lookup) {
// One invocation advances this table FSM by one step.  It must be part of the
// persistent ingress FSM so ingress can reread MMIO between steps.
#pragma HLS inline

    enum State : unsigned char {
        RULE_IDLE = 0,
        RULE_SEND_UPDATE,
        RULE_WAIT_UPDATE_DONE,
        RULE_WAIT_CFG_VALID_LOW
    };

    static State state = RULE_IDLE;
#pragma HLS reset variable=state
#ifdef __SYNTHESIS__
    static ReRuleUpdate pending_update;
#endif
    static ap_uint<5> valid_count = 0;
    static ap_uint<1> table_full = 0;

    status.ack = 0;
    status.full = table_full;
    status.cmd_transition_allowed = 0;
    status.table_ready = (valid_count != 0);

    lookup.ack = 0;
    lookup.hit = 0;
    lookup.config_valid = 0;
    lookup.job_id = 0;
    lookup.cpgp_nm = 0;

    switch (state) {
    case RULE_IDLE:
        if (cfg_in.valid && cfg_in.cmd == RE_CFG_RULE_WRITE) {
            if (cfg_in.arg0 >= RE_MAX_RULE_BYTES || cfg_in.arg0.range(2, 0) != 0) {
                status.full = 1;
                state = RULE_WAIT_CFG_VALID_LOW;
                break;
            }

            if (cfg_in.job_id >= RE_RULE_TABLE_ENTRIES) {
                state = RULE_WAIT_CFG_VALID_LOW;
                break;
            }

            const ap_uint<4> target_index = cfg_in.job_id.range(3, 0);
            const bool new_entry = !re_rule_table_internal::re_rule_entry_valid[target_index];

            if (new_entry) {
                re_rule_table_internal::re_rule_entry_valid[target_index] = 1;
                re_rule_table_internal::re_rule_word_count[target_index] = 0;
                re_rule_table_internal::re_rule_instr_count[target_index] = 0;
                re_rule_table_internal::re_rule_cclass_count[target_index] = 0;
                re_rule_table_internal::re_rule_cpgp_count[target_index] = 0;

                if (valid_count < RE_RULE_TABLE_ENTRIES) {
                    if (valid_count == RE_RULE_TABLE_ENTRIES - 1)
                        table_full = 1;
                    valid_count++;
                }
            }

            const ap_uint<8> word_index = cfg_in.arg0 >> 3;
            // Assign the full vector. A bit-select store to this RAM is
            // implemented with byte enables and would clear the other seven
            // validity bits in the same byte.
            ap_uint<RE_RULE_WORDS_PER_ENTRY> written_words =
                new_entry ? ap_uint<RE_RULE_WORDS_PER_ENTRY>(0)
                          : re_rule_table_internal::re_rule_word_valid[target_index];
            written_words |= ap_uint<RE_RULE_WORDS_PER_ENTRY>(1) << word_index;
            re_rule_table_internal::re_rule_word_valid[target_index] = written_words;

            if (word_index == 0) {
                re_rule_table_internal::re_rule_word_count[target_index] = cfg_in.arg1;
            } else if (word_index == 1) {
                re_rule_table_internal::re_rule_instr_count[target_index] = cfg_in.arg1.range(31, 0);
                re_rule_table_internal::re_rule_cclass_count[target_index] = cfg_in.arg1.range(47, 32);
                re_rule_table_internal::re_rule_cpgp_count[target_index] = cfg_in.arg1.range(63, 48);
            } else {
#ifdef __SYNTHESIS__
                ReRuleUpdate update;
                if (re_rule_table_internal::decodeCFGbuf(
                        target_index, word_index, cfg_in.arg1, update)) {
                    pending_update = update;
                    state = RULE_SEND_UPDATE;
                    break;
                }
#else
                re_rule_table_internal::readCFGbuf(target_index, word_index, cfg_in.arg1);
#endif
            }

            status.ack = 1;
            status.full = table_full;
            status.table_ready = 1;
            state = RULE_WAIT_CFG_VALID_LOW;
        } else if (cfg_in.valid && cfg_in.cmd == RE_CFG_RULE_DELETE) {
            if (cfg_in.job_id >= RE_RULE_TABLE_ENTRIES) {
                state = RULE_WAIT_CFG_VALID_LOW;
                break;
            }

            const ap_uint<4> job_index = cfg_in.job_id.range(3, 0);
            if (re_rule_table_internal::re_rule_entry_valid[job_index]) {
                re_rule_table_internal::re_rule_entry_valid[job_index] = 0;
                re_rule_table_internal::re_rule_word_valid[job_index] = 0;
                re_rule_table_internal::re_rule_word_count[job_index] = 0;
                re_rule_table_internal::re_rule_instr_count[job_index] = 0;
                re_rule_table_internal::re_rule_cclass_count[job_index] = 0;
                re_rule_table_internal::re_rule_cpgp_count[job_index] = 0;
                if (valid_count != 0)
                    valid_count--;
                table_full = 0;
                status.full = 0;
                status.table_ready = (valid_count != 0);
            }

            status.ack = 1;
            state = RULE_WAIT_CFG_VALID_LOW;
        } else if (cfg_in.valid && cfg_in.cmd == RE_CFG_CLEAR) {
            for (int i = 0; i < RE_RULE_TABLE_ENTRIES; ++i) {
#pragma HLS unroll
                re_rule_table_internal::re_rule_entry_valid[i] = 0;
                re_rule_table_internal::re_rule_word_valid[i] = 0;
                re_rule_table_internal::re_rule_word_count[i] = 0;
                re_rule_table_internal::re_rule_instr_count[i] = 0;
                re_rule_table_internal::re_rule_cclass_count[i] = 0;
                re_rule_table_internal::re_rule_cpgp_count[i] = 0;
            }
            valid_count = 0;
            table_full = 0;
            status.full = 0;
            status.table_ready = 0;
            status.ack = 1;
            state = RULE_WAIT_CFG_VALID_LOW;
        } else if (cfg_in.cmd == RE_CFG_NONE && iova_lookup.ack) {
            // The IOVA job ID is the direct index into both preprocessed
            // memories. No raw pattern fetch is needed.
            lookup.ack = 1;
            lookup.job_id = iova_lookup.job_id;

            if (iova_lookup.hit && iova_lookup.job_id < RE_RULE_TABLE_ENTRIES) {
                const ap_uint<4> job_index = iova_lookup.job_id.range(3, 0);
                if (re_rule_table_internal::re_rule_entry_valid[job_index]) {
                    lookup.hit = 1;
                    lookup.config_valid = re_rule_table_internal::config_valid(job_index);
                    lookup.cpgp_nm = re_rule_table_internal::re_rule_cpgp_count[job_index];
                }
            }
        } else {
            status.cmd_transition_allowed = 1;
        }
        break;

    case RULE_SEND_UPDATE:
#ifdef __SYNTHESIS__
        if (rule_updates.write_nb(pending_update))
            state = RULE_WAIT_UPDATE_DONE;
#else
        state = RULE_IDLE;
#endif
        break;

    case RULE_WAIT_UPDATE_DONE:
#ifdef __SYNTHESIS__
        {
            bool done;
            if (rule_update_done.read_nb(done)) {
                status.ack = 1;
                state = RULE_WAIT_CFG_VALID_LOW;
            }
        }
#else
        state = RULE_IDLE;
#endif
        break;

    case RULE_WAIT_CFG_VALID_LOW:
        if (!cfg_in.valid) {
            status.cmd_transition_allowed = 1;
            state = RULE_IDLE;
        }
        break;

    default:
        state = RULE_IDLE;
        break;
    }
}

#endif // RE_ENGINE_RULE_TABLE_HPP
