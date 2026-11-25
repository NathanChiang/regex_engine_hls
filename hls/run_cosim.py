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
import time


def last_log_line(log_path, size):
    if not size:
        return ""
    with log_path.open("rb") as log:
        log.seek(max(0, size - 4096))
        lines = log.read().decode("utf-8", errors="replace").splitlines()
    return lines[-1][-240:] if lines else ""


def run_logged(command, cwd, log_path, env=None, timeout=600,
               heartbeat_seconds=30):
    print("Running " + " ".join(str(item) for item in command), flush=True)
    with log_path.open("w") as log:
        process = subprocess.Popen(command, cwd=str(cwd), stdout=log,
                                   stderr=subprocess.STDOUT, env=env)
        start = time.monotonic()
        while True:
            elapsed = time.monotonic() - start
            remaining = timeout - elapsed
            if remaining <= 0:
                process.kill()
                process.wait()
                raise subprocess.TimeoutExpired(command, timeout)
            try:
                return_code = process.wait(
                    timeout=min(heartbeat_seconds, remaining))
                break
            except subprocess.TimeoutExpired:
                log.flush()
                size = log_path.stat().st_size if log_path.exists() else 0
                elapsed = time.monotonic() - start
                latest = last_log_line(log_path, size)
                latest_text = "; last=" + latest if latest else ""
                print("Still running {}: {:.0f}s elapsed; log={} ({} bytes){}".
                      format(pathlib.Path(str(command[0])).name, elapsed,
                             log_path, size, latest_text), flush=True)
        if return_code:
            raise subprocess.CalledProcessError(return_code, command)
    print("Finished {} in {:.1f}s; log={}".
          format(pathlib.Path(str(command[0])).name,
                 time.monotonic() - start, log_path), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", default="re_engine_cosim")
    parser.add_argument("--solution", default="cosim")
    parser.add_argument("--lines", type=int, default=1000)
    parser.add_argument("--vectors-dir", type=pathlib.Path,
                        help="read existing cosim_input.txt/cosim_expected.txt from this directory")
    parser.add_argument(
        "--reuse-vectors", action="store_true",
        help="reuse cosim_input.txt/cosim_expected.txt already in the work "
             "directory instead of rebuilding the C-reference generator")
    parser.add_argument(
        "--xsim-timeout", type=int, default=0,
        help="xsim wall-clock timeout in seconds; 0 scales with line count")
    args = parser.parse_args()
    if args.lines <= 0:
        parser.error("--lines must be positive")

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

    input_vectors = work / "cosim_input.txt"
    expected_vectors = work / "cosim_expected.txt"
    if args.vectors_dir:
        for target in (input_vectors, expected_vectors):
            source = args.vectors_dir.resolve() / target.name
            if not source.is_file():
                sys.exit("Missing reference vector: " + str(source))
            if source != target.resolve():
                shutil.copy2(source, target)
        args.reuse_vectors = True
    if args.reuse_vectors:
        missing_vectors = [path for path in (input_vectors, expected_vectors)
                           if not path.exists()]
        if missing_vectors:
            sys.exit("Cannot reuse missing C/RTL vectors: " +
                     ", ".join(str(path) for path in missing_vectors))
        try:
            with input_vectors.open() as vectors:
                vector_records = int(vectors.readline().split()[1])
        except (IndexError, ValueError):
            sys.exit("Malformed C/RTL vector header: " + str(input_vectors))
        if vector_records != args.lines:
            sys.exit("Reusable vector count {} does not match --lines {}".
                     format(vector_records, args.lines))
        print("Reusing {} Apache-log C-reference vectors from {}".
              format(vector_records, work), flush=True)
    else:
        compiler = os.environ.get("CXX", "g++")
        generator = work / "test_cosim_vectors"
        onig_include = (re_engine / "third_party" / "oniguruma" / "lib" /
                        "include")
        onig_lib = (re_engine / "third_party" / "oniguruma" / "lib" /
                    "lib")
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

    ip_sources = []
    ip_tcl_files = sorted(rtl.glob("*_ip.tcl"))
    if ip_tcl_files:
        vivado = shutil.which("vivado")
        if not vivado:
            sys.exit("Cannot find vivado to generate HLS IP simulation models; "
                     "source Vivado/Vitis 2021.2 settings first")
        ip_project = work / "generated_ip"
        ip_generation_tcl = work / "generate_ip.tcl"
        ip_tcl_lines = [
            "create_project -force re_engine_cosim_ip \"{}\" -part "
            "xcvc1902-vsva2197-2MP-e-S".format(ip_project.as_posix()),
            "set_property target_language Verilog [current_project]",
            "set_property target_simulator XSim [current_project]",
        ]
        ip_tcl_lines.extend(
            "source \"{}\"".format(source.as_posix())
            for source in ip_tcl_files)
        ip_tcl_lines.extend([
            "generate_target simulation [get_ips]",
            "exit",
        ])
        ip_generation_tcl.write_text("\n".join(ip_tcl_lines) + "\n")
        run_logged([vivado, "-mode", "batch", "-source",
                    str(ip_generation_tcl)], work,
                   work / "ip_generation.log", timeout=600)
        for source_tcl in ip_tcl_files:
            matches = [
                path for extension in (".v", ".vhd", ".vhdl")
                for path in ip_project.rglob(source_tcl.stem + extension)
                if "sim" in path.parts
            ]
            if len(matches) != 1:
                sys.exit("Expected one generated simulation model for {}, "
                         "found {} under {}".format(
                             source_tcl.name, len(matches), ip_project))
            ip_sources.append(matches[0])

    vhdl_sources = [source for source in ip_sources
                    if source.suffix.lower() in (".vhd", ".vhdl")]
    verilog_ip_sources = [source for source in ip_sources
                          if source.suffix.lower() == ".v"]
    if vhdl_sources:
        vhdl_project_lines = [
            'vhdl xil_defaultlib "' + source.as_posix() + '"'
            for source in vhdl_sources
        ]
        (work / "cosim_vhdl.prj").write_text(
            "\n".join(vhdl_project_lines) + "\n")

    project_lines = [
        'verilog xil_defaultlib "' + source.as_posix() + '"'
        for source in verilog_ip_sources
    ] + [
        'verilog xil_defaultlib "' + source.as_posix() + '"'
        for source in sorted(rtl.glob("*.v"))
    ]
    project_lines.append(
        'sv xil_defaultlib "' + (host / "test_cosim.sv").as_posix() + '"')
    (work / "cosim.prj").write_text("\n".join(project_lines) + "\n")
    (work / "run.tcl").write_text("run all\nquit\n")

    # Large log regressions still depend on host speed.  Keep the historical
    # one-hour minimum, but do not kill a valid 5k run merely because it takes
    # longer.  The SystemVerilog cycle watchdog remains the deadlock guard.
    xsim_timeout = (args.xsim_timeout if args.xsim_timeout > 0 else
                    max(3600, args.lines))
    tools = []
    if vhdl_sources:
        tools.append(("xvhdl", ["-prj", "cosim_vhdl.prj"], 600))
    tools.extend([
        ("xvlog", ["-prj", "cosim.prj"], 600),
        ("xelab", ["xil_defaultlib.test_cosim", "-s", "re_cosim",
                    "-mt", "2"], 600),
        ("xsim", ["re_cosim", "-tclbatch", "run.tcl"], xsim_timeout),
    ])
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
    cycle_report = work / "re_engine_kernel_cycles.csv"
    if not cycle_report.exists():
        sys.exit("C/RTL regression did not create cycle report: " +
                 str(cycle_report))
    stall_report = work / "re_engine_kernel_stalls.csv"
    if not stall_report.exists():
        sys.exit("C/RTL regression did not create stall report: " +
                 str(stall_report))
    with (work / "cosim_input.txt").open() as vectors:
        expected_records = int(vectors.readline().split()[1])
    with cycle_report.open() as report:
        cycle_records = sum(1 for _ in report) - 1
    with stall_report.open() as report:
        stall_records = sum(1 for _ in report) - 1
    if cycle_records != expected_records or stall_records != expected_records:
        sys.exit("Incomplete C/RTL reports: expected={} cycles={} stalls={}".
                 format(expected_records, cycle_records, stall_records))
    print(pass_lines[-1])
    print("Cycle report: " + str(cycle_report))
    print("Stall report: " + str(stall_report))


if __name__ == "__main__":
    main()
