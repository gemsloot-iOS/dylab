/* Linux unit test: ObjCLowering string transforms */
#include "../DyLabApp/Bridge/ObjCLowering.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void expect_contains(const char *name, const std::string &hay, const char *needle) {
    if (hay.find(needle) == std::string::npos) {
        fprintf(stderr, "FAIL %s: missing '%s'\n", name, needle);
        failures++;
    } else {
        printf("OK   %s\n", name);
    }
}

int main() {
    const char *src =
        "#import <Foundation/Foundation.h>\n"
        "@interface Foo : NSObject\n"
        "- (id)bar:(id)x;\n"
        "@end\n"
        "@implementation Foo\n"
        "- (id)bar:(id)x {\n"
        "    NSLog(@\"hello\");\n"
        "    return [self baz:x];\n"
        "}\n"
        "@end\n";

    char out[256 * 1024];
    char err[512];
    int rc = dylab_objc_lower(src, out, sizeof(out), err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "lower failed rc=%d err=%s\n", rc, err);
        return 1;
    }
    std::string s(out);
    expect_contains("import", s, "#include <Foundation/Foundation.h>");
    expect_contains("cfstring", s, "__dylab_cfstring(\"hello\")");
    if (s.find("@interface") != std::string::npos) {
        fprintf(stderr, "FAIL @interface still present\n");
        failures++;
    } else {
        printf("OK   @interface stripped\n");
    }
    expect_contains("msgsend", s, "objc_msgSend");
    expect_contains("sel", s, "sel_registerName");
    expect_contains("hook ctor", s, "MSHookMessageEx");
    expect_contains("constructor", s, "__attribute__((constructor))");

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        fprintf(stderr, "---- output head ----\n%.2000s\n", out);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
