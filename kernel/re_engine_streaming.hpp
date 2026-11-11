#ifndef RE_ENGINE_STREAMING_HPP
#define RE_ENGINE_STREAMING_HPP

#include "re_engine_kernel.hpp"
#include "re_engine_rule_table.hpp"
#include "xf_data_analytics/text/re_engine.hpp"

// One descriptor is committed as soon as DMA framing and its rule lookup
// complete. The rule index selects the receiving PU's private memory replica.
struct ReStreamJob {
    ap_uint<16> length;
    ap_uint<9> beats;
    ap_uint<4> rule;
    ap_uint<16> groups;
    ap_uint<64> address;
    ap_uint<32> message_id;
};

struct ReStreamOrder {
    ap_uint<8> pu;
    ap_uint<16> groups;
    ap_uint<64> address;
    ap_uint<32> message_id;
};

// Scheduling reserves a PU without waiting for payload unpacking. The router
// consumes these assignments in the same order as the shared payload FIFO.
struct ReStreamAssignment {
    ap_uint<8> pu;
    ap_uint<9> beats;
};

namespace re_streaming {

// Broadcast one decoded configuration write to every private PU replica and
// acknowledge ingress only after all replicas have committed it.
template <int PU_NM>
void broadcastRuleUpdates(hls::stream<ReRuleUpdate>& updates,
                          hls::stream<bool>& update_done,
                          hls::stream<ReRuleUpdate> pu_updates[PU_NM],
                          hls::stream<bool> pu_update_done[PU_NM]) {
    enum State : unsigned char {
        WAIT_UPDATE = 0,
        SEND_TO_PUS,
        WAIT_FOR_PUS,
        SEND_DONE
    };
    static State state = WAIT_UPDATE;
#pragma HLS reset variable=state
    static ReRuleUpdate pending_update;
    static ap_uint<PU_NM> sent = 0;
    static ap_uint<PU_NM> completed = 0;

    while (true) {
        switch (state) {
        case WAIT_UPDATE:
            if (updates.read_nb(pending_update)) {
                sent = 0;
                completed = 0;
                state = SEND_TO_PUS;
            }
            break;

        case SEND_TO_PUS: {
            ap_uint<PU_NM> next_sent = sent;
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (!sent[p] && pu_updates[p].write_nb(pending_update))
                    next_sent[p] = 1;
            }
            sent = next_sent;
            if (next_sent == ap_uint<PU_NM>(-1))
                state = WAIT_FOR_PUS;
            break;
        }

        case WAIT_FOR_PUS: {
            ap_uint<PU_NM> next_completed = completed;
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                bool done;
                if (!completed[p] && pu_update_done[p].read_nb(done))
                    next_completed[p] = 1;
            }
            completed = next_completed;
            if (next_completed == ap_uint<PU_NM>(-1))
                state = SEND_DONE;
            break;
        }

        case SEND_DONE:
            if (update_done.write_nb(true))
                state = WAIT_UPDATE;
            break;
        }
    }
}

template <int PU_NM>
void dispatchJobsByFreePU(hls::stream<ReStreamJob>& jobs,
                          hls::stream<bool> done[PU_NM],
                          hls::stream<ReStreamJob> pu_jobs[PU_NM],
                          hls::stream<ReStreamAssignment>& assignments) {
    bool free_map[PU_NM];
#pragma HLS array_partition variable=free_map complete
    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
        free_map[p] = true;
    }
    ap_uint<8> next = 0;
    while (true) {
        ReStreamJob job = jobs.read();
        bool found = false;
        ap_uint<8> selected = 0;
        while (!found) {
            // Completion is independent of input arrival. Reclaim every PU
            // before choosing one, then scan all 16 candidates fairly.
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                bool token;
                if (done[p].read_nb(token)) free_map[p] = true;
            }
            for (int offset = 0; offset < PU_NM; ++offset) {
#pragma HLS unroll
                ap_uint<8> candidate = next + offset;
                if (candidate >= PU_NM) candidate -= PU_NM;
                if (!found && free_map[candidate]) {
                    selected = candidate;
                    found = true;
                }
            }
        }
        free_map[selected] = false;
        next = selected + 1;
        if (next == PU_NM) next = 0;

        ReStreamAssignment assignment;
        assignment.pu = selected;
        assignment.beats = job.beats;
        assignments.write(assignment);
        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            if (selected == p) pu_jobs[p].write(job);
        }
    }
}

