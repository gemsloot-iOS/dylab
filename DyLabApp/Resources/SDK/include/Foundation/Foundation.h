#ifndef _DYLAB_FOUNDATION_H
#define _DYLAB_FOUNDATION_H
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef double NSTimeInterval;
typedef long NSInteger;
typedef unsigned long NSUInteger;

/* Opaque ObjC class pointers — real classes resolved via objc_getClass. */
typedef struct objc_object NSString;
typedef struct objc_object NSObject;
typedef struct objc_object NSArray;
typedef struct objc_object NSDictionary;
typedef struct objc_object NSData;
typedef struct objc_object NSBundle;
typedef struct objc_object NSError;

void NSLog(NSString *fmt, ...);

/* Produced by ObjCLowering for @"..." literals. */
const void *__dylab_cfstring(const char *utf8);

/* Cydia Substrate / ElleKit-compatible hook helper (weak stub at link). */
void MSHookMessageEx(Class cls, SEL sel, IMP imp, IMP *result);

#endif
