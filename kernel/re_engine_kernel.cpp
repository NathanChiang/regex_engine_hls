#include "re_engine_kernel.hpp"
#include "re_engine_iova_table.hpp"
#include "re_engine_rule_table.hpp"
#include "re_engine_streaming.hpp"
#include "xf_data_analytics/text/re_engine.hpp"

#define RE_DMA_MAX_BEATS ((RE_MSG_LEN * 8) / 64) // RE_MSG_LEN is in 8-byte units

// AXI4-Stream keep has one bit per byte.
static ap_uint<7> keep_bytes(ap_uint<64> keep) {
#pragma HLS inline
    ap_uint<7> bytes = 0;
    for (int i = 0; i < 64; ++i) {
#pragma HLS unroll
        if (keep[i])
            bytes += 1;
    }
    return bytes;
}

static bool valid_keep(const DmaAxisBeat& beat) {
#pragma HLS inline
    ap_uint<64> next = beat.keep + 1;
    return beat.keep != 0 && (beat.keep & next) == 0 &&
           (beat.last || beat.keep == ap_uint<64>(-1));
}

// Minimal DMA front end.
//
// 1. On SOP, emit the DMA address to the IOVA table for one cycle.
// 2. Buffer the DMA packet while the IOVA -> rule lookup is running.
// 3. When both the packet and rule lookup are complete, return one packet_ready
//    pulse together with the selected job ID. The job ID indexes the
//    preprocessed instruction and bitset memories owned by the rule table.
static void dma_frontend(
    hls::stream<DmaAxisBeat>& dma_in,
    ap_uint<1> runtime_enable,
    const re_rule_lookup_status& rule_lookup,
    ap_uint<64>& dma_address,
    ap_uint<1>& dma_address_valid,
    ap_uint<1>& dma_busy,
    ap_uint<1>& packet_ready,
    ap_uint<5>& selected_job_id,
    ap_uint<16>& selected_cpgp_nm,
    ap_uint<1>& selected_config_valid,
    ap_uint<512> packet_buf[RE_DMA_MAX_BEATS],
    ap_uint<7>& packet_beats,
    ap_uint<16>& packet_len) {
// This is a one-cycle state-machine step called by the persistent ingress
// process.  Keep it in the ingress FSM; an ap_start/ap_done submodule would
// turn the step into a blocking transaction and stop ingress from polling
// the live MMIO command word.
#pragma HLS inline

    enum DmaState : unsigned char {
        DMA_IDLE = 0,
        DMA_ACTIVE,
        DMA_DRAIN
    };

    static DmaState state = DMA_IDLE;
#pragma HLS reset variable=state
    static ap_uint<1> packet_done = 0;
    static ap_uint<1> lookup_done = 0;
    static ap_uint<1> lookup_hit = 0;
    static ap_uint<1> overflow = 0;

    // Pulse outputs.
    dma_address_valid = 0;
    packet_ready = 0;

    if (state == DMA_IDLE) {
        dma_busy = 0;

        if (!runtime_enable || dma_in.empty())
            return;

        DmaAxisBeat beat = dma_in.read();

        // We only start a testcase on SOP. If framing is already broken,
        // discard until the current packet ends.
        if (!beat.user[0]) {
            if (!beat.last) {
                state = DMA_DRAIN;
                dma_busy = 1;
            }
            return;
        }

        packet_buf[0] = beat.data;
        packet_beats = 1;
        packet_len = keep_bytes(beat.keep);

        packet_done = beat.last;
        lookup_done = 0;
        lookup_hit = 0;
        overflow = !valid_keep(beat);
        // One-cycle lookup request. The IOVA table latches the address.
        dma_address = beat.user.range(64, 1);
        dma_address_valid = 1;

        dma_busy = 1;
        state = DMA_ACTIVE;
        return;
    }

    if (state == DMA_DRAIN) {
        dma_busy = 1;
        if (!dma_in.empty()) {
            DmaAxisBeat beat = dma_in.read();
            if (beat.last) {
                state = DMA_IDLE;
                dma_busy = 0;
            }
        }
        return;
    }

    // DMA_ACTIVE
    dma_busy = 1;

    // The rule table already chains the IOVA result internally. Its ack means
    // the complete IOVA -> job_id -> rule lookup is finished.
    if (rule_lookup.ack) {
        lookup_done = 1;
        lookup_hit = rule_lookup.hit;
        selected_job_id = rule_lookup.job_id.range(4, 0);
        selected_cpgp_nm = rule_lookup.cpgp_nm;
        selected_config_valid = rule_lookup.config_valid;
    }

    // Keep buffering the packet while the table lookup is running.
    if (!packet_done && !dma_in.empty()) {
        DmaAxisBeat beat = dma_in.read();

        if (beat.user[0] || !valid_keep(beat))
            overflow = 1;

        if (packet_beats < RE_DMA_MAX_BEATS) {
            packet_buf[packet_beats] = beat.data;
            packet_beats++;
            packet_len += keep_bytes(beat.keep);
        } else {
            overflow = 1;
        }

        if (beat.last)
            packet_done = 1;
    }

    // Release exactly one testcase when both sides are ready.
    if (lookup_done && packet_done) {
        if (lookup_hit && selected_config_valid && !overflow && packet_len != 0 &&
            packet_len <= RE_MAX_PACKET_BYTES) {
            packet_ready = 1;
        }

        state = DMA_IDLE;
        dma_busy = 0;
    }
}

