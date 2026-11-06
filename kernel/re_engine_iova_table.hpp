#ifndef RE_ENGINE_IOVA_TABLE_HPP
#define RE_ENGINE_IOVA_TABLE_HPP

#include <ap_int.h>
#include "re_engine_kernel.hpp"

// Maximum number of logical IOVA -> job_id mappings.
#ifndef RE_IOVA_TABLE_ENTRIES
#define RE_IOVA_TABLE_ENTRIES 1024
#endif

// Runtime lookup organization.
// A DMA address is reduced to a 4-KB page number.  The low 10 bits of that
// page number select one of 1024 buckets.  Each bucket stores up to 8 complete
// range entries and all 8 are compared in parallel during runtime lookup.
#ifndef RE_IOVA_PAGE_SHIFT
#define RE_IOVA_PAGE_SHIFT 12
#endif

#ifndef RE_IOVA_BUCKETS
#define RE_IOVA_BUCKETS 1024
#endif

#ifndef RE_IOVA_BUCKET_DEPTH
#define RE_IOVA_BUCKET_DEPTH 8
#endif

#define RE_IOVA_INDEX_WIDTH  10
#define RE_IOVA_BUCKET_WIDTH 10

static_assert(RE_IOVA_TABLE_ENTRIES == 1024 && RE_IOVA_BUCKETS == 1024 &&
              RE_IOVA_BUCKET_DEPTH == 8 && RE_IOVA_PAGE_SHIFT == 12,
              "IOVA counter/address widths require the 1024 x 8, 4-KB layout");

typedef ap_uint<52> re_iova_page_t;

// Logical configuration entry.  This directory is used only by configuration
// operations so exact delete semantics and the 1024-mapping capacity are kept.
struct re_iova_entry {
    ap_uint<1>  valid;
    ap_uint<16> job_id;
    ap_uint<64> base;
    ap_uint<64> limit;
};

// Runtime bucket entry.  Store the complete range so a packet-time lookup does
// not need a second random read from the logical directory.
struct re_iova_bucket_entry {
    ap_uint<1>  valid;
    ap_uint<RE_IOVA_INDEX_WIDTH> map_index;
    ap_uint<16> job_id;
    ap_uint<64> base;
    ap_uint<64> limit;
};

// Local status from the IOVA configuration block.
struct re_iova_cfg_status {
    ap_uint<1> ack;
    ap_uint<1> full;
    ap_uint<1> cmd_transition_allowed;
    ap_uint<1> table_ready;
};

// Runtime IOVA lookup response.
struct re_iova_lookup_status {
    ap_uint<1>  ack;
    ap_uint<1>  hit;
    ap_uint<16> job_id;
};

// Number of distinct modulo-1024 buckets touched by [base, limit).
// For ranges spanning >= 1024 pages, every bucket is required exactly once.
inline ap_uint<11> re_iova_bucket_count(ap_uint<64> base, ap_uint<64> limit)
{
#pragma HLS inline
    re_iova_page_t first_page = base >> RE_IOVA_PAGE_SHIFT;
    re_iova_page_t last_page  = (limit - 1) >> RE_IOVA_PAGE_SHIFT;
    re_iova_page_t page_span  = last_page - first_page;

    if (page_span >= RE_IOVA_BUCKETS - 1)
        return RE_IOVA_BUCKETS;

    return (ap_uint<11>)(page_span + 1);
}

// Direct 4-KB-page bucket mapping.  Consecutive ranges shorter than 1024 pages
// visit unique buckets; a range of 1024 or more pages visits every bucket.
inline ap_uint<RE_IOVA_BUCKET_WIDTH> re_iova_bucket_id(
    re_iova_page_t first_page,
    ap_uint<11> bucket_offset)
{
#pragma HLS inline
    re_iova_page_t page = first_page + bucket_offset;
    return page.range(RE_IOVA_BUCKET_WIDTH - 1, 0);
}

/*
 * re_engine_iova_table
 * --------------------
 * Configuration remains multi-cycle, but runtime lookup is bucketed:
 *
 *     dma_address -> 4-KB page -> one bucket -> 8 parallel range compares
 *
 * Supported commands and external handshake semantics are kept compatible
 * with the previous table implementation:
 *
 *   RE_CFG_ADDR_WRITE
 *       Insert exact {job_id, base, limit}.  A logical mapping may occupy more
 *       than one runtime bucket.  The write first prechecks every required
 *       bucket, so bucket overflow cannot leave a partially installed mapping.
 *
 *   RE_CFG_ADDR_DELETE
 *       Delete the first exact {job_id, base, limit} mapping, matching the
 *       lowest logical table index just like the previous linear table scan.
 *
 *   RE_CFG_CLEAR
 *       Clear all logical entries and all runtime bucket entries.
 *
 * Runtime lookup
 *       Legal only when cfg_in.cmd == RE_CFG_NONE.  The selected bucket is
 *       read once and all 8 complete ranges are compared in parallel.  If
 *       overlapping mappings match, the lowest logical map_index wins, which
 *       preserves the priority behavior of the previous table scan.
 */
