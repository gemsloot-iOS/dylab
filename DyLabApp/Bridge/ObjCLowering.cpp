#include "ObjCLowering.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *kPreamble = R"PRE(
/* Auto-lowered by DyLab ObjCLowering (Path C) — TinyCC C99 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
/*
 * Foundation.h owns the canonical __dylab_cfstring / MSHookMessageEx
 * (static inline id / void) behind DYLAB_CFSTRING_DEFINED / DYLAB_MSHOOK_DEFINED.
 * Do NOT redeclare with a different type here — TCC errors on incompatible
 * redefinition (e.g. const void * vs id).
 */
#include <Foundation/Foundation.h>

/* Path C: TinyCC has no ObjC `super` keyword — approximate as self.
 * (True objc_msgSendSuper can be added later if needed.) */
#ifndef super
#define super self
#endif

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

)PRE";

static bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_' || c == '$';
}
static bool is_ident(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '$';
}

static void set_err(char *err, size_t cap, const std::string &msg) {
    if (!err || cap == 0) return;
    snprintf(err, cap, "%s", msg.c_str());
}

static std::string replace_all(std::string s, const std::string &a, const std::string &b) {
    size_t pos = 0;
    while ((pos = s.find(a, pos)) != std::string::npos) {
        s.replace(pos, a.size(), b);
        pos += b.size();
    }
    return s;
}

/* #import "X" / #import <X> → #include */
static std::string transform_imports(std::string s) {
    std::ostringstream out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 8, "#import ") == 0 || s.compare(i, 7, "#import\t") == 0) {
            out << "#include ";
            i += 7;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
                out << s[i++];
            }
            continue;
        }
        out << s[i++];
    }
    return out.str();
}

/* @"..." → __dylab_cfstring("...")  (handles \" escapes) */
static std::string transform_at_strings(std::string s) {
    std::ostringstream out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '@' && i + 1 < s.size() && s[i + 1] == '"') {
            out << "__dylab_cfstring(\"";
            i += 2;
            while (i < s.size()) {
                char c = s[i++];
                if (c == '\\' && i < s.size()) {
                    out << '\\' << s[i++];
                    continue;
                }
                if (c == '"') break;
                out << c;
            }
            out << "\")";
            continue;
        }
        out << s[i++];
    }
    return out.str();
}

/* @selector(foo:bar:) → sel_registerName("foo:bar:") */
static std::string transform_selectors(std::string s) {
    std::ostringstream out;
    const std::string key = "@selector(";
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, key.size(), key) == 0) {
            i += key.size();
            std::string sel;
            int depth = 1;
            while (i < s.size() && depth > 0) {
                if (s[i] == '(') depth++;
                else if (s[i] == ')') {
                    depth--;
                    if (depth == 0) {
                        i++;
                        break;
                    }
                }
                if (depth > 0) sel.push_back(s[i]);
                i++;
            }
            /* trim whitespace inside */
            while (!sel.empty() && std::isspace((unsigned char)sel.front())) sel.erase(sel.begin());
            while (!sel.empty() && std::isspace((unsigned char)sel.back())) sel.pop_back();
            out << "sel_registerName(\"" << sel << "\")";
            continue;
        }
        out << s[i++];
    }
    return out.str();
}

/* Strip @interface ... @end blocks (keep nothing — hooks come from @implementation). */
static std::string strip_interfaces(std::string s) {
    std::ostringstream out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 10, "@interface") == 0 &&
            (i == 0 || !is_ident(s[i - 1]))) {
            i += 10;
            while (i < s.size()) {
                if (s.compare(i, 4, "@end") == 0 &&
                    (i + 4 >= s.size() || !is_ident(s[i + 4]))) {
                    i += 4;
                    break;
                }
                i++;
            }
            out << "\n/* interface block removed */\n";
            continue;
        }
        /* @protocol similarly */
        if (s.compare(i, 9, "@protocol") == 0 &&
            (i == 0 || !is_ident(s[i - 1]))) {
            i += 9;
            while (i < s.size()) {
                if (s.compare(i, 4, "@end") == 0) {
                    i += 4;
                    break;
                }
                i++;
            }
            out << "\n/* protocol block removed */\n";
            continue;
        }
        out << s[i++];
    }
    return out.str();
}

struct MethodBits {
    std::string sel;       /* foo:bar: */
    std::string c_name;    /* sanitized */
    std::vector<std::string> arg_names;
    std::string body;      /* without braces */
    bool is_class_method = false;
};

