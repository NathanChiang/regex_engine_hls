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
    ap_uint<16> rule_bytes;
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

enum RePerfEventKind {
    RE_PERF_DMA_SOP       = 1 << 0,
    RE_PERF_IOVA_DONE     = 1 << 1,
    RE_PERF_RULE_DONE     = 1 << 2,
    RE_PERF_PACKET_READY  = 1 << 3,
    RE_PERF_DISPATCH      = 1 << 4,
    RE_PERF_PU_INPUT      = 1 << 5,
    RE_PERF_COMPUTE_START = 1 << 6,
    RE_PERF_COMPUTE_DONE  = 1 << 7,
    RE_PERF_HANDOFF_DONE  = 1 << 8,
    RE_PERF_PU_DONE       = 1 << 9
};

struct RePerfEvent {
    ap_uint<16> flags;
    ap_uint<32> message_id;
    ap_uint<8> job_id;
    ap_uint<8> pu;
    ap_uint<16> message_bytes;
    ap_uint<16> rule_bytes;
    ap_uint<16> dma_beats;
};

struct ReTimedPerfEvent {
    RePerfEvent event;
    ap_uint<48> cycle;
};

// Keep the counter snapshot as independent fields. Vitis HLS 2021.2 turns
// repeated range writes to one wide ap_uint into a long mux chain; independent
// fields synthesize as parallel registers and are compacted only by the FIFO.
struct RePerfCounterSnapshot {
    ap_uint<32> message_id;
    ap_uint<48> cycle;
    ap_uint<32> idle0;
    ap_uint<32> idle1;
    ap_uint<32> idle2;
    ap_uint<32> idle3;
    ap_uint<32> idle4;
    ap_uint<32> idle5;
    ap_uint<32> idle6;
    ap_uint<32> idle7;
    ap_uint<32> idle8;
    ap_uint<32> idle9;
    ap_uint<32> idle10;
    ap_uint<32> idle11;
    ap_uint<32> idle12;
    ap_uint<32> idle13;
    ap_uint<32> idle14;
    ap_uint<32> idle15;
    ap_uint<32> all_busy_cycles;
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
void dispatchMsgByFreePU(hls::stream<ReStreamJob>& jobs,
                         hls::stream<ap_uint<512> >& messages,
                         hls::stream<bool> done[PU_NM],
                         hls::stream<ReStreamJob> pu_jobs[PU_NM],
                         hls::stream<ap_uint<512> > pu_messages[PU_NM],
                         hls::stream<ReStreamOrder>& order,
                         hls::stream<RePerfEvent>& perf_events) {
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

        ReStreamOrder ticket;
        ticket.pu = selected;
        ticket.groups = job.groups;
        ticket.address = job.address;
        ticket.message_id = job.message_id;
        order.write(ticket);

        RePerfEvent perf_event;
        perf_event.flags = RE_PERF_DISPATCH;
        perf_event.message_id = job.message_id;
        perf_event.job_id = job.rule;
        perf_event.pu = selected;
        perf_event.message_bytes = job.length;
        perf_event.rule_bytes = job.rule_bytes;
        perf_event.dma_beats = job.beats;
        perf_events.write_nb(perf_event);
        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            if (selected == p) pu_jobs[p].write(job);
        }
        for (int i = 0; i < job.beats; ++i) {
#pragma HLS pipeline II=1
            ap_uint<512> beat = messages.read();
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (selected == p) pu_messages[p].write(beat);
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
                    hls::stream<bool>& done,
                    ap_uint<8> pu_id,
                    hls::stream<RePerfEvent>& perf_events) {
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
            RePerfEvent perf_event;
            perf_event.flags = RE_PERF_PU_INPUT;
            perf_event.message_id = job.message_id;
            perf_event.job_id = job.rule;
            perf_event.pu = pu_id;
            perf_event.message_bytes = job.length;
            perf_event.rule_bytes = job.rule_bytes;
            perf_event.dma_beats = job.beats;
            perf_events.write_nb(perf_event);

            xf::data_analytics::text::internal::readMsg512ToBuff<MSG_LEN>(
                job.length, messages, message);
            for (int i = 0; i < CPGP_NM + 2; ++i) {
#pragma HLS pipeline II=1
                offsets[2 * i] = -1;
                offsets[2 * i + 1] = -1;
            }
            perf_event.flags = RE_PERF_COMPUTE_START;
            perf_events.write_nb(perf_event);
            xf::data_analytics::text::internal::reExecOneMessage<STACK_SIZE>(
                offsets, instruction_rules[job.rule], bitset_rules[job.rule],
                message, job.length);
            perf_event.flags = RE_PERF_COMPUTE_DONE;
            perf_events.write_nb(perf_event);
            for (int g = 0; g < job.groups + 1; ++g) {
#pragma HLS pipeline II=2
                ap_uint<32> result = 0;
                result.range(15, 0) = offsets[2 * g + 1];
                result.range(31, 16) = offsets[2 * g + 2];
                results.write(result);
            }
            perf_event.flags = RE_PERF_PU_DONE;
            perf_events.write_nb(perf_event);
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
                     hls::stream<bool> done[PU_NM],
                     hls::stream<RePerfEvent> perf_events[PU_NM]) {
    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
        processingUnit<RE_CPGP_NM, RE_MSG_LEN, RE_STACK_SIZE>(
            rule_updates[p], rule_update_done[p], jobs[p], messages[p],
            results[p], done[p], p, perf_events[p]);
    }
}