// A whole packet fits in each PU's FIFO. Unpacking 512-bit beats into the VM's
// 32-bit RAM therefore cannot hold up scheduling or the next packet's routing.
template <int PU_NM>
void routeMessagesByPU(hls::stream<ReStreamAssignment>& assignments,
                       hls::stream<ap_uint<512> >& messages,
                       hls::stream<ap_uint<512> > pu_messages[PU_NM]) {
    while (true) {
        ReStreamAssignment assignment = assignments.read();
        for (int i = 0; i < assignment.beats; ++i) {
#pragma HLS pipeline II=1
            ap_uint<512> beat = messages.read();
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (assignment.pu == p) pu_messages[p].write(beat);
            }
        }
    }
}

template <int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void processingUnit(hls::stream<ReRuleUpdate>& rule_updates,
                    hls::stream<bool>& rule_update_done,
                    hls::stream<ReStreamJob>& jobs,
                    hls::stream<ap_uint<512> >& messages,
                    hls::stream<ap_uint<32> >& results,
                    hls::stream<ReStreamOrder>& completed_jobs,
                    hls::stream<bool>& done,
                    ap_uint<8> pu_id) {
    // A rule is configured once, directly into every PU's private memories.
    // Runtime dispatch passes only the rule index; no rule is copied per job.
    ap_uint<64> instruction_rules[RE_RULE_TABLE_ENTRIES][RE_RULE_INSTR_DEPTH];
#pragma HLS bind_storage variable=instruction_rules type=ram_2p impl=uram
    ap_uint<32> bitset_rules[RE_RULE_TABLE_ENTRIES][RE_RULE_BITSET_DEPTH];
#pragma HLS bind_storage variable=bitset_rules type=ram_2p impl=bram
    ap_uint<32> message[MSG_LEN * 2];
#pragma HLS bind_storage variable=message type=ram_t2p impl=bram
    ap_uint<16> offsets[CPGP_NM * 2 + 4];
#pragma HLS bind_storage variable=offsets type=ram_t2p impl=bram

    while (true) {
        ReRuleUpdate update;
        ReStreamJob job;
        if (rule_updates.read_nb(update)) {
            if (update.bitset) {
                bitset_rules[update.rule][update.index] = update.data.range(31, 0);
                bitset_rules[update.rule][update.index + 1] = update.data.range(63, 32);
            } else {
                instruction_rules[update.rule][update.index] = update.data;
            }
            rule_update_done.write(true);
        } else if (jobs.read_nb(job)) {
            xf::data_analytics::text::internal::readMsg512ToBuff<MSG_LEN>(
                job.length, messages, message);
            for (int i = 0; i < CPGP_NM + 2; ++i) {
#pragma HLS pipeline II=1
                offsets[2 * i] = -1;
                offsets[2 * i + 1] = -1;
            }
            xf::data_analytics::text::internal::reExecOneMessage<STACK_SIZE>(
                offsets, instruction_rules[job.rule], bitset_rules[job.rule],
                message, job.length);
            for (int g = 0; g < job.groups + 1; ++g) {
#pragma HLS pipeline II=2
                ap_uint<32> result = 0;
                result.range(15, 0) = offsets[2 * g + 1];
                result.range(31, 16) = offsets[2 * g + 2];
                results.write(result);
            }
            // Publish only after the complete record is in this PU's FIFO.
            // The collector never waits for a PU that is still computing.
            ReStreamOrder completed;
            completed.pu = pu_id;
            completed.groups = job.groups;
            completed.address = job.address;
            completed.message_id = job.message_id;
            completed_jobs.write(completed);
            // The VM and result copy have finished; the PU's message and
            // capture buffers are safe to overwrite. A full result record
            // fits in its FIFO.
            done.write(true);
        }
    }
}

template <int PU_NM>
void processingUnits(hls::stream<ReRuleUpdate> rule_updates[PU_NM],
                     hls::stream<bool> rule_update_done[PU_NM],
                     hls::stream<ReStreamJob> jobs[PU_NM],
                     hls::stream<ap_uint<512> > messages[PU_NM],
                     hls::stream<ap_uint<32> > results[PU_NM],
                     hls::stream<ReStreamOrder> completed_jobs[PU_NM],
                     hls::stream<bool> done[PU_NM]) {
    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
        processingUnit<RE_CPGP_NM, RE_MSG_LEN, RE_STACK_SIZE>(
            rule_updates[p], rule_update_done[p], jobs[p], messages[p],
            results[p], completed_jobs[p], done[p], p);
    }
}

