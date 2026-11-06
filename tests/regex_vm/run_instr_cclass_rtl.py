#!/usr/bin/env python3
"""Run the independent 16x16 instruction/character-class RTL matrix."""

import csv
import pathlib
import shutil
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent
OPT_RTL = HERE / "build/regex_vm_opt_perf/sol/syn/verilog"
NEW_RTL = HERE / "build/regex_vm_new_perf/sol/syn/verilog"
RESULTS = HERE / "results"
OUTPUT = RESULTS / "regex_vm_instr_cclass_cycles.csv"
RULE_BYTES_OUTPUT = RESULTS / "regex_vm_instr_cclass_rule_bytes.csv"


def tool_path(name):
    found = shutil.which(name)
    if found:
        return found
    candidate = pathlib.Path("C:/Xilinx/Vivado/2021.2/bin") / (name + ".bat")
    if candidate.exists():
        return str(candidate)
    sys.exit("Cannot find " + name + " or the default Vivado 2021.2 install")


def run_tool(work, name, args):
    print("Running {} ({})".format(name, work.name), flush=True)
    with (work / (name + "_console.log")).open("w") as log:
        subprocess.run([tool_path(name)] + args, cwd=str(work), stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=900)


def run_vm(label, rtl, define=None):
    work = HERE / ("build/regex_vm_{}_instr_cclass".format(label))
    work.mkdir(parents=True, exist_ok=True)
    project = ['verilog xil_defaultlib "' + path.as_posix() + '"'
               for path in sorted(rtl.glob("*.v"))]
    project.append('sv xil_defaultlib "' +
                   (HERE / "test_instr_cclass_rtl.sv").as_posix() + '"')
    (work / "matrix.prj").write_text("\n".join(project) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")

    xvlog_args = ["-prj", "matrix.prj"]
    if define:
        xvlog_args = ["-d", define] + xvlog_args
    run_tool(work, "xvlog", xvlog_args)
    snapshot = "regex_vm_{}_instr_cclass".format(label)
    run_tool(work, "xelab", ["xil_defaultlib.test_instr_cclass_rtl",
                              "-s", snapshot, "-mt", "2"])
    run_tool(work, "xsim", [snapshot, "-tclbatch", "run.tcl"])

    console = (work / "xsim_console.log").read_text()
    if "PASS instruction/cclass RTL matrix" not in console or "Fatal:" in console:
        sys.exit("RTL matrix failed; inspect " + str(work / "xsim_console.log"))
    csv_name = "regex_vm_{}_instr_cclass_cycles.csv".format(label)
    with (work / csv_name).open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 256:
        sys.exit("Expected 256 {} rows, got {}".format(label, len(rows)))
    return rows


def main():
    for rtl in (OPT_RTL, NEW_RTL):
        if not rtl.exists():
            sys.exit("Run vitis_hls -f tests/regex_vm/run_perf_hls.tcl first")

    opt_rows = run_vm("opt", OPT_RTL)
    new_rows = run_vm("new", NEW_RTL, "VM_NEW")
    coordinate_fields = [
        "sample", "instr_index", "cclass_index", "instr_num", "cclass_num",
        "header_bytes", "instr_data_bytes", "cclass_data_bytes",
        "original_rule_bytes", "message_bytes",
    ]
    output_fields = coordinate_fields + ["regexVM_opt_cycles", "regexVM_new_cycles"]

    RESULTS.mkdir(parents=True, exist_ok=True)
    with OUTPUT.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=output_fields)
        writer.writeheader()
        for opt, new in zip(opt_rows, new_rows):
            if any(opt[field] != new[field] for field in coordinate_fields):
                sys.exit("Opt/new matrix coordinates differ")
            row = {field: opt[field] for field in coordinate_fields}
            row["regexVM_opt_cycles"] = opt["regexVM_opt_cycles"]
            row["regexVM_new_cycles"] = new["regexVM_new_cycles"]
            writer.writerow(row)

    rule_byte_fields = [
        "sample", "instr_index", "cclass_index", "instr_num", "cclass_num",
        "header_offset_bytes", "header_bytes", "instr_offset_bytes",
        "instr_bytes_per_entry", "instr_data_bytes", "cclass_offset_bytes",
        "cclass_bytes_per_entry", "cclass_data_bytes", "original_rule_bytes",
    ]
    with RULE_BYTES_OUTPUT.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rule_byte_fields)
        writer.writeheader()
        for row in opt_rows:
            header_bytes = int(row["header_bytes"])
            instr_data_bytes = int(row["instr_data_bytes"])
            writer.writerow({
                "sample": row["sample"],
                "instr_index": row["instr_index"],
                "cclass_index": row["cclass_index"],
                "instr_num": row["instr_num"],
                "cclass_num": row["cclass_num"],
                "header_offset_bytes": 0,
                "header_bytes": header_bytes,
                "instr_offset_bytes": header_bytes,
                "instr_bytes_per_entry": 8,
                "instr_data_bytes": row["instr_data_bytes"],
                "cclass_offset_bytes": header_bytes + instr_data_bytes,
                "cclass_bytes_per_entry": 32,
                "cclass_data_bytes": row["cclass_data_bytes"],
                "original_rule_bytes": row["original_rule_bytes"],
            })

    print("PASS combined instruction/cclass RTL matrix: 256 samples")
    print("Cycle CSV: " + str(OUTPUT))
    print("Rule-byte CSV: " + str(RULE_BYTES_OUTPUT))


if __name__ == "__main__":
    main()
