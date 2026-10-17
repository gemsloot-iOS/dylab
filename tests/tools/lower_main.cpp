#include "ObjCLowering.h"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.m out.c\n", argv[0]);
        return 1;
    }
    char err[2048];
    if (dylab_objc_lower_file(argv[1], argv[2], err, sizeof(err)) != 0) {
        fprintf(stderr, "lower failed: %s\n", err);
        return 2;
    }
    return 0;
}