static void mux_cfg_output(
    const cfg_input& cfg_in,
    const re_iova_cfg_status& iova_status,
    const re_rule_cfg_status& rule_status,
    ap_uint<1> result_busy,
    ap_uint<1> engine_ready,
    cfg_output& cfg_out) {
#pragma HLS inline

    cfg_out.ack = 0;
    cfg_out.full = 0;
    cfg_out.cmd_transition_allowed = 0;
    cfg_out.engine_ready = engine_ready;

    switch (cfg_in.cmd) {
    case RE_CFG_ADDR_WRITE:
    case RE_CFG_ADDR_DELETE:
        cfg_out.ack = iova_status.ack;
        cfg_out.full = iova_status.full;
        cfg_out.cmd_transition_allowed = iova_status.cmd_transition_allowed;
        break;

    case RE_CFG_RULE_WRITE:
    case RE_CFG_RULE_DELETE:
        cfg_out.ack = rule_status.ack;
        cfg_out.full = rule_status.full;
        cfg_out.cmd_transition_allowed = rule_status.cmd_transition_allowed;
        break;

    case RE_CFG_CLEAR:
        // Both tables receive CLEAR. The rule table clears immediately, while
        // the IOVA table clears one bucket/step, so IOVA owns completion.
        cfg_out.ack = iova_status.ack;
        cfg_out.full = iova_status.full;
        cfg_out.cmd_transition_allowed = iova_status.cmd_transition_allowed;
        break;

    case RE_CFG_NONE:
    default:
        cfg_out.cmd_transition_allowed =
            !result_busy &&
            iova_status.cmd_transition_allowed &&
            rule_status.cmd_transition_allowed;
        break;
    }
}

#ifdef __SYNTHESIS__
// Take one live AXI-Lite snapshot per ingress iteration.  Keeping this as a
// transaction boundary prevents HLS from hoisting the volatile RAM reads into
// the prologue of the non-terminating ingress process.
static void read_cfg_mmio(volatile ap_uint<32> cfg_mmio[6],
                          cfg_input& cfg_in) {
#pragma HLS inline off
    ap_uint<32> cfg_header = cfg_mmio[0];
    ap_uint<32> arg0_lo = cfg_mmio[2];
    ap_uint<32> arg0_hi = cfg_mmio[3];
    ap_uint<32> arg1_lo = cfg_mmio[4];
    ap_uint<32> arg1_hi = cfg_mmio[5];

    cfg_in.cmd = cfg_header.range(3, 0);
    cfg_in.valid = cfg_header[8];
    cfg_in.job_id = cfg_header.range(31, 16);
    cfg_in.arg0 = (ap_uint<64>(arg0_hi) << 32) | arg0_lo;
    cfg_in.arg1 = (ap_uint<64>(arg1_hi) << 32) | arg1_lo;
}

