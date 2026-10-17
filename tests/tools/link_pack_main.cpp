#include "DebPackager.h"
#include "MachODylibLinker.h"
#include "MachOSigner.h"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.o out.dylib out.deb\n", argv[0]);
        return 1;
    }
    char err[2048];
    if (dylab_link_elf_to_dylib(argv[1], argv[2],
                                "/Library/MobileSubstrate/DynamicLibraries/E2E.dylib", err,
                                sizeof(err)) != 0) {
        fprintf(stderr, "link failed: %s\n", err);
        return 2;
    }
    if (dylab_ldid_sign(argv[2], err, sizeof(err)) != 0) {
        fprintf(stderr, "sign failed: %s\n", err);
        return 3;
    }
    if (dylab_package_deb(argv[2], argv[3], "com.dylab.e2e", "E2E", "1.0.0",
                          "com.apple.UIKit", DYLAB_DEB_ROOTLESS, err, sizeof(err)) != 0) {
        fprintf(stderr, "deb failed: %s\n", err);
        return 4;
    }
    return 0;
}
