#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    printf("Hello from inside the iOS WASI Sandbox!\n");

    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    FILE *fp = fopen("workspace_test.txt", "w");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open workspace_test.txt for writing\n");
        return 1;
    }

    fprintf(fp, "WASI File I/O is working on iOS!\n");
    fclose(fp);

    printf("Wrote workspace_test.txt\n");
    return 0;
}
