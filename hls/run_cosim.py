#!/usr/bin/env python3
"""Run the log-data C-reference/RTL regression on csynth output.

The production kernel is start-once and intentionally never asserts ap_done,
so the transaction-oriented HLS cosim wrapper cannot drive its live AXI-Lite
registers.  This script preserves the same verification flow explicitly:
test_cosim.cpp creates C reference/input vectors, and test_cosim.sv drives the
post-csynth RTL through its real AXI-Lite and AXI4-Stream interfaces.
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys


def run_logged(command, cwd, log_path, env=None, timeout=600):
    print("Running " + " ".join(str(item) for item in command), flush=True)
    with log_path.open("w") as log:
        subprocess.run(command, cwd=str(cwd), stdout=log,
                       stderr=subprocess.STDOUT, check=True, env=env,
                       timeout=timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default="re_engine_cosim")
    parser.add_argument("--solution", default="cosim")
    parser.add_argument("--lines", type=int, default=1000)
    args = parser.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    re_engine = pathlib.Path(
        os.environ.get("REGEX_ENGINE_ROOT", str(here.parent))).resolve()
    host = re_engine / "host"
    re_compile = re_engine / "re_compile"
    rtl = here / "build" / args.project / args.solution / "syn" / "verilog"
    work = here / "build" / ("cosim_" + args.project)
    work.mkdir(parents=True, exist_ok=True)

    top_rtl = rtl / "reEngineKernel.v"
    if not top_rtl.exists():
        sys.exit("Missing csynth RTL: " + str(top_rtl))
    adapter_path = rtl / "reEngineKernel_control_s_axi.v"
    if not adapter_path.exists():
        sys.exit("Missing generated AXI-Lite adapter: " + str(adapter_path))
    adapter = adapter_path.read_text()
    expected_addresses = {
        "ADDR_AP_CTRL": 0x00,
        "ADDR_CFG_OUT_DATA_0": 0x10,
        "ADDR_RESULT_IN_BASE": 0x18,
        "ADDR_CFG_IN_BASE": 0x20,
        "ADDR_RESULT_OUT_BASE": 0x80,
    }
    for name, expected_address in expected_addresses.items():
        match = re.search(name + r"\s*=\s*8'h([0-9a-fA-F]+)", adapter)
        if not match or int(match.group(1), 16) != expected_address:
            sys.exit("Unexpected generated register map for " + name)

    compiler = os.environ.get("CXX", "g++")
    generator = work / "test_cosim_vectors"
    onig_include = re_engine / "third_party" / "oniguruma" / "lib" / "include"
    onig_lib = re_engine / "third_party" / "oniguruma" / "lib" / "lib"
    compile_lib = re_compile / "lib" / "lib"
    compile_command = [
        compiler, "-std=c++14", "-O2",
        str(host / "test_cosim.cpp"),
        "-I" + str(re_engine / "include" / "sw"),
        "-I" + str(onig_include),
        "-I" + str(re_compile / "lib" / "include"),
        "-L" + str(compile_lib),
        "-L" + str(onig_lib),
        "-Wl,-rpath," + str(compile_lib),
        "-Wl,-rpath," + str(onig_lib),
        "-lxfcompile", "-lonig", "-o", str(generator),
    ]
    run_logged(compile_command, work, work / "vector_compile.log")

    generator_env = os.environ.copy()
    old_library_path = generator_env.get("LD_LIBRARY_PATH", "")
    generator_env["LD_LIBRARY_PATH"] = (
        str(compile_lib) + os.pathsep + str(onig_lib) +
        (os.pathsep + old_library_path if old_library_path else ""))
    generator_command = [
        str(generator), "-dir", str(re_engine / "log_data"),
        "-ln", str(args.lines), "-out", str(work),
    ]
    run_logged(generator_command, work, work / "vector_generation.log",
               env=generator_env)

    for data_file in rtl.glob("*.dat"):
        shutil.copy2(str(data_file), str(work / data_file.name))
    project_lines = [
        'verilog xil_defaultlib "' + source.as_posix() + '"'
        for source in sorted(rtl.glob("*.v"))
    ]
    project_lines.append(
        'sv xil_defaultlib "' + (host / "test_cosim.sv").as_posix() + '"')
    (work / "cosim.prj").write_text("\n".join(project_lines) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")

    tools = [
        ("xvlog", ["-prj", "cosim.prj"], 600),
        ("xelab", ["xil_defaultlib.test_cosim", "-s", "re_cosim",
                    "-mt", "2"], 600),
        ("xsim", ["re_cosim", "-tclbatch", "run.tcl"], 3600),
    ]
    for tool, tool_args, timeout in tools:
        executable = shutil.which(tool)
        if not executable:
            sys.exit("Cannot find " + tool +
                     "; source Vivado/Vitis 2021.2 settings first")
        run_logged([executable] + tool_args, work,
                   work / (tool + "_console.log"), timeout=timeout)

    simulation_log = (work / "xsim_console.log").read_text()
    pass_lines = [line for line in simulation_log.splitlines()
                  if "PASS C/RTL log regression" in line]
    if not pass_lines or "Fatal:" in simulation_log:
        sys.exit("C/RTL regression failed; inspect " +
                 str(work / "xsim_console.log"))
    print(pass_lines[-1])


if __name__ == "__main__":
    main()
