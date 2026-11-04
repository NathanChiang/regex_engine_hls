/*
 * Copyright (C) 2019-2022, Xilinx, Inc.
 * Copyright (C) 2022-2023, Advanced Micro Devices, Inc.
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
 * @file re_engine.hpp
 * @brief RE engine implementation
 */
#ifndef XF_TEXT_RE_ENGINE_HPP
#define XF_TEXT_RE_ENGINE_HPP

#include "hls_stream.h"
#include "ap_int.h"
#include "xf_data_analytics/text/regexVM.hpp"

namespace xf {
namespace data_analytics {
namespace text {
namespace internal {

// write data to external DDR/HBM
static void writeToExtMem(hls::stream<ap_uint<32> >& i_strm,
                          hls::stream<bool>& e_strm,
                          hls::stream<ap_uint<16> >& nm_strm,
                          ap_uint<32>* out_buff) {
    bool e = false;
    ap_uint<32> offt = 1;
    do {
        e = e_strm.read();
        ap_uint<16> nm = nm_strm.read();
        for (int i = 0; i < nm; ++i) {
#pragma HLS pipeline II = 1
            out_buff[offt + i] = i_strm.read();
        }
        offt += nm;
    } while (!e);
    out_buff[0] = offt;
}

enum ReExecBlockState { RE_BLK_FREE = 0, RE_BLK_BUSY = 1, RE_BLK_COLLECTING = 2 };

// Dispatches one message at a time to free PUs. Message payload remains in a 512-bit stream and the
// length/serial metadata travels in a separate stream.
template <int PU_NM>
void dispatchMsgByFreePU(hls::stream<ap_uint<16> >& len_strm,
                         hls::stream<ap_uint<512> >& msg_strm,
                         hls::stream<bool> done_strm[PU_NM],
                         hls::stream<ap_uint<64> > meta_strm[PU_NM],
                         hls::stream<ap_uint<512> > pu_msg_strm[PU_NM],
                         hls::stream<ap_uint<8> >& order_strm) {
    bool pu_free[PU_NM];
#pragma HLS array_partition variable = pu_free complete
    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
        pu_free[p] = true;
    }

    ap_uint<8> rr = 0;
    ap_uint<16> in_flight = 0;
    bool end = false;
    while (!end) {
#pragma HLS loop_tripcount min = 1000 max = 1000
        ap_uint<16> len = len_strm.read();
        end = (len == 0);

        if (!end) {
            bool found = false;
            ap_uint<8> pid = 0;

#ifdef __SYNTHESIS__
            while (!found) {
                ap_uint<16> done_cnt = 0;
                for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                    bool done = false;
                    bool got_done = done_strm[p].read_nb(done);
                    if (got_done) {
                        pu_free[p] = true;
                        done_cnt++;
                    }
                }
                in_flight -= done_cnt;

                for (int i = 0; i < PU_NM; ++i) {
#pragma HLS unroll
                    ap_uint<8> cand = rr + i;
                    if (cand >= PU_NM) cand -= PU_NM;
                    if (!found && pu_free[cand]) {
                        pid = cand;
                        found = true;
                    }
                }
            }
#else
            pid = rr;
            found = true;
#endif

            pu_free[pid] = false;
#ifdef __SYNTHESIS__
            in_flight++;
#endif
            rr = pid + 1;
            if (rr >= PU_NM) rr = 0;

            ap_uint<16> len_512b = (len + 63) / 64;
            ap_uint<64> meta = 0;
            meta.range(15, 0) = len;
            meta.range(63, 63) = 0;
            order_strm.write(pid);
            for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                if (pid == p) meta_strm[p].write(meta);
            }

            for (int i = 0; i < len_512b; ++i) {
#pragma HLS pipeline II = 1
#pragma HLS loop_tripcount min = 1 max = 8
                ap_uint<512> msg = msg_strm.read();
                for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
                    if (pid == p) pu_msg_strm[p].write(msg);
                }
            }
        }
    }

#ifdef __SYNTHESIS__
    while (in_flight > 0) {
#pragma HLS loop_tripcount min = 1 max = 16
        ap_uint<16> done_cnt = 0;
        for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
            bool done = false;
            bool got_done = done_strm[p].read_nb(done);
            if (got_done) {
                pu_free[p] = true;
                done_cnt++;
            }
        }
        in_flight -= done_cnt;
    }
