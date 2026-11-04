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
#include <iostream>
#include <fstream>
#include <string.h>

extern "C" {
#include "oniguruma.h"
#include "xf_data_analytics/text/xf_re_compile.h"
}

#include "utils.hpp"

#ifndef HLS_TEST
#include <xcl2.hpp>
#include <CL/cl_ext_xilinx.h>
#else
#include "re_engine_kernel.hpp"
#endif
#include "xf_utils_sw/logger.hpp"
#define MAX_MSG_SZ 1600000
#define MAX_MSG_WORDS_512 (MAX_MSG_SZ / 8)
#define MAX_MSG_NM 60000
#define INSTRUC_SIZE 65536
#define BIT_SET_SIZE 1024
#define OUT_BUFF_DEPTH 20000

struct uint512_t {
    uint64_t d[8];
};

int writeOneLine(uint512_t* msg_buff, uint16_t* len_buff, unsigned int& offt, unsigned int& msg_nm, std::string& line) {
    typedef union {
        char c_a[64];
        uint512_t d;
    } uint512_un;

    unsigned int sz = line.size();
    if (sz > 4088) {
        printf("message length exceed the limitation\n");
        return 0;
    }
    if ((sz + 63) / 64 + offt > MAX_MSG_WORDS_512 || msg_nm >= MAX_MSG_NM - 1) {
        printf("Input log size access supported maximum size\n");
        return -1;
    } else {
        for (unsigned int i = 0; i < (sz + 63) / 64; ++i) {
            uint512_un out;
            for (unsigned int j = 0; j < 64; ++j) {
                if (i * 64 + j < sz) {
                    out.c_a[j] = line[i * 64 + j];
                } else {
                    out.c_a[j] = ' ';
                }
            }
            msg_buff[offt++] = out.d;
        }
        len_buff[msg_nm++] = sz; //(((sz+7)/8 + last_oft) << 3) + (8 - sz%8);
        return 0;
    }
}

