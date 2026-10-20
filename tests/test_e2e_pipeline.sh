#!/usr/bin/env bash
# Path C E2E on Linux: lower → TCC ARM64 ELF .o → link → sign → deb
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BRIDGE="$ROOT/DyLabApp/Bridge"
SDK="$ROOT/DyLabApp/Resources/SDK/include"
TCC_LIBDIR="${TCC_LIBDIR:-/tmp/tcc-arm64-lib}"
OUT="${TMPDIR:-/tmp}/dylab-e2e-$$"
mkdir -p "$OUT"
cleanup() { rm -rf "$OUT"; }
trap cleanup EXIT

CXX="${CXX:-g++}"
CC="${CC:-gcc}"

echo "==> Tools"
"$CXX" -std=c++17 -O2 -I"$BRIDGE" -o "$OUT/lower_tool" \
  "$ROOT/tests/tools/lower_main.cpp" "$BRIDGE/ObjCLowering.cpp"
"$CXX" -std=c++17 -O2 -I"$BRIDGE" -o "$OUT/link_pack" \
  "$ROOT/tests/tools/link_pack_main.cpp" \
  "$BRIDGE/MachODylibLinker.cpp" "$BRIDGE/MachOSigner.cpp" "$BRIDGE/DebPackager.c" -lz

HAVE_TCC=0
if [[ -f "$TCC_LIBDIR/libtcc.a" && -f "$TCC_LIBDIR/libtcc.h" ]]; then
  "$CC" -O2 -I"$TCC_LIBDIR" -o "$OUT/tcc_compile" \
    "$ROOT/tests/tools/tcc_compile_main.c" -L"$TCC_LIBDIR" -ltcc -ldl -lm
  HAVE_TCC=1
fi

run_one() {
  local name="$1"
  local src="$2"
  local work="$OUT/$name"
  mkdir -p "$work"
  echo "==> [$name] ObjCLowering"
  "$OUT/lower_tool" "$src" "$work/Tweak.lowered.c"

  # Must not introduce a conflicting non-inline prototype
  if grep -E '^(const void \*|static id|id) __dylab_cfstring' "$work/Tweak.lowered.c" \
      | grep -v 'static inline id __dylab_cfstring'; then
    echo "FAIL [$name]: incompatible __dylab_cfstring declaration in lowered output" >&2
    return 1
  fi
  # Foundation include must be present (defines the helper)
  if ! grep -q 'Foundation/Foundation.h' "$work/Tweak.lowered.c"; then
    echo "FAIL [$name]: missing Foundation include" >&2
    return 1
  fi
  # Quoted companion headers (TweakClasses.h) must be inlined — TinyCC cannot
  # parse raw @interface from a leftover #include "…".
  if grep -E '#include[[:space:]]+"[^"]+"' "$work/Tweak.lowered.c"; then
    echo "FAIL [$name]: leftover quoted #include in lowered output (should be inlined)" >&2
    return 1
  fi
  if grep -E '@interface|@implementation' "$work/Tweak.lowered.c"; then
    echo "FAIL [$name]: raw ObjC still present in lowered output" >&2
    return 1
  fi
  echo "  lowered OK"

  if [[ "$HAVE_TCC" -eq 1 ]]; then
    echo "==> [$name] TinyCC → ELF .o"
    if ! "$OUT/tcc_compile" "$work/Tweak.lowered.c" "$SDK" "$work/Tweak.o" 2>"$work/tcc.err"; then
      echo "FAIL [$name] tcc:" >&2
      cat "$work/tcc.err" >&2
      head -40 "$work/Tweak.lowered.c" >&2
      return 1
    fi
    file "$work/Tweak.o"
    echo "==> [$name] MachO link + sign + deb"
    if ! "$OUT/link_pack" "$work/Tweak.o" "$work/${name}.dylib" "$work/${name}.deb" 2>"$work/link.err"; then
      echo "FAIL [$name] link/pack:" >&2
      cat "$work/link.err" >&2
      return 1
    fi
    python3 - <<PY
import sys
p="$work/${name}.dylib"
with open(p,"rb") as f: mag=f.read(4)
assert mag==bytes([0xcf,0xfa,0xed,0xfe]), mag.hex()
p2="$work/${name}.deb"
with open(p2,"rb") as f: assert f.read(8)==b"!<arch>\\n"
print("  macho+deb magic OK")
PY
  else
    echo "==> [$name] SKIP tcc/link (set TCC_LIBDIR)"
  fi
  echo "PASS [$name]"
}

run_one sample_class "$ROOT/tests/fixtures/sample_class_tweak.m"
run_one constructor_popup "$ROOT/tests/fixtures/constructor_popup_tweak.m"
# Matches UI "Generate Source" output: Tweak.m + TweakClasses.h side by side
run_one generated_skeleton "$ROOT/tests/fixtures/generated_skeleton/Tweak.m"
echo "ALL E2E PASS"
