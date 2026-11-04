/*
 * Copyright 2019-2026 Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file regexVM_new.hpp
 * @brief Experimental higher-throughput regular-expression VM.
 *
 * This header is intentionally separate from regexVM.hpp so the new VM can be
 * synthesized and regression-tested without changing the production engine.
 * The public function has the same arguments as regexVM_opt, but is named
 * regexVM_new.
 *
 * The implementation contains three throughput-oriented changes:
 *   - six adjacent, side-effect-free consuming instructions may be committed
 *     in one VM iteration;
 *   - the six-lane front end uses the two ports of each backing RAM over a
 *     target II of three, amortizing the VM feedback path across the batch;
 *   - OP_ANYCHAR_STAR scans as many as 24 bytes per iteration and represents
 *     all of its byte-by-byte backtracking choices with one range stack entry.
 *
 * Stack entry types 0..2 retain the production VM encoding. Type 4 is private
 * to this implementation and encodes:
 *   [53:48] type, [47:32] range begin, [31:16] retry PC, [15:0] retry position.
 */

#ifndef XF_TEXT_REGEX_VM_NEW_H
#define XF_TEXT_REGEX_VM_NEW_H

#include "xf_data_analytics/text/regexVM.hpp"

namespace xf {
namespace data_analytics {
namespace text {
namespace internal_new {

static const ap_uint<6> kRangeStackType = 4;
static const ap_uint<6> kBitmapStackType = 5;

static inline bool isSimpleAtom(ap_uint<8> opcode) {
#pragma HLS inline
    // Character classes use a data-dependent bitset address. Keep them on the
    // scalar path so a six-lane batch does not require six bitset RAM ports.
    return opcode == OP_STR_N || opcode == OP_ANYCHAR;
}

static inline ap_uint<3> atomLength(ap_uint<8> opcode, ap_uint<4> encoded_len) {
#pragma HLS inline
    return opcode == OP_STR_N ? (ap_uint<3>)encoded_len : (ap_uint<3>)1;
}

static inline ap_uint<54> makeStackEntry(ap_uint<6> type,
                                         ap_uint<16> mem_id,
                                         ap_uint<16> mem_start,
                                         ap_uint<16> mem_end,
                                         ap_uint<16> opaddr,
                                         ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<54> value = 0;
    internal::combine(value, type, mem_id, mem_start, mem_end, opaddr, str_ptr);
    return value;
}

static inline ap_uint<54> makeRangeEntry(ap_uint<16> begin, ap_uint<16> retry_pc, ap_uint<16> retry_pos) {
#pragma HLS inline
    ap_uint<54> value = 0;
    value.range(53, 48) = kRangeStackType;
    value.range(47, 32) = begin;
    value.range(31, 16) = retry_pc;
    value.range(15, 0) = retry_pos;
    return value;
}

static inline ap_uint<54> makeBitmapEntry(ap_uint<16> begin, ap_uint<16> retry_pc, ap_uint<8> retry_mask) {
#pragma HLS inline
    ap_uint<54> value = 0;
    value.range(53, 48) = kBitmapStackType;
    value.range(47, 32) = begin;
    value.range(31, 16) = retry_pc;
    value.range(7, 0) = retry_mask;
    return value;
}

static inline ap_uint<8> windowByte(ap_uint<192> window, ap_uint<6> byte_offset) {
#pragma HLS inline
    ap_uint<192> shifted = window >> (byte_offset * 8);
    return shifted.range(7, 0);
}

static inline bool atomMatches(ap_uint<64> instruction,
                               ap_uint<192> window,
                               ap_uint<6> byte_offset,
                               ap_uint<16> str_ptr,
                               ap_uint<16> len_msg,
                               ap_uint<32> class_word) {
#pragma HLS inline
    ap_uint<8> opcode = instruction.range(63, 56);
    ap_uint<4> len = instruction.range(51, 48);
    ap_uint<3> consume = atomLength(opcode, len);
    bool in_bounds = ((ap_uint<17>)str_ptr + byte_offset + consume) <= len_msg;
    bool matched = false;

    if (opcode == OP_STR_N) {
        bool literal_match = len <= 4;
        for (int k = 0; k < 4; ++k) {
#pragma HLS unroll
            if (k < len) {
                ap_uint<8> expected = instruction.range(47 - 8 * k, 40 - 8 * k);
                ap_uint<8> actual = windowByte(window, byte_offset + k);
                literal_match = literal_match && (expected == actual);
            }
        }
        matched = in_bounds && literal_match;
    } else if (opcode == OP_CCLASS) {
        ap_uint<8> ch = windowByte(window, byte_offset);
        matched = in_bounds && class_word[(unsigned int)ch & 0x1f];
    } else if (opcode == OP_ANYCHAR) {
        ap_uint<8> ch = windowByte(window, byte_offset);
        matched = in_bounds && ch != 0x0a;
    }
    return matched;
}

template <int INSTR_DEPTH>
static inline ap_uint<384> fetchInstructionWindow(ap_uint<64>* instr_buff, ap_uint<16> opaddr) {
#pragma HLS inline
    ap_uint<384> window = 0;
    for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
        ap_uint<17> addr = (ap_uint<17>)opaddr + lane;
        ap_uint<64> instruction = addr < INSTR_DEPTH ? instr_buff[addr] : (ap_uint<64>)OP_END << 56;
        window.range(64 * lane + 63, 64 * lane) = instruction;
    }
    return window;
}

template <int INSTR_DEPTH>
static inline ap_uint<384> fetchInstructionWindow(ap_uint<512>* instr_buff, ap_uint<16> opaddr) {
#pragma HLS inline
    ap_uint<13> word_addr = opaddr >> 3;
    ap_uint<3> lane_addr = opaddr.range(2, 0);
    ap_uint<17> first_instruction = (ap_uint<17>)word_addr << 3;
    ap_uint<512> first = first_instruction < INSTR_DEPTH ? instr_buff[word_addr] : (ap_uint<512>)0;
    ap_uint<512> second = first_instruction + 8 < INSTR_DEPTH ? instr_buff[word_addr + 1] : (ap_uint<512>)0;
    ap_uint<64> words[16];
#pragma HLS array_partition variable = words complete
    for (int k = 0; k < 8; ++k) {
#pragma HLS unroll
        words[k] = first.range(64 * k + 63, 64 * k);
        words[k + 8] = second.range(64 * k + 63, 64 * k);
    }
    ap_uint<384> window = 0;
    for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
        window.range(64 * lane + 63, 64 * lane) = words[lane_addr + lane];
    }
    return window;
}

static inline ap_uint<192> fetchMessageWindow(ap_uint<32>* msg_buff,
                                               ap_uint<16> str_ptr,
                                               unsigned int len_msg,
                                               bool need_previous,
                                               ap_uint<8>& prev_char,
                                               ap_uint<7>& window_bytes) {
#pragma HLS inline
    ap_uint<2> byte_pos = str_ptr.range(1, 0);
    ap_uint<16> word_addr = str_ptr >> 2;
    ap_uint<16> base_addr = need_previous ? (ap_uint<16>)(word_addr - 1) : word_addr;
    ap_uint<17> word_count = ((ap_uint<17>)len_msg + 3) >> 2;
    ap_uint<192> raw = 0;
    for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
        ap_uint<17> addr = (ap_uint<17>)base_addr + lane;
        ap_uint<32> word = addr < word_count ? msg_buff[addr] : (ap_uint<32>)0;
        raw.range(32 * lane + 31, 32 * lane) = word;
    }
    ap_uint<3> byte_shift = need_previous ? (ap_uint<3>)4 : (ap_uint<3>)byte_pos;
    prev_char = 0x0a;
    if (str_ptr > 0 && byte_shift != 0) {
        ap_uint<192> prev_aligned = raw >> ((byte_shift - 1) * 8);
        prev_char = prev_aligned.range(7, 0);
    }
    window_bytes = 24 - byte_shift;
    return raw >> (byte_shift * 8);
}

static inline ap_uint<192> fetchMessageWindow(ap_uint<512>* msg_buff,
                                               ap_uint<16> str_ptr,
                                               unsigned int len_msg,
                                               bool need_previous,
                                               ap_uint<8>& prev_char,
                                               ap_uint<7>& window_bytes) {
#pragma HLS inline
    ap_uint<6> byte_pos = str_ptr.range(5, 0);
    ap_uint<10> word_addr = str_ptr >> 6;
    ap_uint<10> base_addr = need_previous ? (ap_uint<10>)(word_addr - 1) : word_addr;
    ap_uint<11> word_count = ((ap_uint<17>)len_msg + 63) >> 6;
    ap_uint<512> first = base_addr < word_count ? msg_buff[base_addr] : (ap_uint<512>)0;
    ap_uint<512> second = base_addr + 1 < word_count ? msg_buff[base_addr + 1] : (ap_uint<512>)0;
    ap_uint<64> chunks[16];
#pragma HLS array_partition variable = chunks complete
    for (int k = 0; k < 8; ++k) {
#pragma HLS unroll
        chunks[k] = first.range(64 * k + 63, 64 * k);
        chunks[k + 8] = second.range(64 * k + 63, 64 * k);
    }
    ap_uint<4> chunk_addr = byte_pos >> 3;
    ap_uint<3> byte_in_chunk = byte_pos.range(2, 0);
    ap_uint<192> window = 0;
    for (int lane = 0; lane < 3; ++lane) {
#pragma HLS unroll
        ap_uint<128> pair = ((ap_uint<128>)chunks[chunk_addr + lane + 1] << 64) |
                            chunks[chunk_addr + lane];
        ap_uint<128> aligned = pair >> (byte_in_chunk * 8);
        window.range(64 * lane + 63, 64 * lane) = aligned.range(63, 0);
    }
    prev_char = 0x0a;
    window_bytes = 24;
    return window;
}

static inline bool isMessageWordBoundary(ap_uint<32>*, ap_uint<16> str_ptr) {
#pragma HLS inline
    return str_ptr.range(1, 0) == 0;
}

static inline bool isMessageWordBoundary(ap_uint<512>*, ap_uint<16> str_ptr) {
#pragma HLS inline
    return str_ptr.range(5, 0) == 0;
}

static inline ap_uint<8> fetchPreviousBoundaryByte(ap_uint<32>* msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    return msg_buff[(str_ptr >> 2) - 1].range(31, 24);
}

static inline ap_uint<8> fetchPreviousBoundaryByte(ap_uint<512>* msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    return msg_buff[(str_ptr >> 6) - 1].range(511, 504);
}

static inline ap_uint<8> fetchPreviousByte(ap_uint<32>* msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<16> word_addr = str_ptr >> 2;
    ap_uint<2> byte_pos = str_ptr.range(1, 0);
    if (byte_pos == 0) return msg_buff[word_addr - 1].range(31, 24);
    ap_uint<32> shifted = msg_buff[word_addr] >> ((byte_pos - 1) * 8);
    return shifted.range(7, 0);
}

static inline ap_uint<8> fetchPreviousByte(ap_uint<512>* msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<10> word_addr = str_ptr >> 6;
    ap_uint<6> byte_pos = str_ptr.range(5, 0);
    if (byte_pos == 0) return msg_buff[word_addr - 1].range(511, 504);
    ap_uint<512> shifted = msg_buff[word_addr] >> ((byte_pos - 1) * 8);
    return shifted.range(7, 0);
}

} // namespace internal_new

