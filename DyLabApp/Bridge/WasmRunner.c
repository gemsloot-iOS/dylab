#include "WasmRunner.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wasm_export.h"

#define WAMR_HEAP_SIZE (64 * 1024 * 1024)
#define WASM_STACK_SIZE (256 * 1024)
#define WASM_HEAP_SIZE (1024 * 1024)

static void
append_text(char *buf, size_t buf_size, size_t *used, const char *text)
{
    if (buf == NULL || buf_size == 0 || text == NULL) {
        return;
    }
    size_t len = strlen(text);
    size_t avail = (*used < buf_size) ? (buf_size - *used - 1) : 0;
    if (avail == 0) {
        return;
    }
    if (len > avail) {
        len = avail;
    }
    memcpy(buf + *used, text, len);
    *used += len;
    buf[*used] = '\0';
}

static uint8_t *
read_file_bytes(const char *path, uint32_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    uint8_t *buf;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    buf = (uint8_t *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *out_size = (uint32_t)sz;
    return buf;
}

static int
read_fd_into_buf(int fd, char *buf, size_t buf_size, size_t *used)
{
    char chunk[4096];
    ssize_t n;

    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        size_t avail = (*used < buf_size) ? (buf_size - *used - 1) : 0;
        if (avail == 0) {
            break;
        }
        size_t copy = (size_t)n < avail ? (size_t)n : avail;
        memcpy(buf + *used, chunk, copy);
        *used += copy;
        buf[*used] = '\0';
        if (copy < (size_t)n) {
            break;
        }
    }

    return (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ? -1 : 0;
}

int
run_wasi_tool(const char *wasm_path,
              const char *sandbox_root_dir,
              int argc,
              char **argv,
              char *output_buf,
              size_t output_buf_size)
{
    static char global_heap_buf[WAMR_HEAP_SIZE];

    RuntimeInitArgs init_args;
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    char error_buf[256];
    uint8_t *wasm_file_buf = NULL;
    uint32_t wasm_file_size = 0;
    int pipe_fds[2] = { -1, -1 };
    int saved_stdout = -1;
    int saved_stderr = -1;
    size_t out_used = 0;
    int result = -1;
    bool runtime_inited = false;
    char *default_argv0[] = { (char *)"hello.wasm", NULL };
    char **wasi_argv = argv;
    int wasi_argc = argc;
    char resolved_root[PATH_MAX];
    char map_dot[PATH_MAX + 8];
    char map_workspace[PATH_MAX + 16];
    const char *dir_list[1];
    const char *map_dir_list[2];
    const char *env_list[2];

    if (output_buf != NULL && output_buf_size > 0) {
        output_buf[0] = '\0';
    }

    if (wasm_path == NULL || sandbox_root_dir == NULL) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: wasm_path and sandbox_root_dir are required\n");
        return -1;
    }

    if (realpath(sandbox_root_dir, resolved_root) == NULL) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: realpath(sandbox_root_dir) failed: ");
        append_text(output_buf, output_buf_size, &out_used, strerror(errno));
        append_text(output_buf, output_buf_size, &out_used, "\n");
        return -1;
    }

    if (chdir(resolved_root) != 0) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: chdir(resolved sandbox) failed: ");
        append_text(output_buf, output_buf_size, &out_used, strerror(errno));
        append_text(output_buf, output_buf_size, &out_used, "\n");
        return -1;
    }

    /* Guest preopens at "." and "/workspace", not the long iOS host path. */
    snprintf(map_dot, sizeof(map_dot), ".::%s", resolved_root);
    snprintf(map_workspace, sizeof(map_workspace), "/workspace::%s",
             resolved_root);
    dir_list[0] = ".";
    map_dir_list[0] = map_dot;
    map_dir_list[1] = map_workspace;
    env_list[0] = "HOME=/workspace";
    env_list[1] = "PWD=/workspace";

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = global_heap_buf;
    init_args.mem_alloc_option.pool.heap_size = (uint32_t)sizeof(global_heap_buf);

    if (!wasm_runtime_full_init(&init_args)) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: wasm_runtime_full_init failed\n");
        goto cleanup;
    }
    runtime_inited = true;

    wasm_file_buf = read_file_bytes(wasm_path, &wasm_file_size);
    if (wasm_file_buf == NULL) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: failed to read wasm file\n");
        goto cleanup;
    }

    module = wasm_runtime_load(wasm_file_buf, wasm_file_size, error_buf,
                               (uint32_t)sizeof(error_buf));
    if (module == NULL) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: wasm_runtime_load failed: ");
        append_text(output_buf, output_buf_size, &out_used, error_buf);
        append_text(output_buf, output_buf_size, &out_used, "\n");
        goto cleanup;
    }

    if (wasi_argv == NULL || wasi_argc <= 0) {
        wasi_argv = default_argv0;
        wasi_argc = 1;
    }

    wasm_runtime_set_wasi_args(module, dir_list, 1, map_dir_list, 2, env_list, 2,
                               wasi_argv, wasi_argc);

    module_inst = wasm_runtime_instantiate(module, WASM_STACK_SIZE, WASM_HEAP_SIZE,
                                           error_buf, (uint32_t)sizeof(error_buf));
    if (module_inst == NULL) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: wasm_runtime_instantiate failed: ");
        append_text(output_buf, output_buf_size, &out_used, error_buf);
        append_text(output_buf, output_buf_size, &out_used, "\n");
        goto cleanup;
    }

    if (pipe(pipe_fds) != 0) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: pipe() failed\n");
        goto cleanup;
    }

    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0 || saved_stderr < 0) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: dup() of stdout/stderr failed\n");
        goto cleanup;
    }

    if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
        dup2(pipe_fds[1], STDERR_FILENO) < 0) {
        append_text(output_buf, output_buf_size, &out_used,
                    "error: dup2() redirect failed\n");
        goto cleanup;
    }

    close(pipe_fds[1]);
    pipe_fds[1] = -1;

    {
        bool ok = wasm_application_execute_main(module_inst, wasi_argc, wasi_argv);
        fflush(stdout);
        fflush(stderr);

        if (saved_stdout >= 0) {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
            saved_stdout = -1;
        }
        if (saved_stderr >= 0) {
            dup2(saved_stderr, STDERR_FILENO);
            close(saved_stderr);
            saved_stderr = -1;
        }

        if (pipe_fds[0] >= 0) {
            read_fd_into_buf(pipe_fds[0], output_buf, output_buf_size, &out_used);
            close(pipe_fds[0]);
            pipe_fds[0] = -1;
        }

        if (!ok) {
            const char *exception = wasm_runtime_get_exception(module_inst);
            append_text(output_buf, output_buf_size, &out_used,
                        "\nerror: wasm_application_execute_main failed");
            if (exception != NULL) {
                append_text(output_buf, output_buf_size, &out_used, ": ");
                append_text(output_buf, output_buf_size, &out_used, exception);
            }
            append_text(output_buf, output_buf_size, &out_used, "\n");
            result = -1;
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    if (saved_stdout >= 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (pipe_fds[0] >= 0) {
        close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        close(pipe_fds[1]);
    }

    if (module_inst != NULL) {
        wasm_runtime_deinstantiate(module_inst);
    }
    if (module != NULL) {
        wasm_runtime_unload(module);
    }
    free(wasm_file_buf);
    if (runtime_inited) {
        wasm_runtime_destroy();
    }

    return result;
}
