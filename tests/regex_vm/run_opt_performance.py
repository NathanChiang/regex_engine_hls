#!/usr/bin/env python3
"""Measure regexVM_opt RTL for the 100 rules and their 15-line input blocks."""

import argparse
import csv
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WORK = HERE / "build/opt_performance_rtl"
RTL = HERE / "build/regex_vm_opt_performance/sol/syn/verilog"
RESULT = HERE / "results/regex_vm_opt_performance.csv"
CLOCK_NS = 6


def read_inputs():
    rules = [line.split("\t") for line in
             (ROOT / "log_data/re_engine_performance_test_cases.txt").read_text().splitlines()]
    messages = (ROOT / "log_data/regex_engine.log").read_bytes().splitlines()
    if len(rules) != 100 or len(messages) != 1500:
        raise ValueError("Expected exactly 100 rules and 1,500 input lines")
    for index, row in enumerate(rules):
        case_id = "{:02d}.{:02d}".format(index // 10 + 1, index % 10 + 1)
        if len(row) != 5 or row[0] != case_id:
            raise ValueError("Unexpected rule layout at " + case_id)
        regex = re.compile(row[4].replace(r"\z", r"\Z").encode("ascii"))
        for sample, message in enumerate(messages[index * 15:(index + 1) * 15]):
            if not 1 <= len(message) <= 512 or b"\r" in message:
                raise ValueError("Input length/line-ending error for " + case_id)
            if bool(regex.fullmatch(message)) != (sample < 10):
                raise ValueError("Unexpected input match for {} case {}".format(case_id, sample + 1))
    return rules, messages


def prepare_vectors(rules, messages):
    # The repository's compiler/Oniguruma shared libraries are Linux binaries.
    library_dir = ROOT / "re_compile/lib/lib"
    onig = library_dir / "libonig.so"
    compiler = library_dir / "libxfcompile.so"
    ctypes.CDLL(str(onig), mode=ctypes.RTLD_GLOBAL)
    library = ctypes.CDLL(str(compiler))
    u32, u64 = ctypes.c_uint32, ctypes.c_uint64
    pointer = ctypes.POINTER
    compile_rule = library.xf_re_compile
    compile_rule.argtypes = [ctypes.c_char_p, pointer(u32), pointer(u64),
                            pointer(u32), pointer(u32), pointer(u32),
                            ctypes.c_void_p, ctypes.c_void_p]
    compile_rule.restype = ctypes.c_int
    WORK.mkdir(parents=True, exist_ok=True)
    configs = []
    for row in rules:
        # Allow the compiler ample room, then check the actual RTL capacities.
        bitsets, instructions = (u32 * 65536)(), (u64 * 65536)()
        count, classes, captures = u32(), u32(), u32()
        status = compile_rule(row[4].encode("ascii"), bitsets, instructions,
                              ctypes.byref(count), ctypes.byref(classes),
                              ctypes.byref(captures), None, None)
        if status:
            raise RuntimeError("Rule {} compilation failed: {}".format(row[0], status))
        if not 1 <= count.value <= 4096 or classes.value * 8 > 1024 or captures.value > 255:
            raise ValueError("Rule {} exceeds the wrapper memory capacities".format(row[0]))
        configs.append((list(instructions[:count.value]), list(bitsets[:classes.value * 8])))
    with (WORK / "performance_vectors.txt").open("w", newline="\n") as output:
        output.write("100 15\n")
        for index, (instructions, bitsets) in enumerate(configs):
            output.write("{} {}\n".format(len(instructions), len(bitsets)))
            output.writelines("{:016x}\n".format(word) for word in instructions)
            output.writelines("{:08x}\n".format(word) for word in bitsets)
            for sample in range(15):
                message = messages[index * 15 + sample]
                output.write("{} {}\n".format(len(message), int(sample < 10)))
                for offset in range(0, len(message), 4):
                    word = int.from_bytes(message[offset:offset + 4].ljust(4, b"\0"), "little")
                    output.write("{:08x}\n".format(word))
    print("Compiled 100 rules and prepared 1,500 inputs", flush=True)


def tool(name):
    found = shutil.which(name)
    if found:
        return found
    package = "Vitis_HLS" if name == "vitis_hls" else "Vivado"
    candidate = Path("C:/Xilinx") / package / "2021.2/bin" / (name + ".bat")
    if candidate.is_file():
        return str(candidate)
    raise FileNotFoundError("Cannot find " + name)


def run(command, name, timeout=1800):
    log_path = WORK / (name + "_console.log")
    print("Running {} (log: {})".format(name, log_path), flush=True)
    with log_path.open("w") as log:
        subprocess.run(command, cwd=WORK, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=timeout)


def save_results(rules, messages):
    with (WORK / "cycles.csv").open(newline="") as source:
        measurements = list(csv.DictReader(source))
    if len(measurements) != 1500:
        raise RuntimeError("Expected 1,500 RTL cycle measurements, got " + str(len(measurements)))
    result = []
    for index, measurement in enumerate(measurements):
        rule, sample = index // 15, index % 15
        expected = int(sample < 10)
        actual = int(measurement["actual_match"])
        cycles = int(measurement["cycles"])
        length = len(messages[index])
        if (int(measurement["rule_index"]) != rule + 1 or
                int(measurement["case_index"]) != sample + 1 or
                int(measurement["input_bytes"]) != length or
                int(measurement["expected_match"]) != expected or cycles <= 0):
            raise RuntimeError("Invalid RTL measurement at row " + str(index + 1))
        result.append([rules[rule][0], rules[rule][1], sample + 1, index + 1,
                       length, expected, actual, int(actual == expected), cycles,
                       cycles * CLOCK_NS, round(length * 1000 / (cycles * CLOCK_NS), 6)])
    RESULT.parent.mkdir(parents=True, exist_ok=True)
    with RESULT.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["rule_id", "category", "case_index", "log_line", "input_bytes",
                         "expected_match", "actual_match", "correct", "cycles",
                         "latency_ns", "throughput_MBps"])
        writer.writerows(result)
    mismatches = sum(row[7] == 0 for row in result)
    print("Measured 1,500 cases; {} match-status discrepancies. CSV: {}".format(mismatches, RESULT))
    if mismatches:
        print("Discrepancies are retained in the CSV; no VM implementation changes were made.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true", help="compile Linux rule vectors only")
    parser.add_argument("--skip-synthesis", action="store_true", help="reuse this benchmark's synthesized RTL")
    parser.add_argument("--wsl-distro", default="Ubuntu-20.04", help="Windows: Linux compiler distribution")
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    rules, messages = read_inputs()
    if os.name == "nt":
        if args.prepare_only:
            parser.error("--prepare-only uses Linux compiler libraries; run it under WSL")
        script = subprocess.check_output(["wsl", "-d", args.wsl_distro, "--exec",
                                          "wslpath", "-a", str(Path(__file__).resolve())], text=True).strip()
        run(["wsl", "-d", args.wsl_distro, "--exec", "python3", script, "--prepare-only"], "vectors")
    else:
        prepare_vectors(rules, messages)
    if args.prepare_only:
        return
    if not args.skip_synthesis:
        run([tool("vitis_hls"), "-f", str(HERE / "run_opt_performance.tcl")], "synthesis")
    if not (RTL / "regex_vm_opt_perf.v").is_file():
        raise FileNotFoundError("Missing synthesized optimized VM: " + str(RTL))
    for data in RTL.glob("*.dat"):
        shutil.copy2(data, WORK / data.name)
    project = ['verilog xil_defaultlib "' + p.as_posix() + '"' for p in sorted(RTL.glob("*.v"))]
    project.append('sv xil_defaultlib "' + (HERE / "test_opt_performance_rtl.sv").as_posix() + '"')
    (WORK / "rtl.prj").write_text("\n".join(project) + "\n")
    (WORK / "run.tcl").write_text("run all\nquit\n")
    run([tool("xvlog"), "-prj", "rtl.prj"], "xvlog")
    run([tool("xelab"), "xil_defaultlib.test_opt_performance_rtl", "-s", "performance", "-mt", "2"], "xelab")
    run([tool("xsim"), "performance", "-tclbatch", "run.tcl"], "xsim", timeout=3600)
    console = (WORK / "xsim_console.log").read_text()
    if "COMPLETED regexVM_opt performance: 1500 cases" not in console or "Fatal:" in console:
        raise RuntimeError("Incomplete RTL simulation; inspect " + str(WORK / "xsim_console.log"))
    save_results(rules, messages)


if __name__ == "__main__":
    main()
