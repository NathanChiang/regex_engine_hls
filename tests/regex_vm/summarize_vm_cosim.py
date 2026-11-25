#!/usr/bin/env python3
"""Join HLS transaction latencies to the general bytecode regression cases."""

import csv
import json
import pathlib
import re
import statistics
import sys


def main():
    project = pathlib.Path(__file__).resolve().parent / "build" / sys.argv[1]
    with (project / "cases.csv").open(newline="") as handle:
        cases = list(csv.DictReader(handle))
    report = project / "sol/sim/report/verilog/result.transaction.rpt"
    transactions = re.findall(r"transaction\s+(\d+):\s+(\d+)\s+(\d+|x)", report.read_text())
    if len(cases) != len(transactions) or not cases:
        sys.exit("Case and RTL transaction counts differ")
    families = {}
    for case, (index, latency, interval) in zip(cases, transactions):
        if int(case["case_id"]) != int(index) or case["passed"] != "1":
            sys.exit("Unverified or misordered case: " + str(case))
        case["cycles"] = int(latency)
        case["interval"] = int(interval) if interval != "x" else ""
        families.setdefault(case["family"], []).append(case)
    destination = project / "general_rtl_cycles.csv"
    with destination.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(cases[0]))
        writer.writeheader()
        writer.writerows(cases)
    summary = {}
    for family, rows in families.items():
        cycles = [row["cycles"] for row in rows]
        summary[family] = {
            "cases": len(rows),
            "input_bytes_min": min(int(row["bytes"]) for row in rows),
            "input_bytes_max": max(int(row["bytes"]) for row in rows),
            "cycles_min": min(cycles),
            "cycles_median": statistics.median(cycles),
            "cycles_mean": statistics.mean(cycles),
            "cycles_max": max(cycles),
        }
        print("{:<32} {:4d} cases  cycles min/median/mean/max: {:>5}/{:>7.1f}/{:>7.1f}/{:>5}".format(
            family, len(rows), min(cycles), statistics.median(cycles), statistics.mean(cycles), max(cycles)))
    (project / "general_rtl_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("CSV: " + str(destination))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: summarize_vm_cosim.py HLS_PROJECT_NAME")
    main()
