# Standalone regex engine

This directory collects the source, headers, tests, and third-party inputs used
by `reEngineKernel`. Its build scripts resolve paths inside this directory, so
they do not depend on the surrounding `data_analytics` or `utils` trees.

## Layout

- `kernel/` — `reEngineKernel` and its rule/IOVA/streaming implementation.
- `include/hw/` — the merged L1 and L2 `xf_data_analytics/text` headers.
- `include/sw/` — regex compiler and `xf_utils_sw` host headers.
- `host/` — dependency-free HLS regression and optional log/RTL test benches.
- `hls/` — the main VCK190 C simulation, synthesis, RTL test, and export flow.
- `tests/regex_vm/` — the focused Regex VM C simulation and synthesis flows.
- `src/` and `re_compile/` — the software regex compiler source and build.
- `third_party/oniguruma/` — the copied Oniguruma headers and Linux libraries.
- `third_party/xcl2/` — the copied XRT host helper used by the legacy app flow.

Generated HLS projects are intentionally not included. New main-flow outputs go
under `hls/build/`; focused Regex VM outputs go under `tests/regex_vm/build/`.

## Main HLS flow

Vitis HLS 2021.2 and its C++ synthesis headers (`ap_int.h`, `hls_stream.h`, and
`ap_axi_sdata.h`) must be available from the tool installation. From this
directory on Linux, after sourcing the Vitis settings:

```sh
RE_HLS_ACTION=csim vitis_hls -f hls/run_hls.tcl
RE_HLS_ACTION=synth vitis_hls -f hls/run_hls.tcl
RE_HLS_ACTION=synth_only vitis_hls -f hls/run_hls.tcl
RE_HLS_ACTION=export vitis_hls -f hls/run_hls.tcl
RE_HLS_ACTION=perf vitis_hls -f hls/run_hls.tcl # synthesized RTL performance logs
```

The default smoke test and kernel synthesis do not require Oniguruma, OpenCL,
or `libxfcompile`. See `hls/README.md` for exhaustive, log-data, and direct RTL
verification commands.

The focused Regex VM checks are:

```sh
vitis_hls -f tests/regex_vm/run_hls_new_csim.tcl
vitis_hls -f tests/regex_vm/run_hls_new_synth.tcl
```

`REGEX_ENGINE_ROOT` may override the package location, but is normally not
needed because the Tcl and Python entry points derive it from their own paths.

## Optional software reference library

`make -C re_compile` builds `libxfcompile.so`. That compiler uses private
Oniguruma headers, so its original build recipe clones the Oniguruma source
before compiling. The copied prebuilt Oniguruma files under `third_party/` are
Linux artifacts and are used only by the optional log/reference flows.

## Copied-source map

| Package location | Original location |
| --- | --- |
| `kernel/`, `host/`, `hls/`, configs | `data_analytics/L2/tests/text/reEngine` |
| `include/hw/xf_data_analytics/text` | merged `data_analytics/L1/include/hw/.../text` and `L2/include/hw/.../text` |
| `include/sw/xf_utils_sw` | `utils/L1/include/xf_utils_sw` |
| `include/sw/xf_data_analytics/text` | `data_analytics/L1/include/sw/xf_data_analytics/text` |
| `src/xf_re_compile.c` | `data_analytics/L1/src/sw/xf_re_compile.c` |
| `tests/regex_vm` | `data_analytics/L1/tests/text/regex_vm` source inputs |
| `third_party/oniguruma` | `data_analytics/ext/oniguruma` |
| `third_party/xcl2` | `data_analytics/ext/xcl2` |

Only source/build inputs and the required `log_data/access_5k.log` fixture were
copied. Ignored HLS projects, tool logs, and other generated artifacts from the
original trees were excluded.
