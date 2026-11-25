#!/usr/bin/env python3
"""Run the standalone regexVM_new RTL against the Apache CoSim vectors."""

import csv
import pathlib
import shutil
import statistics
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
RTL = HERE / "build" / (sys.argv[1] if len(sys.argv) > 1 else "regex_vm_new_accel_perf") / "sol/syn/verilog"
WORK = HERE / "build" / ("rtl_" + RTL.parent.parent.parent.name)
VECTORS = HERE / "fixtures/apache_100"


def tool_path(name):
    found = shutil.which(name)
    if found:
        return found
    candidate = pathlib.Path("C:/Xilinx/Vivado/2021.2/bin") / (name + ".bat")
    if candidate.exists():
        return str(candidate)
    sys.exit("Cannot find " + name + " or the default Vivado 2021.2 install")


def run(name, args, timeout=900):
    print("Running " + name, flush=True)
    with (WORK / (name + "_console.log")).open("w") as log:
        subprocess.run([tool_path(name)] + args, cwd=str(WORK), stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout)


def main():
    if not RTL.exists():
        sys.exit("Run RE_VM_ACTION=synth RE_VM_PREDECODED=1 vitis_hls -f tests/regex_vm/run_hls_new_accel.tcl first")
    for name in ("cosim_input.txt", "cosim_expected.txt"):
        if not (VECTORS / name).exists():
            sys.exit("Missing Apache vector: " + str(VECTORS / name))

    WORK.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(VECTORS / "cosim_input.txt", WORK / "cosim_input.txt")
    shutil.copyfile(VECTORS / "cosim_expected.txt", WORK / "cosim_expected.txt")
    project = ['verilog xil_defaultlib "' + path.as_posix() + '"'
               for path in sorted(RTL.glob("*.v"))]
    project.append('sv xil_defaultlib "' +
                   (HERE / "test_new_accel_apache_rtl.sv").as_posix() + '"')
    (WORK / "new_apache.prj").write_text("\n".join(project) + "\n")
    (WORK / "run.tcl").write_text("run all\nquit\n")

    top = (RTL / "regex_vm_new_accel_perf.v").read_text()
    defines = ["-d", "RE_VM_TWO_CLASS_PORTS"] if "bitset_address1" in top else []
    if "instructions_address1" not in top:
        descriptors = RTL.parent.parent.parent / "instruction_rows.txt"
        if not descriptors.exists():
            sys.exit("Missing descriptors; run RE_VM_PREDECODED=1 RE_VM_ACTION=synth first")
        shutil.copyfile(descriptors, WORK / "instruction_rows.txt")
        defines += ["-d", "RE_VM_PREDECODED"]
    run("xvlog", defines + ["-prj", "new_apache.prj"])
    run("xelab", ["xil_defaultlib.test_new_accel_apache_rtl", "-s",
                  "regex_vm_new_apache_rtl", "-mt", "2"])
    run("xsim", ["regex_vm_new_apache_rtl", "-tclbatch", "run.tcl"],
        timeout=1800)

    console = (WORK / "xsim_console.log").read_text()
    if ("PASS regexVM_new Apache RTL differential" not in console or
            "Fatal:" in console):
        sys.exit("RTL differential failed; inspect " +
                 str(WORK / "xsim_console.log"))

    csv_path = WORK / "regex_vm_new_accel_apache_rtl_cycles.csv"
    with csv_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    cycles = [int(row["regexVM_new_accel_cycles"]) for row in rows]
    count = int((VECTORS / "cosim_input.txt").read_text().splitlines()[0].split()[1])
    if len(rows) != count:
        sys.exit("Expected {} cycle rows, got {}".format(count, len(rows)))
    print("PASS regexVM_new Apache RTL differential: {} messages".format(count))
    print("Cycles min/median/mean/max: {}/{:.1f}/{:.1f}/{}".format(
        min(cycles), statistics.median(cycles), statistics.mean(cycles),
        max(cycles)))
    print("CSV: " + str(csv_path))


if __name__ == "__main__":
    main()
