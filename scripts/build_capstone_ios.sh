#!/usr/bin/env bash
# Build Capstone static library for iOS arm64 into Deps/capstone-ios.
# Intended for macOS GitHub Actions runners with Xcode / CMake.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/Deps/capstone-ios"
CAPSTONE_SRC="${ROOT_DIR}/Deps/capstone"
BUILD_DIR="${CAPSTONE_SRC}/build-ios"

mkdir -p "${DEPS_DIR}/lib" "${DEPS_DIR}/include"

if [[ -f "${DEPS_DIR}/lib/libcapstone.a" && -d "${DEPS_DIR}/include/capstone" ]]; then
  echo "==> Capstone iOS artifacts already present at ${DEPS_DIR} (cache hit)"
  ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include/capstone" | head -40
  exit 0
fi

if [[ ! -d "${CAPSTONE_SRC}/.git" ]]; then
  echo "==> Cloning capstone v5 (depth 1)"
  rm -rf "${CAPSTONE_SRC}"
  git clone --depth 1 --branch v5 https://github.com/capstone-engine/capstone.git "${CAPSTONE_SRC}"
else
  echo "==> Reusing Capstone checkout at ${CAPSTONE_SRC}"
fi

echo "==> Configuring Capstone for iOS arm64"
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
# Capstone v5 CPackConfig.txt does `if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")`
# which fails when CMAKE_SYSTEM_PROCESSOR is empty under iOS cross-compile.
# Capstone v5 uses CAPSTONE_ARM64_SUPPORT (not AARCH64).
cmake -S "${CAPSTONE_SRC}" -B "${BUILD_DIR}" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_SYSTEM_PROCESSOR=arm64 \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPACK_GENERATOR= \
  -DBUILD_SHARED_LIBS=OFF \
  -DCAPSTONE_BUILD_SHARED=OFF \
  -DCAPSTONE_BUILD_STATIC=ON \
  -DCAPSTONE_BUILD_TESTS=OFF \
  -DCAPSTONE_BUILD_CSTOOL=OFF \
  -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF \
  -DCAPSTONE_ARM_SUPPORT=OFF \
  -DCAPSTONE_ARM64_SUPPORT=ON \
  -DCAPSTONE_MIPS_SUPPORT=OFF \
  -DCAPSTONE_PPC_SUPPORT=OFF \
  -DCAPSTONE_SPARC_SUPPORT=OFF \
  -DCAPSTONE_SYSZ_SUPPORT=OFF \
  -DCAPSTONE_X86_SUPPORT=OFF \
  -DCAPSTONE_XCORE_SUPPORT=OFF \
  -DCAPSTONE_M68K_SUPPORT=OFF \
  -DCAPSTONE_TMS320C64X_SUPPORT=OFF \
  -DCAPSTONE_M680X_SUPPORT=OFF \
  -DCAPSTONE_EVM_SUPPORT=OFF \
  -DCAPSTONE_MOS65XX_SUPPORT=OFF \
  -DCAPSTONE_WASM_SUPPORT=OFF \
  -DCAPSTONE_BPF_SUPPORT=OFF \
  -DCAPSTONE_RISCV_SUPPORT=OFF \
  -DCAPSTONE_SH_SUPPORT=OFF \
  -DCAPSTONE_TRICORE_SUPPORT=OFF

echo "==> Building libcapstone"
cmake --build "${BUILD_DIR}" --config Release -j "$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

LIB_CANDIDATES=(
  "${BUILD_DIR}/libcapstone.a"
  "${BUILD_DIR}/Release/libcapstone.a"
)

FOUND_LIB=""
for candidate in "${LIB_CANDIDATES[@]}"; do
  if [[ -f "${candidate}" ]]; then
    FOUND_LIB="${candidate}"
    break
  fi
done

if [[ -z "${FOUND_LIB}" ]]; then
  echo "ERROR: libcapstone.a not found under ${BUILD_DIR}" >&2
  find "${BUILD_DIR}" -name 'libcapstone*' -print >&2 || true
  exit 1
fi

echo "==> Staging ${FOUND_LIB} -> ${DEPS_DIR}/lib/libcapstone.a"
cp "${FOUND_LIB}" "${DEPS_DIR}/lib/libcapstone.a"

rm -rf "${DEPS_DIR}/include/capstone"
mkdir -p "${DEPS_DIR}/include"
if [[ -d "${CAPSTONE_SRC}/include/capstone" ]]; then
  cp -R "${CAPSTONE_SRC}/include/capstone" "${DEPS_DIR}/include/"
elif [[ -d "${BUILD_DIR}/include/capstone" ]]; then
  cp -R "${BUILD_DIR}/include/capstone" "${DEPS_DIR}/include/"
else
  echo "ERROR: capstone headers not found" >&2
  exit 1
fi

echo "==> Capstone iOS artifacts ready in ${DEPS_DIR}"
ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include/capstone" | head -40
