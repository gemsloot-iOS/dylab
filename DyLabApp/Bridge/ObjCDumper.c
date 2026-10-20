#include "ObjCDumper.h"
#include "DecompilerEngine.h"
#include "MachOMap.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <mach-o/nlist.h>
#else
struct nlist_64 {
    union {
        uint32_t n_strx;
    } n_un;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};
#define N_EXT 0x01
#define N_TYPE 0x0e
#define N_SECT 0x0e
#endif

#define METHOD_LIST_RELATIVE 0x80000000u

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JB;

static void
jb_init(JB *jb)
{
    jb->cap = 256 * 1024;
    jb->len = 0;
    jb->data = (char *)malloc(jb->cap);
    if (jb->data) {
        jb->data[0] = '\0';
    }
}

static int
jb_grow(JB *jb, size_t need)
{
    if (!jb->data) {
        return -1;
    }
    if (jb->len + need + 1 <= jb->cap) {
        return 0;
    }
    size_t ncap = jb->cap;
    while (jb->len + need + 1 > ncap) {
        ncap *= 2;
    }
    char *nd = (char *)realloc(jb->data, ncap);
    if (!nd) {
        return -1;
    }
    jb->data = nd;
    jb->cap = ncap;
    return 0;
}

static void
jb_puts(JB *jb, const char *s)
{
    size_t n;
    if (!s || jb_grow(jb, 0) != 0) {
        return;
    }
    n = strlen(s);
    if (jb_grow(jb, n) != 0) {
        return;
    }
    memcpy(jb->data + jb->len, s, n);
    jb->len += n;
    jb->data[jb->len] = '\0';
}

static void
jb_printf(JB *jb, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jb_puts(jb, tmp);
}

static void
jb_escape(JB *jb, const char *s)
{
    jb_puts(jb, "\"");
    if (!s) {
        jb_puts(jb, "\"");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[3] = {'\\', (char)*p, 0};
            jb_puts(jb, esc);
        } else if (*p < 0x20) {
            jb_printf(jb, "\\u%04x", *p);
        } else {
            char c[2] = {(char)*p, 0};
            jb_puts(jb, c);
        }
    }
    jb_puts(jb, "\"");
}

