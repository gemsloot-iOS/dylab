#ifndef _DYLAB_OBJC_H
#define _DYLAB_OBJC_H
#include <stddef.h>
#include <stdint.h>
typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_method *Method;
typedef id (*IMP)(id, SEL, ...);
typedef signed char BOOL;
#ifndef YES
#define YES ((BOOL)1)
#define NO  ((BOOL)0)
#endif
#ifndef nil
#define nil ((id)0)
#endif
#ifndef Nil
#define Nil ((Class)0)
#endif
SEL sel_registerName(const char *str);
Class objc_getClass(const char *name);
Class object_getClass(id obj);
Class class_getSuperclass(Class cls);
Method class_getInstanceMethod(Class cls, SEL name);
Method class_getClassMethod(Class cls, SEL name);
IMP method_getImplementation(Method m);
IMP class_replaceMethod(Class cls, SEL name, IMP imp, const char *types);
const char *method_getTypeEncoding(Method m);
id objc_msgSend(id self, SEL op, ...);
id objc_retain(id);
void objc_release(id);
id objc_autorelease(id);
#endif
