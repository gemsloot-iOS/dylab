#ifndef DYLAB_STRUCTURED_DECOMPILER_H
#define DYLAB_STRUCTURED_DECOMPILER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Phase 3B structured decompiler (CFG + folding + demangle).
 * Capstone ARM64 + MachOMap; no extra deps beyond libcapstone.
 *
 * @return 0 on success (best-effort text always written when possible)
 */
int structured_decompile(const char *path,
                         uint64_t file_offset,
                         uint64_t size,
                         uint64_t vma_hint,
                         char *out_buf,
                         size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_STRUCTURED_DECOMPILER_H */