/**
 * @brief Experimental optimized regex VM.
 *
 * @tparam STACK_SIZE Number of physical stack entries. Compressed range
 * entries make OP_ANYCHAR_STAR consume one physical entry instead of one entry
 * per input byte.
 * @tparam INSTR_DEPTH Number of valid entries in instrBuff. The default matches
 * the 2048-byte, 256-word rule-table entries used by the VCK190 design.
 */
template <int STACK_SIZE, int INSTR_DEPTH, typename INSTR_WORD, typename MSG_WORD>
void regexVM_new_core(ap_uint<32>* bitSetBuff,
                      INSTR_WORD* instrBuff,
                      MSG_WORD* msgBuff,
                      unsigned int lenMsg,
                      ap_uint<2>& match,
                      ap_uint<16>* offsetBuff) {
    ap_uint<16> opaddr = 0;
    ap_uint<16> str_ptr = 0;
    ap_uint<16> str_start = 0;
    ap_uint<16> repeat_cnt = 0;

    bool match_s = false;
    bool stk_ovf = false;

    ap_uint<54> stack_buff[STACK_SIZE];
#pragma HLS bind_storage variable = stack_buff type = ram_2p impl = uram
    unsigned int stk = 1;

    // A one-entry write-through cache covers the common push-then-peek RAW
    // hazard while the backing stack remains in UltraRAM.
    bool stk_cache_valid = true;
    unsigned int stk_cache_addr = 0;
    ap_uint<54> stk_cache_data = internal_new::makeStackEntry(0, 0, 0, 0, 0, 0);
    stack_buff[0] = stk_cache_data;

    // Forward the two most recent capture writes around offset RAM latency.
    ap_uint<16> mem_adr_reg_1 = 0;
    ap_uint<16> mem_adr_reg_2 = 0;
    ap_uint<16> mem_dt_reg_1 = 0;
    ap_uint<16> mem_dt_reg_2 = 0;

    // State used only while vector-scanning one OP_ANYCHAR_STAR.
    bool star_active = false;
    unsigned int star_stack_addr = 0;
    ap_uint<16> star_begin = 0;

#ifndef __SYNTHESIS__
    unsigned int exec_iterations = 0;
    unsigned int consumed_bytes = 0;
    unsigned int batched_atom_iterations = 0;
    unsigned int star_bytes = 0;
#endif

LOOP_VM_NEW:
    while (true) {
#pragma HLS pipeline II = 3
#pragma HLS loop_tripcount min = 1000 max = 1000
#pragma HLS dependence variable = offsetBuff inter false
#pragma HLS dependence variable = stack_buff inter false
#ifndef __SYNTHESIS__
        exec_iterations++;
#endif

        // Keep termination local to this iteration. A persistent end flag in
        // the loop condition forms a loop-carried control dependency.
        bool stop_now = false;

        ap_uint<384> instruction_window =
            internal_new::fetchInstructionWindow<INSTR_DEPTH>(instrBuff, opaddr);
        ap_uint<64> instructions[6];
#pragma HLS array_partition variable = instructions complete
        for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
            instructions[lane] = instruction_window.range(64 * lane + 63, 64 * lane);
        }

        ap_uint<64> instruction_1 = instructions[0];
        ap_uint<8> opcode_1 = instruction_1.range(63, 56);
        ap_uint<4> len_1 = instruction_1.range(51, 48);
        ap_uint<16> operand1_1 = instruction_1.range(47, 32);
        ap_uint<16> operand2_1 = instruction_1.range(31, 16);
        ap_uint<16> operand3_1 = instruction_1.range(15, 0);

        ap_uint<8> prev_char = 0x0a;
        ap_uint<7> window_bytes = 0;
        ap_uint<192> char_window = internal_new::fetchMessageWindow(
            msgBuff, str_ptr, lenMsg, false, prev_char, window_bytes);
        if (opcode_1 == OP_BEGIN_LINE && str_ptr > 0) {
            prev_char = internal_new::fetchPreviousByte(msgBuff, str_ptr);
        }
        ap_uint<8> cur_char = char_window.range(7, 0);

        // Build a straight-line batch.  A lane is eligible only while every
        // preceding instruction is a simple consuming atom and the complete
        // atom fits in the 192-bit aligned message window.
        ap_uint<6> atom_lengths[6];
        ap_uint<6> atom_offsets[6];
        bool atom_eligible[6];
#pragma HLS array_partition variable = atom_lengths complete
#pragma HLS array_partition variable = atom_offsets complete
#pragma HLS array_partition variable = atom_eligible complete
        ap_uint<7> planned_bytes = 0;
        bool straight_line = true;
        for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
            ap_uint<8> lane_opcode = instructions[lane].range(63, 56);
            ap_uint<4> lane_len_field = instructions[lane].range(51, 48);
            ap_uint<6> lane_len = internal_new::atomLength(lane_opcode, lane_len_field);
            atom_lengths[lane] = lane_len;
            atom_offsets[lane] = planned_bytes;
            bool fits = planned_bytes + lane_len <= window_bytes;
            atom_eligible[lane] = straight_line && internal_new::isSimpleAtom(lane_opcode) && fits;
            if (atom_eligible[lane])
                planned_bytes += lane_len;
            else
                straight_line = false;

        }

        ap_uint<10> class_addr_1 = opcode_1 == OP_CCLASS
                                             ? (ap_uint<10>)(((unsigned int)cur_char >> 5) + operand1_1)
                                             : (ap_uint<10>)0;
        ap_uint<32> class_word_1 = bitSetBuff[class_addr_1];

        bool need_capture_snapshot = opcode_1 == OP_MEM_START_PUSH;
        ap_uint<16> mem_start = 0;
        ap_uint<16> mem_end = 0;
        if (need_capture_snapshot) {
            if (operand1_1 == mem_adr_reg_1)
                mem_start = mem_dt_reg_1;
            else if (operand1_1 == mem_adr_reg_2)
                mem_start = mem_dt_reg_2;
            else
                mem_start = offsetBuff[operand1_1];

            if (operand1_1 + 1 == mem_adr_reg_1)
                mem_end = mem_dt_reg_1;
            else if (operand1_1 + 1 == mem_adr_reg_2)
                mem_end = mem_dt_reg_2;
            else
                mem_end = offsetBuff[operand1_1 + 1];
        }

        unsigned int top_addr = stk - 1;
        ap_uint<54> top_data = 0;
        if (stk_cache_valid && stk_cache_addr == top_addr)
            top_data = stk_cache_data;
        else
            top_data = stack_buff[top_addr];

        ap_uint<6> top_type = top_data.range(53, 48);
        ap_uint<16> top_mem_id = top_data.range(47, 32);
        ap_uint<16> top_mem_end = top_data.range(31, 16);
        ap_uint<16> top_mem_start = top_data.range(15, 0);
        ap_uint<16> top_pc = top_data.range(31, 16);
        ap_uint<16> top_str = top_data.range(15, 0);
        ap_uint<16> range_begin = top_data.range(47, 32);
        ap_uint<8> retry_mask = top_data.range(7, 0);
        ap_uint<3> retry_bit = 0;
        bool retry_mask_valid = false;
        for (int bit = 0; bit < 8; ++bit) {
#pragma HLS unroll
            if (retry_mask[bit]) {
                retry_bit = bit;
                retry_mask_valid = true;
            }
        }
        ap_uint<16> bitmap_retry_str = range_begin + retry_bit;
        ap_uint<8> remaining_retry_mask = retry_mask & ~((ap_uint<8>)1 << retry_bit);

        bool fail_pop = false;
        bool plain_pop = false;
        bool pop_to_mark = false;
        bool push = false;
        ap_uint<54> push_data = 0;

        if (atom_eligible[0]) {
            bool match_0 = internal_new::atomMatches(instructions[0], char_window, atom_offsets[0], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);
            bool match_1 = internal_new::atomMatches(instructions[1], char_window, atom_offsets[1], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);
            bool match_2 = internal_new::atomMatches(instructions[2], char_window, atom_offsets[2], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);
            bool match_3 = internal_new::atomMatches(instructions[3], char_window, atom_offsets[3], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);
            bool match_4 = internal_new::atomMatches(instructions[4], char_window, atom_offsets[4], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);
            bool match_5 = internal_new::atomMatches(instructions[5], char_window, atom_offsets[5], str_ptr,
                                                     (ap_uint<16>)lenMsg, 0);

            // Prefix-valid bits preserve program order while allowing the
            // byte/instruction totals below to synthesize as balanced adders.
            bool commit_0 = atom_eligible[0] && match_0;
            bool commit_1 = commit_0 && atom_eligible[1] && match_1;
            bool commit_2 = commit_1 && atom_eligible[2] && match_2;
            bool commit_3 = commit_2 && atom_eligible[3] && match_3;
            bool commit_4 = commit_3 && atom_eligible[4] && match_4;
            bool commit_5 = commit_4 && atom_eligible[5] && match_5;

            fail_pop = (atom_eligible[0] && !match_0) ||
                       (commit_0 && atom_eligible[1] && !match_1) ||
                       (commit_1 && atom_eligible[2] && !match_2) ||
                       (commit_2 && atom_eligible[3] && !match_3) ||
                       (commit_3 && atom_eligible[4] && !match_4) ||
                       (commit_4 && atom_eligible[5] && !match_5);

            ap_uint<4> committed_instructions = (ap_uint<1>)commit_0 + (ap_uint<1>)commit_1 +
                                                 (ap_uint<1>)commit_2 + (ap_uint<1>)commit_3 +
                                                 (ap_uint<1>)commit_4 + (ap_uint<1>)commit_5;
            ap_uint<7> committed_bytes = (commit_0 ? atom_lengths[0] : (ap_uint<6>)0) +
                                         (commit_1 ? atom_lengths[1] : (ap_uint<6>)0) +
                                         (commit_2 ? atom_lengths[2] : (ap_uint<6>)0) +
                                         (commit_3 ? atom_lengths[3] : (ap_uint<6>)0) +
                                         (commit_4 ? atom_lengths[4] : (ap_uint<6>)0) +
                                         (commit_5 ? atom_lengths[5] : (ap_uint<6>)0);
            str_ptr += committed_bytes;
            opaddr += committed_instructions;
#ifndef __SYNTHESIS__
            consumed_bytes += committed_bytes;
            if (committed_instructions > 1) batched_atom_iterations++;
#endif
        } else {
            switch (opcode_1) {
                case (OP_END): {
                    match_s = true;
                    stop_now = true;
                    break;
                }
                case (OP_CCLASS): {
                    bool class_match = str_ptr < lenMsg &&
                                       class_word_1[(unsigned int)cur_char & 0x1f];
                    if (class_match) {
                        str_ptr++;
                        opaddr++;
#ifndef __SYNTHESIS__
                        consumed_bytes++;
#endif
                    } else {
                        fail_pop = true;
                    }
                    break;
                }
                case (OP_BEGIN_BUF):
                case (OP_BEGIN_LINE): {
                    if (str_ptr == 0 ||
                        (opcode_1 == OP_BEGIN_LINE && str_ptr != lenMsg && prev_char == 0x0a))
                        opaddr++;
                    else
                        fail_pop = true;
                    break;
                }
                case (OP_END_BUF):
                case (OP_END_LINE): {
                    if (str_ptr == lenMsg ||
                        (opcode_1 == OP_END_LINE && str_ptr < lenMsg && cur_char == 0x0a))
                        opaddr++;
                    else
                        fail_pop = true;
                    break;
                }
                case (OP_MEM_START):
                case (OP_MEM_START_PUSH): {
                    if (opcode_1 == OP_MEM_START_PUSH) {
                        push = true;
                        push_data = internal_new::makeStackEntry(1, operand1_1, mem_start, mem_end, 0, 0);
                    }
                    mem_adr_reg_2 = mem_adr_reg_1;
                    mem_dt_reg_2 = mem_dt_reg_1;
                    mem_adr_reg_1 = operand1_1;
                    mem_dt_reg_1 = str_ptr;
                    offsetBuff[operand1_1] = str_ptr;
                    opaddr++;
                    break;
                }
                case (OP_MEM_END): {
                    mem_adr_reg_2 = mem_adr_reg_1;
                    mem_dt_reg_2 = mem_dt_reg_1;
                    mem_adr_reg_1 = operand1_1;
                    mem_dt_reg_1 = str_ptr;
                    offsetBuff[operand1_1] = str_ptr;
                    opaddr++;
                    break;
                }
                case (OP_JUMP): {
                    opaddr = operand1_1;
                    break;
                }
                case (OP_PUSH): {
                    push = true;
                    push_data = internal_new::makeStackEntry(0, 0, 0, 0, operand1_1, str_ptr);
                    opaddr++;
                    break;
                }
                case (OP_POP): {
                    plain_pop = true;
                    opaddr++;
                    break;
                }
                case (OP_PUSH_OR_JUMP_EXACT1): {
                    if (str_ptr < lenMsg && cur_char == operand2_1.range(15, 8)) {
                        push = true;
                        push_data = internal_new::makeStackEntry(0, 0, 0, 0, operand1_1, str_ptr);
                        opaddr++;
                    } else {
                        opaddr = operand1_1;
                    }
                    break;
                }
                case (OP_REPEAT):
                case (OP_REPEAT_INC): {
                    if (opcode_1 == OP_REPEAT)
                        repeat_cnt = 0;
                    else
                        repeat_cnt++;

                    if ((opcode_1 == OP_REPEAT && operand2_1 == 0) ||
                        (repeat_cnt >= operand2_1 && repeat_cnt < operand3_1)) {
                        push = true;
                        push_data = internal_new::makeStackEntry(0, 0, 0, 0, opaddr + 1, str_ptr);
                        opaddr = operand1_1;
                    } else if (opcode_1 == OP_REPEAT_INC && repeat_cnt < operand2_1) {
                        opaddr = operand1_1;
                    } else {
                        opaddr++;
                    }
                    break;
                }
                case (OP_ANYCHAR_STAR): {
                    bool filter_retries = internal_new::isSimpleAtom(instructions[1].range(63, 56));
                    ap_uint<6> scan_limit = filter_retries ? (ap_uint<6>)8 : (ap_uint<6>)24;
                    ap_uint<6> scan_len = 0;
                    for (int k = 0; k < 24; ++k) {
#pragma HLS unroll
                        ap_uint<8> ch = internal_new::windowByte(char_window, k);
                        if (k >= scan_limit || k >= window_bytes ||
                            (ap_uint<17>)str_ptr + k >= lenMsg || ch == 0x0a)
                            break;
                        scan_len++;
                    }

                    if (scan_len == 0) {
                        star_active = false;
                        opaddr++;
                    } else {
                        if (filter_retries) {
                            // Keep only positions at which the following atom
                            // can match. A failed suffix then skips up to eight
                            // provably useless byte-by-byte retries at once.
                            ap_uint<8> retry_candidates = 0;
                            for (int candidate = 1; candidate <= 8; ++candidate) {
#pragma HLS unroll
                                if (candidate <= scan_len &&
                                    internal_new::atomMatches(instructions[1], char_window, candidate,
                                                              str_ptr, (ap_uint<16>)lenMsg, 0))
                                    retry_candidates[candidate - 1] = 1;
                            }
                            star_active = false;
                            if (retry_candidates != 0) {
                                push = true;
                                push_data = internal_new::makeBitmapEntry(
                                    str_ptr + 1, opaddr + 1, retry_candidates);
                            }
                        } else {
                            // For a control instruction after the star, retain
                            // every logical retry in one continuous range.
                            ap_uint<16> retry_pos = str_ptr + scan_len;
                            if (!star_active) {
                                if (stk >= STACK_SIZE - 1) {
                                    stop_now = true;
                                    stk_ovf = true;
                                } else {
                                    star_active = true;
                                    star_begin = str_ptr + 1;
                                    star_stack_addr = stk;
                                    ap_uint<54> range_data =
                                        internal_new::makeRangeEntry(star_begin, opaddr + 1, retry_pos);
                                    stack_buff[stk] = range_data;
                                    stk_cache_valid = true;
                                    stk_cache_addr = stk;
                                    stk_cache_data = range_data;
                                    stk++;
                                }
                            } else {
                                ap_uint<54> range_data =
                                    internal_new::makeRangeEntry(star_begin, opaddr + 1, retry_pos);
                                stack_buff[star_stack_addr] = range_data;
                                stk_cache_valid = true;
                                stk_cache_addr = star_stack_addr;
                                stk_cache_data = range_data;
                            }
                        }
                        str_ptr += scan_len;
#ifndef __SYNTHESIS__
                        consumed_bytes += scan_len;
                        star_bytes += scan_len;
#endif
                    }
                    break;
                }
                case (OP_MARK): {
                    push = true;
                    push_data = internal_new::makeStackEntry(2, operand1_1, 0, 0, 0, str_ptr);
                    opaddr++;
                    break;
                }
                case (OP_STEP_BACK_START): {
                    ap_int<17> stepped = (ap_int<17>)str_ptr - operand2_1;
                    if (stepped < 0) {
                        fail_pop = true;
                    } else {
                        str_ptr = stepped;
                        if (operand3_1 != 0) {
                            push = true;
                            push_data = internal_new::makeStackEntry(0, 0, 0, 0, opaddr + 1, str_ptr);
                            opaddr = operand1_1;
                        } else {
                            opaddr++;
                        }
                    }
                    break;
                }
                case (OP_FAIL): {
                    fail_pop = true;
                    break;
                }
                case (OP_POP_TO_MARK): {
                    fail_pop = true;
                    pop_to_mark = true;
                    break;
                }
                default: {
                    // Preserve production behavior for unsupported bytecode:
                    // no state is advanced, making the unsupported instruction
                    // visible during C simulation instead of silently matching.
                    break;
                }
            }
        }

        // OP_POP discards one logical entry without applying its saved state.
        // A compressed range is one physical entry but represents many logical
        // alternative entries, so discard just one retry position.
        if (plain_pop) {
            if (top_type == internal_new::kBitmapStackType && retry_mask_valid) {
                if (remaining_retry_mask != 0) {
                    ap_uint<54> shortened =
                        internal_new::makeBitmapEntry(range_begin, top_pc, remaining_retry_mask);
                    stack_buff[top_addr] = shortened;
                    stk_cache_valid = true;
                    stk_cache_addr = top_addr;
                    stk_cache_data = shortened;
                } else if (stk > 0) {
                    stk--;
                }
            } else if (top_type == internal_new::kRangeStackType && top_str > range_begin) {
                ap_uint<54> shortened = internal_new::makeRangeEntry(range_begin, top_pc, top_str - 1);
                stack_buff[top_addr] = shortened;
                stk_cache_valid = true;
                stk_cache_addr = top_addr;
                stk_cache_data = shortened;
            } else if (stk > 0) {
                stk--;
            }
        }

        // Failure and OP_POP_TO_MARK both consume one logical stack entry.
        if (fail_pop) {
            if (stk <= 1) stop_now = true;

            if (top_type == internal_new::kBitmapStackType && retry_mask_valid) {
                if (!pop_to_mark) {
                    opaddr = top_pc;
                    str_ptr = bitmap_retry_str;
                }
                if (remaining_retry_mask != 0) {
                    ap_uint<54> shortened =
                        internal_new::makeBitmapEntry(range_begin, top_pc, remaining_retry_mask);
                    stack_buff[top_addr] = shortened;
                    stk_cache_valid = true;
                    stk_cache_addr = top_addr;
                    stk_cache_data = shortened;
                } else if (stk > 0) {
                    stk--;
                }
            } else if (top_type == internal_new::kRangeStackType) {
                if (!pop_to_mark) {
                    opaddr = top_pc;
                    str_ptr = top_str;
                }
                if (top_str > range_begin) {
                    ap_uint<54> shortened = internal_new::makeRangeEntry(range_begin, top_pc, top_str - 1);
                    stack_buff[top_addr] = shortened;
                    stk_cache_valid = true;
                    stk_cache_addr = top_addr;
                    stk_cache_data = shortened;
                } else if (stk > 0) {
                    stk--;
                }
            } else {
                if (top_type == 0 && !pop_to_mark) {
                    opaddr = top_pc;
                    str_ptr = top_str;
                } else if (top_type == 1) {
                    mem_adr_reg_1 = top_mem_id;
                    mem_dt_reg_1 = top_mem_start;
                    mem_adr_reg_2 = top_mem_id + 1;
                    mem_dt_reg_2 = top_mem_end;
                    offsetBuff[top_mem_id] = top_mem_start;
                    offsetBuff[top_mem_id + 1] = top_mem_end;
                } else if (pop_to_mark && top_type == 2 && top_mem_id == operand1_1) {
                    opaddr++;
                }
                if (stk > 0) stk--;
            }
        } else if (push) {
            if (stk >= STACK_SIZE - 1) {
                stop_now = true;
                stk_ovf = true;
            } else {
                stack_buff[stk] = push_data;
                stk_cache_valid = true;
                stk_cache_addr = stk;
                stk_cache_data = push_data;
                stk++;
            }
        }


        if (stop_now) break;
    }

