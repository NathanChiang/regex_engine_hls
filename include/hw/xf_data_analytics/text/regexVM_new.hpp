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
 * The narrow API is source-compatible with regexVM_opt. The streaming kernel
 * uses private wide message/class memories and configuration-time descriptors.
 * All optimizations recognize bytecode shapes; no expression is hard-coded.
 *
 * The implementation contains these throughput-oriented changes:
 *   - adjacent, side-effect-free consuming instructions may be committed
 *     in one VM iteration (two lanes by default);
 *   - wide memories provide the execution window with two physical reads;
 *   - character-class repetition loops scan 24 bytes per transition using a
 *     256-bit class bitmap, retaining greedy backtracking in range entries;
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

#ifndef XF_REGEX_VM_NEW_WIDE_II
#define XF_REGEX_VM_NEW_WIDE_II 6
#endif
#ifndef XF_REGEX_VM_NEW_ATOM_LANES
#define XF_REGEX_VM_NEW_ATOM_LANES 2
#endif

// Optional deployment policy, not a regex semantic limit. Valid backtracking
// can take arbitrarily many transitions even with a shallow physical stack.
// Zero preserves the original VM's unrestricted execution semantics.
#ifndef XF_REGEX_VM_NEW_MAX_STEPS
#define XF_REGEX_VM_NEW_MAX_STEPS 0
#endif