static uint64_t
ru64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static uint32_t
ru32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int32_t
ri32(const uint8_t *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static const char *
safe_name(const MachOMap *map, uint64_t vma)
{
    const char *s = macho_cstr_at(map, macho_fixup_ptr(map, vma));
    return s ? s : "";
}

/* class_t.data → class_ro_t (handles direct ro or class_rw_t.ro). */
static const uint8_t *
class_ro_for_class(const MachOMap *map, uint64_t class_vma, uint64_t *out_ro_vma)
{
    const uint8_t *cls;
    uint64_t data_ptr;
    const uint8_t *rw_or_ro;
    uint64_t ro_vma;
    const uint8_t *ro;

    cls = macho_vma_to_ptr(map, class_vma, 40);
    if (!cls) {
        return NULL;
    }
    {
        uint64_t raw;
        memcpy(&raw, cls + 32, 8);
        data_ptr = macho_fixup_ptr(map, raw);
    }
    rw_or_ro = macho_vma_to_ptr(map, data_ptr, 16);
    if (!rw_or_ro) {
        return NULL;
    }
    {
        uint64_t raw;
        memcpy(&raw, rw_or_ro + 8, 8);
        uint64_t maybe_ro = macho_fixup_ptr(map, raw);
        if (maybe_ro && macho_vma_to_ptr(map, maybe_ro, 48)) {
            ro_vma = maybe_ro;
        } else {
            ro_vma = data_ptr;
        }
    }
    ro = macho_vma_to_ptr(map, ro_vma, 48);
    if (!ro) {
        return NULL;
    }
    if (out_ro_vma) {
        *out_ro_vma = ro_vma;
    }
    return ro;
}

static void
dump_methods(JB *jb, const MachOMap *map, uint64_t method_list_vma, int *first)
{
    const uint8_t *base;
    uint32_t entsize_flags;
    uint32_t count;
    int relative;
    uint32_t entsize;

    if (method_list_vma == 0) {
        return;
    }
    base = macho_vma_to_ptr(map, method_list_vma, 8);
    if (!base) {
        return;
    }
    entsize_flags = ru32(base);
    count = ru32(base + 4);
    relative = (entsize_flags & METHOD_LIST_RELATIVE) != 0;
    entsize = entsize_flags & 0xFFFFu;
    if (entsize == 0) {
        entsize = relative ? 12 : 24;
    }
    if (count > 4096) {
        count = 4096;
    }

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *mp = base + 8 + (size_t)i * entsize;
        const char *name = "";
        const char *types = "";
        uint64_t imp = 0;
        uint64_t file_off = 0;

        if (relative) {
            if (!macho_vma_to_ptr(map, method_list_vma + 8 + (uint64_t)i * entsize,
                                  12)) {
                break;
            }
            int32_t name_rel = ri32(mp);
            int32_t types_rel = ri32(mp + 4);
            int32_t imp_rel = ri32(mp + 8);
            uint64_t name_addr =
                macho_strip_ptr(method_list_vma + 8 + (uint64_t)i * entsize) +
                (int64_t)name_rel;
            uint64_t types_addr =
                macho_strip_ptr(method_list_vma + 8 + (uint64_t)i * entsize + 4) +
                (int64_t)types_rel;
            uint64_t imp_addr =
                macho_strip_ptr(method_list_vma + 8 + (uint64_t)i * entsize + 8) +
                (int64_t)imp_rel;
            /* Relative name often points to a pointer (selref) or cstring. */
            {
                const uint8_t *np = macho_vma_to_ptr(map, name_addr, 8);
                if (np) {
                    uint64_t maybe = macho_fixup_ptr(map, ru64(np));
                    const char *cs = macho_cstr_at(map, maybe);
                    if (cs && cs[0]) {
                        name = cs;
                    } else {
                        name = safe_name(map, name_addr);
                    }
                } else {
                    name = safe_name(map, name_addr);
                }
            }
            types = safe_name(map, types_addr);
            imp = imp_addr;
        } else {
            if (!macho_vma_to_ptr(map, method_list_vma + 8 + (uint64_t)i * entsize,
                                  24)) {
                break;
            }
            name = safe_name(map, ru64(mp));
            types = safe_name(map, ru64(mp + 8));
            imp = macho_fixup_ptr(map, ru64(mp + 16));
        }

        macho_vma_to_fileoff(map, imp, &file_off);
        if (!*first) {
            jb_puts(jb, ",");
        }
        *first = 0;
        jb_puts(jb, "{\"name\":");
        jb_escape(jb, name);
        jb_puts(jb, ",\"types\":");
        jb_escape(jb, types);
        jb_printf(jb,
                  ",\"imp\":\"0x%llx\",\"file_offset\":%llu,\"size\":%llu}",
                  (unsigned long long)imp, (unsigned long long)file_off,
                  (unsigned long long)macho_function_size_at(map, imp));
    }
}

static void
dump_ivars(JB *jb, const MachOMap *map, uint64_t ivar_list_vma)
{
    const uint8_t *base;
    uint32_t entsize;
    uint32_t count;
    int first = 1;

    jb_puts(jb, "\"ivars\":[");
    if (ivar_list_vma == 0) {
        jb_puts(jb, "]");
        return;
    }
    base = macho_vma_to_ptr(map, ivar_list_vma, 8);
    if (!base) {
        jb_puts(jb, "]");
        return;
    }
    entsize = ru32(base) & 0xFFFFu;
    count = ru32(base + 4);
    if (entsize < 32) {
        entsize = 32;
    }
    if (count > 1024) {
        count = 1024;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *ip = base + 8 + (size_t)i * entsize;
        const char *name;
        const char *type;
        if (!macho_vma_to_ptr(map, ivar_list_vma + 8 + (uint64_t)i * entsize, 24)) {
            break;
        }
        /* ivar_t: offset*, name*, type*, alignment, size */
        name = safe_name(map, ru64(ip + 8));
        type = safe_name(map, ru64(ip + 16));
        if (!first) {
            jb_puts(jb, ",");
        }
        first = 0;
        jb_puts(jb, "{\"name\":");
        jb_escape(jb, name);
        jb_puts(jb, ",\"type\":");
        jb_escape(jb, type);
        jb_puts(jb, "}");
    }
    jb_puts(jb, "]");
}

