#!/usr/bin/env python3
"""Run the start-once production RTL smoke test after HLS synthesis.

Requires Python 3 and the Vivado 2021.2 xvlog/xelab/xsim tools on PATH.
No implementation/place-and-route or board is needed. Each tool is time-bounded.
"""
import pathlib
import os
import shutil
import subprocess
import sys

here = pathlib.Path(__file__).resolve().parent
project_name = os.environ.get("RE_HLS_RTL_PROJECT", "re_engine")
rtl = here / "build" / project_name / "smoke/syn/verilog"
work_suffix = os.environ.get("RE_HLS_RTL_WORK_SUFFIX", "")
work = here / "build" / ("mmio_" + project_name + work_suffix)
if not (rtl / "reEngineKernel.v").exists():
    sys.exit("Synthesize the selected RE_HLS_RTL_PROJECT first")
adapter = (rtl / "reEngineKernel_control_s_axi.v").read_text()
for expected in ["ADDR_CFG_IN_BASE", "ADDR_CFG_OUT_DATA_0", "ADDR_RESULT_IN_BASE", "ADDR_RESULT_OUT_BASE"]:
    if expected not in adapter:
        sys.exit("Unexpected AXI-Lite adapter; review tb_mmio.sv against the generated register map")
work.mkdir(parents=True, exist_ok=True)
for data in rtl.glob("*.dat"):
    shutil.copy2(str(data), str(work / data.name))
project = ['verilog xil_defaultlib "' + p.as_posix() + '"' for p in sorted(rtl.glob("*.v"))]
project.append('sv xil_defaultlib "' + (here / "tb_mmio.sv").as_posix() + '"')
(work / "mmio.prj").write_text("\n".join(project) + "\n")
(work / "run.tcl").write_text("run all\nquit\n")
defines = ["-d", "RE_HLS_RTL_QUICK"] if os.environ.get("RE_HLS_RTL_QUICK") == "1" else []
for tool, args in [("xvlog", defines + ["-prj", "mmio.prj"]),
                   ("xelab", ["xil_defaultlib.tb_mmio", "-s", "mmio", "-mt", "2"]),
                   ("xsim", ["mmio", "-tclbatch", "run.tcl"])]:
    exe = shutil.which(tool)
    if not exe:
        sys.exit("Cannot find " + tool + "; source Vivado 2021.2 settings64 first")
    print("Running " + tool, flush=True)
    with (work / (tool + "_console.log")).open("w") as log:
        subprocess.run([exe] + args, cwd=str(work), stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=600)
log = (work / "xsim_console.log").read_text()
if "PASS production RTL" not in log or "Fatal:" in log:
    sys.exit("RTL test failed: inspect " + str(work / "xsim_console.log"))
print(next(line for line in log.splitlines() if "PASS production RTL" in line))
