#ifndef DYLAB_WASM_RUNNER_H
#define DYLAB_WASM_RUNNER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load and execute a WASI CLI wasm module with the given sandbox root.
 *
 * @param wasm_path          Absolute path to the .wasm file
 * @param sandbox_root_dir   Host directory mounted as the WASI preopen root
 * @param argc               Argument count passed to the module (argv style)
 * @param argv               Argument vector (argv[0] is typically the tool name)
 * @param output_buf         Buffer that receives captured stdout+stderr
 * @param output_buf_size    Capacity of output_buf including NUL terminator
 * @return 0 on success, non-zero on failure
 */
int run_wasi_tool(const char *wasm_path,
                  const char *sandbox_root_dir,
                  int argc,
                  char **argv,
                  char *output_buf,
                  size_t output_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_WASM_RUNNER_H */
