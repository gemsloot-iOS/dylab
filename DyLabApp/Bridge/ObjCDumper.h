#ifndef DYLAB_OBJC_DUMPER_H
#define DYLAB_OBJC_DUMPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dump ObjC classes/methods/properties/ivars + callable symbols/function starts
 * as JSON into out_buf.
 * @return 0 on success
 */
int objc_dump_classes_json(const char *path, char *out_buf, size_t out_buf_size);

/**
 * Generate TweakClasses.h + Tweak.m from ObjC metadata in the Mach-O.
 * Discovers classes via __objc_classlist (chained-fixup aware), including
 * class methods from the metaclass, and fills method bodies via the
 * structured decompiler. Falls back to DyLabHookTarget only when no ObjC
 * classes can be resolved (e.g. non-ObjC binary).
 * @return 0 on success
 */
int objc_generate_header_and_skeleton(const char *path,
                                      const char *out_dir,
                                      char *out_header_path,
                                      size_t out_header_path_size,
                                      char *out_tweak_path,
                                      size_t out_tweak_path_size,
                                      char *err_buf,
                                      size_t err_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_OBJC_DUMPER_H */
