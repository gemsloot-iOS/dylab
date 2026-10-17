/* Linux unit test: DebPackager produces a valid ar .deb */
#include "../DyLabApp/Bridge/DebPackager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *dylib = "/tmp/dylab_test.dylib";
    const char *deb = "/tmp/dylab_test.deb";
    FILE *f = fopen(dylib, "wb");
    if (!f) return 1;
    fwrite("fakedylib", 1, 9, f);
    fclose(f);

    char err[512];
    int rc = dylab_package_deb(dylib, deb, "com.dylab.test", "TestTweak", "1.0.0",
                               "com.apple.SpringBoard", DYLAB_DEB_ROOTLESS, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "package failed: %s\n", err);
        return 2;
    }
    f = fopen(deb, "rb");
    if (!f) return 3;
    char mag[8];
    if (fread(mag, 1, 8, f) != 8 || memcmp(mag, "!<arch>\n", 8) != 0) {
        fprintf(stderr, "bad ar magic\n");
        fclose(f);
        return 4;
    }
    fclose(f);
    puts("OK deb ar magic");
    puts("ALL PASS");
    return 0;
}
