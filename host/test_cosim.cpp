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

// Generate deterministic vectors for the persistent RTL testbench.  This uses
// the same log, regex compiler, byte packing, and Oniguruma reference as
// test.cpp.  test_cosim.sv consumes the files and checks the csynth RTL.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include "oniguruma.h"
#include "xf_data_analytics/text/xf_re_compile.h"
}

namespace {

const unsigned int kMaxMessageBytes = 4088;
const unsigned int kInstructionWords = 65536;
const unsigned int kBitsetWords = 1024;
const uint64_t kRangeBase = 0x10000000ULL;
const uint64_t kRangeSize = 0x1000ULL;
const unsigned int kRangeCount = 8;

struct Message {
    uint16_t length;
    uint64_t address;
    std::vector<std::array<uint64_t, 8> > beats;
};

const char* find_option(int argc, const char* argv[], const char* option) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], option) == 0) return argv[i + 1];
    return 0;
}

bool load_messages(const std::string& path, int line_limit,
                   std::vector<std::string>& lines,
                   std::vector<Message>& messages) {
    std::ifstream input(path.c_str());
    if (!input.is_open()) {
        std::cerr << "Cannot open input log " << path << "\n";
        return false;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        const bool selected = !line.empty() &&
                              (line_limit < 0 || line_number < line_limit);
        ++line_number;
        if (!selected) continue;
        if (line.size() > kMaxMessageBytes) {
            std::cout << "Skipping overlength message\n";
            continue;
        }

        Message message;
        message.length = static_cast<uint16_t>(line.size());
        message.address = kRangeBase +
                          (messages.size() % kRangeCount) * kRangeSize + 0x100;
        const unsigned int beat_count = (line.size() + 63) / 64;
        message.beats.resize(beat_count);
        for (unsigned int beat = 0; beat < beat_count; ++beat) {
            message.beats[beat].fill(0);
            for (unsigned int byte = 0; byte < 64; ++byte) {
                const unsigned int index = beat * 64 + byte;
                const uint8_t value = static_cast<uint8_t>(
                    index < line.size() ? line[index] : ' ');
                message.beats[beat][byte / 8] |=
                    static_cast<uint64_t>(value) << (8 * (byte % 8));
            }
        }
        lines.push_back(line);
        messages.push_back(message);
    }
    return true;
}

bool make_reference(const std::vector<std::string>& lines,
                    const std::string& pattern,
                    unsigned int capture_count,
                    std::vector<uint32_t>& reference) {
    OnigEncoding encodings[1] = {ONIG_ENCODING_ASCII};
    onig_initialize(encodings, 1);

    regex_t* regex = 0;
    OnigErrorInfo error_info;
    UChar* pattern_begin = reinterpret_cast<UChar*>(
        const_cast<char*>(pattern.c_str()));
    int rc = onig_new(&regex, pattern_begin, pattern_begin + pattern.size(),
                      ONIG_OPTION_DEFAULT, ONIG_ENCODING_ASCII,
                      ONIG_SYNTAX_DEFAULT, &error_info);
    if (rc != ONIG_NORMAL) {
        UChar message[ONIG_MAX_ERROR_MESSAGE_LEN];
        onig_error_code_to_str(message, rc, &error_info);
        std::cerr << "Oniguruma compile failed: " << message << "\n";
        onig_end();
        return false;
    }

    OnigRegion* region = onig_region_new();
    const unsigned int words_per_message = capture_count + 1;
    reference.assign(lines.size() * words_per_message, 0);

    for (unsigned int m = 0; m < lines.size(); ++m) {
        UChar* begin = reinterpret_cast<UChar*>(
            const_cast<char*>(lines[m].c_str()));
        UChar* end = begin + lines[m].size();
        rc = onig_search(regex, begin, end, begin, end, region,
                         ONIG_OPTION_NONE);
        if (rc >= 0) {
            const unsigned int base = m * words_per_message;
            reference[base] = 1;
            if (static_cast<unsigned int>(region->num_regs) > capture_count) {
                std::cerr << "Reference capture count exceeds compiled header\n";
                onig_region_free(region, 1);
                onig_free(regex);
                onig_end();
                return false;
            }
            for (int group = 0; group < region->num_regs; ++group) {
                uint32_t packed =
                    static_cast<uint32_t>(region->end[group] * 65536);
                packed += static_cast<uint32_t>(region->beg[group]);
                reference[base + 1 + group] = packed;
            }
        } else if (rc != ONIG_MISMATCH) {
            UChar message[ONIG_MAX_ERROR_MESSAGE_LEN];
            onig_error_code_to_str(message, rc);
            std::cerr << "Oniguruma search failed: " << message << "\n";
            onig_region_free(region, 1);
            onig_free(regex);
            onig_end();
            return false;
        }
    }

    onig_region_free(region, 1);
    onig_free(regex);
    onig_end();
    return true;
}

} // namespace