static void
dump_properties(JB *jb, const MachOMap *map, uint64_t prop_list_vma)
{
    const uint8_t *base;
    uint32_t entsize;
    uint32_t count;
    int first = 1;

    jb_puts(jb, "\"properties\":[");
    if (prop_list_vma == 0) {
        jb_puts(jb, "]");
        return;
    }
    base = macho_vma_to_ptr(map, prop_list_vma, 8);
    if (!base) {
        jb_puts(jb, "]");
        return;
    }
    entsize = ru32(base);
    count = ru32(base + 4);
    if (entsize < 16) {
        entsize = 16;
    }
    if (count > 1024) {
        count = 1024;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *pp = base + 8 + (size_t)i * entsize;
        const char *name;
        const char *attrs;
        if (!macho_vma_to_ptr(map, prop_list_vma + 8 + (uint64_t)i * entsize, 16)) {
            break;
        }
        name = safe_name(map, ru64(pp));
        attrs = safe_name(map, ru64(pp + 8));
        if (!first) {
            jb_puts(jb, ",");
        }
        first = 0;
        jb_puts(jb, "{\"name\":");
        jb_escape(jb, name);
        jb_puts(jb, ",\"attributes\":");
        jb_escape(jb, attrs);
        jb_puts(jb, "}");
    }
    jb_puts(jb, "]");
}

static int
dump_one_class(JB *jb, const MachOMap *map, uint64_t class_vma, int *first_class)
{
    const uint8_t *cls;
    const uint8_t *ro;
    const char *cname;
    uint64_t methods_vma;
    uint64_t ivars_vma;
    uint64_t props_vma;
    uint64_t meta_vma;
    int first_method = 1;

    cls = macho_vma_to_ptr(map, class_vma, 40);
    if (!cls) {
        return -1;
    }
    ro = class_ro_for_class(map, class_vma, NULL);
    if (!ro) {
        return -1;
    }
    /* class_ro_t: flags, instanceStart, instanceSize, reserved, ivarLayout, name, baseMethods, ... */
    cname = safe_name(map, ru64(ro + 24));
    methods_vma = macho_fixup_ptr(map, ru64(ro + 32));
    ivars_vma = macho_fixup_ptr(map, ru64(ro + 48));
    props_vma = macho_fixup_ptr(map, ru64(ro + 64));
    /* class_t.isa → metaclass (class methods live there). */
    meta_vma = macho_fixup_ptr(map, ru64(cls));

    if (!*first_class) {
        jb_puts(jb, ",");
    }
    *first_class = 0;
    jb_puts(jb, "{\"name\":");
    jb_escape(jb, cname && cname[0] ? cname : "Unknown");
    jb_printf(jb, ",\"vma\":\"0x%llx\",", (unsigned long long)macho_strip_ptr(class_vma));
    jb_puts(jb, "\"methods\":[");
    dump_methods(jb, map, methods_vma, &first_method);
    if (meta_vma && meta_vma != class_vma) {
        const uint8_t *meta_ro = class_ro_for_class(map, meta_vma, NULL);
        if (meta_ro) {
            uint64_t class_methods =
                macho_fixup_ptr(map, ru64(meta_ro + 32));
            dump_methods(jb, map, class_methods, &first_method);
        }
    }
    jb_puts(jb, "],");
    dump_ivars(jb, map, ivars_vma);
    jb_puts(jb, ",");
    dump_properties(jb, map, props_vma);
    jb_puts(jb, "}");
    return 0;
}

static void
dump_callables(JB *jb, const MachOMap *map, const uint64_t *method_imps,
               int n_method_imps)
{
    MachOTextFn *fns;
    int n;
    int first = 1;

    jb_puts(jb, "\"callables\":[");
    fns = (MachOTextFn *)calloc(DYLAB_MAX_TEXT_FNS, sizeof(MachOTextFn));
    if (!fns) {
        jb_puts(jb, "]");
        return;
    }
    n = macho_collect_text_functions(map, method_imps, n_method_imps, fns,
                                     DYLAB_MAX_TEXT_FNS);
    for (int i = 0; i < n; i++) {
        if (!first) {
            jb_puts(jb, ",");
        }
        first = 0;
        jb_puts(jb, "{\"kind\":");
        jb_escape(jb, fns[i].kind[0] ? fns[i].kind : "symbol");
        jb_puts(jb, ",\"name\":");
        jb_escape(jb, fns[i].name);
        jb_printf(jb,
                  ",\"vma\":\"0x%llx\",\"file_offset\":%llu,\"size\":%llu}",
                  (unsigned long long)fns[i].vma,
                  (unsigned long long)fns[i].file_offset,
                  (unsigned long long)fns[i].size);
        if (jb->len > 400000) {
            break;
        }
    }
    free(fns);
    jb_puts(jb, "]");
}