// This is the continuously running input/configuration side of the kernel.
// The AXI-Lite command and result ports remain live while VM workers run.
static void re_stream_ingress(hls::stream<DmaAxisBeat>& dma_in,
                              volatile ap_uint<32> cfg_mmio[6],
                              volatile ap_uint<32> cfg_out_mmio[1],
                              hls::stream<ReRuleUpdate>& rule_updates,
                              hls::stream<bool>& rule_update_done,
                              hls::stream<ReStreamJob>& jobs,
                              hls::stream<ap_uint<512> >& messages) {
    ap_uint<4> state = RE_CFG_NONE;
    ap_uint<1> cfg_ack_held = 0;
    ap_uint<1> cfg_full_held = 0;
    ap_uint<64> dma_address = 0;
    ap_uint<1> dma_address_valid = 0;
    ap_uint<1> dma_busy = 0;
    ap_uint<512> packet_buf[RE_DMA_MAX_BEATS];
#pragma HLS bind_storage variable=packet_buf type=ram_2p impl=bram
    ap_uint<7> packet_beats = 0;
    ap_uint<16> packet_len = 0;
    ap_uint<5> selected_job_id = 0;
    ap_uint<16> selected_cpgp_nm = 0;
    ap_uint<1> selected_config_valid = 0;

    while (true) {
        cfg_input cfg_in;
        cfg_output cfg_status;
        read_cfg_mmio(cfg_mmio, cfg_in);
        re_iova_cfg_status iova_status;
        re_iova_lookup_status iova_lookup;
        re_rule_cfg_status rule_status;
        re_rule_lookup_status rule_lookup;
        cfg_input table_cfg = cfg_in;
        table_cfg.cmd = state;
        if (state == RE_CFG_NONE && dma_busy) {
            table_cfg.valid = 0;
            table_cfg.job_id = 0;
            table_cfg.arg0 = 0;
            table_cfg.arg1 = 0;
        }
        re_engine_iova_table(table_cfg, dma_address, dma_address_valid,
                             iova_status, iova_lookup);
        re_engine_rule_table(table_cfg, iova_lookup,
                             rule_updates, rule_update_done,
                             rule_status, rule_lookup);

        ap_uint<1> packet_ready = 0;
        if (state == RE_CFG_NONE) {
            ap_uint<1> runtime_enable =
                !dma_busy && cfg_in.cmd == RE_CFG_NONE && !cfg_in.valid;
            dma_frontend(dma_in, runtime_enable, rule_lookup, dma_address,
                         dma_address_valid, dma_busy, packet_ready,
                         selected_job_id, selected_cpgp_nm,
                         selected_config_valid, packet_buf,
                         packet_beats, packet_len);

            if (packet_ready) {
                ReStreamJob job;
                job.length = packet_len;
                job.beats = packet_beats;
                job.rule = selected_job_id.range(3, 0);
                job.groups = selected_cpgp_nm;
                job.address = dma_address;

                jobs.write(job);
                for (int i = 0; i < packet_beats; ++i) {
#pragma HLS pipeline II=1
                    messages.write(packet_buf[i]);
                }
            }
        } else {
            dma_address_valid = 0;
        }

        cfg_input active_cfg = cfg_in;
        active_cfg.cmd = state;
        mux_cfg_output(active_cfg, iova_status, rule_status,
                       dma_busy, iova_status.table_ready && rule_status.table_ready,
                       cfg_status);
        if (!cfg_in.valid) {
            cfg_ack_held = 0;
            cfg_full_held = 0;
        } else {
            cfg_ack_held |= cfg_status.ack;
            cfg_full_held |= cfg_status.full;
            cfg_status.ack = cfg_ack_held;
            cfg_status.full = cfg_full_held;
        }
        if (state == RE_CFG_NONE && (dma_busy || packet_ready))
            cfg_status.cmd_transition_allowed = 0;

        if (cfg_in.cmd != state) {
            ap_uint<1> transition_allowed = 0;
            if (state == RE_CFG_NONE) {
                transition_allowed = !dma_busy && !packet_ready &&
                    iova_status.cmd_transition_allowed &&
                    rule_status.cmd_transition_allowed;
            } else if (state == RE_CFG_CLEAR) {
                transition_allowed = iova_status.cmd_transition_allowed;
            } else {
                transition_allowed =
                    (state == RE_CFG_ADDR_WRITE || state == RE_CFG_ADDR_DELETE)
                        ? iova_status.cmd_transition_allowed
                        : rule_status.cmd_transition_allowed;
            }
            if (transition_allowed) state = cfg_in.cmd;
        }
        ap_uint<32> status_word = 0;
        status_word[0] = cfg_status.ack;
        status_word[8] = cfg_status.full;
        status_word[16] = cfg_status.cmd_transition_allowed;
        status_word[24] = cfg_status.engine_ready;
        cfg_out_mmio[0] = status_word;
    }
}

