#!/usr/bin/env bash
# Build SampleTweak.dylib (iphoneos arm64) and package sample_tweak.deb into
# DyLabApp/Resources/. Intended for macOS GitHub Actions (macos-15) runners.
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

echo "==> Assembling sample_tweak.deb"
(
  cd "${WORK}"
  ar rcs sample_tweak.deb debian-binary control.tar.gz data.tar.gz
)

cp "${WORK}/sample_tweak.deb" "${OUT_DIR}/sample_tweak.deb"
# Also keep a copy of the dylib next to the deb for convenience in Resources.
cp "${WORK}/pkg/Library/MobileSubstrate/DynamicLibraries/SampleTweak.dylib" \
  "${OUT_DIR}/SampleTweak.dylib"

ls -lh "${OUT_DIR}/sample_tweak.deb" "${OUT_DIR}/SampleTweak.dylib"
echo "==> sample_tweak.deb ready"