/* Collect method IMP VMAs for size merging into callables. */
static void
collect_method_imps_from_list(const MachOMap *map, uint64_t method_list_vma,
                              uint64_t *out, int *n, int max)
{
    const uint8_t *base;
    uint32_t entsize_flags;
    uint32_t count;
    int relative;
    uint32_t entsize;

    if (method_list_vma == 0 || *n >= max) {
        return;
    }
    base = macho_vma_to_ptr(map, method_list_vma, 8);
    if (!base) {
        return;
    }
    entsize_flags = ru32(base);
    count = ru32(base + 4);
    relative = (entsize_flags & METHOD_LIST_RELATIVE) != 0;
    entsize = entsize_flags & 0xFFFFu;
    if (entsize == 0) {
        entsize = relative ? 12 : 24;
    }
    if (count > 4096) {
        count = 4096;
    }
    for (uint32_t i = 0; i < count && *n < max; i++) {
        const uint8_t *mp = base + 8 + (size_t)i * entsize;
        uint64_t imp = 0;
        if (relative) {
            int32_t imp_rel = ri32(mp + 8);
            imp = macho_strip_ptr(method_list_vma + 8 + (uint64_t)i * entsize +
                                  8) +
                  (int64_t)imp_rel;
        } else {
            if (!macho_vma_to_ptr(map, method_list_vma + 8 + (uint64_t)i * entsize,
                                  24)) {
                break;
            }
            imp = macho_fixup_ptr(map, ru64(mp + 16));
        }
        if (imp) {
            out[(*n)++] = imp;
        }
    }
}

int
objc_dump_classes_json(const char *path, char *out_buf, size_t out_buf_size)
{
    MachOMap map;
    JB jb;
    const MachOSect *classlist = NULL;
    int first_class = 1;
    uint64_t *method_imps = NULL;
    int n_imps = 0;

    if (!out_buf || out_buf_size == 0) {
        return -1;
    }
    out_buf[0] = '\0';
    if (macho_map_load(path, &map) != 0) {
        snprintf(out_buf, out_buf_size, "{\"error\":\"failed to load Mach-O\"}");
        return -1;
    }

    jb_init(&jb);
    if (!jb.data) {
        macho_map_free(&map);
        return -1;
    }

    method_imps = (uint64_t *)calloc(DYLAB_MAX_TEXT_FNS, sizeof(uint64_t));

    jb_puts(&jb, "{\"chained_fixups\":");
    jb_puts(&jb, map.has_chained_fixups ? "true" : "false");
    jb_puts(&jb, ",\"classes\":[");

    classlist = macho_find_sect(&map, "__DATA_CONST", "__objc_classlist");
    if (!classlist) {
        classlist = macho_find_sect(&map, "__DATA", "__objc_classlist");
    }
    if (classlist && classlist->size >= 8) {
        uint64_t count = classlist->size / 8;
        if (count > 4096) {
            count = 4096;
        }
        for (uint64_t i = 0; i < count; i++) {
            const uint8_t *slot =
                macho_vma_to_ptr(&map, classlist->addr + i * 8, 8);
            uint64_t class_vma;
            const uint8_t *cls;
            const uint8_t *ro;
            uint64_t methods_vma;
            if (!slot) {
                continue;
            }
            class_vma = macho_fixup_ptr(&map, ru64(slot));
            if (!class_vma) {
                continue;
            }
            dump_one_class(&jb, &map, class_vma, &first_class);

            /* Also gather IMPs for size merge */
            if (!method_imps) {
                continue;
            }
            cls = macho_vma_to_ptr(&map, class_vma, 40);
            if (!cls) {
                continue;
            }
            ro = class_ro_for_class(&map, class_vma, NULL);
            if (!ro) {
                continue;
            }
            methods_vma = macho_fixup_ptr(&map, ru64(ro + 32));
            collect_method_imps_from_list(&map, methods_vma, method_imps,
                                          &n_imps, DYLAB_MAX_TEXT_FNS);
            {
                uint64_t meta_vma = macho_fixup_ptr(&map, ru64(cls));
                const uint8_t *meta_ro;
                if (meta_vma && meta_vma != class_vma &&
                    (meta_ro = class_ro_for_class(&map, meta_vma, NULL))) {
                    uint64_t cm = macho_fixup_ptr(&map, ru64(meta_ro + 32));
                    collect_method_imps_from_list(&map, cm, method_imps,
                                                  &n_imps, DYLAB_MAX_TEXT_FNS);
                }
            }
        }
    }

    jb_puts(&jb, "],");
    dump_callables(&jb, &map, method_imps, n_imps);
    jb_puts(&jb, "}");

    snprintf(out_buf, out_buf_size, "%s", jb.data ? jb.data : "{}");
    free(jb.data);
    free(method_imps);
    macho_map_free(&map);
    return 0;
}

