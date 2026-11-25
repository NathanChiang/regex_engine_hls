#!/usr/bin/env python3
"""Compare 15 generic cases against original optimized VM RTL.

The bench checks match status and captures. Existing original-VM semantic
mismatches are recorded without a speedup ratio.
"""
import argparse
import csv
import pathlib
import shutil
import subprocess

HERE = pathlib.Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", default="regex_vm_opt_perf")
    parser.add_argument("--new-cycles", type=pathlib.Path,
                        default=HERE / "results/regex_vm_new_general_rtl_cycles.csv")
    args = parser.parse_args()
    rtl = HERE / "build" / args.project / "sol/syn/verilog"
    if not (rtl / "regex_vm_opt_perf.v").is_file():
        parser.error("Synthesize RE_VM_VARIANT=opt with run_hls_new_accel.tcl first")
    work = HERE / "build/opt_general_rtl"
    work.mkdir(parents=True, exist_ok=True)
    for data in rtl.glob("*.dat"):
        shutil.copy2(data, work / data.name)
    project = ['verilog xil_defaultlib "' + p.as_posix() + '"' for p in sorted(rtl.glob("*.v"))]
    project.append('sv xil_defaultlib "' + (HERE / "test_opt_general_rtl.sv").as_posix() + '"')
    (work / "rtl.prj").write_text("\n".join(project) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")
    for tool, options in [("xvlog", ["-prj", "rtl.prj"]),
                          ("xelab", ["xil_defaultlib.test_opt_general_rtl", "-s", "generic", "-mt", "2"]),
                          ("xsim", ["generic", "-tclbatch", "run.tcl"])]:
        executable = shutil.which(tool)
        if not executable:
            executable = str(pathlib.Path("C:/Xilinx/Vivado/2021.2/bin") / (tool + ".bat"))
        print("Running " + tool, flush=True)
        with (work / (tool + ".log")).open("w") as log:
            subprocess.run([executable] + options, cwd=work, stdout=log, stderr=subprocess.STDOUT,
                           timeout=900, check=True)
    console = (work / "xsim.log").read_text()
    if "COMPLETED generic original-VM RTL" not in console or "Fatal:" in console:
        raise SystemExit("Baseline validation failed: " + str(work / "xsim.log"))
    with args.new_cycles.open(newline="") as handle:
        new = {(r["family"], r["bytes"], r["expected_match"]): int(r["cycles"])
               for r in csv.DictReader(handle)}
    with (work / "generic_opt_cycles.csv").open(newline="") as handle:
        original = list(csv.DictReader(handle))
    if len(original) != 15:
        raise SystemExit("Incomplete original-VM measurement")
    with (work / "generic_comparison.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["family", "bytes", "expected_match", "original_actual_match", "original_correct",
                         "original_opt_cycles", "new_cycles", "speedup"])
        for row in original:
            key = row["family"], row["bytes"], row["expected_match"]
            new_cycles = new[key]
            speedup = round(int(row["cycles"]) / new_cycles, 4) if row["correct"] == "1" else ""
            writer.writerow([*key, row["actual_match"], row["correct"], row["cycles"], new_cycles, speedup])
    print("Measured 15 original-VM RTL cases; correct=" + str(sum(r["correct"] == "1" for r in original)))
    print("Comparison: " + str(work / "generic_comparison.csv"))


if __name__ == "__main__":
    main()
