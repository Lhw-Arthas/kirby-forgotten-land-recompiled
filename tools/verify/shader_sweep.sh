#!/bin/sh
# Shader sweep over the player's game files (tools/verify/shader_sweep.c): every shader program in the RomFS through
# the translator, the backend's GLSL compile, spirv-val and a pipeline on this PC's driver. Per failure class: stage,
# phase, the instruction or construct, programs affected. Output (report.txt, failures.tsv, fail/) goes to <out>,
# outside the repo: it describes the game's shaders.
# usage: tools/verify/shader_sweep.sh <romfs dir> <out dir> [--no-vk] [--structured]    (Windows / MSYS2 ucrt64)
#        SWEEP_VALIDATION=1: the Khronos validation layer too (slow)
set -e
cd "$(dirname "$0")/../.."
ROMFS=${1:?romfs dir}
OUT=${2:?out dir}
shift 2
mkdir -p "$OUT"
CC=${CC:-clang}
# (the binary goes with its output: a sweep still running keeps its own)
$CC -O2 -w -Iruntime -Iruntime/nvn -D_FILE_OFFSET_BITS=64 tools/verify/shader_sweep.c runtime/nvn/maxwell/translate.c \
    runtime/nvn/vulkan/shaders.c -o "$OUT/shader_sweep" -lvulkan-1 -lshaderc_shared -lSPIRV-Tools-shared -lzstd
for d in /ucrt64/bin /c/msys64/ucrt64/bin; do
  [ -f "$d/VkLayer_khronos_validation.json" ] && export VK_ADD_LAYER_PATH="$(cygpath -w "$d")"
done
"$OUT/shader_sweep" "$ROMFS" "$OUT" "$@"
