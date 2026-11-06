#!/usr/bin/env python3
"""Run both independent RTL sweeps and write one 200-point comparison CSV."""

import csv
import pathlib
import subprocess
import sys


HERE = pathlib.Path(__file__).resolve().parent
OPT_RUNNER = HERE / "run_opt_perf_rtl.py"
NEW_RUNNER = HERE / "run_new_perf_rtl.py"
OPT_CSV = HERE / "build/regex_vm_opt_rtl_perf/regex_vm_opt_cycles.csv"
NEW_CSV = HERE / "build/regex_vm_new_rtl_perf/regex_vm_new_cycles.csv"
OUTPUT_DIR = HERE / "results"
OUTPUT_CSV = OUTPUT_DIR / "regex_vm_cycles.csv"


def run(script):
    subprocess.run([sys.executable, str(script)], cwd=str(HERE.parent.parent),
                   check=True)


def read_rows(path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def main():
    # Run independently so neither DUT's reset, idle, or execution time enters
    # the other DUT's start-to-done cycle count.
    run(OPT_RUNNER)
    run(NEW_RUNNER)

    opt_rows = read_rows(OPT_CSV)
    new_rows = read_rows(NEW_CSV)
    if len(opt_rows) != 200 or len(new_rows) != 200:
        sys.exit("Expected 200 rows from each independent RTL sweep")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    fields = ["sample", "rule_instructions", "rule_bytes", "message_bytes",
              "regexVM_opt_cycles", "regexVM_new_cycles"]
    with OUTPUT_CSV.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for opt, new in zip(opt_rows, new_rows):
            keys = ("sample", "rule_instructions", "rule_bytes", "message_bytes")
            if any(opt[key] != new[key] for key in keys):
                sys.exit("Sweep coordinates differ between opt and new")
            writer.writerow({
                "sample": opt["sample"],
                "rule_instructions": opt["rule_instructions"],
                "rule_bytes": opt["rule_bytes"],
                "message_bytes": opt["message_bytes"],
                "regexVM_opt_cycles": opt["regexVM_opt_cycles"],
                "regexVM_new_cycles": new["regexVM_new_cycles"],
            })

    print("PASS combined regex VM RTL sweep: 200 samples")
    print("CSV: " + str(OUTPUT_CSV))


if __name__ == "__main__":
    main()