static std::string sanitize(const std::string &in) {
    std::string o = "dylab_";
    for (char c : in) {
        if (is_ident(c)) o.push_back(c);
        else if (c == ':') o += "_";
        else o.push_back('_');
    }
    return o;
}

/* Parse one method starting at `- (` or `+ (` */
static bool parse_method(const std::string &s, size_t &i, MethodBits &m) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    if (i >= s.size()) return false;
    if (s[i] != '-' && s[i] != '+') return false;
    m.is_class_method = (s[i] == '+');
    i++;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    if (i >= s.size() || s[i] != '(') return false;
    /* skip return type (id) */
    int depth = 0;
    do {
        if (s[i] == '(') depth++;
        else if (s[i] == ')') depth--;
        i++;
    } while (i < s.size() && depth > 0);

    std::string sel;
    std::vector<std::string> args;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        if (i < s.size() && s[i] == '{') break;
        /* name piece */
        size_t start = i;
        while (i < s.size() && is_ident(s[i])) i++;
        std::string piece = s.substr(start, i - start);
        if (piece.empty()) {
            if (i < s.size() && s[i] == ':') {
                /* unusual */
            } else break;
        }
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        if (i < s.size() && s[i] == ':') {
            sel += piece;
            sel.push_back(':');
            i++;
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            if (i < s.size() && s[i] == '(') {
                int d = 0;
                do {
                    if (s[i] == '(') d++;
                    else if (s[i] == ')') d--;
                    i++;
                } while (i < s.size() && d > 0);
            }
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            size_t as = i;
            while (i < s.size() && is_ident(s[i])) i++;
            args.push_back(s.substr(as, i - as));
        } else {
            /* unary selector */
            sel = piece;
            break;
        }
    }
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    if (i >= s.size() || s[i] != '{') return false;
    i++; /* { */
    int bdepth = 1;
    size_t body_start = i;
    while (i < s.size() && bdepth > 0) {
        if (s[i] == '{') bdepth++;
        else if (s[i] == '}') {
            bdepth--;
            if (bdepth == 0) break;
        } else if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            while (i < s.size() && s[i] != q) {
                if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                else i++;
            }
            if (i < s.size()) i++;
            continue;
        }
        i++;
    }
    m.body = s.substr(body_start, i - body_start);
    if (i < s.size() && s[i] == '}') i++;
    m.sel = sel;
    m.arg_names = args;
    m.c_name = sanitize(sel.empty() ? "unknown" : sel);
    return !m.sel.empty();
}

