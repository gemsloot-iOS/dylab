#ifndef DYLAB_TWEAK_BUILDER_H
#define DYLAB_TWEAK_BUILDER_H

#include "DebPackager.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DylabBuildLogFn)(void *ctx, const char *line);

/**
 * Full Path C pipeline:
 *  [1/4] ObjC lower  →  [2/4] TinyCC ELF .o  →  [3/4] Mach-O link+sign  →  [4/4] .deb
 *
 * @param tweak_m_path     Input Tweak.m
 * @param sdk_include_dir  Bundled SDK/include path (C99 + objc stubs)
 * @param out_dir          Output directory for .c / .o / .dylib / .deb
 * @param dylib_basename   e.g. MyTweak.dylib
 * @param package_id       deb package id
 * @param package_name     deb display name
 * @param version          deb version
 * @param filter_bundle    MobileSubstrate filter bundle id
 * @param style            Rootful / Rootless
 * @param log_fn           Optional stage logger
 * @param log_ctx          Context for log_fn
 * @param out_dylib_path   Optional capacity buffer filled with written dylib path
 * @param out_dylib_cap
 * @param out_deb_path     Optional buffer for .deb path
 * @param out_deb_cap
 * @param err_buf
 * @param err_cap
 */
int dylab_build_tweak(const char *tweak_m_path,
                      const char *sdk_include_dir,
                      const char *out_dir,
                      const char *dylib_basename,
                      const char *package_id,
                      const char *package_name,
                      const char *version,
                      const char *filter_bundle,
                      DylabDebStyle style,
                      DylabBuildLogFn log_fn,
                      void *log_ctx,
                      char *out_dylib_path,
                      size_t out_dylib_cap,
                      char *out_deb_path,
                      size_t out_deb_cap,
                      char *err_buf,
                      size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_TWEAK_BUILDER_H */
