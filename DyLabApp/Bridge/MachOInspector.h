#ifndef DYLAB_MACHO_INSPECTOR_H
#define DYLAB_MACHO_INSPECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Inspect a Mach-O (or Fat) binary and write a structured JSON report into
 * out_buf. Prefer the CPU_TYPE_ARM64 slice when the file is Fat.
 *
 * @return 0 on success, non-zero on failure
 */
int inspect_macho(const char *path, char *out_buf, size_t out_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_MACHO_INSPECTOR_H */