/* [expr] message sends → objc_msgSend. Nested OK via repeated scans. */
static std::string transform_messages(std::string s) {
    /* Multiple passes for nesting */
    for (int pass = 0; pass < 32; pass++) {
        bool changed = false;
        std::ostringstream out;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '[') {
                /* Find matching ] */
                int depth = 0;
                size_t start = i;
                size_t j = i;
                bool in_str = false;
                for (; j < s.size(); j++) {
                    char c = s[j];
                    if (in_str) {
                        if (c == '\\' && j + 1 < s.size()) {
                            j++;
                            continue;
                        }
                        if (c == '"') in_str = false;
                        continue;
                    }
                    if (c == '"') {
                        in_str = true;
                        continue;
                    }
                    if (c == '[') depth++;
                    else if (c == ']') {
                        depth--;
                        if (depth == 0) {
                            j++;
                            break;
                        }
                    }
                }
                if (depth != 0) {
                    out << s[i++];
                    continue;
                }
                std::string inner = s.substr(start + 1, j - start - 2);
                /* Don't rewrite if inner still has '[' — wait for inner first.
                   Actually we scan left-to-right; for nesting we want innermost.
                   So if inner contains '[', skip this bracket for now. */
                if (inner.find('[') != std::string::npos) {
                    out << s[i++];
                    continue;
                }
                /* Parse receiver + selector pieces */
                size_t p = 0;
                while (p < inner.size() && std::isspace((unsigned char)inner[p])) p++;
                size_t rs = p;
                /* receiver: ident or (cast)expr — take until whitespace before sel */
                if (p < inner.size() && inner[p] == '(') {
                    int d = 0;
                    do {
                        if (inner[p] == '(') d++;
                        else if (inner[p] == ')') d--;
                        p++;
                    } while (p < inner.size() && d > 0);
                    while (p < inner.size() && !std::isspace((unsigned char)inner[p]) &&
                           inner[p] != ':')
                        p++;
                } else {
                    while (p < inner.size() && !std::isspace((unsigned char)inner[p])) p++;
                }
                std::string recv = inner.substr(rs, p - rs);
                while (p < inner.size() && std::isspace((unsigned char)inner[p])) p++;

                std::string sel;
                std::vector<std::string> args;
                if (p >= inner.size()) {
                    out << s[i++];
                    continue;
                }
                /* unary or keyword */
                bool has_colon = inner.find(':', p) != std::string::npos;
                if (!has_colon) {
                    size_t ss = p;
                    while (p < inner.size() && is_ident(inner[p])) p++;
                    sel = inner.substr(ss, p - ss);
                } else {
                    while (p < inner.size()) {
                        while (p < inner.size() && std::isspace((unsigned char)inner[p])) p++;
                        size_t ss = p;
                        while (p < inner.size() && is_ident(inner[p])) p++;
                        std::string piece = inner.substr(ss, p - ss);
                        while (p < inner.size() && std::isspace((unsigned char)inner[p])) p++;
                        if (p < inner.size() && inner[p] == ':') {
                            sel += piece;
                            sel.push_back(':');
                            p++;
                            while (p < inner.size() && std::isspace((unsigned char)inner[p])) p++;
                            /* argument expression until next sel piece or end.
                               Heuristic: if next tokens look like "name:" then stop before name. */
                            size_t as = p;
                            int pd = 0;
                            while (p < inner.size()) {
                                char c = inner[p];
                                if (c == '(') pd++;
                                else if (c == ')') pd--;
                                else if (pd == 0 && std::isspace((unsigned char)c)) {
                                    /* peek ahead for ident: */
                                    size_t q = p;
                                    while (q < inner.size() && std::isspace((unsigned char)inner[q])) q++;
                                    size_t qs = q;
                                    while (q < inner.size() && is_ident(inner[q])) q++;
                                    while (q < inner.size() && std::isspace((unsigned char)inner[q])) q++;
                                    if (q < inner.size() && inner[q] == ':' && qs > p) {
                                        break;
                                    }
                                }
                                p++;
                            }
                            std::string arg = inner.substr(as, p - as);
                            while (!arg.empty() && std::isspace((unsigned char)arg.back())) arg.pop_back();
                            args.push_back(arg);
                        } else {
                            sel = piece;
                            break;
                        }
                    }
                }
                if (sel.empty()) {
                    out << s[i++];
                    continue;
                }

                /* Class name heuristic: Capitalized bare ident → objc_getClass */
                std::string recv_expr = recv;
                if (!recv.empty() && std::isupper((unsigned char)recv[0])) {
                    bool all_ident = true;
                    for (char c : recv)
                        if (!is_ident(c)) {
                            all_ident = false;
                            break;
                        }
                    if (all_ident) {
                        recv_expr = std::string("(id)objc_getClass(\"") + recv + "\")";
                    }
                }

                std::ostringstream call;
                /* Build typed objc_msgSend */
                call << "((id(*)(id,SEL";
                for (size_t a = 0; a < args.size(); a++) call << ",id";
                call << "))objc_msgSend)(" << recv_expr << ", sel_registerName(\"" << sel << "\")";
                for (const auto &a : args) call << ", " << a;
                call << ")";
                out << call.str();
                i = j;
                changed = true;
                continue;
            }
            out << s[i++];
        }
        s = out.str();
        if (!changed) break;
    }
    return s;
}

static std::string emit_impl_hooks(const std::string &cls, const std::vector<MethodBits> &methods) {
    std::ostringstream out;
    out << "\n/* ---- hooks for " << cls << " ---- */\n";
    for (const auto &m : methods) {
        out << "static IMP " << m.c_name << "_orig = NULL;\n";
        out << "static id " << m.c_name << "(id self, SEL _cmd";
        for (size_t a = 0; a < m.arg_names.size(); a++) {
            out << ", id " << (m.arg_names[a].empty() ? ("a" + std::to_string(a)) : m.arg_names[a]);
        }
        out << ") {\n";
        std::string body = m.body;
        /* self / super remain; body already message-lowered in outer pass */
        out << body;
        if (body.find("return") == std::string::npos) {
            out << "\n    return nil;\n";
        }
        out << "}\n";
    }
    out << "static void __attribute__((constructor)) " << sanitize(cls) << "_init(void) {\n";
    out << "    Class cls = objc_getClass(\"" << cls << "\");\n";
    out << "    if (!cls) return;\n";
    for (const auto &m : methods) {
        out << "    MSHookMessageEx(cls, sel_registerName(\"" << m.sel << "\"), (IMP)"
            << m.c_name << ", &" << m.c_name << "_orig);\n";
    }
    out << "}\n";
    return out.str();
}