template <int PU_NM>
void collectResults(hls::stream<ReStreamOrder>& order,
                    hls::stream<ap_uint<32> > results[PU_NM],
                    hls::stream<ReStreamOrder>& output_order,
                    hls::stream<ap_uint<32> >& output_words,
                    hls::stream<RePerfEvent>& perf_events) {
    while (true) {
        ReStreamOrder ticket = order.read();
        output_order.write(ticket);
        for (int i = 0; i < ticket.groups + 1; ++i) {
#pragma HLS pipeline II=1
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (ticket.pu == p) output_words.write(results[p].read());
            }
        }
        RePerfEvent perf_event;
        perf_event.flags = 0;
        perf_event.message_id = ticket.message_id;
        perf_event.job_id = 0;
        perf_event.pu = ticket.pu;
        perf_event.message_bytes = 0;
        perf_event.rule_bytes = 0;
        perf_event.dma_beats = 0;
        perf_events.write_nb(perf_event);
    }
}

// Keep the clock itself in a deliberately small II=1 process. Formatting and
// slot correlation are more complex and need not run every clock as long as
// events carry the cycle at which they occurred.
template <int PU_NM>
void timestampPerformanceEvents(
        hls::stream<RePerfEvent>& ingress_events,
        hls::stream<RePerfEvent>& dispatch_events,
        hls::stream<RePerfEvent> pu_events[PU_NM],
        hls::stream<RePerfEvent>& result_events,
        hls::stream<ReTimedPerfEvent>& timed_ingress_events,
        hls::stream<ReTimedPerfEvent>& timed_dispatch_events,
        hls::stream<ReTimedPerfEvent> timed_pu_events[PU_NM],
        hls::stream<ReTimedPerfEvent>& timed_result_events,
        hls::stream<RePerfCounterSnapshot>& counter_snapshots) {
    static ap_uint<48> cycle = 0;
    static ap_uint<1> pu_busy[PU_NM];
#pragma HLS array_partition variable=pu_busy complete
    static ap_uint<32> pu_idle[PU_NM];
#pragma HLS array_partition variable=pu_idle complete
    static ap_uint<32> all_busy_cycles = 0;
    static ap_uint<1> measurement_active = 0;

    while (true) {
#pragma HLS pipeline II=1
        ap_uint<48> now = cycle++;

        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            if (measurement_active && !pu_busy[p]) pu_idle[p]++;
        }
        ap_uint<1> busy_l1[8];
        ap_uint<1> busy_l2[4];
        ap_uint<1> busy_l3[2];
#pragma HLS array_partition variable=busy_l1 complete
#pragma HLS array_partition variable=busy_l2 complete
#pragma HLS array_partition variable=busy_l3 complete
        for (int p = 0; p < 8; ++p) {
#pragma HLS unroll
            busy_l1[p] = pu_busy[2 * p] & pu_busy[2 * p + 1];
        }
        for (int p = 0; p < 4; ++p) {
#pragma HLS unroll
            busy_l2[p] = busy_l1[2 * p] & busy_l1[2 * p + 1];
        }
        for (int p = 0; p < 2; ++p) {
#pragma HLS unroll
            busy_l3[p] = busy_l2[2 * p] & busy_l2[2 * p + 1];
        }
        if (measurement_active && (busy_l3[0] & busy_l3[1]))
            all_busy_cycles++;

        RePerfEvent event;
        ReTimedPerfEvent timed;
        if (ingress_events.read_nb(event)) {
            // Exclude rule/IOVA configuration time from utilization. The first
            // DMA SOP marks the beginning of the measured workload window.
            if (!measurement_active && (event.flags & RE_PERF_DMA_SOP)) {
                measurement_active = 1;
                for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                    pu_idle[p] = 0;
                }
                all_busy_cycles = 0;
            }
            timed.event = event;
            timed.cycle = now;
            timed_ingress_events.write_nb(timed);
        }
        if (dispatch_events.read_nb(event)) {
            timed.event = event;
            timed.cycle = now;
            timed_dispatch_events.write_nb(timed);
        }
        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            RePerfEvent pu_event;
            if (pu_events[p].read_nb(pu_event)) {
                if (pu_event.flags & RE_PERF_PU_INPUT) pu_busy[p] = 1;
                if (pu_event.flags & RE_PERF_PU_DONE) pu_busy[p] = 0;
                ReTimedPerfEvent pu_timed;
                pu_timed.event = pu_event;
                pu_timed.cycle = now;
                timed_pu_events[p].write_nb(pu_timed);
            }
        }
        if (result_events.read_nb(event)) {
            timed.event = event;
            timed.cycle = now;
            timed_result_events.write_nb(timed);

            RePerfCounterSnapshot counters;
            counters.message_id = event.message_id;
            counters.cycle = now;
            counters.idle0 = pu_idle[0];
            counters.idle1 = pu_idle[1];
            counters.idle2 = pu_idle[2];
            counters.idle3 = pu_idle[3];
            counters.idle4 = pu_idle[4];
            counters.idle5 = pu_idle[5];
            counters.idle6 = pu_idle[6];
            counters.idle7 = pu_idle[7];
            counters.idle8 = pu_idle[8];
            counters.idle9 = pu_idle[9];
            counters.idle10 = pu_idle[10];
            counters.idle11 = pu_idle[11];
            counters.idle12 = pu_idle[12];
            counters.idle13 = pu_idle[13];
            counters.idle14 = pu_idle[14];
            counters.idle15 = pu_idle[15];
            counters.all_busy_cycles = all_busy_cycles;
            counter_snapshots.write_nb(counters);
        }
    }
}

