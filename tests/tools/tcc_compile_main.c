#include <libtcc.h>
#include <stdio.h>
#include <string.h>

static void on_err(void *opaque, const char *msg) {
    (void)opaque;
    fprintf(stderr, "tcc: %s\n", msg ? msg : "");
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s input.c sdk_include out.o\n", argv[0]);
        return 1;
    }
    TCCState *s = tcc_new();
    if (!s) return 2;
    tcc_set_error_func(s, NULL, on_err);
    tcc_set_output_type(s, TCC_OUTPUT_OBJ);
    tcc_add_include_path(s, argv[2]);
    tcc_define_symbol(s, "__APPLE__", NULL);
    tcc_define_symbol(s, "__arm64__", NULL);
    tcc_define_symbol(s, "__aarch64__", NULL);
    tcc_define_symbol(s, "TARGET_OS_IPHONE", "1");
    if (tcc_add_file(s, argv[1]) < 0) {
        tcc_delete(s);
        return 3;
    }
    if (tcc_output_file(s, argv[3]) < 0) {
        tcc_delete(s);
        return 4;
    }
    tcc_delete(s);
    return 0;
}
