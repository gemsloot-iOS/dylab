#!/usr/bin/env bash
# Build SampleTweak.dylib (iphoneos arm64) and package sample_tweak.deb into
# DyLabApp/Resources/. Intended for macOS GitHub Actions (macos-15) runners.
#
# Uses a portable short-name ar writer so member names are always
# "debian-binary", "control.tar.gz", "data.tar.gz" — not BSD "#1/<len>".
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/DyLabApp/Resources"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/dylab-sample-deb.XXXXXX")"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

mkdir -p "${OUT_DIR}" "${WORK}/src" "${WORK}/pkg/Library/MobileSubstrate/DynamicLibraries" "${WORK}/debian"

cat > "${WORK}/src/SampleTweak.m" <<'EOF'
#import <Foundation/Foundation.h>

@interface SampleTweak : NSObject
+ (NSString *)greeting;
@end

@implementation SampleTweak
+ (NSString *)greeting {
    return @"Hello from SampleTweak";
}
@end

__attribute__((visibility("default")))
const char *sample_tweak_version(void) {
    return "1.0.0";
}
EOF

echo "==> Compiling SampleTweak.dylib (iphoneos arm64)"
xcrun -sdk iphoneos clang -arch arm64 \
  -dynamiclib \
  -fobjc-arc \
  -framework Foundation \
  -install_name "@rpath/SampleTweak.dylib" \
  -o "${WORK}/pkg/Library/MobileSubstrate/DynamicLibraries/SampleTweak.dylib" \
  "${WORK}/src/SampleTweak.m"

cat > "${WORK}/debian/control" <<'EOF'
Package: com.dylab.sampletweak
Name: SampleTweak
Version: 1.0.0
Architecture: iphoneos-arm
Description: DyLab sample tweak dylib for Mach-O inspection
Maintainer: DyLab <dylab@example.com>
Section: Tweaks
EOF

echo "==> Creating data.tar.gz"
(
  cd "${WORK}/pkg"
  tar -czf "${WORK}/data.tar.gz" .
)

echo "==> Creating control.tar.gz"
(
  cd "${WORK}/debian"
  tar -czf "${WORK}/control.tar.gz" control
)

echo "2.0" > "${WORK}/debian-binary"

# Portable ar with short names (avoids macOS BSD "#1/<len>" extended names).
write_ar_short() {
  local out="$1"
  shift
  python3 - "$out" "$@" <<'PY'
import sys, time, os
out = sys.argv[1]
members = sys.argv[2:]
uid = os.getuid() if hasattr(os, "getuid") else 0
gid = os.getgid() if hasattr(os, "getgid") else 0
mtime = int(time.time())
with open(out, "wb") as f:
    f.write(b"!<arch>\n")
    for path in members:
        name = os.path.basename(path)
        if len(name) > 16:
            raise SystemExit(f"ar short name too long: {name}")
        data = open(path, "rb").read()
        # GNU-style trailing slash in the 16-byte name field is optional;
        # pad with spaces. Keep basename only (<=16).
        name_field = name.encode("ascii")
        name_field = name_field + b" " * (16 - len(name_field))
        header = (
            name_field
            + f"{mtime}".encode().ljust(12)
            + f"{uid}".encode().ljust(6)
            + f"{gid}".encode().ljust(6)
            + b"100644".ljust(8)
            + f"{len(data)}".encode().ljust(10)
            + b"`\n"
        )
        assert len(header) == 60
        f.write(header)
        f.write(data)
        if len(data) % 2 == 1:
            f.write(b"\n")
print(out)
PY
}

echo "==> Assembling sample_tweak.deb (portable short-name ar)"
write_ar_short "${WORK}/sample_tweak.deb" \
  "${WORK}/debian-binary" \
  "${WORK}/control.tar.gz" \
  "${WORK}/data.tar.gz"

cp "${WORK}/sample_tweak.deb" "${OUT_DIR}/sample_tweak.deb"
cp "${WORK}/pkg/Library/MobileSubstrate/DynamicLibraries/SampleTweak.dylib" \
  "${OUT_DIR}/SampleTweak.dylib"

# Sanity: member names should be readable as plain text near the start.
if ! python3 - <<PY
import sys
data = open("${OUT_DIR}/sample_tweak.deb", "rb").read()
assert data.startswith(b"!<arch>\\n"), "bad ar magic"
assert b"data.tar.gz" in data[:512], "data.tar.gz name missing from ar headers"
print("ar member names OK")
PY
then
  echo "ERROR: sample_tweak.deb ar names look wrong" >&2
  exit 1
fi

ls -lh "${OUT_DIR}/sample_tweak.deb" "${OUT_DIR}/SampleTweak.dylib"
echo "==> sample_tweak.deb ready"