// Keep MMIO reads and writes as explicit transactions.  A persistent process
// never returns, so ordinary output-reference stores are otherwise dead-store
// eliminated and direct volatile reads can be hoisted into its prologue.
static ap_uint<1> read_result_ack(volatile ap_uint<32> result_in[2]) {
#pragma HLS inline off
    ap_uint<32> ack_word = result_in[0];
    return ack_word[0];
}

static void write_result_mmio(volatile ap_uint<512> result_out[2],
                              const ap_uint<512>& payload,
                              const ap_uint<64>& address,
                              ap_uint<1> ack,
                              ap_uint<1> valid,
                              ap_uint<1> halt) {
#pragma HLS inline off
    ap_uint<512> metadata = 0;
    metadata.range(63, 0) = address;
    // Preserve the byte-aligned layout formerly generated for match_out:
    // address at 0xc0, then ack/valid/halt at bits 0/8/16 of 0xc8.
    metadata[64] = ack;
    metadata[72] = valid;
    metadata[80] = halt;
    result_out[0] = payload;
    result_out[1] = metadata;
}

static void re_stream_egress(volatile ap_uint<32> result_in[2],
                             volatile ap_uint<512> result_out[2],
                             hls::stream<ReStreamOrder>& order,
                             hls::stream<ap_uint<32> >& words) {
    write_result_mmio(result_out, 0, 0, 0, 0, 0);
    while (true) {
        ReStreamOrder ticket = order.read();
        ap_uint<16> total = ticket.groups + 1;
        for (int base = 0; base < total; base += 16) {
            ap_uint<512> chunk = 0;
            for (int i = 0; i < 16; ++i) {
#pragma HLS pipeline II=1
                if (base + i < total)
                    chunk.range(32 * i + 31, 32 * i) = words.read();
            }
            // Require low before each rising-edge acknowledgement.
            ap_uint<1> sampled_ack = read_result_ack(result_in);
            while (sampled_ack) {
                write_result_mmio(result_out, chunk, ticket.address, 1, 0, 0);
                sampled_ack = read_result_ack(result_in);
            }
            // Results wait for MMIO acknowledgement without stopping DMA;
            // queue backpressure is the only admission throttle.
            write_result_mmio(result_out, chunk, ticket.address, 0, 1, 0);
            sampled_ack = read_result_ack(result_in);
            while (!sampled_ack) {
                sampled_ack = read_result_ack(result_in);
            }
            write_result_mmio(result_out, chunk, ticket.address, 1, 0, 0);
            // Also clear the acknowledgement after the final chunk of a
            // record; there may be no next ticket to trigger this transition.
            sampled_ack = read_result_ack(result_in);
            while (sampled_ack) {
                sampled_ack = read_result_ack(result_in);
            }
            write_result_mmio(result_out, chunk, ticket.address, 0, 0, 0);
        }
    }
}
#endif

