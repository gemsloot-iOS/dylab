#ifndef _DYLAB_FOUNDATION_H
#define _DYLAB_FOUNDATION_H
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef double NSTimeInterval;
typedef long NSInteger;
typedef unsigned long NSUInteger;

/* Opaque ObjC class stand-ins — real classes via objc_getClass. */
typedef struct objc_object NSString;
typedef struct objc_object NSObject;
typedef struct objc_object NSArray;
typedef struct objc_object NSDictionary;
typedef struct objc_object NSData;
typedef struct objc_object NSBundle;
typedef struct objc_object NSError;

void NSLog(NSString *fmt, ...);

/*
 * Canonical @"..." helper — one signature everywhere (must match ObjCLowering).
 * Guarded so Foundation.h and the lowering preamble never collide.
 */
#ifndef DYLAB_CFSTRING_DEFINED
#define DYLAB_CFSTRING_DEFINED 1
typedef struct {
    void *isa;
    uint32_t info;
    uint32_t pad;
    const char *data;
    size_t length;
} __DYLAB_CFString;

static inline id __dylab_cfstring(const char *utf8) {
    size_t n = utf8 ? strlen(utf8) : 0;
    __DYLAB_CFString *s = (__DYLAB_CFString *)calloc(1, sizeof(*s));
    if (!s) return nil;
    s->isa = (void *)objc_getClass("NSConstantString");
    if (!s->isa) s->isa = (void *)objc_getClass("__NSCFConstantString");
    if (!s->isa) s->isa = (void *)objc_getClass("NSString");
    s->info = 0x07c8;
    s->data = utf8 ? utf8 : "";
    s->length = n;
    return (id)s;
}
#endif /* DYLAB_CFSTRING_DEFINED */

#ifndef DYLAB_MSHOOK_DEFINED
#define DYLAB_MSHOOK_DEFINED 1
/* Substrate/ElleKit-compatible hook — inline fallback via class_replaceMethod. */
static inline void MSHookMessageEx(Class cls, SEL sel, IMP imp, IMP *result) {
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) m = class_getClassMethod(cls, sel);
    if (!m) {
        if (result) *result = NULL;
        class_replaceMethod(cls, sel, imp, "@@:");
        return;
    }
    IMP old = method_getImplementation(m);
    if (result) *result = old;
    {
        const char *types = method_getTypeEncoding(m);
        class_replaceMethod(cls, sel, imp, types ? types : "@@:");
    }
}
#endif /* DYLAB_MSHOOK_DEFINED */

#endif /* _DYLAB_FOUNDATION_H */