#endif

    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS pipeline II = 1
        ap_uint<64> meta = 0;
        meta.range(63, 63) = 1;
        meta_strm[p].write(meta);
    }
    order_strm.write(0xff);
}

// Moves a single 512-bit message from the PU stream into regexVM's 32-bit message buffer.
template <int MSG_LEN>
void readMsg512ToBuff(ap_uint<16> len, hls::stream<ap_uint<512> >& msg_strm, ap_uint<32> msg_buff[MSG_LEN * 2]) {
    ap_uint<16> len_512b = (len + 63) / 64;
    ap_uint<16> len_32b = ((len + 7) / 8) * 2;

    for (int i = 0; i < len_512b; ++i) {
#pragma HLS loop_tripcount min = 1 max = 8
        ap_uint<512> in = msg_strm.read();
        for (int j = 0; j < 16; ++j) {
#pragma HLS pipeline II = 1
            int word_id = 16 * i + j;
            if (word_id < len_32b) {
                msg_buff[word_id] = in.range(32 * j + 31, 32 * j);
            }
        }
    }
    // regexVM speculatively reads the current and next 32-bit word, including
    // at end-of-message. The public message limit reserves these guard words.
    if (len_32b + 1 < MSG_LEN * 2) {
        msg_buff[len_32b] = 0;
        msg_buff[len_32b + 1] = 0;
    }
}

template <int STACK_SIZE>
void reExecOneMessage(ap_uint<16>* mem_oft_buff,
                      ap_uint<64>* instr_buff,
                      ap_uint<32>* bitset_buff,
                      ap_uint<32>* msg_buff,
                      unsigned int msg_len) {
    ap_uint<2> match = 3;
    if (msg_len > 0) {
        xf::data_analytics::text::regexVM_opt<STACK_SIZE>(bitset_buff, instr_buff, msg_buff, msg_len, match,
                                                          mem_oft_buff + 3);
    }

    mem_oft_buff[1] = match;
    mem_oft_buff[2] = 0;
}

// One independent regex execution block. It cycles through free, busy and collecting states for each testcase.
template <int INSTR_DEPTH, int CCLASS_NM, int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void reExecIndependentBlock(ap_uint<16> cpgp_nm,
                            hls::stream<ap_uint<64> >& meta_strm,
                            hls::stream<ap_uint<512> >& msg_strm,
                            ap_uint<64>* instr_buff,
                            ap_uint<32>* bitset_buff,
                            hls::stream<ap_uint<32> >& result_strm,
                            hls::stream<bool>& done_strm) {
    ap_uint<32> msg_buff[MSG_LEN * 2];
#pragma HLS bind_storage variable = msg_buff type = ram_t2p impl = bram

    // Three leading slots precede the VM's capture offsets; collection also
    // reads the match flag pair. Reserve that prefix at maximum cpgp_nm.
    ap_uint<16> mem_oft_buff[CPGP_NM * 2 + 4];
#pragma HLS bind_storage variable = mem_oft_buff type = ram_t2p impl = bram

    for (int i = 0; i < CPGP_NM + 2; ++i) {
#pragma HLS pipeline II = 1
        mem_oft_buff[2 * i] = -1;
        mem_oft_buff[2 * i + 1] = -1;
    }

    ReExecBlockState state = RE_BLK_FREE;
    bool last = false;
    while (!last) {
#pragma HLS loop_tripcount min = 1000 max = 1000
        state = RE_BLK_FREE;
        ap_uint<64> meta = meta_strm.read();
        last = meta.range(63, 63);

        if (!last) {
            ap_uint<16> len = meta.range(15, 0);

            state = RE_BLK_BUSY;
            readMsg512ToBuff<MSG_LEN>(len, msg_strm, msg_buff);
            reExecOneMessage<STACK_SIZE>(mem_oft_buff, instr_buff, bitset_buff, msg_buff, len);

            state = RE_BLK_COLLECTING;
            for (int g = 0; g < cpgp_nm + 1; ++g) {
#pragma HLS pipeline II = 2
                ap_uint<32> out = 0;
                out.range(15, 0) = mem_oft_buff[g * 2 + 1];
                out.range(31, 16) = mem_oft_buff[g * 2 + 2];
                mem_oft_buff[g * 2 + 1] = -1;
                mem_oft_buff[g * 2 + 2] = -1;
                result_strm.write(out);
            }
#ifdef __SYNTHESIS__
            done_strm.write(true);
#endif
        }
    }
}

