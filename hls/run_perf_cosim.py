#!/usr/bin/env python3
"""Drive the synthesized persistent kernel with the performance RTL bench."""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys


def run_logged(command, cwd, log_path, timeout):
    print("Running " + " ".join(str(item) for item in command), flush=True)
    with log_path.open("w") as log:
        subprocess.run(command, cwd=str(cwd), stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default="re_engine_perf")
    parser.add_argument("--solution", default="perf")
    args = parser.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    root = here.parent
    rtl = here / "build" / args.project / args.solution / "syn" / "verilog"
    work = here / "build" / ("perf_cosim_" + args.project)
    work.mkdir(parents=True, exist_ok=True)

    top_rtl = rtl / "reEngineKernel.v"
    adapter_path = rtl / "reEngineKernel_control_s_axi.v"
    if not top_rtl.exists() or not adapter_path.exists():
        sys.exit("Missing csynth RTL under " + str(rtl))

    # Fail early if a tool-version change moved the MMIO windows used by the
    # testbench. The diagnostic AXIS output must not affect this map.
    adapter = adapter_path.read_text()
    expected_addresses = {
        "ADDR_AP_CTRL": 0x00,
        "ADDR_CFG_OUT_DATA_0": 0x10,
        "ADDR_RESULT_IN_BASE": 0x18,
        "ADDR_CFG_IN_BASE": 0x20,
        "ADDR_RESULT_OUT_BASE": 0x80,
    }
    for name, expected in expected_addresses.items():
        match = re.search(name + r"\s*=\s*8'h([0-9a-fA-F]+)", adapter)
        if not match or int(match.group(1), 16) != expected:
            sys.exit("Unexpected generated register map for " + name)

    for data_file in rtl.glob("*.dat"):
        shutil.copy2(str(data_file), str(work / data_file.name))
    project_lines = [
        'verilog xil_defaultlib "' + source.as_posix() + '"'
        for source in sorted(rtl.glob("*.v"))
    ]
    project_lines.append(
        'sv xil_defaultlib "' + (root / "host" / "test_perf.sv").as_posix() + '"')
    (work / "perf_cosim.prj").write_text("\n".join(project_lines) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")

    tools = [
        ("xvlog", ["-prj", "perf_cosim.prj"], 600),
        ("xelab", ["xil_defaultlib.test_perf", "-s", "re_perf_cosim",
                   "-mt", "2"], 600),
        ("xsim", ["re_perf_cosim", "-tclbatch", "run.tcl"], 7200),
    ]
    for tool, arguments, timeout in tools:
        executable = shutil.which(tool)
        if not executable:
            sys.exit("Cannot find " + tool +
                     "; source Vivado/Vitis 2021.2 settings first")
        run_logged([executable] + arguments, work,
                   work / (tool + "_console.log"), timeout)

    simulation_log = (work / "xsim_console.log").read_text()
    pass_lines = [line for line in simulation_log.splitlines()
                  if "PASS performance run" in line]
    if not pass_lines or "Fatal:" in simulation_log:
        sys.exit("Performance C/RTL simulation failed; inspect " +
                 str(work / "xsim_console.log"))

    print(pass_lines[-1])
    print("Message results: " + str(work / "perf_message_results.log"))
    print("Engine timing:  " + str(work / "perf_engine.log"))
    print("Workload map:   " + str(work / "perf_workload.log"))


if __name__ == "__main__":
    main()