inline void re_engine_iova_table(
    const cfg_input& cfg_in,
    const ap_uint<64>& dma_address,
    const ap_uint<1>& dma_address_valid,
    re_iova_cfg_status& status,
    re_iova_lookup_status& lookup)
{
// One invocation advances this table FSM by one step.  It must be part of the
// persistent ingress FSM so ingress can reread MMIO between steps.
#pragma HLS inline

    enum State : unsigned char {
        IOVA_IDLE = 0,
        IOVA_WRITE_FIND_FREE,
        IOVA_WRITE_PRECHECK,
        IOVA_WRITE_DIRECTORY,
        IOVA_WRITE_BUCKETS,
        IOVA_DELETE_FIND,
        IOVA_DELETE_BUCKETS,
        IOVA_DELETE_DIRECTORY,
        IOVA_CLEAR,
        IOVA_LOOKUP,
        IOVA_WAIT_VALID_LOW,
        IOVA_WAIT_LOOKUP_VALID_LOW,
        IOVA_DELETE_CHECK
    };

    // Logical directory.  This is not used on the runtime lookup datapath.
    static re_iova_entry directory[RE_IOVA_TABLE_ENTRIES];
#pragma HLS bind_storage variable=directory type=ram_2p impl=bram

    // Eight independent memories.  One entry from each lane is read for the
    // selected bucket, providing eight complete candidate ranges in parallel.
    static re_iova_bucket_entry bucket[RE_IOVA_BUCKET_DEPTH][RE_IOVA_BUCKETS];
#pragma HLS array_partition variable=bucket complete dim=1
#pragma HLS bind_storage variable=bucket type=ram_2p impl=bram

    static State state = IOVA_IDLE;
#pragma HLS reset variable=state

    static ap_uint<11> valid_count = 0; // 0..1024
    static ap_uint<1> table_full = 0;

    // Generic scan index for the logical directory and clear operation.
    static ap_uint<RE_IOVA_INDEX_WIDTH> scan_index = 0;
    // Register the wide delete comparison before it drives the scan/control FSM.
    static ap_uint<1> delete_match = 0;

    // Latched configuration request.
    static ap_uint<16> req_job_id = 0;
    static ap_uint<64> req_base = 0;
    static ap_uint<64> req_limit = 0;
    static ap_uint<RE_IOVA_INDEX_WIDTH> req_map_index = 0;
    static re_iova_page_t req_first_page = 0;
    static ap_uint<11> req_bucket_count = 0;
    static ap_uint<11> bucket_offset = 0;

    // Latched runtime lookup request.
    static ap_uint<64> req_dma_address = 0;

    // Pulse outputs default low on every invocation.
    status.ack = 0;
    status.full = 0;
    status.cmd_transition_allowed = 0;
    status.table_ready = (valid_count != 0);

    lookup.ack = 0;
    lookup.hit = 0;
    lookup.job_id = 0;

    switch (state) {
    case IOVA_IDLE:
        // Configuration has priority over runtime lookup.
        if (cfg_in.valid) {
            if (cfg_in.cmd == RE_CFG_ADDR_WRITE) {
                // Ranges are half-open: [base, limit).
                if (cfg_in.arg0 >= cfg_in.arg1) {
                    // Preserve previous behavior: malformed range completes
                    // without ack and waits for cfg_in.valid to drop.
                    state = IOVA_WAIT_VALID_LOW;
                    break;
                }

                req_job_id = cfg_in.job_id;
                req_base = cfg_in.arg0;
                req_limit = cfg_in.arg1;
                req_first_page = cfg_in.arg0 >> RE_IOVA_PAGE_SHIFT;
                req_bucket_count = re_iova_bucket_count(cfg_in.arg0, cfg_in.arg1);
                scan_index = 0;
                state = IOVA_WRITE_FIND_FREE;
            }
            else if (cfg_in.cmd == RE_CFG_ADDR_DELETE) {
                req_job_id = cfg_in.job_id;
                req_base = cfg_in.arg0;
                req_limit = cfg_in.arg1;

                if (cfg_in.arg0 < cfg_in.arg1) {
                    req_first_page = cfg_in.arg0 >> RE_IOVA_PAGE_SHIFT;
                    req_bucket_count = re_iova_bucket_count(cfg_in.arg0, cfg_in.arg1);
                }
                else {
                    req_first_page = 0;
                    req_bucket_count = 0;
                }

                scan_index = 0;
                state = IOVA_DELETE_FIND;
            }
            else if (cfg_in.cmd == RE_CFG_CLEAR) {
                scan_index = 0;
                state = IOVA_CLEAR;
            }
            // Other configuration commands belong to other blocks.
        }
        else if (cfg_in.cmd == RE_CFG_NONE && dma_address_valid) {
            req_dma_address = dma_address;
            state = IOVA_LOOKUP;
        }
        else {
            status.cmd_transition_allowed = 1;
        }
        break;

    case IOVA_WRITE_FIND_FREE: {
        re_iova_entry entry = directory[scan_index];

        if (!entry.valid) {
            req_map_index = scan_index;
            bucket_offset = 0;
            state = IOVA_WRITE_PRECHECK;
        }
        else if (scan_index == RE_IOVA_TABLE_ENTRIES - 1) {
            table_full = 1;
            status.full = 1;
            state = IOVA_WAIT_VALID_LOW;
        }
        else {
            scan_index++;
        }
        break;
    }

    case IOVA_WRITE_PRECHECK: {
        const ap_uint<RE_IOVA_BUCKET_WIDTH> bucket_id =
            re_iova_bucket_id(req_first_page, bucket_offset);

        bool has_free = false;

        // Check all eight lanes in parallel.  Nothing is written in this state,
        // so a later overflow cannot leave a partially installed mapping.
        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            re_iova_bucket_entry entry = bucket[lane][bucket_id];
            if (!entry.valid)
                has_free = true;
        }

        if (!has_free) {
            status.full = 1;
            state = IOVA_WAIT_VALID_LOW;
        }
        else if (bucket_offset + 1 == req_bucket_count) {
            state = IOVA_WRITE_DIRECTORY;
        }
        else {
            bucket_offset++;
        }
        break;
    }

    case IOVA_WRITE_DIRECTORY: {
        re_iova_entry new_entry;
        new_entry.valid = 1;
        new_entry.job_id = req_job_id;
        new_entry.base = req_base;
        new_entry.limit = req_limit;
        directory[req_map_index] = new_entry;

        if (valid_count < RE_IOVA_TABLE_ENTRIES) {
            if (valid_count == RE_IOVA_TABLE_ENTRIES - 1)
                table_full = 1;
            valid_count++;
        }

        bucket_offset = 0;
        state = IOVA_WRITE_BUCKETS;
        break;
    }

    case IOVA_WRITE_BUCKETS: {
        const ap_uint<RE_IOVA_BUCKET_WIDTH> bucket_id =
            re_iova_bucket_id(req_first_page, bucket_offset);

        bool found_free = false;
        ap_uint<3> free_lane = 0;

        // Precheck guarantees a free lane still exists.  Configuration is
        // serialized, so no other operation can consume it between states.
        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            re_iova_bucket_entry entry = bucket[lane][bucket_id];
            if (!found_free && !entry.valid) {
                found_free = true;
                free_lane = lane;
            }
        }

        re_iova_bucket_entry new_entry;
        new_entry.valid = 1;
        new_entry.map_index = req_map_index;
        new_entry.job_id = req_job_id;
        new_entry.base = req_base;
        new_entry.limit = req_limit;

        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            if (found_free && free_lane == lane)
                bucket[lane][bucket_id] = new_entry;
        }

        if (bucket_offset + 1 == req_bucket_count) {
            status.ack = 1;
            status.full = table_full;
            status.table_ready = 1;
            state = IOVA_WAIT_VALID_LOW;
        }
        else {
            bucket_offset++;
        }
        break;
    }

    case IOVA_DELETE_FIND: {
        re_iova_entry entry = directory[scan_index];
        delete_match = entry.valid &&
                       entry.job_id == req_job_id &&
                       entry.base == req_base &&
                       entry.limit == req_limit;
        state = IOVA_DELETE_CHECK;
        break;
    }

    case IOVA_DELETE_CHECK: {
        if (delete_match) {
            req_map_index = scan_index;
            bucket_offset = 0;
            state = IOVA_DELETE_BUCKETS;
        }
        else if (scan_index == RE_IOVA_TABLE_ENTRIES - 1) {
            // Preserve previous behavior: deleting a missing mapping still
            // completes successfully from the command-handshake perspective.
            status.ack = 1;
            state = IOVA_WAIT_VALID_LOW;
        }
        else {
            scan_index++;
            state = IOVA_DELETE_FIND;
        }
        break;
    }

    case IOVA_DELETE_BUCKETS: {
        const ap_uint<RE_IOVA_BUCKET_WIDTH> bucket_id =
            re_iova_bucket_id(req_first_page, bucket_offset);

        // Exactly one copy with this map_index can exist in each touched bucket.
        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            re_iova_bucket_entry entry = bucket[lane][bucket_id];
            if (entry.valid && entry.map_index == req_map_index) {
                entry.valid = 0;
                bucket[lane][bucket_id] = entry;
            }
        }

        if (bucket_offset + 1 == req_bucket_count) {
            state = IOVA_DELETE_DIRECTORY;
        }
        else {
            bucket_offset++;
        }
        break;
    }

    case IOVA_DELETE_DIRECTORY: {
        re_iova_entry entry = directory[req_map_index];
        entry.valid = 0;
        directory[req_map_index] = entry;

        if (valid_count != 0)
            valid_count--;

        table_full = 0;
        status.full = 0;
        status.ack = 1;
        status.table_ready = (valid_count != 0);
        state = IOVA_WAIT_VALID_LOW;
        break;
    }

    case IOVA_CLEAR: {
        // Clear one logical entry and one whole runtime bucket per cycle.
        re_iova_entry dir_entry;
        dir_entry.valid = 0;
        dir_entry.job_id = 0;
        dir_entry.base = 0;
        dir_entry.limit = 0;
        directory[scan_index] = dir_entry;

        re_iova_bucket_entry bucket_entry;
        bucket_entry.valid = 0;
        bucket_entry.map_index = 0;
        bucket_entry.job_id = 0;
        bucket_entry.base = 0;
        bucket_entry.limit = 0;

        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            bucket[lane][scan_index] = bucket_entry;
        }

        if (scan_index == RE_IOVA_TABLE_ENTRIES - 1) {
            valid_count = 0;
            table_full = 0;
            status.full = 0;
            status.table_ready = 0;
            status.ack = 1;
            state = IOVA_WAIT_VALID_LOW;
        }
        else {
            scan_index++;
        }
        break;
    }

    case IOVA_LOOKUP: {
        const re_iova_page_t page = req_dma_address >> RE_IOVA_PAGE_SHIFT;
        const ap_uint<RE_IOVA_BUCKET_WIDTH> bucket_id =
            page.range(RE_IOVA_BUCKET_WIDTH - 1, 0);

        bool found_match = false;
        ap_uint<RE_IOVA_INDEX_WIDTH> best_index = 0;
        ap_uint<16> found_job_id = 0;

        // Eight complete range entries are read and compared in parallel.
        // Lowest map_index wins for compatibility with the old table scan.
        for (int lane = 0; lane < RE_IOVA_BUCKET_DEPTH; ++lane) {
#pragma HLS unroll
            re_iova_bucket_entry entry = bucket[lane][bucket_id];
            bool match = entry.valid &&
                         req_dma_address >= entry.base &&
                         req_dma_address < entry.limit;

            if (match && (!found_match || entry.map_index < best_index)) {
                found_match = true;
                best_index = entry.map_index;
                found_job_id = entry.job_id;
            }
        }

        lookup.ack = 1;
        lookup.hit = found_match ? 1 : 0;
        lookup.job_id = found_match ? found_job_id : (ap_uint<16>)0;
        state = IOVA_WAIT_LOOKUP_VALID_LOW;
        break;
    }

    case IOVA_WAIT_VALID_LOW:
        // Prevent a level-held configuration valid from being consumed twice.
        if (!cfg_in.valid) {
            status.cmd_transition_allowed = 1;
            state = IOVA_IDLE;
        }
        break;

    case IOVA_WAIT_LOOKUP_VALID_LOW:
        // Prevent a level-held runtime lookup valid from being consumed twice.
        if (!dma_address_valid) {
            status.cmd_transition_allowed = 1;
            state = IOVA_IDLE;
        }
        break;

    default:
        state = IOVA_IDLE;
        break;
    }
}

#endif // RE_ENGINE_IOVA_TABLE_HPP
