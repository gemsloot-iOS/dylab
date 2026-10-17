/* Linux smoke: TCC ARM64 ELF .o → Mach-O dylib → ad-hoc sign */
#include "../DyLabApp/Bridge/MachODylibLinker.h"
#include "../DyLabApp/Bridge/MachOSigner.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if __has_include(<libtcc.h>)
#include <libtcc.h>
#else
#error "need libtcc"
#endif

static void terr(void *, const char *m) { fprintf(stderr, "tcc: %s\n", m); }

int main() {
    const char *src =
        "int add(int a, int b) { return a + b; }\n"
        "extern int printf(const char *, ...);\n"
        "__attribute__((constructor)) static void init(void) { printf(\"hi\\n\"); }\n";

    TCCState *s = tcc_new();
    if (!s) return 1;
    tcc_set_error_func(s, nullptr, terr);
    tcc_set_output_type(s, TCC_OUTPUT_OBJ);
    if (tcc_compile_string(s, src) < 0) return 2;
    const char *obj = "/tmp/dylab_link_smoke.o";
    const char *dylib = "/tmp/dylab_link_smoke.dylib";
    if (tcc_output_file(s, obj) < 0) return 3;
    tcc_delete(s);

    char err[1024];
    if (dylab_link_elf_to_dylib(obj, dylib, "/Library/MobileSubstrate/DynamicLibraries/Smoke.dylib",
                                err, sizeof(err)) != 0) {
        fprintf(stderr, "link failed: %s\n", err);
        return 4;
    }
    FILE *f = fopen(dylib, "rb");
    if (!f) return 5;
    unsigned char mag[4];
    if (fread(mag, 1, 4, f) != 4) return 6;
    fclose(f);
    /* MH_MAGIC_64 little-endian: cf fa ed fe */
    if (!(mag[0] == 0xcf && mag[1] == 0xfa && mag[2] == 0xed && mag[3] == 0xfe)) {
        fprintf(stderr, "bad macho magic %02x%02x%02x%02x\n", mag[0], mag[1], mag[2], mag[3]);
        return 7;
    }
    puts("OK macho magic");

    if (dylab_ldid_sign(dylib, err, sizeof(err)) != 0) {
        fprintf(stderr, "sign failed: %s\n", err);
        return 8;
    }
    puts("OK signed");
    puts("ALL PASS");
    return 0;
}
