#ifndef DYLAB_DECOMPILER_ENGINE_H
#define DYLAB_DECOMPILER_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECOMPILE_MODE_DISASM = 0,
    DECOMPILE_MODE_HEURISTIC_C = 1,
    DECOMPILE_MODE_STRUCTURED = 2
} DecompileMode;

/**
 * Decompile / disassemble a function-like range in a Mach-O binary.
 *
 * @param path            Mach-O / Fat path
 * @param file_offset     File offset of code (thin-file relative if Fat slice)
 * @param size            Max bytes (0 = until ret / 256 insn / section end)
 * @param vma_hint        Preferred VMA for annotations (0 = derive from map)
 * @param mode            Output mode
 * @param out_buf         Destination text buffer
 * @param out_buf_size    Capacity
 * @return 0 on success
 */
int decompile_function(const char *path,
                       uint64_t file_offset,
                       uint64_t size,
                       uint64_t vma_hint,
                       DecompileMode mode,
                       char *out_buf,
                       size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_DECOMPILER_ENGINE_H */