// Correlate already-timestamped milestones. All streams are diagnostic taps,
// so this formatter has no backpressure path into matching.
template <int PU_NM>
void monitorPerformance(hls::stream<ReTimedPerfEvent>& ingress_events,
                        hls::stream<ReTimedPerfEvent>& dispatch_events,
                        hls::stream<ReTimedPerfEvent> pu_events[PU_NM],
                        hls::stream<ReTimedPerfEvent>& result_events,
                        hls::stream<RePerfCounterSnapshot>& counter_snapshots,
                        hls::stream<RePerfRecord>& perf_out) {
    const int SLOT_NM = 64;
    static ap_uint<32> slot_id[SLOT_NM];
    static ap_uint<8> slot_job[SLOT_NM];
    static ap_uint<8> slot_pu[SLOT_NM];
    static ap_uint<16> slot_length[SLOT_NM];
    static ap_uint<16> slot_rule_bytes[SLOT_NM];
    static ap_uint<16> slot_beats[SLOT_NM];
    static ap_uint<40> sop_cycle[SLOT_NM];
    static ap_uint<40> iova_cycle[SLOT_NM];
    static ap_uint<40> rule_cycle[SLOT_NM];
    static ap_uint<40> ready_cycle[SLOT_NM];
    static ap_uint<40> dispatch_cycle[SLOT_NM];
    static ap_uint<40> input_cycle[PU_NM][SLOT_NM];
    static ap_uint<40> compute_start_cycle[PU_NM][SLOT_NM];
    static ap_uint<40> compute_done_cycle[PU_NM][SLOT_NM];
    static ap_uint<40> result_cycle[SLOT_NM];
    static ap_uint<1> slot_valid[SLOT_NM];
    static ap_uint<1> input_valid[PU_NM][SLOT_NM];
    static ap_uint<1> compute_start_valid[PU_NM][SLOT_NM];
    static ap_uint<1> compute_done_valid[PU_NM][SLOT_NM];
    static ap_uint<1> result_valid[SLOT_NM];
    static RePerfCounterSnapshot counter_record[SLOT_NM];
    static ap_uint<1> counter_valid[SLOT_NM];
#pragma HLS array_partition variable=input_cycle complete dim=1
#pragma HLS array_partition variable=compute_start_cycle complete dim=1
#pragma HLS array_partition variable=compute_done_cycle complete dim=1
#pragma HLS array_partition variable=input_valid complete dim=1
#pragma HLS array_partition variable=compute_start_valid complete dim=1
#pragma HLS array_partition variable=compute_done_valid complete dim=1

    static ap_uint<32> ingress_handoff_cycles = 0;
    static ap_uint<32> scheduler_wait_cycles = 0;
    static ap_uint<16> correlator_drops = 0;
    static ap_uint<16> completed_messages = 0;
    static ap_uint<6> scan_slot = 0;
    static ap_uint<1> snapshot_pending = 0;
    static ap_uint<32> snapshot_message_id = 0;

    while (true) {
        ReTimedPerfEvent timed_event;
        RePerfEvent event;
        if (ingress_events.read_nb(timed_event)) {
            event = timed_event.event;
            ap_uint<40> now = timed_event.cycle.range(39, 0);
            ap_uint<6> slot = event.message_id.range(5, 0);
            if (event.flags & RE_PERF_DMA_SOP) {
                if (slot_valid[slot] && slot_id[slot] != event.message_id)
                    correlator_drops++;
                slot_valid[slot] = 1;
                slot_id[slot] = event.message_id;
                slot_job[slot] = event.job_id;
                slot_pu[slot] = 0;
                slot_length[slot] = 0;
                slot_rule_bytes[slot] = 0;
                slot_beats[slot] = 0;
                sop_cycle[slot] = now;
                iova_cycle[slot] = 0;
                rule_cycle[slot] = 0;
                ready_cycle[slot] = 0;
                dispatch_cycle[slot] = 0;
                result_cycle[slot] = 0;
                result_valid[slot] = 0;
                counter_valid[slot] = 0;
                for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                    input_valid[p][slot] = 0;
                    compute_start_valid[p][slot] = 0;
                    compute_done_valid[p][slot] = 0;
                }
            }
            if (slot_valid[slot] && slot_id[slot] == event.message_id) {
                if (event.flags & RE_PERF_IOVA_DONE) iova_cycle[slot] = now;
                if (event.flags & RE_PERF_RULE_DONE) {
                    rule_cycle[slot] = now;
                    slot_job[slot] = event.job_id;
                    slot_rule_bytes[slot] = event.rule_bytes;
                }
                if (event.flags & RE_PERF_PACKET_READY) {
                    ready_cycle[slot] = now;
                    slot_length[slot] = event.message_bytes;
                    slot_beats[slot] = event.dma_beats;
                }
                if (event.flags & RE_PERF_HANDOFF_DONE) {
                    ingress_handoff_cycles += now - ready_cycle[slot];
                }
            }
        }

        if (dispatch_events.read_nb(timed_event)) {
            event = timed_event.event;
            ap_uint<40> now = timed_event.cycle.range(39, 0);
            ap_uint<6> slot = event.message_id.range(5, 0);
            if (slot_valid[slot] && slot_id[slot] == event.message_id) {
                dispatch_cycle[slot] = now;
                slot_pu[slot] = event.pu;
                slot_job[slot] = event.job_id;
                slot_length[slot] = event.message_bytes;
                slot_rule_bytes[slot] = event.rule_bytes;
                slot_beats[slot] = event.dma_beats;
            }
        }

        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            ReTimedPerfEvent pu_timed_event;
            if (pu_events[p].read_nb(pu_timed_event)) {
                RePerfEvent pu_event = pu_timed_event.event;
                ap_uint<40> now = pu_timed_event.cycle.range(39, 0);
                ap_uint<6> slot = pu_event.message_id.range(5, 0);
                if (slot_valid[slot] && slot_id[slot] == pu_event.message_id) {
                    if (pu_event.flags & RE_PERF_PU_INPUT) {
                        input_cycle[p][slot] = now;
                        input_valid[p][slot] = 1;
                    }
                    if (pu_event.flags & RE_PERF_COMPUTE_START) {
                        compute_start_cycle[p][slot] = now;
                        compute_start_valid[p][slot] = 1;
                    }
                    if (pu_event.flags & RE_PERF_COMPUTE_DONE) {
                        compute_done_cycle[p][slot] = now;
                        compute_done_valid[p][slot] = 1;
                    }
                }
            }
        }

        if (result_events.read_nb(timed_event)) {
            event = timed_event.event;
            ap_uint<40> now = timed_event.cycle.range(39, 0);
            ap_uint<6> slot = event.message_id.range(5, 0);
            if (slot_valid[slot] && slot_id[slot] == event.message_id) {
                result_cycle[slot] = now;
                result_valid[slot] = 1;
            }
        }

        RePerfCounterSnapshot counters;
        if (counter_snapshots.read_nb(counters)) {
            ap_uint<6> slot = counters.message_id.range(5, 0);
            counter_record[slot] = counters;
            counter_valid[slot] = 1;
        }

        // A snapshot follows its message record. If the AXIS consumer pauses,
        // retain pending records locally while functional streams continue.
        if (snapshot_pending) {
            RePerfRecord snapshot = 0;
            snapshot.range(3, 0) = 2;
            snapshot.range(35, 4) = snapshot_message_id;
            ap_uint<6> snapshot_slot = snapshot_message_id.range(5, 0);
            RePerfCounterSnapshot counters = counter_record[snapshot_slot];
            snapshot.range(83, 36) = counters.cycle;
            snapshot.range(103, 84) = counters.idle0.range(19, 0);
            snapshot.range(123, 104) = counters.idle1.range(19, 0);
            snapshot.range(143, 124) = counters.idle2.range(19, 0);
            snapshot.range(163, 144) = counters.idle3.range(19, 0);
            snapshot.range(183, 164) = counters.idle4.range(19, 0);
            snapshot.range(203, 184) = counters.idle5.range(19, 0);
            snapshot.range(223, 204) = counters.idle6.range(19, 0);
            snapshot.range(243, 224) = counters.idle7.range(19, 0);
            snapshot.range(263, 244) = counters.idle8.range(19, 0);
            snapshot.range(283, 264) = counters.idle9.range(19, 0);
            snapshot.range(303, 284) = counters.idle10.range(19, 0);
            snapshot.range(323, 304) = counters.idle11.range(19, 0);
            snapshot.range(343, 324) = counters.idle12.range(19, 0);
            snapshot.range(363, 344) = counters.idle13.range(19, 0);
            snapshot.range(383, 364) = counters.idle14.range(19, 0);
            snapshot.range(403, 384) = counters.idle15.range(19, 0);
            snapshot.range(427, 404) =
                counters.all_busy_cycles.range(23, 0);
            snapshot.range(451, 428) = ingress_handoff_cycles;
            snapshot.range(475, 452) = scheduler_wait_cycles;
            snapshot.range(491, 476) = correlator_drops;
            snapshot.range(507, 492) = completed_messages;
            if (perf_out.write_nb(snapshot)) snapshot_pending = 0;
        } else {
            ap_uint<6> slot = scan_slot;
            ap_uint<8> p = slot_pu[slot];
            bool complete = slot_valid[slot] && result_valid[slot] &&
                            counter_valid[slot] &&
                            p < PU_NM && input_valid[p][slot] &&
                            compute_start_valid[p][slot] &&
                            compute_done_valid[p][slot];
            if (complete) {
                RePerfRecord record = 0;
                record.range(3, 0) = 1;
                record.range(35, 4) = slot_id[slot];
                record.range(43, 36) = slot_job[slot];
                record.range(51, 44) = p;
                record.range(67, 52) = slot_length[slot];
                record.range(83, 68) = slot_rule_bytes[slot];
                record.range(123, 84) = sop_cycle[slot];
                record.range(163, 124) = iova_cycle[slot];
                record.range(203, 164) = rule_cycle[slot];
                record.range(243, 204) = ready_cycle[slot];
                record.range(283, 244) = dispatch_cycle[slot];
                record.range(323, 284) = input_cycle[p][slot];
                record.range(363, 324) = compute_start_cycle[p][slot];
                record.range(403, 364) = compute_done_cycle[p][slot];
                record.range(443, 404) = result_cycle[slot];
                record.range(459, 444) = slot_beats[slot];
                record.range(475, 460) = correlator_drops;
                if (perf_out.write_nb(record)) {
                    scheduler_wait_cycles +=
                        dispatch_cycle[slot] - ready_cycle[slot];
                    completed_messages++;
                    snapshot_message_id = slot_id[slot];
                    snapshot_pending = 1;
                    slot_valid[slot] = 0;
                    result_valid[slot] = 0;
                    counter_valid[slot] = 0;
                }
            }
            scan_slot++;
        }
    }
}