#ifndef __SYNTHESIS__
#ifdef XF_REGEX_VM_NEW_PROFILE
    printf("[RE PERF NEW] len=%u, iterations=%u, consumed=%u, batched=%u, star_bytes=%u\n", lenMsg,
           exec_iterations, consumed_bytes, batched_atom_iterations, star_bytes);
#endif
#endif

    if (match_s) {
        match = 1;
        offsetBuff[0] = str_start;
        offsetBuff[1] = str_ptr;
    } else {
        match = stk_ovf ? (ap_uint<2>)2 : (ap_uint<2>)0;
    }
}

/**
 * @brief Source-compatible interface using the production 64-bit instruction
 * and 32-bit message memories.
 */
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_new(ap_uint<32>* bitSetBuff,
                 ap_uint<64>* instrBuff,
                 ap_uint<32>* msgBuff,
                 unsigned int lenMsg,
                 ap_uint<2>& match,
                 ap_uint<16>* offsetBuff) {
    regexVM_new_core<STACK_SIZE, INSTR_DEPTH>(bitSetBuff, instrBuff, msgBuff, lenMsg, match, offsetBuff);
}

/**
 * @brief High-throughput interface for the VCK190 datapath.
 *
 * Instructions are packed eight per 512-bit word, with the lowest-numbered
 * instruction in bits [63:0]. Message bytes use the same little-endian byte
 * order, 64 bytes per word. Two reads from each wide memory provide every
 * six-instruction / 24-byte execution window, avoiding the narrow-port limit.
 */
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_new_wide(ap_uint<32>* bitSetBuff,
                      ap_uint<512>* instrBuff,
                      ap_uint<512>* msgBuff,
                      unsigned int lenMsg,
                      ap_uint<2>& match,
                      ap_uint<16>* offsetBuff) {
    regexVM_new_core<STACK_SIZE, INSTR_DEPTH>(bitSetBuff, instrBuff, msgBuff, lenMsg, match, offsetBuff);
}

// Alternate spelling for call sites that want to emphasize that this is the
// experimental successor to regexVM_opt.
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_opt_new(ap_uint<32>* bitSetBuff,
                     ap_uint<64>* instrBuff,
                     ap_uint<32>* msgBuff,
                     unsigned int lenMsg,
                     ap_uint<2>& match,
                     ap_uint<16>* offsetBuff) {
#pragma HLS inline
    regexVM_new<STACK_SIZE, INSTR_DEPTH>(bitSetBuff, instrBuff, msgBuff, lenMsg, match, offsetBuff);
}

} // namespace text
} // namespace data_analytics
} // namespace xf

// Opt-in source-compatible test hook. Defining this before including the
// header redirects later calls spelled regexVM_opt<...>(...) to regexVM_new
// without changing regexVM.hpp or production call sites.
#ifdef XF_REGEX_VM_NEW_REPLACE_OPT
#define regexVM_opt regexVM_new
#endif

#endif // XF_TEXT_REGEX_VM_NEW_H