template <int PU_NM, int INSTR_DEPTH, int CCLASS_NM, int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void reExecIndependentBlocks(ap_uint<16> cpgp_nm,
                             hls::stream<ap_uint<64> > meta_strm[PU_NM],
                             hls::stream<ap_uint<512> > pu_msg_strm[PU_NM],
                             ap_uint<64>* instr_buff,
                             ap_uint<32>* bitset_buff,
                             hls::stream<ap_uint<32> > result_strm[PU_NM],
                             hls::stream<bool> done_strm[PU_NM]) {
    for (int p = 0; p < PU_NM; ++p) {
#pragma HLS unroll
        reExecIndependentBlock<INSTR_DEPTH, CCLASS_NM, CPGP_NM, MSG_LEN, STACK_SIZE>(
            cpgp_nm, meta_strm[p], pu_msg_strm[p], instr_buff, bitset_buff, result_strm[p], done_strm[p]);
    }
}

// Collects completed testcases in original testcase serial order.
template <int PU_NM, int CPGP_NM>
void collectOrderedREResult(ap_uint<16> cpgp_nm,
                            hls::stream<ap_uint<8> >& order_strm,
                            hls::stream<ap_uint<32> > result_strm[PU_NM],
                            hls::stream<ap_uint<32> >& o_strm,
                            hls::stream<ap_uint<16> >& o_nm_strm,
                            hls::stream<bool>& o_e_strm) {
    bool end = false;
    while (!end) {
#pragma HLS loop_tripcount min = 1000 max = 1000
        ap_uint<8> pid = order_strm.read();
        end = (pid == 0xff);

        if (!end) {
            o_e_strm.write(false);
            o_nm_strm.write(cpgp_nm + 1);
            for (int p = 0; p < PU_NM; ++p) {
                if (pid == p) {
                    for (int g = 0; g < cpgp_nm + 1; ++g) {
#pragma HLS pipeline II = 1
                        o_strm.write(result_strm[p].read());
                    }
                }
            }
        }
    }

    o_nm_strm.write(0);
    o_e_strm.write(true);
}

// executates all message by multiple PU with multiple loop
template <int PU_NM, int INSTR_DEPTH, int CCLASS_NM, int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void reExecMPU(ap_uint<16> cpgp_nm,
               hls::stream<ap_uint<16> >& len_strm,
               hls::stream<ap_uint<512> >& msg_strm,
               ap_uint<64>* instr_buff,
               ap_uint<32>* bitset_buff,
               hls::stream<ap_uint<32> >& o_strm,
               hls::stream<ap_uint<16> >& o_nm_strm,
               hls::stream<bool>& o_e_strm) {
#pragma HLS dataflow
    hls::stream<ap_uint<64> > meta_strm[PU_NM];
#pragma HLS stream variable = meta_strm depth = 2
#pragma HLS bind_storage variable = meta_strm type = fifo impl = lutram

    hls::stream<ap_uint<512> > pu_msg_strm[PU_NM];
#pragma HLS stream variable = pu_msg_strm depth = 8
#pragma HLS bind_storage variable = pu_msg_strm type = fifo impl = bram

    hls::stream<ap_uint<32> > result_strm[PU_NM];
#pragma HLS stream variable = result_strm depth = 1024
#pragma HLS bind_storage variable = result_strm type = fifo impl = bram

    hls::stream<bool> done_strm[PU_NM];
#pragma HLS stream variable = done_strm depth = 32
#pragma HLS bind_storage variable = done_strm type = fifo impl = lutram

    hls::stream<ap_uint<8> > order_strm("order_strm");
#pragma HLS stream variable = order_strm depth = 64
#pragma HLS bind_storage variable = order_strm type = fifo impl = lutram

    dispatchMsgByFreePU<PU_NM>(len_strm, msg_strm, done_strm, meta_strm, pu_msg_strm, order_strm);
    reExecIndependentBlocks<PU_NM, INSTR_DEPTH, CCLASS_NM, CPGP_NM, MSG_LEN, STACK_SIZE>(
        cpgp_nm, meta_strm, pu_msg_strm, instr_buff, bitset_buff, result_strm, done_strm);
    collectOrderedREResult<PU_NM, CPGP_NM>(cpgp_nm, order_strm, result_strm, o_strm, o_nm_strm, o_e_strm);
}

