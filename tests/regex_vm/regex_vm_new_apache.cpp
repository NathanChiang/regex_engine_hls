#include "ap_int.h"
#include "xf_data_analytics/text/regexVM_new.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace xf::data_analytics::text;


#ifdef COSIM_OPT
extern "C" unsigned int regex_vm_opt_perf(ap_uint<32> bitset[1024], ap_uint<64> instructions[4096],
                                          ap_uint<32> message[1024], unsigned int length, ap_uint<16> offsets[512]);
#endif

#ifdef COSIM_ACCEL
extern "C" unsigned int regex_vm_new_accel_perf(ap_uint<256> bitset[128], ap_uint<512> instructions[32],
                                                ap_uint<512> message[64], unsigned int length,
                                                ap_uint<16> offsets[512]);
#endif

static unsigned long long parseHex64(const std::string &text) {
    unsigned long long value = 0;
    std::istringstream input(text);
    input >> std::hex >> value;
    return value;
}

static unsigned parseHexByte(const std::string &text, std::size_t position) {
    return static_cast<unsigned>(std::strtoul(text.substr(position, 2).c_str(), 0, 16));
}

int main(int argc, const char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: regex_vm_new_apache <cosim_input.txt>\n";
        return 1;
    }

    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 1;
    }

    unsigned configuration_words = 0;
    unsigned message_count = 0;
    unsigned result_words = 0;
    input >> configuration_words >> message_count >> result_words;
    std::vector<unsigned long long> configuration(configuration_words);
    for (unsigned i = 0; i < configuration_words; ++i) {
        std::string word;
        input >> word;
        configuration[i] = parseHex64(word);
    }

    const unsigned instruction_count = static_cast<unsigned>(configuration[1]);
    const unsigned class_count = static_cast<unsigned>(configuration[1] >> 32) & 0xffff;
    const unsigned capture_count = static_cast<unsigned>(configuration[1] >> 48) & 0xffff;
    if (instruction_count > 256 || capture_count > 256 ||
        2 + instruction_count + class_count * 4 != configuration_words) {
        std::cerr << "invalid rule-vector header\n";
        return 1;
    }

    ap_uint<64> instructions[4096];
    ap_uint<32> bitset[1024];
    for (unsigned i = 0; i < 4096; ++i)
        instructions[i] = (ap_uint<64>)OP_END << 56;
    for (unsigned i = 0; i < 1024; ++i)
        bitset[i] = 0;
    for (unsigned i = 0; i < instruction_count; ++i)
        instructions[i] = configuration[2 + i];
    const unsigned class_base = 2 + instruction_count;
    for (unsigned i = 0; i < class_count * 4; ++i) {
        const unsigned long long packed = configuration[class_base + i];
        bitset[2 * i] = packed;
        bitset[2 * i + 1] = packed >> 32;
    }

#if defined(COSIM_ACCEL) && defined(XF_REGEX_VM_NEW_PREDECODED)
    ap_uint<512> decoded_instructions[256];
    ap_uint<512> empty_window = 0;
    for (unsigned lane = 0; lane < 8; ++lane)
        empty_window.range(64 * lane + 63, 64 * lane) = (ap_uint<64>)OP_END << 56;
    for (unsigned pc = 0; pc < 256; ++pc) {
        decoded_instructions[pc] = internal_new::decodeInstructionWindow(empty_window, pc);
    }
    internal_new::prepareRule<256>(instructions, decoded_instructions, instruction_count);
    const char *export_path = std::getenv("RE_VM_DESCRIPTOR_EXPORT");
    if (export_path) {
        std::ofstream output(export_path);
        if (!output) {
            std::cerr << "cannot export descriptors\n";
            return 1;
        }
        for (unsigned pc = 0; pc < 256; ++pc) {
            std::string hex = decoded_instructions[pc].to_string(16);
            if (hex.size() > 1 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
                hex.erase(0, 2);
            output << std::string(128 - hex.size(), '0') << hex << '\n';
        }
    }
#endif

    unsigned failures = 0;
    for (unsigned message_id = 0; message_id < message_count; ++message_id) {
        unsigned length = 0;
        std::string address;
        unsigned beat_count = 0;
        input >> length >> address >> beat_count;
        ap_uint<32> message[1024];
        for (unsigned i = 0; i < 1024; ++i)
            message[i] = 0;
        for (unsigned beat = 0; beat < beat_count; ++beat) {
            std::string packed;
            input >> packed;
            if (packed.size() != 128) {
                std::cerr << "invalid payload beat for message " << message_id << "\n";
                return 1;
            }
            for (unsigned byte = 0; byte < 64; ++byte) {
                const unsigned value = parseHexByte(packed, packed.size() - 2 * (byte + 1));
                const unsigned index = beat * 64 + byte;
                message[index >> 2].range(8 * (index & 3) + 7, 8 * (index & 3)) = value;
            }
        }

        // regexVM_opt speculatively prefetches operand-indexed capture slots
        // for every opcode, including large literal operands. Those ignored
        // reads still need valid C++ storage in the software oracle.
        static ap_uint<16> old_offsets[65537];
        ap_uint<16> new_offsets[512];
        for (unsigned i = 0; i < 512; ++i)
            old_offsets[i] = new_offsets[i] = 0xffff;
        ap_uint<2> old_match = 3;
        ap_uint<2> new_match = 3;
        regexVM_opt<8192>(bitset, instructions, message, length, old_match, old_offsets);
#ifdef COSIM_ACCEL
        ap_uint<512> wide_instructions[32], wide_message[64];
        ap_uint<256> wide_bitset[128];
        for (unsigned i = 0; i < 32; ++i)
            for (unsigned k = 0; k < 8; ++k)
                wide_instructions[i].range(64 * k + 63, 64 * k) = instructions[8 * i + k];
        for (unsigned i = 0; i < 64; ++i)
            for (unsigned k = 0; k < 16; ++k)
                wide_message[i].range(32 * k + 31, 32 * k) = message[16 * i + k];
        for (unsigned i = 0; i < 128; ++i)
            for (unsigned k = 0; k < 8; ++k)
                wide_bitset[i].range(32 * k + 31, 32 * k) = bitset[8 * i + k];
        new_match = regex_vm_new_accel_perf(wide_bitset,
#ifdef XF_REGEX_VM_NEW_PREDECODED
                                            decoded_instructions,
#else
                                            wide_instructions,
#endif
                                            wide_message, length, new_offsets);
#elif defined(COSIM_OPT)
        new_match = regex_vm_opt_perf(bitset, instructions, message, length, new_offsets);
#else
        regexVM_new<8192, 256>(bitset, instructions, message, length, new_match, new_offsets);
#endif

        bool equal = old_match == new_match;
        for (unsigned i = 0; i < capture_count * 2; ++i)
            equal = equal && old_offsets[i] == new_offsets[i];
        if (!equal) {
            ++failures;
            std::cout << "FAIL message=" << message_id << " bytes=" << length
                      << " old_match=" << static_cast<unsigned>(old_match)
                      << " new_match=" << static_cast<unsigned>(new_match)
                      << " old_range=" << static_cast<unsigned>(old_offsets[0]) << ".."
                      << static_cast<unsigned>(old_offsets[1]) << " new_range=" << static_cast<unsigned>(new_offsets[0])
                      << ".." << static_cast<unsigned>(new_offsets[1]) << "\n";
        }
    }

    std::cout << (failures ? "FAIL" : "PASS") << " regexVM_new Apache differential: messages=" << message_count
              << " failures=" << failures << " result_words=" << result_words << "\n";
    return failures ? 1 : 0;
}
