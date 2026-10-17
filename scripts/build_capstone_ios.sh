#!/usr/bin/env bash
# Build Capstone static library for iOS arm64 into Deps/capstone-ios.
# Uses Capstone's Makefile / make.sh ios path (CMake iOS cross ignores/overwrites flags).
# Intended for macOS GitHub Actions runners with Xcode.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/Deps/capstone-ios"
CAPSTONE_SRC="${ROOT_DIR}/Deps/capstone"

mkdir -p "${DEPS_DIR}/lib" "${DEPS_DIR}/include"

verify_arm64_lib() {
  local lib="$1"
  if [[ ! -f "${lib}" ]]; then
    return 1
  fi
  local info
  info="$(lipo -info "${lib}" 2>&1 || true)"
  echo "lipo -info ${lib}: ${info}"
  echo "${info}" | grep -q 'arm64'
}

if [[ -f "${DEPS_DIR}/lib/libcapstone.a" && -d "${DEPS_DIR}/include/capstone" ]]; then
  if verify_arm64_lib "${DEPS_DIR}/lib/libcapstone.a"; then
    echo "==> Capstone iOS arm64 artifacts already present at ${DEPS_DIR} (cache hit)"
    ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include/capstone" | head -40
    exit 0
  fi
  echo "==> Cache present but not arm64 — rebuilding"
  rm -f "${DEPS_DIR}/lib/libcapstone.a"
  rm -rf "${DEPS_DIR}/include/capstone"
fi

if [[ ! -d "${CAPSTONE_SRC}/.git" ]]; then
  echo "==> Cloning capstone v5 (depth 1)"
  rm -rf "${CAPSTONE_SRC}"
  git clone --depth 1 --branch v5 https://github.com/capstone-engine/capstone.git "${CAPSTONE_SRC}"
else
  echo "==> Reusing Capstone checkout at ${CAPSTONE_SRC}"
fi

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
export CC="$(xcrun --sdk iphoneos -f clang)"
# Capstone Makefile on Darwin adds -arch from LIBARCHS when MACOS_UNIVERSAL=yes
# (see make.sh build_iOS / ios_arm64). Also pass target + sysroot explicitly.
export CFLAGS="-arch arm64 -isysroot ${IPHONEOS_SDK} -miphoneos-version-min=16.0 -target arm64-apple-ios16.0 -Os"
export LDFLAGS="-arch arm64 -isysroot ${IPHONEOS_SDK} -miphoneos-version-min=16.0 -target arm64-apple-ios16.0"
export LIBARCHS="arm64"
export MACOS_UNIVERSAL="yes"
export CAPSTONE_ARCHS="aarch64"
export CAPSTONE_SHARED="no"
export CAPSTONE_STATIC="yes"
export CAPSTONE_BUILD_CORE_ONLY="yes"

echo "==> iPhoneOS SDK: ${IPHONEOS_SDK}"
echo "==> CC: ${CC}"
echo "==> CFLAGS: ${CFLAGS}"

cd "${CAPSTONE_SRC}"

echo "==> Cleaning Capstone"
make clean || true
rm -f libcapstone.a libcapstone.*.dylib libcapstone.dylib 2>/dev/null || true

echo "==> Building Capstone static (aarch64 / ios arm64) via Makefile"
# Official Capstone path: ./make.sh ios_arm64 — replicate with our arch/shared flags.
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
make -j"${NCPU}" \
  CAPSTONE_ARCHS=aarch64 \
  CAPSTONE_SHARED=no \
  CAPSTONE_STATIC=yes \
  CAPSTONE_BUILD_CORE_ONLY=yes \
  MACOS_UNIVERSAL=yes \
  LIBARCHS=arm64 \
  CC="${CC}" \
  CFLAGS="${CFLAGS}" \
  LDFLAGS="${LDFLAGS}"

FOUND_LIB=""
for candidate in \

  ; do
  if [[ -f "${candidate}" ]]; then
    FOUND_LIB="${candidate}"
    break
  fi
done
# Also search under CAPSTONE_SRC
if [[ -z "${FOUND_LIB}" ]]; then
  FOUND_LIB="$(find "${CAPSTONE_SRC}" -maxdepth 2 -name 'libcapstone.a' -print -quit || true)"
fi

if [[ -z "${FOUND_LIB}" || ! -f "${FOUND_LIB}" ]]; then
  echo "ERROR: libcapstone.a not found after make" >&2
  ls -la "${CAPSTONE_SRC}" | head -40 >&2
  exit 1
fi

echo "==> Verifying architecture of ${FOUND_LIB}"
if ! verify_arm64_lib "${FOUND_LIB}"; then
  echo "ERROR: expected arm64 in libcapstone.a" >&2
  # Show first object arch for debugging
  otool -hv "${FOUND_LIB}" 2>&1 | head -40 || true
  exit 1
fi

# Confirm a compile line would have used arm64 (dump one .o if present)
SAMPLE_OBJ="$(find "${CAPSTONE_SRC}" -name 'cs.o' -print -quit 2>/dev/null || true)"
if [[ -n "${SAMPLE_OBJ}" ]]; then
  echo "==> Sample object: $(lipo -info "${SAMPLE_OBJ}" 2>&1 || file "${SAMPLE_OBJ}")"
fi

echo "==> Staging ${FOUND_LIB} -> ${DEPS_DIR}/lib/libcapstone.a"
cp "${FOUND_LIB}" "${DEPS_DIR}/lib/libcapstone.a"

rm -rf "${DEPS_DIR}/include/capstone"
mkdir -p "${DEPS_DIR}/include"
if [[ -d "${CAPSTONE_SRC}/include/capstone" ]]; then
  cp -R "${CAPSTONE_SRC}/include/capstone" "${DEPS_DIR}/include/"
else
  echo "ERROR: capstone headers not found" >&2
  exit 1
fi

if ! verify_arm64_lib "${DEPS_DIR}/lib/libcapstone.a"; then
  echo "ERROR: staged libcapstone.a is not arm64" >&2
  exit 1
fi

echo "==> Capstone iOS artifacts ready in ${DEPS_DIR}"
ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include/capstone" | head -40
