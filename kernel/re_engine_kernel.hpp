#ifndef RE_ENGINE_KERNEL_HPP
#define RE_ENGINE_KERNEL_HPP

#include <ap_int.h>
#include <hls_stream.h>
#include <ap_axi_sdata.h>

#define RE_PU_NM              16
#ifdef RE_HLS_QUICK_CSIM
// Keep the focused C simulation fast while exercising the same control path.
#define RE_INSTR_DEPTH        64
#define RE_CCLASS_NM          8
#define RE_CPGP_NM            2
#define RE_MSG_LEN            16
#define RE_STACK_SIZE         256
#else
#define RE_INSTR_DEPTH        (1 << 12)
#define RE_CCLASS_NM          128
#define RE_CPGP_NM            512
#define RE_MSG_LEN            512
#define RE_STACK_SIZE         (1 << 13)
#endif
#define RE_DMA_DATA_WIDTH     512
#define RE_RESULT_DATA_WIDTH  512
#define RE_MAX_RULES          16
#define RE_MAX_ADDRS_RANGE    1024
#define RE_MAX_PACKET_BYTES   (RE_MSG_LEN * 8 - 8) // VM reads one word ahead

#define RE_CFG_NONE          0
#define RE_CFG_ADDR_WRITE    1
#define RE_CFG_ADDR_DELETE   2
#define RE_CFG_RULE_WRITE    3
#define RE_CFG_RULE_DELETE   4
#define RE_CFG_CLEAR         5

// Use the vendor AXIS type: a plain struct is packed into TDATA by Vitis HLS.
// TUSER[0] = SOP; TUSER[64:1] = destination DMA address (valid at SOP).
// TKEEP is a contiguous low-byte mask; every non-final beat must be full.
typedef ap_axiu<RE_DMA_DATA_WIDTH, 65, 0, 0> DmaAxisBeat;

// One already-decoded write into every PU's private rule-memory replica.
// Configuration is acknowledged only after all PUs have applied this update.
struct ReRuleUpdate {
    ap_uint<4> rule;
    ap_uint<1> bitset;
    ap_uint<9> index;
    ap_uint<64> data;
};

struct cfg_input {
    // referencing the RE_CFG_* constants above
    ap_uint<4> cmd;
    // only modify any of the table when this is high
    ap_uint<1> valid;
    // self-explanatory
    ap_uint<16> job_id;
    // at RE_CFG_ADDR_WRITE, set to base address
    // at RE_CFG_RULE_WRITE, set to offset within the job's 2048-byte rule
    // region. The rule table decodes the word into its instruction or bitset
    // memory as the write is accepted.
    ap_uint<64> arg0;
    // at RE_CFG_ADDR_WRITE, set to limit address
    // at RE_CFG_RULE_WRITE, set to one 64-bit rule/configuration word
    ap_uint<64> arg1;
};

struct cfg_output {
    // Transaction completion; held while cfg_input.valid remains asserted.
    ap_uint<1> ack;
    // at RE_CFG_ADDR_WRITE, means the table is full and cannot accept more entries
    // at RE_CFG_RULE_WRITE, means the rule memory is full OR you exceed 2048 bytes of rules for this job
    ap_uint<1> full;
    // means you can change the cfg_input.cmd to a new value
    ap_uint<1> cmd_transition_allowed;
    // means both the address and rule tables have at least one valid entry, so the engine can start processing packets
    ap_uint<1> engine_ready;
};

struct match_out {
    // Payload precedes metadata in the AXI-Lite memory window. The kernel
    // constructs the whole record locally before publishing it.
    ap_uint<512> output_buf;
    ap_uint<64>  address;
    // Echo of the sampled result_in level. Software waits for ack==0 before
    // raising result_in, then for ack==1 before lowering it again.
    ap_uint<1>   ack;
    // Asserted when a result chunk is available in output_buf.
    ap_uint<1>   valid;
    // Legacy step-model backpressure flag. The streaming hardware keeps this
    // low and uses AXIS TREADY when its input queues are full.
    ap_uint<1>   halt;
};

extern "C" void reEngineKernel(

    // Passive PCIe/NVMe snoop stream
    hls::stream<DmaAxisBeat>& dma_in,

    // MMIO configuration
#ifdef __SYNTHESIS__
    volatile ap_uint<32> cfg_in[6],
#else
    cfg_input  cfg_in,
#endif
#ifdef __SYNTHESIS__
    volatile ap_uint<32> cfg_out[1],
#else
    cfg_output& cfg_out,
#endif

    // A rising edge on result_in acknowledges one valid chunk. Drive it low
    // and wait for result_out.ack==0 before acknowledging the next chunk.
#ifdef __SYNTHESIS__
    volatile ap_uint<32> result_in[2],
#else
    ap_uint<1>  result_in,
#endif
#ifdef __SYNTHESIS__
    volatile ap_uint<512> result_out[2]
#else
    match_out& result_out
#endif
);

#endif
