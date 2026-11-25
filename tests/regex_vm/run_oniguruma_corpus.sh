#!/usr/bin/env bash
# Host-only Linux regression using the existing compiler/Oniguruma libraries.
set -euo pipefail
regex_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
regex_include=${VITIS_HLS_INCLUDE:-${XILINX_HLS:+$XILINX_HLS/include}}
if [[ -z "$regex_include" || ! -f "$regex_include/ap_int.h" ]]; then
    echo 'Set VITIS_HLS_INCLUDE to the Vitis HLS include directory' >&2
    exit 1
fi
regex_variant=new
regex_flags=()
if [[ ${1:-} == --baseline ]]; then
    regex_variant=opt
    regex_flags+=(-DREGEX_VM_CORPUS_BASELINE)
elif [[ $# -ne 0 ]]; then
    echo 'Usage: run_oniguruma_corpus.sh [--baseline]' >&2
    exit 1
fi
mkdir -p "$regex_root/tests/regex_vm/build"
regex_binary="$regex_root/tests/regex_vm/build/${regex_variant}_oniguruma"
"${CXX:-g++}" -std=c++14 -O2 -D__gmp_const=const -Wno-unknown-pragmas "${regex_flags[@]}" \
    -I"$regex_root/include/hw" -I"$regex_root/include/sw" \
    -I"$regex_root/third_party/oniguruma/lib/include" -I"$regex_include" \
    "$regex_root/tests/regex_vm/regex_vm_new_oniguruma.cpp" \
    -L"$regex_root/re_compile/lib/lib" -L"$regex_root/third_party/oniguruma/lib/lib" \
    -lxfcompile -lonig -Wl,-rpath,"$regex_root/re_compile/lib/lib" \
    -Wl,-rpath,"$regex_root/third_party/oniguruma/lib/lib" -o "$regex_binary"
exec "$regex_binary"