namespace xf {
namespace data_analytics {
namespace text {
namespace internal_new {

// Vitis 2021.2 can clear untouched lanes when a variable range of a RAM
// element is assigned directly. Keep replacement separate from the RAM store
// so configuration always commits a complete 256-bit class bitmap.
static ap_uint<256> replaceClassChunk(ap_uint<256> previous, ap_uint<2> chunk, ap_uint<64> payload) {
#pragma HLS inline off
#pragma HLS pipeline II=1
    ap_uint<256> result = 0;
    for (int lane = 0; lane < 4; ++lane) {
#pragma HLS unroll
        result.range(64 * lane + 63, 64 * lane) =
            chunk == lane ? payload : (ap_uint<64>)previous.range(64 * lane + 63, 64 * lane);
    }
    return result;
}

template <unsigned N> struct StackAddressBits {
    static const unsigned value = 1 + StackAddressBits<(N + 1) / 2>::value;
};
template <> struct StackAddressBits<2> { static const unsigned value = 1; };
template <> struct StackAddressBits<1> { static const unsigned value = 1; };

static const ap_uint<6> kRangeStackType = 4;
static const ap_uint<6> kBitmapStackType = 5;

static inline bool isSimpleAtom(ap_uint<8> opcode) {
#pragma HLS inline
    // Character classes use a data-dependent bitset address. Keep them on the
    // scalar path so a six-lane batch does not require six bitset RAM ports.
    return (opcode == OP_STR_N) | (opcode == OP_ANYCHAR);
}

static inline ap_uint<3> atomLength(ap_uint<8> opcode, ap_uint<4> encoded_len) {
#pragma HLS inline
    return opcode == OP_STR_N ? (ap_uint<3>)encoded_len : (ap_uint<3>)1;
}

static inline ap_uint<54> makeStackEntry(ap_uint<6> type, ap_uint<16> mem_id, ap_uint<16> mem_start,
                                         ap_uint<16> mem_end, ap_uint<16> opaddr, ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<54> value = 0;
    value.range(53, 48) = type;
    if (type == 0) {
        value.range(31, 16) = opaddr;
        value.range(15, 0) = str_ptr;
    } else if (type == 1) {
        value.range(47, 32) = mem_id;
        value.range(31, 16) = mem_end;
        value.range(15, 0) = mem_start;
    } else {
        value.range(47, 32) = mem_id;
        value.range(31, 16) = type == 2 ? (ap_uint<16>)0 : opaddr;
        value.range(15, 0) = str_ptr;
    }
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
    ap_uint<8> bytes[24];
#pragma HLS array_partition variable = bytes complete
    for (int k = 0; k < 24; ++k) {
#pragma HLS unroll
        bytes[k] = window.range(8 * k + 7, 8 * k);
    }
    return byte_offset < 24 ? bytes[byte_offset] : (ap_uint<8>)0;
}

static inline bool classContains(ap_uint<256> bits, ap_uint<8> ch) {
#pragma HLS inline
    // A variable ap_uint bit reference builds a 256-bit shifted mask in
    // Vitis 2021.2. Select a 32-bit word first, then a bit in that word.
    ap_uint<32> words[8];
#pragma HLS array_partition variable = words complete
    for (int k = 0; k < 8; ++k) {
#pragma HLS unroll
        words[k] = bits.range(32 * k + 31, 32 * k);
    }
    ap_uint<3> word_index = ch.range(7, 5);
    ap_uint<5> bit_index = ch.range(4, 0);
    ap_uint<32> shifted = words[word_index] >> bit_index;
    return shifted[0];
}

static inline bool atomMatches(ap_uint<64> instruction, ap_uint<192> window, ap_uint<6> byte_offset,
                               ap_uint<16> str_ptr, ap_uint<16> len_msg, ap_uint<32> class_word) {
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
static inline ap_uint<512> fetchInstructionWindow(ap_uint<64> *instr_buff, ap_uint<16> opaddr) {
#pragma HLS inline
    ap_uint<512> window = 0;
    // The source-compatible kernel memory has only two read ports.  Restrict
    // this overload to two lanes; requesting six reads makes Vitis 2021.2
    // reuse the same RAM return wires across scheduled stages and corrupt the
    // instruction window in RTL.  The 512-bit overload below retains all six.
    for (int lane = 0; lane < 8; ++lane) {
#pragma HLS unroll
        ap_uint<17> addr = (ap_uint<17>)opaddr + lane;
        ap_uint<64> instruction = lane < 2 && addr < INSTR_DEPTH ? instr_buff[addr] : (ap_uint<64>)OP_END << 56;
        window.range(64 * lane + 63, 64 * lane) = instruction;
    }
    return window;
}

template <int INSTR_DEPTH>
static inline ap_uint<512> fetchInstructionWindow(ap_uint<512> *instr_buff, ap_uint<16> opaddr) {
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
    ap_uint<512> window = 0;
    for (int lane = 0; lane < 8; ++lane) {
#pragma HLS unroll
        window.range(64 * lane + 63, 64 * lane) = words[lane_addr + lane];
    }
    return window;
}

static inline ap_uint<192> fetchMessageWindow(ap_uint<32> *msg_buff, ap_uint<16> str_ptr, unsigned int len_msg,
                                              bool need_previous, ap_uint<8> &prev_char, ap_uint<7> &window_bytes) {
#pragma HLS inline
    ap_uint<2> byte_pos = str_ptr.range(1, 0);
    ap_uint<16> word_addr = str_ptr >> 2;
    ap_uint<16> base_addr = need_previous ? (ap_uint<16>)(word_addr - 1) : word_addr;
    ap_uint<17> word_count = ((ap_uint<17>)len_msg + 3) >> 2;
    ap_uint<192> raw = 0;
    // As above, keep the narrow message-memory implementation within its two
    // physical read ports.  Eight raw bytes provide up to two literal atoms
    // or a short vectorized star scan without time-multiplexed return hazards.
    for (int lane = 0; lane < 6; ++lane) {
#pragma HLS unroll
        ap_uint<17> addr = (ap_uint<17>)base_addr + lane;
        ap_uint<32> word = lane < 2 && addr < word_count ? msg_buff[addr] : (ap_uint<32>)0;
        raw.range(32 * lane + 31, 32 * lane) = word;
    }
    ap_uint<3> byte_shift = need_previous ? (ap_uint<3>)4 : (ap_uint<3>)byte_pos;
    prev_char = 0x0a;
    if (str_ptr > 0 && byte_shift != 0) {
        ap_uint<192> prev_aligned = raw >> ((byte_shift - 1) * 8);
        prev_char = prev_aligned.range(7, 0);
    }
    window_bytes = 8 - byte_shift;
    return raw >> (byte_shift * 8);
}

static inline ap_uint<192> fetchMessageWindow(ap_uint<512> *msg_buff, ap_uint<16> str_ptr, unsigned int len_msg,
                                              bool /*need_previous*/, ap_uint<8> &prev_char, ap_uint<7> &window_bytes) {
#pragma HLS inline
    ap_uint<6> byte_pos = str_ptr.range(5, 0);
    ap_uint<10> word_addr = str_ptr >> 6;
    // At a 64-byte boundary the current word alone supplies the entire
    // 24-byte window. Spend the other port on the preceding word so anchors
    // never need a third message read. Address selection depends only on ptr.
    bool previous_word = str_ptr > 0 && byte_pos == 0;
    ap_uint<10> base_addr = previous_word ? (ap_uint<10>)(word_addr - 1) : word_addr;
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
    ap_uint<4> chunk_addr = previous_word ? (ap_uint<4>)8 : (ap_uint<4>)(byte_pos >> 3);
    ap_uint<3> byte_in_chunk = byte_pos.range(2, 0);
    ap_uint<192> window = 0;
    for (int lane = 0; lane < 3; ++lane) {
#pragma HLS unroll
        ap_uint<128> pair = ((ap_uint<128>)chunks[chunk_addr + lane + 1] << 64) | chunks[chunk_addr + lane];
        ap_uint<128> aligned = pair >> (byte_in_chunk * 8);
        window.range(64 * lane + 63, 64 * lane) = aligned.range(63, 0);
    }
    prev_char = 0x0a;
    if (previous_word) {
        prev_char = first.range(511, 504);
    } else if (str_ptr > 0) {
        ap_uint<6> previous_byte = byte_pos - 1;
        ap_uint<3> previous_chunk = previous_byte.range(5, 3);
        ap_uint<6> shift = (ap_uint<6>)previous_byte.range(2, 0) << 3;
        ap_uint<64> shifted = chunks[previous_chunk] >> shift;
        prev_char = shifted.range(7, 0);
    }
    window_bytes = 24;
    return window;
}

static inline bool isMessageWordBoundary(ap_uint<32> *, ap_uint<16> str_ptr) {
#pragma HLS inline
    return str_ptr.range(1, 0) == 0;
}

static inline bool isMessageWordBoundary(ap_uint<512> *, ap_uint<16> str_ptr) {
#pragma HLS inline
    return str_ptr.range(5, 0) == 0;
}

static inline bool windowIncludesPrevious(ap_uint<32> *) {
#pragma HLS inline
    return false;
}
static inline bool windowIncludesPrevious(ap_uint<512> *) {
#pragma HLS inline
    return true;
}

static inline ap_uint<8> fetchPreviousBoundaryByte(ap_uint<32> *msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    return msg_buff[(str_ptr >> 2) - 1].range(31, 24);
}

static inline ap_uint<8> fetchPreviousBoundaryByte(ap_uint<512> *msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    return msg_buff[(str_ptr >> 6) - 1].range(511, 504);
}

static inline ap_uint<8> fetchPreviousByte(ap_uint<32> *msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<16> word_addr = str_ptr >> 2;
    ap_uint<2> byte_pos = str_ptr.range(1, 0);
    if (byte_pos == 0)
        return msg_buff[word_addr - 1].range(31, 24);
    ap_uint<32> shifted = msg_buff[word_addr] >> ((byte_pos - 1) * 8);
    return shifted.range(7, 0);
}

static inline ap_uint<8> fetchPreviousByte(ap_uint<512> *msg_buff, ap_uint<16> str_ptr) {
#pragma HLS inline
    ap_uint<10> word_addr = str_ptr >> 6;
    ap_uint<6> byte_pos = str_ptr.range(5, 0);
    if (byte_pos == 0)
        return msg_buff[word_addr - 1].range(511, 504);
    ap_uint<512> shifted = msg_buff[word_addr] >> ((byte_pos - 1) * 8);
    return shifted.range(7, 0);
}

// A class occupies eight consecutive 32-bit words in the compiler image.
// Packing those words, rather than replicating a RAM for every scanned byte,
// makes all 256 membership bits available with one physical memory read.
template <typename WORD> struct ClassMemory;

template <> struct ClassMemory<ap_uint<32>> {
    static const bool parallel = false;
    static ap_uint<256> bitmap(ap_uint<32> *, ap_uint<16>) {
#pragma HLS inline
        return 0;
    }
    static bool contains(ap_uint<32> *memory, ap_uint<16> base, ap_uint<8> ch, ap_uint<256>) {
#pragma HLS inline
        ap_uint<32> word = memory[base + (ch >> 5)];
        return word[ch.range(4, 0)];
    }
};

template <> struct ClassMemory<ap_uint<256>> {
    static const bool parallel = true;
    static ap_uint<256> bitmap(ap_uint<256> *memory, ap_uint<16> base) {
#pragma HLS inline
        return memory[base >> 3];
    }
    static bool contains(ap_uint<256> *, ap_uint<16>, ap_uint<8> ch, ap_uint<256> bits) {
#pragma HLS inline
        return classContains(bits, ch);
    }
};

// A configuration-time descriptor keeps runtime decoding off the VM feedback
// path. Bits 0..127 retain the current and next raw instructions. The remaining
// fields describe recognized loops and a conservative lazy-suffix prefix.
static inline ap_uint<512> decodeInstructionWindow(ap_uint<512> window, ap_uint<16> opaddr) {
#pragma HLS inline
    ap_uint<64> instructions[8];
#pragma HLS array_partition variable = instructions complete
    for (int lane = 0; lane < 8; ++lane) {
#pragma HLS unroll
        instructions[lane] = window.range(64 * lane + 63, 64 * lane);
    }
    ap_uint<64> instruction_1 = instructions[0];
    ap_uint<8> opcode_1 = instruction_1.range(63, 56);
    ap_uint<4> len_1 = instruction_1.range(51, 48);
    bool literal_capture =
        opcode_1 == OP_STR_N && len_1 > 0 && len_1 <= 4 && instructions[1].range(63, 56) == OP_MEM_START;
    bool prefix_space = opcode_1 == OP_STR_N && len_1 == 1 && instructions[1].range(63, 56) == OP_PUSH_OR_JUMP_EXACT1;
    bool start_field = opcode_1 == OP_MEM_START || literal_capture;
    ap_uint<2> capture_slot = literal_capture ? (ap_uint<2>)1 : (ap_uint<2>)0;
    ap_uint<2> head_slot =
        literal_capture ? (ap_uint<2>)2 : start_field || prefix_space ? (ap_uint<2>)1 : (ap_uint<2>)0;
    bool plus_class = start_field && instructions[head_slot].range(63, 56) == OP_CCLASS;
    ap_uint<64> first_class = instructions[head_slot];
    if (plus_class)
        head_slot++;
    ap_uint<16> capture_address = instructions[capture_slot].range(47, 32);
    ap_uint<6> literal_bytes = literal_capture || prefix_space ? (ap_uint<6>)len_1 : (ap_uint<6>)0;
    ap_uint<6> mandatory_bytes = literal_bytes + (plus_class ? 1 : 0);
    ap_uint<16> loop_head = opaddr + head_slot;
    ap_uint<64> loop_push = instructions[head_slot];
    ap_uint<64> loop_body = instructions[head_slot + 1];
    ap_uint<64> loop_tail_1 = instructions[head_slot + 2];
    ap_uint<64> loop_tail_2 = instructions[head_slot + 3];
    ap_uint<16> loop_exit = loop_push.range(47, 32);
    ap_uint<8> loop_opcode = loop_body.range(63, 56);
    bool loop_atom = loop_opcode == OP_CCLASS || loop_opcode == OP_ANYCHAR ||
                     (loop_opcode == OP_STR_N && loop_body.range(51, 48) == 1);
    bool head_push = loop_push.range(63, 56) == OP_PUSH ||
                     (loop_push.range(63, 56) == OP_PUSH_OR_JUMP_EXACT1 && loop_opcode == OP_STR_N &&
                      loop_body.range(51, 48) == 1 && loop_push.range(31, 24) == loop_body.range(47, 40));
    bool possessive_loop = loop_tail_1.range(63, 56) == OP_POP && loop_tail_2.range(63, 56) == OP_JUMP &&
                           loop_tail_2.range(47, 32) == loop_head && loop_exit == loop_head + 4;
    bool greedy_loop =
        loop_tail_1.range(63, 56) == OP_JUMP && loop_tail_1.range(47, 32) == loop_head && loop_exit == loop_head + 3;

    bool valid = head_push && loop_atom && (possessive_loop || greedy_loop) &&
                 (!prefix_space || instruction_1.range(47, 40) == loop_body.range(47, 40));
    ap_uint<64> follow = instructions[head_slot + (possessive_loop ? 4 : 3)];
    ap_uint<16> end_address = follow.range(47, 32);
    bool end_field = follow.range(63, 56) == OP_MEM_END && (!start_field || end_address != capture_address);
    ap_uint<512> result = 0;
    result.range(127, 0) = window.range(127, 0);
    if (valid) {
        result[128] = 1;
        result[129] = start_field;
        result[130] = plus_class;
        result[131] = possessive_loop;
        result[132] = end_field;
        result.range(136, 133) = literal_bytes;
        result.range(140, 137) = mandatory_bytes;
        result.range(159, 144) = loop_head;
        result.range(175, 160) = loop_exit;
        result.range(191, 176) = capture_address;
        result.range(207, 192) = end_address;
        result.range(223, 208) = loop_body.range(47, 32);
        result.range(239, 224) = first_class.range(47, 32);
        result.range(247, 240) = loop_body.range(47, 40);
        result.range(255, 248) = loop_opcode;
    }
    bool prefix_end = instructions[1].range(63, 56) == OP_MEM_END;
    ap_uint<64> branch_or_atom = prefix_end ? instructions[2] : instructions[1];
    bool has_second = branch_or_atom.range(63, 56) == OP_PUSH;
    ap_uint<64> atom = has_second ? (prefix_end ? instructions[3] : instructions[2]) : branch_or_atom;
    ap_uint<4> atom_length = atom.range(51, 48);
    result[256] = atom.range(63, 56) == OP_STR_N && atom_length > 0 && atom_length <= 4;
    result[257] = has_second;
    result[258] = opcode_1 == OP_PUSH && (ap_uint<17>)instruction_1.range(47, 32) + 1 == opaddr;
    result.range(271, 264) = atom.range(47, 40);
    result.range(287, 272) = branch_or_atom.range(47, 32);
    return result;
}

// Isolate the configuration-only decoder from the executing VM's controller.
static ap_uint<512> decodeConfiguredWindow(ap_uint<512> window, ap_uint<16> pc) {
#pragma HLS inline off
#pragma HLS pipeline II = 1
    return decodeInstructionWindow(window, pc);
}

template <int INSTR_DEPTH, typename WORD>
static inline ap_uint<512> fetchDecodedInstruction(WORD *memory, ap_uint<16> pc) {
#pragma HLS inline
    ap_uint<512> empty = ((ap_uint<512>)OP_END << 56) | ((ap_uint<512>)OP_END << 120);
    return pc < INSTR_DEPTH ? (ap_uint<512>)memory[pc] : empty;
}

// Compile only after the complete rule has arrived. The raw memory needs one
// read per clock, independent of the runtime descriptor width. In particular,
// a lazy loop's body and remote alternative can be resolved here instead of
// spending message cycles building and warming a runtime prefix cache.
template <int INSTR_DEPTH> static void prepareRule(ap_uint<64> *raw, ap_uint<512> *descriptors, ap_uint<16> count) {
#pragma HLS inline off
    for (unsigned pc = 0; pc < count && pc < INSTR_DEPTH; ++pc) {
        ap_uint<512> window = 0;
        for (unsigned lane = 0; lane < 8; ++lane) {
#pragma HLS pipeline II = 1
            ap_uint<64> instruction =
                pc + lane < count && pc + lane < INSTR_DEPTH ? raw[pc + lane] : (ap_uint<64>)OP_END << 56;
            window = (window >> 64) | ((ap_uint<512>)instruction << 448);
        }
        ap_uint<512> descriptor = decodeConfiguredWindow(window, pc);
        ap_uint<64> previous = pc > 0 ? raw[pc - 1] : (ap_uint<64>)0;
        ap_uint<16> alt_pc = descriptor.range(287, 272);
        ap_uint<64> alternative = alt_pc < count && alt_pc < INSTR_DEPTH ? raw[alt_pc] : (ap_uint<64>)0;
        bool second_valid =
            alternative.range(63, 56) == OP_STR_N && alternative.range(51, 48) > 0 && alternative.range(51, 48) <= 4;
        descriptor[259] = descriptor[258] && descriptor[256] && previous.range(63, 56) == OP_CCLASS &&
                          (!descriptor[257] || second_valid);
        descriptor.range(303, 288) = previous.range(47, 32);
        descriptor.range(311, 304) = alternative.range(47, 40);
        descriptors[pc] = descriptor;
    }
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
template <int STACK_SIZE, int INSTR_DEPTH, int LOOP_II = 8, bool PREDECODED = false, typename CLASS_WORD,
          typename INSTR_WORD, typename MSG_WORD>
void regexVM_new_core(CLASS_WORD *bitSetBuff, INSTR_WORD *instrBuff, MSG_WORD *msgBuff, unsigned int lenMsg,
                      ap_uint<2> &match, ap_uint<16> *offsetBuff) {
    static_assert(STACK_SIZE >= 2, "The VM needs a sentinel and at least one stack entry");
    static_assert(XF_REGEX_VM_NEW_ATOM_LANES >= 1 && XF_REGEX_VM_NEW_ATOM_LANES <= 6,
                  "The packed-wide VM supports one through six atom lanes");
    ap_uint<16> opaddr = 0;
    ap_uint<16> str_ptr = 0;
    ap_uint<16> str_start = 0;
    ap_uint<16> repeat_cnt = 0;

    // One loop-carried value encodes both execution and completion. Keeping
    // termination in this same value avoids losing a separate boolean along
    // a deeply predicated Vitis 2021.2 pipeline path.
    ap_uint<2> final_status = 3; // 3=running, 0=no match, 1=match, 2=error

#if XF_REGEX_VM_NEW_MAX_STEPS > 0
    unsigned int exec_watchdog = 0;
#endif

    ap_uint<54> stack_buff[STACK_SIZE];
#pragma HLS bind_storage variable = stack_buff type = ram_2p impl = uram
    typedef ap_uint<internal_new::StackAddressBits<STACK_SIZE>::value> StackAddress;
    StackAddress stk = 1;

    // A one-entry write-through cache covers the common push-then-peek RAW
    // hazard while the backing stack remains in UltraRAM.
    bool stk_cache_valid = true;
    StackAddress stk_cache_addr = 0;
    ap_uint<54> stk_cache_data = internal_new::makeStackEntry(0, 0, 0, 0, 0, 0);
    stack_buff[0] = stk_cache_data;

    // Forward the two most recent capture writes around offset RAM latency.
    ap_uint<16> mem_adr_reg_1 = 0;
    ap_uint<16> mem_adr_reg_2 = 0;
    ap_uint<16> mem_dt_reg_1 = 0;
    ap_uint<16> mem_dt_reg_2 = 0;

    // State used only while vector-scanning one OP_ANYCHAR_STAR.
    bool star_active = false;
    StackAddress star_stack_addr = 0;
    ap_uint<16> star_begin = 0;

    // A lazy CCLASS/PUSH-backedge loop can skip only positions at which its
    // suffix's first literal cannot start. Support an optional leading
    // MEM_END and one PUSH with two literal arms. Other shapes retain the
    // ordinary interpreter, including nullable or capture-changing prefixes.
    bool last_class_valid = false;
    ap_uint<16> last_class_pc = 0;
    ap_uint<256> last_class_bitmap = 0;
    bool first_building = false;
    bool first_valid = false;
    bool first_supported = false;
    bool first_has_second = false;
    ap_uint<16> first_head = 0;
    ap_uint<16> first_alt_pc = 0;
    ap_uint<8> first_byte_a = 0;
    ap_uint<8> first_byte_b = 0;

#ifndef __SYNTHESIS__
    unsigned int exec_iterations = 0;
    unsigned int consumed_bytes = 0;
    unsigned int batched_atom_iterations = 0;
    unsigned int star_bytes = 0;
#endif

LOOP_VM_NEW:
    while (final_status == 3) {
        // All loop-carried state and RAM dependencies remain visible to HLS.
        // The requested II is a target; validate the achieved schedule in RTL.
#ifndef XF_REGEX_VM_NEW_NO_PIPELINE
#pragma HLS pipeline II = LOOP_II
#endif
#pragma HLS loop_tripcount min = 1000 max = 1000
#ifndef __SYNTHESIS__
        exec_iterations++;
#endif

        ap_uint<2> next_status = final_status;
#if XF_REGEX_VM_NEW_MAX_STEPS > 0
        bool step_limit_reached = exec_watchdog >= XF_REGEX_VM_NEW_MAX_STEPS;
        exec_watchdog++;
#endif

        ap_uint<16> fetch_pc = !PREDECODED && first_building ? first_alt_pc : opaddr;
        ap_uint<512> instruction_window;
        if (PREDECODED)
            instruction_window = internal_new::fetchDecodedInstruction<INSTR_DEPTH>(instrBuff, fetch_pc);
        else
            instruction_window = internal_new::fetchInstructionWindow<INSTR_DEPTH>(instrBuff, fetch_pc);
        ap_uint<512> descriptor =
            PREDECODED ? instruction_window : internal_new::decodeInstructionWindow(instruction_window, fetch_pc);
        ap_uint<64> instructions[8];
#pragma HLS array_partition variable = instructions complete
        for (int lane = 0; lane < 8; ++lane) {
#pragma HLS unroll
            instructions[lane] = PREDECODED && lane >= 2
                                     ? (ap_uint<64>)OP_END << 56
                                     : (ap_uint<64>)instruction_window.range(64 * lane + 63, 64 * lane);
        }

        ap_uint<64> instruction_1 = instructions[0];
        ap_uint<8> opcode_1 = instruction_1.range(63, 56);
        ap_uint<4> len_1 = instruction_1.range(51, 48);
        ap_uint<16> operand1_1 = instruction_1.range(47, 32);
        ap_uint<16> operand2_1 = instruction_1.range(31, 16);
        ap_uint<16> operand3_1 = instruction_1.range(15, 0);

        ap_uint<8> prev_char = 0x0a;
        ap_uint<7> window_bytes = 0;
        ap_uint<192> char_window =
            internal_new::fetchMessageWindow(msgBuff, str_ptr, lenMsg, false, prev_char, window_bytes);
        if (opcode_1 == OP_BEGIN_LINE && str_ptr > 0 && !internal_new::windowIncludesPrevious(msgBuff) &&
            internal_new::isMessageWordBoundary(msgBuff, str_ptr)) {
            prev_char = internal_new::fetchPreviousBoundaryByte(msgBuff, str_ptr);
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
            atom_eligible[lane] =
                lane < XF_REGEX_VM_NEW_ATOM_LANES && straight_line && internal_new::isSimpleAtom(lane_opcode) && fits;
            if (atom_eligible[lane])
                planned_bytes += lane_len;
            else
                straight_line = false;
        }

        // Recognize the compiler's greedy and possessive single-byte loops:
        // PUSH exit; atom; [POP]; JUMP head. There are no capture writes or
        // other observable side effects inside these loops. The exit target
        // must be exact; arbitrary branches continue through the scalar VM.
        // A plain capture start immediately before the loop can commit with
        // its first scan. A capture end at the exact exit can commit with the
        // last scan. MEM_START_PUSH remains scalar because it saves a second
        // independent stack entry, which would exceed the stack write port.
        bool start_field = descriptor[129];
        bool plus_class = descriptor[130];
        bool possessive_loop = descriptor[131];
        bool end_field = descriptor[132];
        ap_uint<6> literal_bytes = descriptor.range(136, 133);
        ap_uint<6> mandatory_bytes = descriptor.range(140, 137);
        ap_uint<16> loop_head = descriptor.range(159, 144);
        ap_uint<16> loop_exit = descriptor.range(175, 160);
        ap_uint<16> capture_address = descriptor.range(191, 176);
        ap_uint<16> end_address = descriptor.range(207, 192);
        ap_uint<16> loop_class_base = descriptor.range(223, 208);
        ap_uint<16> first_class_base = descriptor.range(239, 224);
        ap_uint<8> loop_literal = descriptor.range(247, 240);
        ap_uint<8> loop_opcode = descriptor.range(255, 248);
        bool prefix_ok = literal_bytes == 0 ||
                         internal_new::atomMatches(instruction_1, char_window, 0, str_ptr, (ap_uint<16>)lenMsg, 0);
        ap_uint<256> first_class_bits =
            internal_new::ClassMemory<CLASS_WORD>::bitmap(bitSetBuff, plus_class ? first_class_base : (ap_uint<16>)0);
        if (plus_class)
            prefix_ok =
                prefix_ok && (ap_uint<17>)str_ptr + literal_bytes < lenMsg &&
                internal_new::classContains(first_class_bits, internal_new::windowByte(char_window, literal_bytes));
        bool scan_loop =
            !first_building && internal_new::ClassMemory<CLASS_WORD>::parallel && descriptor[128] && prefix_ok;
        // Prefix validation must not serialize the two class RAM reads.
        bool fetch_loop_class = !first_building && internal_new::ClassMemory<CLASS_WORD>::parallel && descriptor[128];
        bool fetch_lazy_class = PREDECODED && descriptor[259];
        ap_uint<16> class_base = fetch_lazy_class ? (ap_uint<16>)descriptor.range(303, 288)
                                                  : fetch_loop_class ? loop_class_base : operand1_1;
        ap_uint<256> class_bitmap = internal_new::ClassMemory<CLASS_WORD>::bitmap(
            bitSetBuff, (fetch_lazy_class || opcode_1 == OP_CCLASS || (fetch_loop_class && loop_opcode == OP_CCLASS))
                            ? class_base
                            : (ap_uint<16>)0);
        bool class_match_1 = false;
        if (opcode_1 == OP_CCLASS)
            class_match_1 =
                internal_new::ClassMemory<CLASS_WORD>::contains(bitSetBuff, operand1_1, cur_char, class_bitmap);

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

        StackAddress top_addr = stk - 1;
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
        bool capture_write_0 = false, capture_write_1 = false;
        ap_uint<16> capture_addr_0 = 0, capture_addr_1 = 0;
        ap_uint<16> capture_data_0 = 0, capture_data_1 = 0;
        bool batch_head = !first_building && internal_new::isSimpleAtom(opcode_1);
        // Every scalar predicate reads the state at entry. In particular, a
        // disabled star comparator must not depend on a preceding batch's new
        // pointer; that creates a long, physically unnecessary feedback path.
        ap_uint<16> scalar_str_ptr = str_ptr;
        ap_uint<16> scalar_opaddr = opaddr;

        if (scan_loop) {
            if (start_field) {
                capture_write_0 = true;
                capture_addr_0 = capture_address;
                capture_data_0 = str_ptr + literal_bytes;
            }
            ap_uint<32> accepted = 0;
            for (int k = 0; k < 24; ++k) {
#pragma HLS unroll
                ap_uint<8> ch = char_window.range(8 * k + 7, 8 * k);
                bool accepts = loop_opcode == OP_CCLASS ? internal_new::classContains(class_bitmap, ch)
                                                        : loop_opcode == OP_ANYCHAR ? ch != 0x0a : ch == loop_literal;
                if (k < literal_bytes || (plus_class && k == literal_bytes))
                    accepts = true;
                accepted[31 - k] = k < window_bytes && (ap_uint<17>)str_ptr + k < lenMsg && accepts;
            }
            // A priority encoder avoids a chain of 24 conditional increments
            // on the message-pointer feedback path. The low eight zero bits
            // guarantee that the complement always has a set bit.
            ap_uint<32> rejected = ~accepted;
            ap_uint<6> scan_len = rejected.countLeadingZeros();
            if (scan_len > mandatory_bytes && !possessive_loop) {
                // One range represents precisely the alternatives left by
                // successful PUSH/atom/JUMP iterations, including the start
                // position. The failing iteration's PUSH is already popped.
                push = true;
                push_data = internal_new::makeRangeEntry(str_ptr + mandatory_bytes, loop_exit, str_ptr + scan_len - 1);
            }
            str_ptr += scan_len;
            bool complete = scan_len < window_bytes || str_ptr == lenMsg;
            opaddr = complete ? loop_exit : loop_head;
            if (complete && end_field) {
                capture_write_1 = true;
                capture_addr_1 = end_address;
                capture_data_1 = str_ptr;
                opaddr = loop_exit + 1;
            }
#ifndef __SYNTHESIS__
            consumed_bytes += scan_len;
#endif
        } else if (batch_head) {
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

            fail_pop = (atom_eligible[0] && !match_0) || (commit_0 && atom_eligible[1] && !match_1) ||
                       (commit_1 && atom_eligible[2] && !match_2) || (commit_2 && atom_eligible[3] && !match_3) ||
                       (commit_3 && atom_eligible[4] && !match_4) || (commit_4 && atom_eligible[5] && !match_5);

            ap_uint<4> committed_instructions = (ap_uint<1>)commit_0 + (ap_uint<1>)commit_1 + (ap_uint<1>)commit_2 +
                                                (ap_uint<1>)commit_3 + (ap_uint<1>)commit_4 + (ap_uint<1>)commit_5;
            ap_uint<7> committed_bytes =
                (commit_0 ? atom_lengths[0] : (ap_uint<6>)0) + (commit_1 ? atom_lengths[1] : (ap_uint<6>)0) +
                (commit_2 ? atom_lengths[2] : (ap_uint<6>)0) + (commit_3 ? atom_lengths[3] : (ap_uint<6>)0) +
                (commit_4 ? atom_lengths[4] : (ap_uint<6>)0) + (commit_5 ? atom_lengths[5] : (ap_uint<6>)0);
            str_ptr += committed_bytes;
            opaddr += committed_instructions;
#ifndef __SYNTHESIS__
            consumed_bytes += committed_bytes;
            if (committed_instructions > 1)
                batched_atom_iterations++;
#endif
        }
        // Keep one scalar switch, with a byte-sized private no-op for a
        // transition already committed by the vector front end.
        ap_uint<8> scalar_opcode =
            first_building ? (ap_uint<8>)254 : scan_loop || batch_head ? (ap_uint<8>)255 : opcode_1;
        switch ((unsigned int)scalar_opcode) {
            case 254: {
                if (!first_building) {
                    next_status = 2;
                } else {
                    first_supported = opcode_1 == OP_STR_N && len_1 > 0 && len_1 <= 4;
                    first_byte_b = operand1_1.range(15, 8);
                    first_building = false;
                    first_valid = true;
                }
                break;
            }
            case 255:
                if (!scan_loop && !batch_head)
                    next_status = 2;
                break;
            case OP_END: {
                next_status = 1;
                break;
            }
            case OP_CCLASS: {
                if (internal_new::ClassMemory<CLASS_WORD>::parallel) {
                    if (!PREDECODED) {
                        last_class_valid = true;
                        last_class_pc = scalar_opaddr;
                        last_class_bitmap = class_bitmap;
                    }
                }
                bool class_match = scalar_str_ptr < lenMsg && class_match_1;
                if (class_match) {
                    str_ptr = scalar_str_ptr + 1;
                    opaddr = scalar_opaddr + 1;
#ifndef __SYNTHESIS__
                    consumed_bytes++;
#endif
                } else {
                    fail_pop = true;
                }
                break;
            }
            case OP_BEGIN_BUF:
            case OP_BEGIN_LINE: {
                if (scalar_str_ptr == 0 || (opcode_1 == OP_BEGIN_LINE && scalar_str_ptr != lenMsg && prev_char == 0x0a))
                    opaddr = scalar_opaddr + 1;
                else
                    fail_pop = true;
                break;
            }
            case OP_END_BUF:
            case OP_END_LINE: {
                if (scalar_str_ptr == lenMsg ||
                    (opcode_1 == OP_END_LINE && scalar_str_ptr < lenMsg && cur_char == 0x0a))
                    opaddr = scalar_opaddr + 1;
                else
                    fail_pop = true;
                break;
            }
            case OP_MEM_START:
            case OP_MEM_START_PUSH: {
                if (opcode_1 == OP_MEM_START_PUSH) {
                    push = true;
                    push_data = internal_new::makeStackEntry(1, operand1_1, mem_start, mem_end, 0, 0);
                }
                capture_write_0 = true;
                capture_addr_0 = operand1_1;
                capture_data_0 = scalar_str_ptr;
                opaddr = scalar_opaddr + 1;
                break;
            }
            case OP_MEM_END: {
                capture_write_0 = true;
                capture_addr_0 = operand1_1;
                capture_data_0 = scalar_str_ptr;
                opaddr = scalar_opaddr + 1;
                break;
            }
            case OP_JUMP: {
                opaddr = operand1_1;
                break;
            }
            case OP_PUSH: {
                bool lazy_loop = PREDECODED ? (bool)descriptor[259]
                                            : internal_new::ClassMemory<CLASS_WORD>::parallel && last_class_valid &&
                                                  operand1_1 == last_class_pc && descriptor[258];
                bool first_known = PREDECODED || (first_valid && first_head == scalar_opaddr);
                bool lazy_ready = lazy_loop && first_known && (PREDECODED || first_supported);
                ap_uint<256> lazy_bitmap = PREDECODED ? class_bitmap : last_class_bitmap;
                ap_uint<8> byte_a = PREDECODED ? (ap_uint<8>)descriptor.range(271, 264) : first_byte_a;
                ap_uint<8> byte_b = PREDECODED ? (ap_uint<8>)descriptor.range(311, 304) : first_byte_b;
                bool has_second = PREDECODED ? (bool)descriptor[257] : first_has_second;
                if (!PREDECODED && lazy_loop && !first_known && scalar_str_ptr < lenMsg &&
                    internal_new::classContains(last_class_bitmap, cur_char)) {
                    first_head = scalar_opaddr;
                    first_valid = true;
                    first_supported = false;
                    first_supported = descriptor[256];
                    first_has_second = descriptor[257];
                    first_byte_a = descriptor.range(271, 264);
                    if (first_supported && first_has_second) {
                        first_alt_pc = descriptor.range(287, 272);
                        first_building = true;
                        first_valid = false;
                    }
                } else {
                    ap_uint<32> accepted = 0;
                    for (int k = 0; k < 24; ++k) {
#pragma HLS unroll
                        ap_uint<8> ch = char_window.range(8 * k + 7, 8 * k);
                        accepted[31 - k] = lazy_ready && k < window_bytes && (ap_uint<17>)scalar_str_ptr + k < lenMsg &&
                                           internal_new::classContains(lazy_bitmap, ch) && ch != byte_a &&
                                           (!has_second || ch != byte_b);
                    }
                    ap_uint<32> rejected = ~accepted;
                    ap_uint<6> skip = rejected.countLeadingZeros();
                    if (skip != 0) {
                        str_ptr = scalar_str_ptr + skip;
#ifndef __SYNTHESIS__
                        consumed_bytes += skip;
#endif
                    } else {
                        push = true;
                        push_data = internal_new::makeStackEntry(0, 0, 0, 0, operand1_1, scalar_str_ptr);
                        opaddr = scalar_opaddr + 1;
                    }
                }
                break;
            }
            case OP_POP: {
                plain_pop = true;
                opaddr = scalar_opaddr + 1;
                break;
            }
            case OP_PUSH_OR_JUMP_EXACT1: {
                if (scalar_str_ptr < lenMsg && cur_char == operand2_1.range(15, 8)) {
                    push = true;
                    push_data = internal_new::makeStackEntry(0, 0, 0, 0, operand1_1, scalar_str_ptr);
                    opaddr = scalar_opaddr + 1;
                } else {
                    opaddr = operand1_1;
                }
                break;
            }
            case OP_REPEAT:
            case OP_REPEAT_INC: {
                if (opcode_1 == OP_REPEAT)
                    repeat_cnt = 0;
                else
                    repeat_cnt++;

                if ((opcode_1 == OP_REPEAT && operand2_1 == 0) ||
                    (repeat_cnt >= operand2_1 && repeat_cnt < operand3_1)) {
                    push = true;
                    push_data = internal_new::makeStackEntry(0, 0, 0, 0, scalar_opaddr + 1, scalar_str_ptr);
                    opaddr = operand1_1;
                } else if (opcode_1 == OP_REPEAT_INC && repeat_cnt < operand2_1) {
                    opaddr = operand1_1;
                } else {
                    opaddr = scalar_opaddr + 1;
                }
                break;
            }
            case OP_ANYCHAR_STAR: {
                bool filter_retries = internal_new::isSimpleAtom(instructions[1].range(63, 56));
                ap_uint<6> scan_limit = filter_retries ? (ap_uint<6>)8 : (ap_uint<6>)24;
                ap_uint<32> accepted = 0;
                for (int k = 0; k < 24; ++k) {
#pragma HLS unroll
                    ap_uint<8> ch = internal_new::windowByte(char_window, k);
                    accepted[31 - k] =
                        k < scan_limit && k < window_bytes && (ap_uint<17>)scalar_str_ptr + k < lenMsg && ch != 0x0a;
                }
                ap_uint<32> rejected = ~accepted;
                ap_uint<6> scan_len = rejected.countLeadingZeros();

                if (scan_len == 0) {
                    star_active = false;
                    opaddr = scalar_opaddr + 1;
                } else {
                    if (filter_retries) {
                        // Keep only positions at which the following atom
                        // can match. A failed suffix then skips up to eight
                        // provably useless byte-by-byte retries at once.
                        ap_uint<8> retry_candidates = 0;
                        for (int candidate = 0; candidate < 8; ++candidate) {
#pragma HLS unroll
                            ap_uint<3> following_len =
                                internal_new::atomLength(instructions[1].range(63, 56), instructions[1].range(51, 48));
                            // Bytes beyond this window have not been read.
                            // Keep those candidates for scalar verification.
                            if (candidate < scan_len &&
                                (candidate + following_len > window_bytes ||
                                 internal_new::atomMatches(instructions[1], char_window, candidate, scalar_str_ptr,
                                                           (ap_uint<16>)lenMsg, 0)))
                                retry_candidates[candidate] = 1;
                        }
                        star_active = false;
                        if (retry_candidates != 0) {
                            push = true;
                            push_data =
                                internal_new::makeBitmapEntry(scalar_str_ptr, scalar_opaddr + 1, retry_candidates);
                        }
                    } else {
                        // For a control instruction after the star, retain
                        // every logical retry in one continuous range.
                        ap_uint<16> retry_pos = scalar_str_ptr + scan_len - 1;
                        if (!star_active) {
                            star_active = true;
                            star_begin = scalar_str_ptr;
                            star_stack_addr = stk;
                            push = true;
                            push_data = internal_new::makeRangeEntry(star_begin, scalar_opaddr + 1, retry_pos);
                        } else {
                            ap_uint<54> range_data =
                                internal_new::makeRangeEntry(star_begin, scalar_opaddr + 1, retry_pos);
                            stack_buff[star_stack_addr] = range_data;
                            stk_cache_valid = true;
                            stk_cache_addr = star_stack_addr;
                            stk_cache_data = range_data;
                        }
                    }
                    str_ptr = scalar_str_ptr + scan_len;
#ifndef __SYNTHESIS__
                    consumed_bytes += scan_len;
                    star_bytes += scan_len;
#endif
                }
                break;
            }
            case OP_MARK: {
                push = true;
                push_data = internal_new::makeStackEntry(2, operand1_1, 0, 0, 0, scalar_str_ptr);
                opaddr = scalar_opaddr + 1;
                break;
            }
            case OP_STEP_BACK_START: {
                ap_int<17> stepped = (ap_int<17>)scalar_str_ptr - operand2_1;
                if (stepped < 0) {
                    fail_pop = true;
                } else {
                    str_ptr = stepped;
                    if (operand3_1 != 0) {
                        push = true;
                        push_data = internal_new::makeStackEntry(0, 0, 0, 0, scalar_opaddr + 1, (ap_uint<16>)stepped);
                        opaddr = operand1_1;
                    } else {
                        opaddr = scalar_opaddr + 1;
                    }
                }
                break;
            }
            case OP_FAIL: {
                fail_pop = true;
                break;
            }
            case OP_POP_TO_MARK: {
                fail_pop = true;
                pop_to_mark = true;
                break;
            }
            default:
                next_status = 2;
                break;
        }

        // OP_POP discards one logical entry without applying its saved state.
        // A compressed range is one physical entry but represents many logical
        // alternative entries, so discard just one retry position.
        if (plain_pop) {
            if (top_type == internal_new::kBitmapStackType && retry_mask_valid) {
                if (remaining_retry_mask != 0) {
                    ap_uint<54> shortened = internal_new::makeBitmapEntry(range_begin, top_pc, remaining_retry_mask);
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
            // Failure and OP_POP_TO_MARK both consume one logical stack entry.
        } else if (fail_pop) {
            if (stk <= 1)
                next_status = 0;

            if (top_type == internal_new::kBitmapStackType && retry_mask_valid) {
                if (!pop_to_mark) {
                    opaddr = top_pc;
                    str_ptr = bitmap_retry_str;
                }
                if (remaining_retry_mask != 0) {
                    ap_uint<54> shortened = internal_new::makeBitmapEntry(range_begin, top_pc, remaining_retry_mask);
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
                    capture_write_0 = true;
                    capture_addr_0 = top_mem_id;
                    capture_data_0 = top_mem_start;
                    capture_write_1 = true;
                    capture_addr_1 = top_mem_id + 1;
                    capture_data_1 = top_mem_end;
                } else if (pop_to_mark && top_type == 2 && top_mem_id == operand1_1) {
                    opaddr++;
                }
                if (stk > 0)
                    stk--;
            }
        } else if (push) {
            if (stk >= STACK_SIZE - 1) {
                next_status = 2;
            } else {
                stack_buff[stk] = push_data;
                stk_cache_valid = true;
                stk_cache_addr = stk;
                stk_cache_data = push_data;
                stk++;
            }
        }
        // One write site per physical port keeps all opcodes' stores in the
        // same pipeline stage and makes the forwarding contract explicit.
        if (capture_write_0) {
            offsetBuff[capture_addr_0] = capture_data_0;
            mem_adr_reg_2 = mem_adr_reg_1;
            mem_dt_reg_2 = mem_dt_reg_1;
            mem_adr_reg_1 = capture_addr_0;
            mem_dt_reg_1 = capture_data_0;
        }
        if (capture_write_1) {
            offsetBuff[capture_addr_1] = capture_data_1;
            mem_adr_reg_2 = mem_adr_reg_1;
            mem_dt_reg_2 = mem_dt_reg_1;
            mem_adr_reg_1 = capture_addr_1;
            mem_dt_reg_1 = capture_data_1;
        }
#if XF_REGEX_VM_NEW_MAX_STEPS > 0
        final_status = step_limit_reached ? (ap_uint<2>)2 : next_status;
#else
        final_status = next_status;
#endif
    }

#ifndef __SYNTHESIS__
#ifdef XF_REGEX_VM_NEW_PROFILE
    printf("[RE PERF NEW] len=%u, iterations=%u, consumed=%u, batched=%u, star_bytes=%u\n", lenMsg, exec_iterations,
           consumed_bytes, batched_atom_iterations, star_bytes);
#endif
#endif

    if (final_status == 1) {
        match = 1;
        offsetBuff[0] = str_start;
        offsetBuff[1] = str_ptr;
    } else {
        match = final_status;
    }
}

/**
 * @brief Source-compatible interface using the production 64-bit instruction
 * and 32-bit message memories.
 */
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_new(ap_uint<32> *bitSetBuff, ap_uint<64> *instrBuff, ap_uint<32> *msgBuff, unsigned int lenMsg,
                 ap_uint<2> &match, ap_uint<16> *offsetBuff) {
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
void regexVM_new_wide(ap_uint<32> *bitSetBuff, ap_uint<512> *instrBuff, ap_uint<512> *msgBuff, unsigned int lenMsg,
                      ap_uint<2> &match, ap_uint<16> *offsetBuff) {
    regexVM_new_core<STACK_SIZE, INSTR_DEPTH>(bitSetBuff, instrBuff, msgBuff, lenMsg, match, offsetBuff);
}

// Native streaming-PU layout. The configuration protocol stays 64-bit; only
// the local storage changes. Class offsets in bytecode remain 32-bit-word
// offsets, so compiled rule images and capture/result formats are unchanged.
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_new_wide(ap_uint<256> *bitSetBuff, ap_uint<512> *instrBuff, ap_uint<512> *msgBuff, unsigned int lenMsg,
                      ap_uint<2> &match, ap_uint<16> *offsetBuff) {
    regexVM_new_core<STACK_SIZE, INSTR_DEPTH, XF_REGEX_VM_NEW_WIDE_II>(bitSetBuff, instrBuff, msgBuff, lenMsg, match,
                                                                       offsetBuff);
}

/** Native kernel interface. Each instruction address selects a descriptor
 * produced by internal_new::decodeInstructionWindow during configuration. */
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_new_predecoded(ap_uint<256> *bitSetBuff, ap_uint<512> *instrBuff, ap_uint<512> *msgBuff,
                            unsigned int lenMsg, ap_uint<2> &match, ap_uint<16> *offsetBuff) {
    static_assert(XF_REGEX_VM_NEW_ATOM_LANES <= 2 || INSTR_DEPTH == 0,
                  "Predecoded rows retain two literal instructions");
    regexVM_new_core<STACK_SIZE, INSTR_DEPTH, XF_REGEX_VM_NEW_WIDE_II, true>(bitSetBuff, instrBuff, msgBuff, lenMsg,
                                                                             match, offsetBuff);
}

// Alternate spelling for call sites that want to emphasize that this is the
// experimental successor to regexVM_opt.
template <int STACK_SIZE, int INSTR_DEPTH = 256>
void regexVM_opt_new(ap_uint<32> *bitSetBuff, ap_uint<64> *instrBuff, ap_uint<32> *msgBuff, unsigned int lenMsg,
                     ap_uint<2> &match, ap_uint<16> *offsetBuff) {
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
