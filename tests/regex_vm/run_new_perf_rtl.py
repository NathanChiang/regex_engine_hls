#!/usr/bin/env python3
"""Run the 200-point post-csynth regexVM_new RTL cycle sweep."""

import csv
import pathlib
import shutil
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent
RTL = HERE / "build/regex_vm_new_perf/sol/syn/verilog"
WORK = HERE / "build/regex_vm_new_rtl_perf"


def tool_path(name):
    found = shutil.which(name)
    if found:
        return found
    candidate = pathlib.Path("C:/Xilinx/Vivado/2021.2/bin") / (name + ".bat")
    if candidate.exists():
        return str(candidate)
    sys.exit("Cannot find " + name + " or the default Vivado 2021.2 install")


def run(name, args):
    print("Running " + name, flush=True)
    with (WORK / (name + "_console.log")).open("w") as log:
        subprocess.run([tool_path(name)] + args, cwd=str(WORK), stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=900)


def main():
    if not RTL.exists():
        sys.exit("Run vitis_hls -f tests/regex_vm/run_perf_hls.tcl first")

    WORK.mkdir(parents=True, exist_ok=True)
    project = ['verilog xil_defaultlib "' + path.as_posix() + '"'
               for path in sorted(RTL.glob("*.v"))]
    project.append('sv xil_defaultlib "' +
                   (HERE / "test_new_perf_rtl.sv").as_posix() + '"')
    (WORK / "new_perf.prj").write_text("\n".join(project) + "\n")
    (WORK / "run.tcl").write_text("run all\nquit\n")

    run("xvlog", ["-prj", "new_perf.prj"])
    run("xelab", ["xil_defaultlib.test_new_perf_rtl", "-s",
                  "regex_vm_new_rtl_perf", "-mt", "2"])
    run("xsim", ["regex_vm_new_rtl_perf", "-tclbatch", "run.tcl"])

    console = (WORK / "xsim_console.log").read_text()
    if "PASS regexVM_new RTL sweep" not in console or "Fatal:" in console:
        sys.exit("RTL sweep failed; inspect " + str(WORK / "xsim_console.log"))

    csv_path = WORK / "regex_vm_new_cycles.csv"
    with csv_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 200:
        sys.exit("Expected 200 CSV samples, got " + str(len(rows)))

    cycles = [int(row["regexVM_new_cycles"]) for row in rows]
    print("PASS regexVM_new RTL sweep: 200 samples")
    print("CSV: " + str(csv_path))
    print("First: {} instructions, {} cycles".format(
        rows[0]["rule_instructions"], cycles[0]))
    print("Last: {} instructions, {} cycles".format(
        rows[-1]["rule_instructions"], cycles[-1]))


if __name__ == "__main__":
    main()
