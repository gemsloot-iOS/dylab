#!/usr/bin/env bash
# Build TinyCC (libtcc.a) for iOS arm64 — ARM64 ELF codegen (no Mach-O target).
# Path C: libtcc emits ELF64 .o; DyLab's MachODylibLinker produces MH_DYLIB.
# Intended for macOS GitHub Actions runners with Xcode.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT_DIR}/Deps/tcc-ios"
TCC_SRC="${ROOT_DIR}/Deps/tinycc"

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

if [[ -f "${DEPS_DIR}/lib/libtcc.a" && -f "${DEPS_DIR}/include/libtcc.h" ]]; then
  if verify_arm64_lib "${DEPS_DIR}/lib/libtcc.a"; then
    echo "==> TinyCC iOS arm64 artifacts already present at ${DEPS_DIR} (cache hit)"
    ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include" | head -40
    exit 0
  fi
  echo "==> Cache present but not arm64 — rebuilding"
  rm -f "${DEPS_DIR}/lib/libtcc.a"
fi

if [[ ! -d "${TCC_SRC}/.git" ]]; then
  echo "==> Cloning TinyCC (mob)"
  rm -rf "${TCC_SRC}"
  # Prefer GitHub mirror; fall back to repo.or.cz
  if ! git clone --depth 1 --branch mob https://github.com/TinyCC/tinycc.git "${TCC_SRC}"; then
    git clone --depth 1 git://repo.or.cz/tinycc.git "${TCC_SRC}"
  fi
else
  echo "==> Reusing TinyCC checkout at ${TCC_SRC}"
fi

IPHONEOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC_BIN="$(xcrun --sdk iphoneos -f clang)"
AR_BIN="$(xcrun --sdk iphoneos -f ar)"
IOS_MIN="16.0"
TARGET="arm64-apple-ios${IOS_MIN}"

echo "==> iPhoneOS SDK: ${IPHONEOS_SDK}"
echo "==> CC: ${CC_BIN}"

cd "${TCC_SRC}"

# Host configure just to generate tccdefs_.h / version; we override target macros.
if [[ ! -f config.h ]]; then
  echo "==> Running ./configure (host) for tccdefs_.h"
  ./configure --prefix=/tmp/tcc-ios-host --disable-static || ./configure
fi
if [[ ! -f tccdefs_.h ]]; then
  echo "==> Generating tccdefs_.h"
  make tccdefs_.h
fi

VERSION="$(tr -d '\n' < VERSION 2>/dev/null || echo 0.9.28)"
cat > "${TCC_SRC}/config-ios.h" <<EOF
/* DyLab iOS staging config — ARM64 ELF codegen for libtcc on device */
#define TCC_VERSION "${VERSION}"
#define CONFIG_TCC_PREDEFS 1
#ifndef CONFIG_TCCDIR
#define CONFIG_TCCDIR "/usr/local/lib/tcc"
#endif
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_BACKTRACE 0
EOF

CFLAGS=(
  -arch arm64
  -isysroot "${IPHONEOS_SDK}"
  -miphoneos-version-min="${IOS_MIN}"
  -target "${TARGET}"
  -Os
  -fPIC
  -Wall
  -Wno-unused-parameter
  -Wno-unused-function
  -Wno-missing-field-initializers
  -I"${TCC_SRC}"
  -include "${TCC_SRC}/config-ios.h"
  -DONE_SOURCE=1
  -DTCC_TARGET_ARM64=1
  -DCONFIG_TCC_PREDEFS=1
  -DCONFIG_TCC_BCHECK=0
  -DCONFIG_TCC_BACKTRACE=0
)

echo "==> Compiling libtcc.c (ONE_SOURCE, TCC_TARGET_ARM64 ELF)"
rm -f "${TCC_SRC}/libtcc-ios.o"
"${CC_BIN}" "${CFLAGS[@]}" -c "${TCC_SRC}/libtcc.c" -o "${TCC_SRC}/libtcc-ios.o"

echo "==> Archiving libtcc.a"
rm -f "${DEPS_DIR}/lib/libtcc.a"
"${AR_BIN}" rcs "${DEPS_DIR}/lib/libtcc.a" "${TCC_SRC}/libtcc-ios.o"

echo "==> Staging headers"
cp -f "${TCC_SRC}/libtcc.h" "${DEPS_DIR}/include/libtcc.h"
# Minimal tcclib helpers some tumbleweed code may include
if [[ -f "${TCC_SRC}/tcclib.h" ]]; then
  cp -f "${TCC_SRC}/tcclib.h" "${DEPS_DIR}/include/tcclib.h"
fi

if ! verify_arm64_lib "${DEPS_DIR}/lib/libtcc.a"; then
  echo "ERROR: libtcc.a is not arm64" >&2
  exit 1
fi

echo "==> TinyCC iOS artifacts ready:"
ls -la "${DEPS_DIR}/lib" "${DEPS_DIR}/include"
echo "OK"