/* Replace @implementation Class ... @end with hooks */
static std::string transform_implementations(std::string s) {
    std::ostringstream out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 15, "@implementation") == 0 &&
            (i == 0 || !is_ident(s[i - 1]))) {
            i += 15;
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            size_t cs = i;
            while (i < s.size() && is_ident(s[i])) i++;
            std::string cls = s.substr(cs, i - cs);
            /* optional : Super — skip to methods */
            while (i < s.size() && s[i] != '-' && s[i] != '+' &&
                   s.compare(i, 4, "@end") != 0) {
                /* skip category name (Foo) */
                if (s[i] == '(') {
                    while (i < s.size() && s[i] != ')') i++;
                    if (i < s.size()) i++;
                    continue;
                }
                i++;
            }
            std::vector<MethodBits> methods;
            while (i < s.size() && s.compare(i, 4, "@end") != 0) {
                while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
                if (s.compare(i, 4, "@end") == 0) break;
                MethodBits m;
                size_t before = i;
                if (!parse_method(s, i, m)) {
                    if (i == before) i++;
                    continue;
                }
                methods.push_back(m);
            }
            if (s.compare(i, 4, "@end") == 0) i += 4;
            out << emit_impl_hooks(cls, methods);
            continue;
        }
        out << s[i++];
    }
    return out.str();
}

static std::string lower_source(const std::string &input) {
    std::string s = input;
    s = transform_imports(s);
    s = transform_at_strings(s);
    s = transform_selectors(s);
    s = strip_interfaces(s);
    /* Message-send lower before splitting impls so method bodies are C */
    s = transform_messages(s);
    s = transform_implementations(s);
    /* Remaining @ keywords */
    s = replace_all(s, "@end", "/* @end */");
    s = replace_all(s, "@private", "");
    s = replace_all(s, "@public", "");
    s = replace_all(s, "@protected", "");
    s = replace_all(s, "@optional", "");
    s = replace_all(s, "@required", "");
    return std::string(kPreamble) + s;
}

} // namespace

extern "C" int
dylab_objc_lower(const char *objc_src,
                 char *out_buf,
                 size_t out_cap,
                 char *err_buf,
                 size_t err_cap)
{
    if (!objc_src || !out_buf || out_cap == 0) {
        set_err(err_buf, err_cap, "invalid arguments");
        return -1;
    }
    try {
        std::string out = lower_source(objc_src);
        if (out.size() + 1 > out_cap) {
            set_err(err_buf, err_cap, "output buffer too small");
            return -2;
        }
        memcpy(out_buf, out.c_str(), out.size() + 1);
        if (err_buf && err_cap) err_buf[0] = '\0';
        return 0;
    } catch (const std::exception &ex) {
        set_err(err_buf, err_cap, ex.what());
        return -3;
    } catch (...) {
        set_err(err_buf, err_cap, "unknown lowering error");
        return -3;
    }
}

extern "C" int
dylab_objc_lower_file(const char *in_path,
                      const char *out_path,
                      char *err_buf,
                      size_t err_cap)
{
    if (!in_path || !out_path) {
        set_err(err_buf, err_cap, "invalid paths");
        return -1;
    }
    FILE *f = fopen(in_path, "rb");
    if (!f) {
        set_err(err_buf, err_cap, "cannot open input");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 8 * 1024 * 1024) {
        fclose(f);
        set_err(err_buf, err_cap, "input too large");
        return -1;
    }
    std::string src((size_t)sz, '\0');
    if (sz && fread(&src[0], 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        set_err(err_buf, err_cap, "read failed");
        return -1;
    }
    fclose(f);

    std::vector<char> out(std::max((size_t)sz * 8 + 65536, (size_t)256 * 1024));
    char err[512];
    int rc = dylab_objc_lower(src.c_str(), out.data(), out.size(), err, sizeof(err));
    if (rc != 0) {
        set_err(err_buf, err_cap, err);
        return rc;
    }
    FILE *o = fopen(out_path, "wb");
    if (!o) {
        set_err(err_buf, err_cap, "cannot open output");
        return -1;
    }
    size_t n = strlen(out.data());
    if (fwrite(out.data(), 1, n, o) != n) {
        fclose(o);
        set_err(err_buf, err_cap, "write failed");
        return -1;
    }
    fclose(o);
    if (err_buf && err_cap) err_buf[0] = '\0';
    return 0;
}