template <int PU_NM>
void engine(hls::stream<ReRuleUpdate>& rule_updates,
            hls::stream<bool>& rule_update_done,
            hls::stream<ReStreamJob>& jobs,
            hls::stream<ap_uint<512> >& messages,
            hls::stream<ReStreamOrder>& output_order,
            hls::stream<ap_uint<32> >& output_words,
            hls::stream<RePerfEvent>& ingress_perf_events,
            hls::stream<RePerfRecord>& perf_out) {
#pragma HLS dataflow disable_start_propagation
    hls::stream<ReRuleUpdate> pu_rule_updates[PU_NM];
#pragma HLS stream variable=pu_rule_updates depth=2
    hls::stream<bool> pu_rule_update_done[PU_NM];
#pragma HLS stream variable=pu_rule_update_done depth=2
    hls::stream<ReStreamJob> pu_jobs[PU_NM];
#pragma HLS stream variable=pu_jobs depth=2
    hls::stream<ap_uint<512> > pu_messages[PU_NM];
#pragma HLS stream variable=pu_messages depth=8
    hls::stream<ap_uint<32> > pu_results[PU_NM];
#pragma HLS stream variable=pu_results depth=1024
#pragma HLS bind_storage variable=pu_results type=fifo impl=bram
    hls::stream<bool> done[PU_NM];
#pragma HLS stream variable=done depth=2
    hls::stream<ReStreamOrder> order;
#pragma HLS stream variable=order depth=64
    hls::stream<RePerfEvent> dispatch_perf_events;
#pragma HLS stream variable=dispatch_perf_events depth=64
    hls::stream<RePerfEvent> pu_perf_events[PU_NM];
#pragma HLS stream variable=pu_perf_events depth=8
    hls::stream<RePerfEvent> result_perf_events;
#pragma HLS stream variable=result_perf_events depth=64
    hls::stream<ReTimedPerfEvent> timed_ingress_perf_events;
#pragma HLS stream variable=timed_ingress_perf_events depth=128
    hls::stream<ReTimedPerfEvent> timed_dispatch_perf_events;
#pragma HLS stream variable=timed_dispatch_perf_events depth=128
    hls::stream<ReTimedPerfEvent> timed_pu_perf_events[PU_NM];
#pragma HLS stream variable=timed_pu_perf_events depth=128
    hls::stream<ReTimedPerfEvent> timed_result_perf_events;
#pragma HLS stream variable=timed_result_perf_events depth=128
    hls::stream<RePerfCounterSnapshot> perf_counter_snapshots;
#pragma HLS stream variable=perf_counter_snapshots depth=64
#pragma HLS bind_storage variable=perf_counter_snapshots type=fifo impl=bram

    broadcastRuleUpdates<PU_NM>(rule_updates, rule_update_done,
                                pu_rule_updates, pu_rule_update_done);
    dispatchMsgByFreePU<PU_NM>(jobs, messages, done, pu_jobs, pu_messages,
                               order, dispatch_perf_events);
    processingUnits<PU_NM>(pu_rule_updates, pu_rule_update_done,
                           pu_jobs, pu_messages, pu_results, done,
                           pu_perf_events);
    collectResults<PU_NM>(order, pu_results, output_order, output_words,
                          result_perf_events);
    timestampPerformanceEvents<PU_NM>(
        ingress_perf_events, dispatch_perf_events, pu_perf_events,
        result_perf_events, timed_ingress_perf_events,
        timed_dispatch_perf_events, timed_pu_perf_events,
        timed_result_perf_events, perf_counter_snapshots);
    monitorPerformance<PU_NM>(
        timed_ingress_perf_events, timed_dispatch_perf_events,
        timed_pu_perf_events, timed_result_perf_events,
        perf_counter_snapshots, perf_out);
}

} // namespace re_streaming

#endif