// Only ready records compete for output; no dispatch-order ticket can block
// a later, faster job. Round-robin arbitration breaks ties fairly. The queue
// preserves the order in which completions are accepted while MMIO is stalled.
template <int PU_NM>
void mergeCompletedJobs(hls::stream<ReStreamOrder> completed_jobs[PU_NM],
                        hls::stream<ReStreamOrder>& completions) {
    ap_uint<8> next = 0;
    while (true) {
        // Allow registers between round-robin mask/select/encode/advance operations.
        // Two clocks cover this feedback path with the 3.8 ns scheduling budget.
#pragma HLS pipeline II=2
        ap_uint<PU_NM> ready = 0;
        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            ready[p] = !completed_jobs[p].empty();
        }
        ap_uint<PU_NM> eligible = ready & (ap_uint<PU_NM>(-1) << next);
        if (eligible == 0) eligible = ready;
        // Isolate the first ready bit with a small carry chain, rather than
        // cascading 2*PU_NM conditional priority muxes in the feedback path.
        ap_uint<PU_NM> grant = eligible & ap_uint<PU_NM>(-eligible);
        ap_uint<8> selected = 0;
        for (int bit = 0; bit < 8; ++bit) {
#pragma HLS unroll
            ap_uint<PU_NM> mask = 0;
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                mask[p] = (p >> bit) & 1;
            }
            selected[bit] = (grant & mask) != 0;
        }
        if (grant != 0 && !completions.full()) {
            ReStreamOrder completed;
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (selected == p) completed = completed_jobs[p].read();
            }
            // One syntactic write avoids HLS scheduling sixteen potential
            // writes to the same FIFO port, even though grant is one-hot.
            completions.write(completed);
            next = selected + 1;
            if (next == PU_NM) next = 0;
        }
    }
}

template <int PU_NM>
void collectResults(hls::stream<ReStreamOrder>& completions,
                    hls::stream<ap_uint<32> > results[PU_NM],
                    hls::stream<ReStreamOrder>& output_order,
                    hls::stream<ap_uint<32> >& output_words) {
    while (true) {
        ReStreamOrder ticket = completions.read();
        output_order.write(ticket);
        for (int i = 0; i < ticket.groups + 1; ++i) {
#pragma HLS pipeline II=1
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (ticket.pu == p) output_words.write(results[p].read());
            }
        }
    }
}

template <int PU_NM>
void engine(hls::stream<ReRuleUpdate>& rule_updates,
            hls::stream<bool>& rule_update_done,
            hls::stream<ReStreamJob>& jobs,
            hls::stream<ap_uint<512> >& messages,
            hls::stream<ReStreamOrder>& output_order,
            hls::stream<ap_uint<32> >& output_words) {
#pragma HLS dataflow disable_start_propagation
    hls::stream<ReRuleUpdate> pu_rule_updates[PU_NM];
#pragma HLS stream variable=pu_rule_updates depth=2
    hls::stream<bool> pu_rule_update_done[PU_NM];
#pragma HLS stream variable=pu_rule_update_done depth=2
    hls::stream<ReStreamJob> pu_jobs[PU_NM];
#pragma HLS stream variable=pu_jobs depth=2
    hls::stream<ap_uint<512> > pu_messages[PU_NM];
#pragma HLS stream variable=pu_messages depth=64
// Preserve a full packet per PU without wasting deep BRAMs on 64-word FIFOs.
#pragma HLS bind_storage variable=pu_messages type=fifo impl=lutram
    hls::stream<ap_uint<32> > pu_results[PU_NM];
#pragma HLS stream variable=pu_results depth=1024
#pragma HLS bind_storage variable=pu_results type=fifo impl=bram
    hls::stream<ReStreamOrder> completed_jobs[PU_NM];
#pragma HLS stream variable=completed_jobs depth=2
    hls::stream<bool> done[PU_NM];
#pragma HLS stream variable=done depth=2
    hls::stream<ReStreamAssignment> assignments;
#pragma HLS stream variable=assignments depth=64
    hls::stream<ReStreamOrder> completions;
#pragma HLS stream variable=completions depth=64

    broadcastRuleUpdates<PU_NM>(rule_updates, rule_update_done,
                                pu_rule_updates, pu_rule_update_done);
    dispatchJobsByFreePU<PU_NM>(jobs, done, pu_jobs, assignments);
    routeMessagesByPU<PU_NM>(assignments, messages, pu_messages);
    processingUnits<PU_NM>(pu_rule_updates, pu_rule_update_done,
                           pu_jobs, pu_messages, pu_results, completed_jobs, done);
    mergeCompletedJobs<PU_NM>(completed_jobs, completions);
    collectResults<PU_NM>(completions, pu_results, output_order, output_words);
}

} // namespace re_streaming

#endif