// gets message from DDR/HBM, execuates it by multiple PU and stores the result to DDR/HBM
template <int PU_NM, int INSTR_DEPTH, int CCLASS_NM, int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void reExec(ap_uint<16> cpgp_nm,
            ap_uint<64>* instr_buff,
            ap_uint<32>* bitset_buff,
            hls::stream<ap_uint<16> >& msg_len_strm,
            hls::stream<ap_uint<512> >& msg_in_strm,
            ap_uint<32>* out_buff) {
#pragma HLS dataflow
    hls::stream<ap_uint<32> > out_strm("out_strm");
#pragma HLS stream variable = out_strm depth = 32

    hls::stream<ap_uint<16> > nm_strm("nm_strm");
#pragma HLS stream variable = nm_strm depth = 8

    hls::stream<bool> e_strm("e_strm");
#pragma HLS stream variable = e_strm depth = 8

    reExecMPU<PU_NM, INSTR_DEPTH, CCLASS_NM, CPGP_NM, MSG_LEN, STACK_SIZE>(cpgp_nm, msg_len_strm, msg_in_strm, instr_buff,
                                                                           bitset_buff, out_strm, nm_strm, e_strm);

    writeToExtMem(out_strm, e_strm, nm_strm, out_buff);
}

} // namespace internal

/**
 * @brief The reEngine executes input messages with a preprocessed RE pattern.
 * The rule table owns the per-job instruction and bitset memories. The selected
 * entries are passed directly to this function, so configuration decoding is
 * not repeated for every message.
 *
 * @tparam PU_NM Number of processing units in parallel.
 * @tparam INSTR_DEPTH The depth of instruction buffer in 64-bit.
 * @tparam CCLASS_NM Supported max number of character classes in regular expression pattern.
 * @tparam CPGP_NM Supported max number of capturing group in regular expression pattern.
 * @tparam MSG_LEN Supported max length for each message in 8-byte.
 * @tparam STACK_SIZE Max size of internal stack buffer in regex-VM.
 *
 * @param cpgp_nm Number of capturing groups for the selected rule.
 * @param instr_buff Pointer to the selected rule's preprocessed instructions.
 * @param bitset_buff Pointer to the selected rule's preprocessed bitset.
 * @param msg_in_strm Input messages to be matched by the regular expression.
 * @param len_in_strm input length for each message.
 * @param out_buff Output match results.
 *
 */

template <int PU_NM, int INSTR_DEPTH, int CCLASS_NM, int CPGP_NM, int MSG_LEN, int STACK_SIZE>
void reEngine(ap_uint<16> cpgp_nm,
              ap_uint<64>* instr_buff,
              ap_uint<32>* bitset_buff,
              volatile unsigned int* msg_ready,
              volatile unsigned int* cfg_done,
              hls::stream<ap_uint<512> >& msg_in_strm,
              hls::stream<ap_uint<16> >& len_in_strm,
              ap_uint<32>* out_buff) {
    cfg_done[0] = 1;
    while (msg_ready[0] == 0) {
#pragma HLS loop_tripcount min = 1 max = 1
    }

    // execute regexVM to match the message from input streams
    internal::reExec<PU_NM, INSTR_DEPTH, CCLASS_NM, CPGP_NM, MSG_LEN, STACK_SIZE>(cpgp_nm, instr_buff, bitset_buff,
                                                                                  len_in_strm, msg_in_strm, out_buff);
}

} // namespace text
} // namespace data_analytics
} // namespace xf

#endif //_RE_ENGINE_HPP_