int gen_reference(
    std::string file_path, std::string pattern, unsigned int cpgp_num, uint32_t* out_buff, int start_ln, int end_ln) {
    int r;
    unsigned char *start, *range, *end;
    regex_t* reg;
    OnigErrorInfo einfo;
    OnigRegion* region;
    OnigEncoding use_encs[1];

    use_encs[0] = ONIG_ENCODING_ASCII;
    onig_initialize(use_encs, sizeof(use_encs) / sizeof(use_encs[0]));

    UChar* pattern_c = (UChar*)pattern.c_str();
    r = onig_new(&reg, pattern_c, pattern_c + strlen((char*)pattern_c), ONIG_OPTION_DEFAULT, ONIG_ENCODING_ASCII,
                 ONIG_SYNTAX_DEFAULT, &einfo);
    if (r != ONIG_NORMAL) {
        char s[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str((UChar*)s, r, &einfo);
        fprintf(stderr, "ERROR: %s\n", s);
        return -1;
    }

    region = onig_region_new();

    unsigned int oft = 0;

    std::string line;
    // match all the message
    std::ifstream log_file(file_path);
    int cnt = 0;
    if (log_file.is_open()) {
        while (getline(log_file, line)) {
            // remove the empty line
            if (line.size() > 0 && cnt >= start_ln && (end_ln < 0 || cnt < end_ln)) {
                if (line.size() > 4088) {
                    printf("No action due to message length exceed the limitation\n");
                    continue;
                }
                UChar* str = (UChar*)line.c_str();
                end = str + strlen((char*)str);
                start = str;
                range = end;
                r = onig_search(reg, str, end, start, range, region, ONIG_OPTION_NONE);
                if (r >= 0) {
                    int i;

                    // fprintf(stderr, "match at %d\n", r);
                    // store 1 if match
                    out_buff[oft++] = 1;
                    // store the offset for each group
                    for (i = 0; i < region->num_regs; i++) {
                        // fprintf(stderr, "%d: (%d-%d)\n", i, region->beg[i], region->end[i]);
                        out_buff[oft + i] = region->end[i] * 65536;
                        out_buff[oft + i] += region->beg[i];
                    }
                    oft += cpgp_num;
                } else if (r == ONIG_MISMATCH) {
                    printf("search fail, lnm = %d\n", cnt);
                    // store 0 if not match
                    out_buff[oft++] = 0;
                    oft += cpgp_num;
                } else { /* error */
                    char s[ONIG_MAX_ERROR_MESSAGE_LEN];
                    onig_error_code_to_str((UChar*)s, r);
                    fprintf(stderr, "ERROR: %s\n", s);
                    onig_region_free(region, 1 /* 1:free self, 0:free contents only */);
                    onig_free(reg);
                    onig_end();
                    return -1;
                }
            }
            cnt++;
        }
        onig_region_free(region, 1 /* 1:free self, 0:free contents only */);
        onig_free(reg);
        onig_end();
        return 0;
    } else {
        printf("Input file open failed.\n");
        return -1;
    }
}

int compare_result(unsigned int cpgp_num, unsigned int msg_nm, uint32_t* ref, unsigned int* act_out) {
    bool flag = false;
    for (unsigned int i = 0; i < msg_nm; ++i) {
        for (unsigned int j = 0; j < cpgp_num + 1; ++j) {
            if (j == 0) {
                if (ref[i * (cpgp_num + 1) + j] != act_out[i * (cpgp_num + 1) + j + 1]) {
                    printf("[Mismatch],msg:%d, ref: %d, act: %d\n", i, ref[i * (cpgp_num + 1) + j],
                           act_out[i * (cpgp_num + 1) + j + 1]);
                    return -1;
                } else {
                    flag = ref[i * (cpgp_num + 1) + j];
                }
            } else {
                // only match, then check the capture group
                if (flag && ref[i * (cpgp_num + 1) + j] != act_out[i * (cpgp_num + 1) + j + 1]) {
                    printf("[Mismatch], msg:%d, capture group:%d, ref:[%d, %d], act:[%d, %d]\n", i, j - 1,
                           ref[i * (cpgp_num + 1) + j] % 65536, ref[i * (cpgp_num + 1) + j] / 65536,
                           act_out[i * (cpgp_num + 1) + j + 1] % 65536, act_out[i * (cpgp_num + 1) + j + 1] / 65536);
                    return -1;
                }
            }
        }
    }
    printf("[Correct] actual result match with reference\n");
    return 0;
}

int main(int argc, const char* argv[]) {
    std::cout << "\n-----------Test RE-Engine------------\n";
    xf::common::utils_sw::Logger logger(std::cout, std::cerr);
    // command argument parser
    ArgParser parser(argc, argv);

    // prepare the input data
    uint512_t* msg_buff = aligned_alloc<uint512_t>(MAX_MSG_WORDS_512);
    uint16_t* len_buff = aligned_alloc<uint16_t>(MAX_MSG_NM);

    std::string in_file_path;
    std::string in_dir = "log_data";
    if (parser.getCmdOption("-dir", in_dir)) {
        // std::cout<<"Error: xclbin path must be set!\n";
        in_file_path = in_dir + "/access_5k.log";
        printf("in_file_path = %s\n", in_file_path.c_str());
    } else {
        printf("Please set the log path\n");
        return -1;
    }
    int ln = 0;
    std::string tmp;
    if (parser.getCmdOption("-ln", tmp)) {
        // std::cout<<"Error: xclbin path must be set!\n";
        try {
            ln = std::stoi(tmp) + 1;
        } catch (...) {
            ln = 0;
        }
    }

    int start_ln = 0;
    ;
    int end_ln = -1 + ln;
    std::ifstream log_file(in_file_path);
    unsigned int offt = 0;
    unsigned int msg_nm = 0;
    unsigned int actual_msg_nm = 0;
    std::string line;
    int cnt = 0;
    if (log_file.is_open()) {
        while (getline(log_file, line)) {
            if (line.size() > 0 && cnt >= start_ln && (end_ln < 0 || cnt < end_ln)) {
                if (writeOneLine(msg_buff, len_buff, offt, msg_nm, line) != 0) return -1;
            }
            cnt++;
        }
        len_buff[msg_nm++] = 0;
        actual_msg_nm = msg_nm - 1;
        printf("actual log size: %d, msg number: %d\n", offt, actual_msg_nm);
    } else {
        printf("Input log file open failed.\n");
        return -1;
    }
    // prepare the configuration
    // const char* pattern =
    //    "^(?<host>[^ ]*) [^ ]* (?<user>[^ ]*) \\[(?<time>[^\\]]*)\\] \"(?<method>\\S+)(?: +(?<path>[^ ]*) +\\S*)?\" "
    //    "(?<code>[^ ]*) (?<size>[^ ]*)(?: \"(?<referer>[^\\\"]*)\" \"(?<agent>[^\\\"]*)\")?$";
    const char* pattern =
        "^(?<remote>[^ ]*) (?<host>[^ ]*) (?<user>[^ ]*) \\[(?<time>[^\\]]*)\\] \"(?<method>\\S+)(?: "
        "+(?<path>[^\\\"]*?)(?: +\\S*)?)?\" (?<code>[^ ]*) (?<size>[^ ]*)(?: \"(?<referer>[^\\\"]*)\" "
        "\"(?<agent>[^\\\"]*)\"(?:\\s+(?<http_x_forwarded_for>[^ ]+))?)?$";
    unsigned int instr_num = 0;
    unsigned int cclass_num = 0;
    unsigned int cpgp_num = 0;
    // bit set map for op cclass
    unsigned int* bitset = new unsigned int[BIT_SET_SIZE];
    // instruction list
    uint64_t* cfg_buff = aligned_alloc<uint64_t>(INSTRUC_SIZE);
    uint8_t* cpgp_name_val = aligned_alloc<uint8_t>(1024);
    uint32_t* cpgp_name_offt = aligned_alloc<uint32_t>(20);
    // message buffer
    if (xf_re_compile(pattern, bitset, cfg_buff + 2, &instr_num, &cclass_num, &cpgp_num,
                      cpgp_name_val, cpgp_name_offt) != 0) {
        fprintf(stderr, "Regex compilation failed\n");
        return -1;
    }
    printf("Name Table\n");
    for (int i = 0; i < cpgp_num; ++i) {
        printf("%d: ", i);
        for (int j = 0; j < cpgp_name_offt[i + 1] - cpgp_name_offt[i]; ++j) {
            printf("%c", cpgp_name_val[j + cpgp_name_offt[i]]);
        }
        printf("\n");
    }

    cpgp_num++;
    unsigned int cfg_nm = 2 + instr_num;
    printf("instr_num = %d, cclass_num = %d, cpgp_num = %d\n", instr_num, cclass_num, cpgp_num);
    for (unsigned int i = 0; i < cclass_num * 4; ++i) {
        uint64_t tmp = bitset[i * 2 + 1];
        tmp = tmp << 32;
        tmp += bitset[i * 2];
        cfg_buff[cfg_nm++] = tmp;
    }
    typedef union {
        struct {
            uint32_t instr_nm;
            uint16_t cc_nm;
            uint16_t gp_nm;
        } head_st;
        uint64_t d;
    } cfg_info;
    cfg_info cfg_h;
    cfg_h.head_st.instr_nm = instr_num;
    cfg_h.head_st.cc_nm = cclass_num;
    cfg_h.head_st.gp_nm = cpgp_num;

    cfg_buff[0] = cfg_nm;
    cfg_buff[1] = cfg_h.d;
    // generate the reference result
    uint32_t* ref_result = new uint32_t[(cpgp_num + 1) * actual_msg_nm];
    if (gen_reference(in_file_path, pattern, cpgp_num, ref_result, start_ln, end_ln) != 0)
        return -1;

    std::vector<uint32_t*> out_buff_vec;
    unsigned int out_nm = std::max<unsigned int>(1 + (cpgp_num + 1) * actual_msg_nm, OUT_BUFF_DEPTH);
    uint32_t* out_buff = aligned_alloc<uint32_t>(out_nm);
    memset(out_buff, 0, sizeof(uint32_t) * out_nm);
    out_buff_vec.push_back(out_buff);
    uint32_t* msg_ready = aligned_alloc<uint32_t>(1);
    uint32_t* cfg_done = aligned_alloc<uint32_t>(1);
    msg_ready[0] = 1;
    cfg_done[0] = 0;
#ifdef HLS_TEST
    // New reEngineKernel interface.
    hls::stream<DmaAxisBeat> dma_in;
    cfg_input cfg_in = {};
    cfg_output cfg_out = {};
    ap_uint<1> result_in = 0;
    match_out result_out = {};

    const ap_uint<16> TEST_JOB_ID = 1;
    const ap_uint<64> RANGE_BASE = 0x10000000ULL;
    const ap_uint<64> RANGE_SIZE = 0x1000ULL;
    const int NUM_RANGES = 8;
    const unsigned int RESULT_WORDS_PER_MESSAGE = cpgp_num + 1;
    unsigned int result_message = 0;
    unsigned int result_word = 0;

    // One call is one FSM step, which takes multiple clocks in synthesized RTL.
    auto tick = [&]() {
        reEngineKernel(dma_in, cfg_in, cfg_out, result_in, result_out);

        // Reassemble the sixteen-word result chunks into the original
        // reEngine output layout so the existing reference comparison can be
        // used for HLS_TEST as well.
        if (result_out.valid && !result_in) {
            unsigned int remaining = RESULT_WORDS_PER_MESSAGE - result_word;
            unsigned int words = remaining > 16 ? 16 : remaining;
            for (unsigned int i = 0; i < words; ++i) {
                out_buff[1 + result_message * RESULT_WORDS_PER_MESSAGE + result_word + i] =
                    result_out.output_buf.range(32 * i + 31, 32 * i);
            }
            result_word += words;
            if (result_word == RESULT_WORDS_PER_MESSAGE) {
                result_word = 0;
                result_message++;
            }
            result_in = 1;
        } else {
            result_in = 0;
        }
    };

    // Change command only while valid is low. This lets the kernel FSM move
    // into the requested cfg_in.cmd state before a transaction is asserted.
    auto select_cmd = [&](ap_uint<4> cmd) {
        cfg_in.valid = 0;
        cfg_in.cmd = cmd;
        tick();
        tick();
    };

    // Issue one configuration transaction. Hold valid until the selected
    // configuration block acknowledges it, then drop valid for one cycle so
    // its WAIT_VALID_LOW state can return to IDLE.
    auto cfg_write = [&](ap_uint<4> cmd,
                         ap_uint<16> job_id,
                         ap_uint<64> arg0,
                         ap_uint<64> arg1) -> bool {
        select_cmd(cmd);

        cfg_in.job_id = job_id;
        cfg_in.arg0 = arg0;
        cfg_in.arg1 = arg1;
        cfg_in.valid = 1;

        bool done = false;
        // A full directory scan plus two passes over all buckets takes >3072
        // steps. CLEAR alone takes 1025; 128 was insufficient after banking.
        for (int cycle = 0; cycle < 4096; ++cycle) {
            tick();
            if (cfg_out.ack || cfg_out.full) {
                done = cfg_out.ack;
                break;
            }
        }

        cfg_in.valid = 0;
        tick();
        return done;
    };

    // Start from an empty IOVA/rule table.
    if (!cfg_write(RE_CFG_CLEAR, 0, 0, 0)) {
        printf("CLEAR failed\n");
        return -1;
    }

    // Job 1 owns eight simple, non-overlapping address ranges.
    for (int i = 0; i < NUM_RANGES; ++i) {
        ap_uint<64> base = RANGE_BASE + (ap_uint<64>)i * RANGE_SIZE;
        ap_uint<64> limit = base + RANGE_SIZE;

        if (!cfg_write(RE_CFG_ADDR_WRITE, TEST_JOB_ID, base, limit)) {
            printf("address range %d write failed\n", i);
            return -1;
        }
    }

    // The rule table stores one 2048-byte rule as 256 x 64-bit writes.
    if (cfg_nm > 256) {
        printf("compiled rule is %u bytes; rule table supports only 2048 bytes\n",
               cfg_nm * 8);
        return -1;
    }

    for (unsigned int i = 0; i < cfg_nm; ++i) {
        if (!cfg_write(RE_CFG_RULE_WRITE,
                       TEST_JOB_ID,
                       (ap_uint<64>)(i * 8),
                       (ap_uint<64>)cfg_buff[i])) {
            printf("rule word %u write failed\n", i);
            return -1;
        }
    }

    // Enter normal runtime mode.
    select_cmd(RE_CFG_NONE);
    cfg_in.cmd = RE_CFG_NONE;
    cfg_in.valid = 0;
    cfg_in.job_id = 0;
    cfg_in.arg0 = 0;
    cfg_in.arg1 = 0;

    // Feed the original test messages. Message n uses address range n % 8.
    // Each range address is deliberately chosen inside [base, limit).
    unsigned int msg_word = 0;
    for (unsigned int m = 0; m < actual_msg_nm; ++m) {
        unsigned int bytes_left = len_buff[m];
        unsigned int beats = (bytes_left + 63) / 64;

        ap_uint<64> base = RANGE_BASE +
                           (ap_uint<64>)(m % NUM_RANGES) * RANGE_SIZE;
        ap_uint<64> dma_address = base + 0x100;

        for (unsigned int b = 0; b < beats; ++b) {
            DmaAxisBeat beat = {};
            for (int lane = 0; lane < 8; ++lane)
                beat.data.range(64 * lane + 63, 64 * lane) = msg_buff[msg_word].d[lane];
            ++msg_word;
            beat.user[0] = (b == 0);
            beat.last = (b == beats - 1);
            beat.user.range(64, 1) = dma_address;

            unsigned int beat_bytes = bytes_left > 64 ? 64 : bytes_left;
            beat.keep = (beat_bytes == 64)
                            ? (ap_uint<64>)(~0ULL)
                            : (ap_uint<64>)((1ULL << beat_bytes) - 1ULL);
            beat.strb = beat.keep;

            dma_in.write(beat);
            bytes_left -= beat_bytes;
        }

        // Let DMA -> IOVA -> rule lookup -> reEngine finish this packet before
        // sending the next one. This is intentionally simple, not throughput-oriented.
        int cycle = 0;
        for (; cycle < 4096 && result_message <= m; ++cycle)
            tick();
        if (result_message != m + 1) {
            fprintf(stderr, "Timed out waiting for message %u\n", m);
            return -1;
        }
        tick(); // acknowledge the last chunk
    }

    // A few final idle cycles to drain the last transaction.
    for (int cycle = 0; cycle < 128; ++cycle)
        tick();

    out_buff[0] = 1 + result_message * RESULT_WORDS_PER_MESSAGE;

#else
    std::string xclbin_path;
    if (!parser.getCmdOption("-xclbin", xclbin_path)) {
        std::cout << "Error: xclbin path must be set!\n";
    }
    cl_int cl_err;
    // Get device
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    cl::Device device = devices[0];

    // create context and command queue
    cl::Context context(device, NULL, NULL, NULL, &cl_err);
    logger.logCreateContext(cl_err);
    cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &cl_err);
    logger.logCreateCommandQueue(cl_err);
    std::string devName = device.getInfo<CL_DEVICE_NAME>();
    std::cout << "Selected Device " << devName << "\n";

    cl::Program::Binaries xclBins = xcl::import_binary_file(xclbin_path);
    devices.resize(1);
    cl::Program program(context, devices, xclBins, NULL, &cl_err);
    logger.logCreateProgram(cl_err);

    std::string krnl_name = "reEngineKernel";
    cl_uint cu_number;
    {
        cl::Kernel k(program, krnl_name.c_str());
        k.getInfo(CL_KERNEL_COMPUTE_UNIT_COUNT, &cu_number);
    }
    std::vector<cl::Kernel> kernel(cu_number);
    for (cl_uint i = 0; i < cu_number; ++i) {
        std::string krnl_full_name = krnl_name + ":{" + krnl_name + "_" + std::to_string(i + 1) + "}";
        kernel[i] = cl::Kernel(program, krnl_full_name.c_str(), &cl_err);
        logger.logCreateKernel(cl_err);
    }
    for (cl_uint i = 0; i < cu_number - 1; ++i) {
        uint32_t* out_buff = aligned_alloc<uint32_t>(out_nm);
        memset(out_buff, 0, sizeof(uint32_t) * out_nm);
        out_buff_vec.push_back(out_buff);
    }
    std::vector<cl_mem_ext_ptr_t> inCfgMemExt(cu_number);
    std::vector<cl_mem_ext_ptr_t> msgReadyMemExt(cu_number);
    std::vector<cl_mem_ext_ptr_t> cfgDoneMemExt(cu_number);
    std::vector<cl_mem_ext_ptr_t> inMsgMemExt(cu_number); // {1,msg_buff,kernel()};
    std::vector<cl_mem_ext_ptr_t> inLenMemExt(cu_number); //{2,len_buff, kernel()};
    std::vector<cl_mem_ext_ptr_t> outMemExt(cu_number);   // {3, out_buff, kernel()};
    for (cl_uint i = 0; i < cu_number; ++i) {
        inCfgMemExt[i] = {0, cfg_buff, kernel[i]()};
        msgReadyMemExt[i] = {1, msg_ready, kernel[i]()};
        cfgDoneMemExt[i] = {2, cfg_done, kernel[i]()};
        inMsgMemExt[i] = {3, msg_buff, kernel[i]()};
        inLenMemExt[i] = {4, len_buff, kernel[i]()};
        outMemExt[i] = {5, out_buff_vec[i], kernel[i]()};
    }
    std::vector<cl::Buffer> inMsgBuff(cu_number); //
    std::vector<cl::Buffer> inCfgBuff(cu_number); //
    std::vector<cl::Buffer> msgReadyBuff(cu_number);
    std::vector<cl::Buffer> cfgDoneBuff(cu_number);
    std::vector<cl::Buffer> inLenBuff(cu_number); //
    std::vector<cl::Buffer> outBuff(cu_number);   //
    for (cl_uint i = 0; i < cu_number; ++i) {
        inMsgBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                  sizeof(uint512_t) * offt, &inMsgMemExt[i]);
        inCfgBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                  sizeof(uint64_t) * cfg_nm, &inCfgMemExt[i]);
        msgReadyBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                       sizeof(uint32_t), &msgReadyMemExt[i]);
        cfgDoneBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                     sizeof(uint32_t), &cfgDoneMemExt[i]);
        inLenBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                  sizeof(uint16_t) * msg_nm, &inLenMemExt[i]);
        outBuff[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                sizeof(uint32_t) * out_nm, &outMemExt[i]);
    }

    // set args
    for (cl_uint i = 0; i < cu_number; ++i) {
        int j = 0;
        kernel[i].setArg(j++, inCfgBuff[i]);
        kernel[i].setArg(j++, msgReadyBuff[i]);
        kernel[i].setArg(j++, cfgDoneBuff[i]);
        kernel[i].setArg(j++, inMsgBuff[i]);
        kernel[i].setArg(j++, inLenBuff[i]);
        kernel[i].setArg(j++, outBuff[i]);
    }

    std::vector<cl::Memory> in_vec;
    for (cl_uint i = 0; i < cu_number; ++i) {
        in_vec.push_back(inMsgBuff[i]);
        in_vec.push_back(inCfgBuff[i]);
        in_vec.push_back(msgReadyBuff[i]);
        in_vec.push_back(inLenBuff[i]);
    }

    struct timeval tv0, tv1, tv2, tv3;
    int num_rep = 1;
    for (int i = 0; i < num_rep; ++i) {
        gettimeofday(&tv0, 0);
        q.enqueueMigrateMemObjects(in_vec, 0, nullptr, nullptr);
        q.finish();
        gettimeofday(&tv1, 0);
        for (cl_uint c = 0; c < cu_number; ++c) {
            q.enqueueTask(kernel[c], nullptr, nullptr);
        }

        q.finish();
        gettimeofday(&tv2, 0);
        std::vector<cl::Memory> out_vec;
        for (cl_uint i = 0; i < cu_number; ++i) {
            out_vec.push_back(outBuff[i]);
            out_vec.push_back(cfgDoneBuff[i]);
        }
        q.enqueueMigrateMemObjects(out_vec, CL_MIGRATE_MEM_OBJECT_HOST, nullptr, nullptr);
        q.finish();
        gettimeofday(&tv3, 0);
        int h2d_tm = tvdiff(&tv0, &tv1);
        int exec_tm = tvdiff(&tv1, &tv2);
        int d2h_tm = tvdiff(&tv2, &tv3);
        std::cout << "H2D data transfer time " << h2d_tm / 1000 << " ms\n"
                  << "Kernel exect time " << exec_tm / 1000 << " ms\n"
                  << "D2H data transfer time " << d2h_tm / 1000 << " ms\n";
        printf("offt = %d, cu_number = %d\n", offt, cu_number);
        double throughput = (double)offt / exec_tm * 1000 / 1024 / 1024 * cu_number;
        std::cout << " total throughput " << throughput << std::endl;
    }
#endif
    // HLS_TEST now reassembles result_out chunks into the same layout used by
    // the original software/OpenCL path.
#ifdef HLS_TEST
    printf("[HLS_TEST] configuration and DMA feed completed\n");
    int nerr = (result_message != actual_msg_nm || result_word != 0)
                   ? -1
                   : compare_result(cpgp_num, actual_msg_nm, ref_result, out_buff);
#else
    int nerr = compare_result(cpgp_num, actual_msg_nm, ref_result, out_buff_vec[0]);
#endif
    // free buffer
    free(cfg_buff);
    delete[] bitset;
    delete[] ref_result;
    free(msg_buff);
    int buff_nm = out_buff_vec.size();
    for (int i = 0; i < buff_nm; ++i) {
        free(out_buff_vec[i]);
    }
    free(len_buff);
    free(msg_ready);
    free(cfg_done);
    free(cpgp_name_offt);
    free(cpgp_name_val);
    nerr ? logger.error(xf::common::utils_sw::Logger::Message::TEST_FAIL)
         : logger.info(xf::common::utils_sw::Logger::Message::TEST_PASS);
    return nerr;
}