static int
ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int
is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static void
sanitize_ident(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    if (!in || !out || out_sz == 0) {
        return;
    }
    for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        if (is_ident_char(in[i])) {
            out[j++] = in[i];
        } else if (in[i] == ':' && j + 1 < out_sz) {
            out[j++] = '_';
        }
    }
    if (j == 0) {
        snprintf(out, out_sz, "Unknown");
        return;
    }
    out[j] = '\0';
    if (!isalpha((unsigned char)out[0]) && out[0] != '_') {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "C_%s", out);
        snprintf(out, out_sz, "%s", tmp);
    }
}

static void
emit_indented_body(FILE *f, const char *body)
{
    /* Skip leading signature lines from structured output; emit statements. */
    const char *p = body;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        /* Skip file-level comments / outer braces from decompiler wrapper */
        if (strncmp(line, "//", 2) == 0 || strncmp(line, "id ", 3) == 0 ||
            strcmp(line, "{") == 0 || strcmp(line, "}") == 0 ||
            (line[0] == '\0')) {
            /* keep structured comments that are useful */
            if (strncmp(line, "// Phase", 8) == 0 ||
                strncmp(line, "// CFG", 6) == 0) {
                fprintf(f, "    %s\n", line);
            }
        } else {
            fprintf(f, "    %s\n", line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static void
emit_method_body_from_decompile(FILE *f, const char *dylib_path, uint64_t imp,
                                uint64_t file_off)
{
    char *buf;
    uint64_t size;
    int rc;

    if (!dylib_path || imp == 0) {
        fprintf(f, "    return nil; /* no imp */\n");
        return;
    }
    buf = (char *)malloc(256 * 1024);
    if (!buf) {
        fprintf(f, "    return nil; /* OOM */\n");
        return;
    }
    size = 0; /* engine derives from function starts */
    rc = decompile_function(dylib_path, file_off, size, imp,
                            DECOMPILE_MODE_STRUCTURED, buf, 256 * 1024);
    if (rc != 0 && (!buf[0])) {
        fprintf(f, "    return nil; /* decompile failed */\n");
        free(buf);
        return;
    }
    fprintf(f, "    /* Phase 3B structured body */\n");
    emit_indented_body(f, buf);
    /* Ensure a return if decompiler didn't emit one at top-level */
    if (strstr(buf, "return") == NULL) {
        fprintf(f, "    return nil;\n");
    }
    free(buf);
}

/* Emit method signature pieces for skeleton (sel "foo:bar:" → -/+ (id)foo:(id)a0 bar:(id)a1). */
static void
emit_method_decl(FILE *f, const char *sel, int is_class_method)
{
    char buf[512];
    char *parts[32];
    int nparts = 0;
    char *tok;
    char *save = NULL;
    int arg = 0;
    const char kind = is_class_method ? '+' : '-';

    if (!sel || !sel[0]) {
        fprintf(f, "%c (id)dylab_unknown;\n", kind);
        return;
    }
    snprintf(buf, sizeof(buf), "%s", sel);
    tok = strtok_r(buf, ":", &save);
    while (tok && nparts < 32) {
        parts[nparts++] = tok;
        tok = strtok_r(NULL, ":", &save);
    }
    if (nparts == 1 && (!sel[strlen(sel) - 1] || strchr(sel, ':') == NULL)) {
        char idbuf[256];
        sanitize_ident(parts[0], idbuf, sizeof(idbuf));
        fprintf(f, "%c (id)%s;\n", kind, idbuf);
        return;
    }
    fprintf(f, "%c (id)", kind);
    for (int i = 0; i < nparts; i++) {
        char idbuf[256];
        sanitize_ident(parts[i], idbuf, sizeof(idbuf));
        if (i == 0) {
            fprintf(f, "%s:(id)a%d", idbuf, arg++);
        } else {
            fprintf(f, " %s:(id)a%d", idbuf, arg++);
        }
    }
    /* Trailing colon with empty final part already handled by strtok; if sel ends
       with ':' and last component empty, nparts still covers named pieces. */
    if (sel[strlen(sel) - 1] == ':' && nparts == 0) {
        fprintf(f, "dylab_sel:(id)a0");
    }
    fprintf(f, ";\n");
}

static void
emit_method_impl(FILE *f, const char *sel, int is_class_method,
                 const char *dylib_path, uint64_t imp, uint64_t file_off)
{
    char buf[512];
    char *parts[32];
    int nparts = 0;
    char *tok;
    char *save = NULL;
    int arg = 0;
    const char kind = is_class_method ? '+' : '-';

    if (!sel || !sel[0]) {
        fprintf(f, "%c (id)dylab_unknown {\n", kind);
        emit_method_body_from_decompile(f, dylib_path, imp, file_off);
        fprintf(f, "}\n");
        return;
    }
    snprintf(buf, sizeof(buf), "%s", sel);
    tok = strtok_r(buf, ":", &save);
    while (tok && nparts < 32) {
        parts[nparts++] = tok;
        tok = strtok_r(NULL, ":", &save);
    }
    if (nparts == 1 && strchr(sel, ':') == NULL) {
        char idbuf[256];
        sanitize_ident(parts[0], idbuf, sizeof(idbuf));
        fprintf(f, "%c (id)%s {\n", kind, idbuf);
        emit_method_body_from_decompile(f, dylib_path, imp, file_off);
        fprintf(f, "}\n");
        return;
    }
    fprintf(f, "%c (id)", kind);
    for (int i = 0; i < nparts; i++) {
        char idbuf[256];
        sanitize_ident(parts[i], idbuf, sizeof(idbuf));
        if (i == 0) {
            fprintf(f, "%s:(id)a%d", idbuf, arg++);
        } else {
            fprintf(f, " %s:(id)a%d", idbuf, arg++);
        }
    }
    fprintf(f, " {\n");
    emit_method_body_from_decompile(f, dylib_path, imp, file_off);
    fprintf(f, "}\n");
}

enum { MAX_SKEL_CLASSES = 64, MAX_SKEL_METHODS = 48 };

typedef struct {
    char sel[128];
    uint64_t imp;
    uint64_t off;
    int is_class; /* + method from metaclass */
} SkelMethod;

typedef struct {
    char name[96];
    SkelMethod methods[MAX_SKEL_METHODS];
    int nmethods;
} SkelClass;

static void
collect_methods_into(const MachOMap *map, uint64_t methods_vma, int is_class,
                     SkelClass *sc)
{
    const uint8_t *mbase;
    uint32_t entsize_flags;
    uint32_t mcount;
    int relative;
    uint32_t entsize;

    if (!methods_vma || sc->nmethods >= MAX_SKEL_METHODS) {
        return;
    }
    mbase = macho_vma_to_ptr(map, methods_vma, 8);
    if (!mbase) {
        return;
    }
    entsize_flags = ru32(mbase);
    mcount = ru32(mbase + 4);
    relative = (entsize_flags & METHOD_LIST_RELATIVE) != 0;
    entsize = entsize_flags & 0xFFFFu;
    if (entsize == 0) {
        entsize = relative ? 12 : 24;
    }
    if (mcount > 4096) {
        mcount = 4096;
    }
    for (uint32_t mi = 0; mi < mcount && sc->nmethods < MAX_SKEL_METHODS;
         mi++) {
        const uint8_t *mp = mbase + 8 + (size_t)mi * entsize;
        const char *name = "";
        uint64_t imp = 0;
        uint64_t foff = 0;
        SkelMethod *sm;

        if (relative) {
            int32_t name_rel = ri32(mp);
            int32_t imp_rel = ri32(mp + 8);
            uint64_t name_addr =
                macho_strip_ptr(methods_vma + 8 + (uint64_t)mi * entsize) +
                (int64_t)name_rel;
            uint64_t imp_addr =
                macho_strip_ptr(methods_vma + 8 + (uint64_t)mi * entsize + 8) +
                (int64_t)imp_rel;
            const uint8_t *np = macho_vma_to_ptr(map, name_addr, 8);
            if (np) {
                uint64_t maybe = macho_fixup_ptr(map, ru64(np));
                const char *cs = macho_cstr_at(map, maybe);
                name = (cs && cs[0]) ? cs : safe_name(map, name_addr);
            } else {
                name = safe_name(map, name_addr);
            }
            imp = imp_addr;
        } else {
            if (!macho_vma_to_ptr(map,
                                  methods_vma + 8 + (uint64_t)mi * entsize,
                                  24)) {
                break;
            }
            name = safe_name(map, ru64(mp));
            imp = macho_fixup_ptr(map, ru64(mp + 16));
        }
        macho_vma_to_fileoff(map, imp, &foff);
        if (!name || !name[0]) {
            continue;
        }
        sm = &sc->methods[sc->nmethods++];
        snprintf(sm->sel, sizeof(sm->sel), "%s", name);
        sm->imp = imp;
        sm->off = foff;
        sm->is_class = is_class;
    }
}

static void
write_skeleton_from_map(FILE *hf, FILE *tf, MachOMap *map, const char *path)
{
    const MachOSect *classlist;
    int class_count = 0;
    SkelClass *classes;

    classes = (SkelClass *)calloc(MAX_SKEL_CLASSES, sizeof(SkelClass));
    if (!classes) {
        fprintf(hf,
                "@interface DyLabHookTarget : NSObject\n"
                "- (id)init;\n"
                "@end\n");
        fprintf(tf,
                "@implementation DyLabHookTarget\n"
                "- (id)init { return [super init]; }\n"
                "@end\n");
        return;
    }

    classlist = macho_find_sect(map, "__DATA_CONST", "__objc_classlist");
    if (!classlist) {
        classlist = macho_find_sect(map, "__DATA", "__objc_classlist");
    }

    if (classlist && classlist->size >= 8) {
        uint64_t count = classlist->size / 8;
        if (count > MAX_SKEL_CLASSES) {
            count = MAX_SKEL_CLASSES;
        }
        for (uint64_t i = 0; i < count; i++) {
            const uint8_t *slot =
                macho_vma_to_ptr(map, classlist->addr + i * 8, 8);
            uint64_t class_vma;
            const uint8_t *cls;
            const uint8_t *ro;
            const char *cname;
            SkelClass *sc;

            if (!slot) {
                continue;
            }
            class_vma = macho_fixup_ptr(map, ru64(slot));
            if (!class_vma) {
                continue;
            }
            cls = macho_vma_to_ptr(map, class_vma, 40);
            if (!cls) {
                continue;
            }
            ro = class_ro_for_class(map, class_vma, NULL);
            if (!ro) {
                continue;
            }
            cname = safe_name(map, ru64(ro + 24));
            if (!cname || !cname[0]) {
                cname = "Unknown";
            }
            sc = &classes[class_count];
            sanitize_ident(cname, sc->name, sizeof(sc->name));
            sc->nmethods = 0;
            collect_methods_into(map, macho_fixup_ptr(map, ru64(ro + 32)), 0,
                                 sc);
            {
                uint64_t meta_vma = macho_fixup_ptr(map, ru64(cls));
                const uint8_t *meta_ro;
                if (meta_vma && meta_vma != class_vma &&
                    (meta_ro = class_ro_for_class(map, meta_vma, NULL))) {
                    collect_methods_into(
                        map, macho_fixup_ptr(map, ru64(meta_ro + 32)), 1, sc);
                }
            }
            class_count++;
        }
    }

    fprintf(hf,
            "/* Auto-generated by DyLab ObjCDumper — Path C ready */\n"
            "#import <Foundation/Foundation.h>\n\n");
    fprintf(tf,
            "/* Auto-generated by DyLab ObjCDumper — Path C ready */\n"
            "#import \"TweakClasses.h\"\n"
            "#import <Foundation/Foundation.h>\n\n"
            "/*\n"
            " * Tweak with structured decompiler method bodies.\n"
            " * Compile via Path C: Lower → TinyCC → Mach-O → sign → .deb\n"
            " * Source binary: %s\n"
            " */\n\n"
            "__attribute__((constructor))\n"
            "static void dylab_tweak_init(void) {\n"
            "    NSLog(@\"[DyLab] Tweak loaded\");\n"
            "}\n\n",
            path);

    if (class_count == 0) {
        /* Fallback only when the binary has no discoverable ObjC classes. */
        fprintf(hf,
                "@interface DyLabHookTarget : NSObject\n"
                "- (id)init;\n"
                "@end\n");
        fprintf(tf,
                "@implementation DyLabHookTarget\n"
                "- (id)init {\n"
                "    self = [super init];\n"
                "    return self;\n"
                "}\n"
                "@end\n");
        free(classes);
        return;
    }

    for (int i = 0; i < class_count; i++) {
        SkelClass *sc = &classes[i];
        fprintf(hf, "@interface %s : NSObject\n", sc->name);
        for (int m = 0; m < sc->nmethods; m++) {
            emit_method_decl(hf, sc->methods[m].sel, sc->methods[m].is_class);
        }
        if (sc->nmethods == 0) {
            fprintf(hf, "- (id)init;\n");
        }
        fprintf(hf, "@end\n\n");

        fprintf(tf, "@implementation %s\n", sc->name);
        for (int m = 0; m < sc->nmethods; m++) {
            emit_method_impl(tf, sc->methods[m].sel, sc->methods[m].is_class,
                             path, sc->methods[m].imp, sc->methods[m].off);
        }
        if (sc->nmethods == 0) {
            fprintf(tf,
                    "- (id)init {\n"
                    "    self = [super init];\n"
                    "    return self;\n"
                    "}\n");
        }
        fprintf(tf, "@end\n\n");
    }
    free(classes);
}

int
objc_generate_header_and_skeleton(const char *path,
                                  const char *out_dir,
                                  char *out_header_path,
                                  size_t out_header_path_size,
                                  char *out_tweak_path,
                                  size_t out_tweak_path_size,
                                  char *err_buf,
                                  size_t err_buf_size)
{
    MachOMap map;
    char header_path[1024];
    char tweak_path[1024];
    FILE *hf;
    FILE *tf;

    if (!path || !out_dir) {
        if (err_buf && err_buf_size) {
            snprintf(err_buf, err_buf_size, "null argument");
        }
        return -1;
    }
    if (ensure_dir(out_dir) != 0) {
        if (err_buf && err_buf_size) {
            snprintf(err_buf, err_buf_size, "cannot create out_dir");
        }
        return -1;
    }

    if (macho_map_load(path, &map) != 0) {
        if (err_buf && err_buf_size) {
            snprintf(err_buf, err_buf_size, "failed to load Mach-O");
        }
        return -1;
    }

    snprintf(header_path, sizeof(header_path), "%s/TweakClasses.h", out_dir);
    snprintf(tweak_path, sizeof(tweak_path), "%s/Tweak.m", out_dir);

    hf = fopen(header_path, "w");
    tf = fopen(tweak_path, "w");
    if (!hf || !tf) {
        if (hf) {
            fclose(hf);
        }
        if (tf) {
            fclose(tf);
        }
        macho_map_free(&map);
        if (err_buf && err_buf_size) {
            snprintf(err_buf, err_buf_size, "failed to write skeleton files");
        }
        return -1;
    }

    write_skeleton_from_map(hf, tf, &map, path);
    fclose(hf);
    fclose(tf);
    macho_map_free(&map);

    if (out_header_path && out_header_path_size) {
        snprintf(out_header_path, out_header_path_size, "%s", header_path);
    }
    if (out_tweak_path && out_tweak_path_size) {
        snprintf(out_tweak_path, out_tweak_path_size, "%s", tweak_path);
    }
    if (err_buf && err_buf_size) {
        err_buf[0] = '\0';
    }
    return 0;
}
