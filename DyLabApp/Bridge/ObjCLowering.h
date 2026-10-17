#ifndef DYLAB_OBJC_LOWERING_H
#define DYLAB_OBJC_LOWERING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Source-to-source ObjC → C99 for TinyCC.
 *
 * Transforms (Path C):
 *  - #import → #include
 *  - @"..." → __dylab_cfstring("...")
 *  - @selector(x) → sel_registerName("x")
 *  - [recv sel:args] → typed objc_msgSend(...)
 *  - @interface → forward typedef / stripped
 *  - @implementation Class { methods } → static hooks + constructor
 *    using class_replaceMethod / MSHookMessageEx
 *
 * @param objc_src   Input ObjC source (Tweak.m)
 * @param out_buf    Destination C source
 * @param out_cap    Capacity of out_buf
 * @param err_buf    Optional error text
 * @param err_cap    Capacity of err_buf
 * @return 0 on success
 */
int dylab_objc_lower(const char *objc_src,
                     char *out_buf,
                     size_t out_cap,
                     char *err_buf,
                     size_t err_cap);

/**
 * Lower a file on disk; writes UTF-8 C source to out_path.
 */
int dylab_objc_lower_file(const char *in_path,
                          const char *out_path,
                          char *err_buf,
                          size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_OBJC_LOWERING_H */
