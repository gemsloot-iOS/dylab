#!/usr/bin/env bash
# Build WAMR (iwasm) static library for iOS arm64 and stage into Deps/wamr-ios.
# Intended for macOS GitHub Actions runners with Xcode / CMake available.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/Deps/wamr-ios"
WAMR_SRC="${ROOT_DIR}/Deps/wasm-micro-runtime"
BUILD_DIR="${WAMR_SRC}/product-mini/platforms/darwin/build-ios"

mkdir -p "${DEPS_DIR}/lib" "${DEPS_DIR}/include"

if [[ ! -d "${WAMR_SRC}/.git" ]]; then
  echo "==> Cloning wasm-micro-runtime (depth 1)"
  rm -rf "${WAMR_SRC}"
  git clone --depth 1 https://github.com/bytecodealliance/wasm-micro-runtime.git "${WAMR_SRC}"
else
  echo "==> Reusing existing WAMR checkout at ${WAMR_SRC}"
fi

echo "==> Configuring WAMR for iOS arm64"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cmake -S "${WAMR_SRC}/product-mini/platforms/darwin" -B "${BUILD_DIR}" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DWAMR_BUILD_TARGET=AARCH64 \
  -DWAMR_BUILD_INTERP=1 \
  -DWAMR_BUILD_FAST_INTERP=1 \
  -DWAMR_BUILD_AOT=0 \
  -DWAMR_BUILD_JIT=0 \
  -DWAMR_BUILD_LIBC_WASI=1 \
  -DWAMR_BUILD_LIBC_BUILTIN=1

echo "==> Building libiwasm"
cmake --build "${BUILD_DIR}" --config Release -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

LIB_CANDIDATES=(
  "${BUILD_DIR}/libiwasm.a"
  "${BUILD_DIR}/Release/libiwasm.a"
  "${BUILD_DIR}/libiwasm.dylib"
)

FOUND_LIB=""
for candidate in "${LIB_CANDIDATES[@]}"; do
  if [[ -f "${candidate}" ]]; then
    FOUND_LIB="${candidate}"
    break
  fi
done

if [[ -z "${FOUND_LIB}" ]]; then
  echo "ERROR: libiwasm not found under ${BUILD_DIR}" >&2
  find "${BUILD_DIR}" -name 'libiwasm*' -print >&2 || true
  exit 1
fi

echo "==> Staging ${FOUND_LIB} -> ${DEPS_DIR}/lib/libiwasm.a"
cp "${FOUND_LIB}" "${DEPS_DIR}/lib/libiwasm.a"

HEADER_ROOT="${WAMR_SRC}/core/iwasm/include"
for header in wasm_export.h lib_export.h; do
  if [[ ! -f "${HEADER_ROOT}/${header}" ]]; then
    echo "ERROR: missing header ${HEADER_ROOT}/${header}" >&2
    exit 1
  fi
  cp "${HEADER_ROOT}/${header}" "${DEPS_DIR}/include/${header}"
done

echo "==> WAMR iOS artifacts ready in ${DEPS_DIR}"
ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include"
