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

static void expect_absent(const char *name, const std::string &hay, const char *needle) {
    if (hay.find(needle) != std::string::npos) {
        fprintf(stderr, "FAIL %s: unexpectedly found '%s'\n", name, needle);
        failures++;
    } else {
        printf("OK   %s\n", name);
    }
}

static std::string read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return {};
    }
    std::string s((size_t)sz, '\0');
    if (sz) {
        size_t n = fread(&s[0], 1, (size_t)sz, f);
        (void)n;
    }
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
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

    /* File API must inline Generate-Source companion header TweakClasses.h */
    const char *fixture = nullptr;
    if (argc > 1) {
        fixture = argv[1];
    } else {
        fixture = "fixtures/generated_skeleton/Tweak.m";
    }
    const char *tmp_out = "/tmp/dylab_lower_skeleton_test.c";
    err[0] = '\0';
    rc = dylab_objc_lower_file(fixture, tmp_out, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "lower_file(%s) failed rc=%d err=%s\n", fixture, rc, err);
        failures++;
    } else {
        std::string lowered = read_file(tmp_out);
        expect_contains("skeleton inlined marker", lowered, "begin include \"TweakClasses.h\"");
        expect_absent("no leftover TweakClasses include", lowered,
                      "#include \"TweakClasses.h\"");
        expect_absent("no @interface after lower", lowered, "@interface");
        expect_absent("no @implementation after lower", lowered, "@implementation");
        expect_contains("skeleton hooks", lowered, "MSHookMessageEx");
    }
    remove(tmp_out);

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        fprintf(stderr, "---- output head ----\n%.2000s\n", out);
        return 1;
    }
    puts("ALL PASS");
    return 0;
}
