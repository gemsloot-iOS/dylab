#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *
open_workspace_file(const char **used_path)
{
    const char *candidates[] = {
        "workspace_test.txt",
        "/workspace/workspace_test.txt",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *fp = fopen(candidates[i], "w");
        if (fp != NULL) {
            *used_path = candidates[i];
            return fp;
        }
        fprintf(stderr, "fopen(\"%s\", \"w\") failed: %s\n", candidates[i],
                strerror(errno));
    }
    return NULL;
}

int
main(int argc, char **argv)
{
    printf("Hello from inside the iOS WASI Sandbox!\n");

    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    const char *used_path = NULL;
    FILE *fp = open_workspace_file(&used_path);
    if (fp == NULL) {
        fprintf(stderr, "Failed to open workspace_test.txt for writing\n");
        return 1;
    }

    fprintf(fp, "WASI File I/O is working on iOS!\n");
    fclose(fp);

    printf("Wrote %s\n", used_path);
    return 0;
}