extern "C" void reEngineKernel(
    hls::stream<DmaAxisBeat>& dma_in,
#ifdef __SYNTHESIS__
    volatile ap_uint<32> cfg_in[6],
#else
    cfg_input cfg_in,
#endif
#ifdef __SYNTHESIS__
    volatile ap_uint<32> cfg_out[1],
#else
    cfg_output& cfg_out,
#endif
#ifdef __SYNTHESIS__
    volatile ap_uint<32> result_in[2],
#else
    ap_uint<1> result_in,
#endif
#ifdef __SYNTHESIS__
    volatile ap_uint<512> result_out[2]) {
#else
    match_out& result_out) {
#endif

#if defined(__SYNTHESIS__) || defined(RE_HLS_COSIM)
    // Transaction wrapper for C/RTL cosimulation of this multi-cycle step.
#pragma HLS interface s_axilite port=return bundle=control
#else
#pragma HLS interface ap_ctrl_none port=return
#endif
#pragma HLS interface axis port=dma_in
#pragma HLS interface s_axilite port=cfg_in bundle=control
#pragma HLS interface s_axilite port=cfg_out bundle=control
#pragma HLS interface s_axilite port=result_in bundle=control
#pragma HLS interface s_axilite port=result_out bundle=control
#ifdef __SYNTHESIS__
#pragma HLS dataflow disable_start_propagation
    // Rule writes cross the persistent-process boundary as explicit FIFO
    // transactions. The actual memories live inside the PUs, so HLS cannot
    // insert a transaction-level PIPO dependency on ingress completion.
    hls::stream<ReRuleUpdate> rule_updates("rule_updates");
#pragma HLS stream variable=rule_updates depth=2
    hls::stream<bool> rule_update_done("rule_update_done");
#pragma HLS stream variable=rule_update_done depth=2
    hls::stream<ReStreamJob> jobs("jobs");
#pragma HLS stream variable=jobs depth=64
    hls::stream<ap_uint<512> > messages("messages");
#pragma HLS stream variable=messages depth=1024
#pragma HLS bind_storage variable=messages type=fifo impl=bram
    hls::stream<ReStreamOrder> output_order("output_order");
#pragma HLS stream variable=output_order depth=64
    hls::stream<ap_uint<32> > output_words("output_words");
#pragma HLS stream variable=output_words depth=1024
#pragma HLS bind_storage variable=output_words type=fifo impl=bram

    re_stream_ingress(dma_in, cfg_in, cfg_out,
                      rule_updates, rule_update_done, jobs, messages);
    re_streaming::engine<RE_PU_NM>(rule_updates, rule_update_done,
                                   jobs, messages,
                                   output_order, output_words);
    re_stream_egress(result_in, result_out, output_order, output_words);
#else
    // Top-level FSM state. The numeric values intentionally match cfg_in.cmd.
    // A software command therefore requests the next kernel state directly.
    static ap_uint<4> state = RE_CFG_NONE;
#pragma HLS reset variable=state

    static volatile unsigned int msg_ready[1];
    static volatile unsigned int re_cfg_done[1];

    static hls::stream<ap_uint<512> > msg_strm("msg_strm");
#pragma HLS stream variable=msg_strm depth=64
#pragma HLS bind_storage variable=msg_strm type=fifo impl=bram

    static hls::stream<ap_uint<16> > len_strm("len_strm");
#pragma HLS stream variable=len_strm depth=2

    static ap_uint<32> re_out_buff[RE_CPGP_NM + 2];
#pragma HLS bind_storage variable=re_out_buff type=ram_2p impl=bram

    static ap_uint<512> packet_buf[RE_DMA_MAX_BEATS];
#pragma HLS bind_storage variable=packet_buf type=ram_2p impl=bram

    static ap_uint<7> packet_beats = 0;
    static ap_uint<16> packet_len = 0;
    static ap_uint<5> selected_job_id = 0;
    static ap_uint<16> selected_cpgp_nm = 0;
    static ap_uint<1> selected_config_valid = 0;

    static ap_uint<64> dma_address = 0;
    static ap_uint<1> dma_address_valid = 0;
    static ap_uint<1> dma_busy = 0;
#pragma HLS reset variable=dma_address_valid
#pragma HLS reset variable=dma_busy

    // reEngine writes one 32-bit word per match/capture result. Expose those
    // words sixteen at a time through the existing 512-bit result port.
    static ap_uint<1> result_pending = 0;
    static ap_uint<16> result_word_index = 1;
    static ap_uint<16> result_word_count = 1; // exclusive end index
    static ap_uint<64> result_address = 0;
    static ap_uint<1> result_ack_seen = 0;
#pragma HLS reset variable=result_pending
#pragma HLS reset variable=result_ack_seen
    const bool result_ack = result_in && !result_ack_seen;
    result_ack_seen = result_in;

    // AXI-Lite software cannot reliably observe a one-invocation ack pulse.
    static ap_uint<1> cfg_ack_held = 0;
    static ap_uint<1> cfg_full_held = 0;
#pragma HLS reset variable=cfg_ack_held
#pragma HLS reset variable=cfg_full_held
    if (!cfg_in.valid) {
        cfg_ack_held = 0;
        cfg_full_held = 0;
    }

    // Build a snapshot locally. Writing defaults directly into the AXI-Lite
    // output memory would briefly erase a pending result on every iteration.
    match_out next_result;
    next_result.ack = result_ack_seen;
    next_result.valid = 0;
    next_result.halt = 0;
    next_result.address = 0;
    next_result.output_buf = 0;

    if (result_pending) {
        // Consume the previous chunk first. The output below then presents
        // the next chunk in the same invocation, avoiding a duplicate chunk.
        if (result_ack) {
            if (result_word_index + 16 >= result_word_count) {
                result_pending = 0;
            } else {
                result_word_index += 16;
            }
        }

        if (result_pending) {
            next_result.valid = 1;
            next_result.halt = 1;
            next_result.address = result_address;

            for (int i = 0; i < 16; ++i) {
#pragma HLS pipeline II=1
                if (result_word_index + i < result_word_count) {
                    next_result.output_buf.range(32 * i + 31, 32 * i) =
                        re_out_buff[result_word_index + i];
                }
            }
        }
    }
    result_out = next_result;

    // Default status objects for this cycle.
    re_iova_cfg_status iova_status;
    re_iova_lookup_status iova_lookup;
    re_rule_cfg_status rule_status;
    re_rule_lookup_status rule_lookup;

    // The tables see the ACTIVE state, not an unaccepted new command.
    cfg_input table_cfg = cfg_in;
    table_cfg.cmd = state;

    // During runtime packet processing, configuration remains disabled until
    // the complete IOVA -> rule lookup and packet buffering operation finishes.
    if (state == RE_CFG_NONE && dma_busy) {
        table_cfg.valid = 0;
        table_cfg.job_id = 0;
        table_cfg.arg0 = 0;
        table_cfg.arg1 = 0;
    }

    // Both tables are clocked every kernel call. Internally they decide whether
    // the current state is a configuration operation or a runtime lookup.
    re_engine_iova_table(
        table_cfg,
        dma_address,
        dma_address_valid,
        iova_status,
        iova_lookup);

    re_engine_rule_table(
        table_cfg,
        iova_lookup,
        rule_status,
        rule_lookup);

    ap_uint<1> packet_ready = 0;

    switch (state) {

    case RE_CFG_NONE: {
        // Runtime state. New DMA packets may start only while software is also
        // requesting NONE and no configuration write is asserted.
        ap_uint<1> runtime_enable =
            (!dma_busy && !result_pending && cfg_in.cmd == RE_CFG_NONE && !cfg_in.valid);

        dma_frontend(
            dma_in,
            runtime_enable,
            rule_lookup,
            dma_address,
            dma_address_valid,
            dma_busy,
            packet_ready,
            selected_job_id,
            selected_cpgp_nm,
            selected_config_valid,
            packet_buf,
            packet_beats,
            packet_len);

        if (packet_ready) {
            len_strm.write(packet_len);
            for (int i = 0; i < RE_DMA_MAX_BEATS; ++i) {
#pragma HLS pipeline II=1
                if (i < packet_beats)
                    msg_strm.write(packet_buf[i]);
            }
            len_strm.write(0);

            msg_ready[0] = 1;
            re_cfg_done[0] = 0;

            xf::data_analytics::text::reEngine<
                RE_PU_NM,
                RE_INSTR_DEPTH,
                RE_CCLASS_NM,
                RE_CPGP_NM,
                RE_MSG_LEN,
                RE_STACK_SIZE>(
                    selected_cpgp_nm,
                    re_rule_instr_mem[0][selected_job_id],
                    re_rule_bitset_mem[0][selected_job_id],
                    msg_ready,
                    re_cfg_done,
                    msg_strm,
                    len_strm,
                    re_out_buff);

            // re_out_buff[0] is the exclusive end offset; result words begin
            // at re_out_buff[1]. Clamp the value to the allocated buffer.
            ap_uint<32> result_end = re_out_buff[0];
            if (result_end > RE_CPGP_NM + 2)
                result_end = RE_CPGP_NM + 2;
            result_word_index = 1;
            result_word_count = result_end;
            result_address = dma_address;
            result_pending = (result_end > 1);
        }
        break;
    }

    case RE_CFG_ADDR_WRITE:
    case RE_CFG_ADDR_DELETE:
        // IOVA table owns these states. table_cfg carries cfg_in.valid/job_id/
        // arg0/arg1 directly into re_engine_iova_table().
        break;

    case RE_CFG_RULE_WRITE:
    case RE_CFG_RULE_DELETE:
        // Rule table owns these states.
        break;

    case RE_CFG_CLEAR:
        // Both tables receive CLEAR. IOVA completion controls when the state may
        // change because its clear operation spans multiple cycles.
        break;

    default:
        // Recover from an invalid state/command by returning to runtime mode.
        state = RE_CFG_NONE;
        break;
    }

    // Report status for the ACTIVE state.
    cfg_input active_cfg = cfg_in;
    active_cfg.cmd = state;
    mux_cfg_output(
        active_cfg,
        iova_status,
        rule_status,
        result_pending,
        (ap_uint<1>)(iova_status.table_ready && rule_status.table_ready),
        cfg_out);

    if (cfg_in.valid) {
        cfg_ack_held |= cfg_out.ack;
        cfg_full_held |= cfg_out.full;
        cfg_out.ack = cfg_ack_held;
        cfg_out.full = cfg_full_held;
    }
    // A packet can be buffered after the tables finish their lookup.
    if (state == RE_CFG_NONE && dma_busy)
        cfg_out.cmd_transition_allowed = 0;

    // ------------------------------------------------------------
    // State transition
    // ------------------------------------------------------------
    // cfg_in.cmd is the requested next state. Do not switch in the middle of
    // an active DMA transaction or before the current configuration block says
    // that its command may be changed.
    if (cfg_in.cmd != state) {
        ap_uint<1> transition_allowed = 0;

        switch (state) {
        case RE_CFG_NONE:
            transition_allowed =
                !dma_busy &&
                !result_pending &&
                iova_status.cmd_transition_allowed &&
                rule_status.cmd_transition_allowed;
            break;

        case RE_CFG_ADDR_WRITE:
        case RE_CFG_ADDR_DELETE:
            transition_allowed = iova_status.cmd_transition_allowed;
            break;

        case RE_CFG_RULE_WRITE:
        case RE_CFG_RULE_DELETE:
            transition_allowed = rule_status.cmd_transition_allowed;
            break;

        case RE_CFG_CLEAR:
            // IOVA clear is the long-running clear operation.
            transition_allowed = iova_status.cmd_transition_allowed;
            break;

        default:
            transition_allowed = 1;
            break;
        }

        if (transition_allowed)
            state = cfg_in.cmd;
    }
#endif
}