int main(int argc, const char* argv[]) {
    const char* directory_arg = find_option(argc, argv, "-dir");
    const char* output_arg = find_option(argc, argv, "-out");
    const char* lines_arg = find_option(argc, argv, "-ln");
    if (!directory_arg || !output_arg) {
        std::cerr << "usage: test_cosim -dir <log_data> -out <directory> [-ln count]\n";
        return 1;
    }

    int line_limit = -1;
    if (lines_arg) line_limit = std::atoi(lines_arg);
    const std::string input_path =
        std::string(directory_arg) + "/access_5k.log";

    std::vector<std::string> lines;
    std::vector<Message> messages;
    if (!load_messages(input_path, line_limit, lines, messages) ||
        messages.empty())
        return 1;

    const std::string pattern =
        "^(?<remote>[^ ]*) (?<host>[^ ]*) (?<user>[^ ]*) "
        "\\[(?<time>[^\\]]*)\\] \"(?<method>\\S+)(?: "
        "+(?<path>[^\\\"]*?)(?: +\\S*)?)?\" (?<code>[^ ]*) "
        "(?<size>[^ ]*)(?: \"(?<referer>[^\\\"]*)\" "
        "\"(?<agent>[^\\\"]*)\"(?:\\s+"
        "(?<http_x_forwarded_for>[^ ]+))?)?$";

    std::vector<unsigned int> bitset(kBitsetWords, 0);
    std::vector<uint64_t> configuration(kInstructionWords, 0);
    std::vector<uint8_t> capture_names(1024, 0);
    std::vector<uint32_t> capture_offsets(20, 0);
    unsigned int instruction_count = 0;
    unsigned int class_count = 0;
    unsigned int capture_count = 0;
    if (xf_re_compile(pattern.c_str(), bitset.data(), configuration.data() + 2,
                      &instruction_count, &class_count, &capture_count,
                      capture_names.data(), capture_offsets.data()) != 0) {
        std::cerr << "Regex compilation failed\n";
        return 1;
    }

    ++capture_count; // Include group zero, matching test.cpp and the VM header.
    unsigned int configuration_words = 2 + instruction_count;
    for (unsigned int i = 0; i < class_count * 4; ++i) {
        configuration[configuration_words++] =
            (static_cast<uint64_t>(bitset[2 * i + 1]) << 32) |
            bitset[2 * i];
    }
    if (configuration_words > 256) {
        std::cerr << "Compiled rule exceeds the 2048-byte rule slot\n";
        return 1;
    }
    configuration[0] = configuration_words;
    configuration[1] = static_cast<uint64_t>(instruction_count) |
                       (static_cast<uint64_t>(class_count) << 32) |
                       (static_cast<uint64_t>(capture_count) << 48);

    std::vector<uint32_t> reference;
    if (!make_reference(lines, pattern, capture_count, reference)) return 1;

    const std::string output_directory(output_arg);
    std::ofstream input((output_directory + "/cosim_input.txt").c_str());
    std::ofstream expected((output_directory + "/cosim_expected.txt").c_str());
    if (!input.is_open() || !expected.is_open()) {
        std::cerr << "Cannot create CoSim vector files in "
                  << output_directory << "\n";
        return 1;
    }

    const unsigned int words_per_message = capture_count + 1;
    input << configuration_words << ' ' << messages.size() << ' '
          << words_per_message << '\n';
    input << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < configuration_words; ++i)
        input << std::setw(16) << configuration[i] << '\n';
    for (unsigned int m = 0; m < messages.size(); ++m) {
        input << std::dec << messages[m].length << ' ' << std::hex
              << std::setw(16) << messages[m].address << std::dec << ' '
              << messages[m].beats.size() << '\n';
        input << std::hex;
        for (unsigned int beat = 0; beat < messages[m].beats.size(); ++beat) {
            for (int lane = 7; lane >= 0; --lane)
                input << std::setw(16) << messages[m].beats[beat][lane];
            input << '\n';
        }
    }

    expected << messages.size() << ' ' << words_per_message << '\n';
    expected << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < reference.size(); ++i)
        expected << std::setw(8) << reference[i] << '\n';

    std::cout << "Generated C/RTL vectors: messages=" << messages.size()
              << ", instructions=" << instruction_count
              << ", classes=" << class_count
              << ", capture groups=" << capture_count << "\n";
    return 0;
}
