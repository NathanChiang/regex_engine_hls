#!/usr/bin/env python3
"""Run the same 100-rule benchmark on the narrow regexVM_new interface."""

import argparse
import csv
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
import run_opt_performance as benchmark

HERE = Path(__file__).resolve().parent
benchmark.WORK = HERE / "build/new_performance_rtl"
benchmark.RTL = HERE / "build/regex_vm_new_performance/sol/syn/verilog"
benchmark.RESULT = HERE / "results/regex_vm_new_performance.csv"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-synthesis", action="store_true")
    parser.add_argument("--wsl-distro", default="Ubuntu-20.04")
    args = parser.parse_args()
    work, rtl = benchmark.WORK, benchmark.RTL
    work.mkdir(parents=True, exist_ok=True)
    rules, messages = benchmark.read_inputs()
    if os.name == "nt":
        if args.prepare_only:
            parser.error("--prepare-only requires the Linux compiler libraries")
        script = subprocess.check_output(["wsl", "-d", args.wsl_distro, "--exec",
                                          "wslpath", "-a", str(Path(__file__).resolve())], text=True).strip()
        benchmark.run(["wsl", "-d", args.wsl_distro, "--exec", "python3", script,
                       "--prepare-only"], "vectors")
    else:
        benchmark.prepare_vectors(rules, messages)
    if args.prepare_only:
        return
    if not args.skip_synthesis:
        benchmark.run([benchmark.tool("vitis_hls"), "-f", str(HERE / "run_new_performance.tcl")], "synthesis")
    if not (rtl / "regex_vm_new_perf.v").is_file():
        raise FileNotFoundError("Missing synthesized regexVM_new: " + str(rtl))
    for data in rtl.glob("*.dat"):
        shutil.copy2(data, work / data.name)
    project = ['verilog xil_defaultlib "' + p.as_posix() + '"' for p in sorted(rtl.glob("*.v"))]
    project.append('sv xil_defaultlib "' + (HERE / "test_new_performance_rtl.sv").as_posix() + '"')
    (work / "rtl.prj").write_text("\n".join(project) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")
    benchmark.run([benchmark.tool("xvlog"), "-prj", "rtl.prj"], "xvlog")
    benchmark.run([benchmark.tool("xelab"), "xil_defaultlib.test_new_performance_rtl",
                   "-s", "performance", "-mt", "2"], "xelab")
    benchmark.run([benchmark.tool("xsim"), "performance", "-tclbatch", "run.tcl"], "xsim", timeout=3600)
    console = (work / "xsim_console.log").read_text()
    if "COMPLETED regexVM_new performance: 1500 cases" not in console or "Fatal:" in console:
        raise RuntimeError("Incomplete RTL simulation; inspect " + str(work / "xsim_console.log"))
    benchmark.save_results(rules, messages)
    with benchmark.RESULT.open(newline="") as result:
        if any(row["correct"] != "1" for row in csv.DictReader(result)):
            raise RuntimeError("regexVM_new has match discrepancies; see the CSV")


if __name__ == "__main__":
    main()
