#include "MachOInspector.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#else
#define MH_MAGIC_64 0xfeedfacf
#define MH_CIGAM_64 0xcffaedfe
#define FAT_MAGIC 0xcafebabe
#define FAT_CIGAM 0xbebafeca
#define CPU_TYPE_ARM64 ((cpu_type_t)0x0100000c)
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_LOAD_DYLIB 0xc
#define LC_ID_DYLIB 0xd
#define LC_LOAD_WEAK_DYLIB 0x18
#define LC_RPATH 0x1c
#define LC_REEXPORT_DYLIB 0x1f
#define N_EXT 0x01
#define N_UNDF 0x0
#define N_TYPE 0x0e
typedef int cpu_type_t;
typedef int cpu_subtype_t;
struct mach_header_64 {
    uint32_t magic;
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};
struct fat_header {
    uint32_t magic;
    uint32_t nfat_arch;
};
struct fat_arch {
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    uint32_t offset;
    uint32_t size;
    uint32_t align;
};
struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};
struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};
struct section_64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};
struct dylib {
    uint32_t name;
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
};
struct dylib_command {
    uint32_t cmd;
    uint32_t cmdsize;
    struct dylib dylib;
};
struct rpath_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t path;
};
struct symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};
struct nlist_64 {
    union {
        uint32_t n_strx;
    } n_un;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};
static uint32_t
OSSwapInt32(uint32_t x)
{
    return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) |
           (x >> 24);
}
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

static void
jb_init(JsonBuf *jb)
{
    jb->cap = 64 * 1024;
    jb->len = 0;
    jb->data = (char *)malloc(jb->cap);
    if (jb->data) {
        jb->data[0] = '\0';
    }
}

static int
jb_reserve(JsonBuf *jb, size_t need)
{
    if (jb->data == NULL) {
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
    if (nd == NULL) {
        return -1;
    }
    jb->data = nd;
    jb->cap = ncap;
    return 0;
}

static void
jb_puts(JsonBuf *jb, const char *s)
{
    size_t n;
    if (s == NULL || jb_reserve(jb, 0) != 0) {
        return;
    }
    n = strlen(s);
    if (jb_reserve(jb, n) != 0) {
        return;
    }
    memcpy(jb->data + jb->len, s, n);
    jb->len += n;
    jb->data[jb->len] = '\0';
}

static void
jb_printf(JsonBuf *jb, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    jb_puts(jb, tmp);
}

static void
jb_escape(JsonBuf *jb, const char *s)
{
    jb_puts(jb, "\"");
    if (s == NULL) {
        jb_puts(jb, "\"");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[3] = { '\\', (char)*p, 0 };
            jb_puts(jb, esc);
        } else if (*p < 0x20) {
            jb_printf(jb, "\\u%04x", *p);
        } else {
            char c[2] = { (char)*p, 0 };
            jb_puts(jb, c);
        }
    }
    jb_puts(jb, "\"");
}

static int
is_objc_section(const char *sectname)
{
    return strcmp(sectname, "__objc_classlist") == 0 ||
           strcmp(sectname, "__objc_methname") == 0 ||
           strcmp(sectname, "__objc_selrefs") == 0 ||
           strcmp(sectname, "__objc_classname") == 0 ||
           strcmp(sectname, "__objc_methtype") == 0;
}

static uint32_t
u32_native(uint32_t v, int swap)
{
    return swap ? OSSwapInt32(v) : v;
}

static const char *
filetype_name(uint32_t filetype)
{
    switch (filetype) {
        case 0x1:
            return "MH_OBJECT";
        case 0x2:
            return "MH_EXECUTE";
        case 0x6:
            return "MH_DYLIB";
        case 0x8:
            return "MH_BUNDLE";
        case 0x9:
            return "MH_DYLIB_STUB";
        default:
            return "UNKNOWN";
    }
}

static const char *
dylib_cmd_name(uint32_t cmd)
{
    switch (cmd) {
        case LC_ID_DYLIB:
            return "LC_ID_DYLIB";
        case LC_LOAD_WEAK_DYLIB:
            return "LC_LOAD_WEAK_DYLIB";
        case LC_REEXPORT_DYLIB:
            return "LC_REEXPORT_DYLIB";
        default:
            return "LC_LOAD_DYLIB";
    }
}

static int
find_arm64_slice(const uint8_t *file, size_t file_len, size_t *out_off,
                 size_t *out_size)
{
    const struct fat_header *fh;
    uint32_t magic;
    int swap = 0;
    uint32_t nfat;
    const struct fat_arch *archs;

    if (file_len < sizeof(struct fat_header)) {
        return -1;
    }
    fh = (const struct fat_header *)file;
    magic = fh->magic;
    if (magic == FAT_CIGAM) {
        swap = 1;
    } else if (magic != FAT_MAGIC) {
        return -1;
    }

    nfat = u32_native(fh->nfat_arch, swap);
    if (sizeof(struct fat_header) + (size_t)nfat * sizeof(struct fat_arch) >
        file_len) {
        return -1;
    }

    archs = (const struct fat_arch *)(file + sizeof(struct fat_header));
    for (uint32_t i = 0; i < nfat; i++) {
        cpu_type_t cputype =
            (cpu_type_t)u32_native((uint32_t)archs[i].cputype, swap);
        uint32_t offset = u32_native(archs[i].offset, swap);
        uint32_t size = u32_native(archs[i].size, swap);
        if (cputype == CPU_TYPE_ARM64) {
            if ((size_t)offset + size > file_len) {
                return -1;
            }
            *out_off = offset;
            *out_size = size;
            return 0;
        }
    }
    return -1;
}

static int
inspect_thin(const uint8_t *base, size_t len, int is_fat_slice, JsonBuf *jb)
{
    const struct mach_header_64 *mh;
    int swap = 0;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t filetype;
    uint32_t flags;
    cpu_type_t cputype;
    const uint8_t *cursor;
    const uint8_t *cmds_end;
    const uint8_t *scan;
    int first;
    JsonBuf exports;
    JsonBuf imports;

    if (len < sizeof(struct mach_header_64)) {
        return -1;
    }

    mh = (const struct mach_header_64 *)base;
    if (mh->magic == MH_CIGAM_64) {
        swap = 1;
    } else if (mh->magic != MH_MAGIC_64) {
        return -1;
    }

    ncmds = u32_native(mh->ncmds, swap);
    sizeofcmds = u32_native(mh->sizeofcmds, swap);
    filetype = u32_native(mh->filetype, swap);
    flags = u32_native(mh->flags, swap);
    cputype = (cpu_type_t)u32_native((uint32_t)mh->cputype, swap);

    cursor = base + sizeof(struct mach_header_64);
    cmds_end = cursor + sizeofcmds;
    if ((size_t)(cmds_end - base) > len) {
        return -1;
    }

    jb_puts(jb, "{");
    jb_printf(jb, "\"fat\":%s,", is_fat_slice ? "true" : "false");
    if (is_fat_slice) {
        jb_puts(jb, "\"slice\":\"arm64\",");
    }
    jb_puts(jb, "\"header\":{");
    jb_printf(jb, "\"magic\":\"0x%08x\",",
              (unsigned)(swap ? OSSwapInt32(mh->magic) : mh->magic));
    jb_printf(jb, "\"cputype\":%d,", (int)cputype);
    jb_puts(jb, "\"filetype\":");
    jb_escape(jb, filetype_name(filetype));
    jb_printf(jb, ",\"ncmds\":%u,", ncmds);
    jb_printf(jb, "\"sizeofcmds\":%u,", sizeofcmds);
    jb_printf(jb, "\"flags\":\"0x%x\"", flags);
    jb_puts(jb, "},");

    jb_puts(jb, "\"load_dylibs\":[");
    first = 1;
    scan = cursor;
    for (uint32_t i = 0; i < ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)scan;
        uint32_t cmd = u32_native(lc->cmd, swap);
        uint32_t cmdsize = u32_native(lc->cmdsize, swap);
        if (cmdsize < sizeof(struct load_command) || scan + cmdsize > cmds_end) {
            break;
        }
        if (cmd == LC_LOAD_DYLIB || cmd == LC_ID_DYLIB ||
            cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB) {
            const struct dylib_command *dc = (const struct dylib_command *)scan;
            uint32_t name_off = u32_native(dc->dylib.name, swap);
            const char *name =
                (name_off < cmdsize) ? (const char *)(scan + name_off) : "";
            if (!first) {
                jb_puts(jb, ",");
            }
            first = 0;
            jb_puts(jb, "{\"cmd\":");
            jb_escape(jb, dylib_cmd_name(cmd));
            jb_puts(jb, ",\"name\":");
            jb_escape(jb, name);
            jb_puts(jb, "}");
        }
        scan += cmdsize;
    }
    jb_puts(jb, "],");

    jb_puts(jb, "\"rpaths\":[");
    first = 1;
    scan = cursor;
    for (uint32_t i = 0; i < ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)scan;
        uint32_t cmd = u32_native(lc->cmd, swap);
        uint32_t cmdsize = u32_native(lc->cmdsize, swap);
        if (cmdsize < sizeof(struct load_command) || scan + cmdsize > cmds_end) {
            break;
        }
        if (cmd == LC_RPATH) {
            const struct rpath_command *rc = (const struct rpath_command *)scan;
            uint32_t path_off = u32_native(rc->path, swap);
            const char *path =
                (path_off < cmdsize) ? (const char *)(scan + path_off) : "";
            if (!first) {
                jb_puts(jb, ",");
            }
            first = 0;
            jb_escape(jb, path);
        }
        scan += cmdsize;
    }
    jb_puts(jb, "],");

    jb_puts(jb, "\"segments\":[");
    first = 1;
    scan = cursor;
    for (uint32_t i = 0; i < ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)scan;
        uint32_t cmd = u32_native(lc->cmd, swap);
        uint32_t cmdsize = u32_native(lc->cmdsize, swap);
        if (cmdsize < sizeof(struct load_command) || scan + cmdsize > cmds_end) {
            break;
        }
        if (cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg =
                (const struct segment_command_64 *)scan;
            char segname[17];
            uint32_t nsects = u32_native(sg->nsects, swap);
            const struct section_64 *sect =
                (const struct section_64 *)(scan +
                                            sizeof(struct segment_command_64));

            memset(segname, 0, sizeof(segname));
            memcpy(segname, sg->segname, 16);
            if (!first) {
                jb_puts(jb, ",");
            }
            first = 0;
            jb_puts(jb, "{\"name\":");
            jb_escape(jb, segname);
            jb_printf(jb, ",\"vmaddr\":\"0x%llx\",",
                      (unsigned long long)sg->vmaddr);
            jb_printf(jb, "\"vmsize\":\"0x%llx\",",
                      (unsigned long long)sg->vmsize);
            jb_puts(jb, "\"sections\":[");
            for (uint32_t s = 0; s < nsects; s++) {
                char sectname[17];
                memset(sectname, 0, sizeof(sectname));
                memcpy(sectname, sect[s].sectname, 16);
                if (s > 0) {
                    jb_puts(jb, ",");
                }
                jb_puts(jb, "{\"name\":");
                jb_escape(jb, sectname);
                jb_printf(jb, ",\"size\":%llu,",
                          (unsigned long long)sect[s].size);
                jb_puts(jb, "\"objc\":");
                jb_puts(jb, is_objc_section(sectname) ? "true" : "false");
                jb_puts(jb, "}");
            }
            jb_puts(jb, "]}");
        }
        scan += cmdsize;
    }
    jb_puts(jb, "],");

    jb_init(&exports);
    jb_init(&imports);
    jb_puts(&exports, "[");
    jb_puts(&imports, "[");
    first = 1;
    {
        int first_imp = 1;
        scan = cursor;
        for (uint32_t i = 0; i < ncmds; i++) {
            const struct load_command *lc = (const struct load_command *)scan;
            uint32_t cmd = u32_native(lc->cmd, swap);
            uint32_t cmdsize = u32_native(lc->cmdsize, swap);
            if (cmdsize < sizeof(struct load_command) ||
                scan + cmdsize > cmds_end) {
                break;
            }
            if (cmd == LC_SYMTAB) {
                const struct symtab_command *st =
                    (const struct symtab_command *)scan;
                uint32_t symoff = u32_native(st->symoff, swap);
                uint32_t nsyms = u32_native(st->nsyms, swap);
                uint32_t stroff = u32_native(st->stroff, swap);
                uint32_t strsize = u32_native(st->strsize, swap);
                const struct nlist_64 *syms;
                const char *strtab;
                uint32_t max_syms;

                if ((size_t)symoff + (size_t)nsyms * sizeof(struct nlist_64) >
                        len ||
                    (size_t)stroff + strsize > len) {
                    break;
                }
                syms = (const struct nlist_64 *)(base + symoff);
                strtab = (const char *)(base + stroff);
                max_syms = nsyms > 500 ? 500 : nsyms;
                for (uint32_t s = 0; s < max_syms; s++) {
                    uint32_t strx = u32_native(syms[s].n_un.n_strx, swap);
                    const char *name;
                    uint8_t type;
                    int is_undef;
                    int is_ext;

                    if (strx >= strsize) {
                        continue;
                    }
                    name = strtab + strx;
                    if (name[0] == '\0') {
                        continue;
                    }
                    type = syms[s].n_type;
                    is_undef = (type & N_TYPE) == N_UNDF;
                    is_ext = (type & N_EXT) != 0;
                    if (is_undef && is_ext) {
                        if (!first_imp) {
                            jb_puts(&imports, ",");
                        }
                        first_imp = 0;
                        jb_escape(&imports, name);
                    } else if (!is_undef && is_ext) {
                        if (!first) {
                            jb_puts(&exports, ",");
                        }
                        first = 0;
                        jb_escape(&exports, name);
                    }
                }
            }
            scan += cmdsize;
        }
    }
    jb_puts(&exports, "]");
    jb_puts(&imports, "]");

    jb_puts(jb, "\"exports\":");
    jb_puts(jb, exports.data ? exports.data : "[]");
    jb_puts(jb, ",\"imports\":");
    jb_puts(jb, imports.data ? imports.data : "[]");
    jb_puts(jb, "}");

    free(exports.data);
    free(imports.data);
    return jb->data ? 0 : -1;
}

int
inspect_macho(const char *path, char *out_buf, size_t out_buf_size)
{
    FILE *fp;
    long sz;
    uint8_t *file = NULL;
    JsonBuf jb;
    size_t thin_off = 0;
    size_t thin_size = 0;
    const uint8_t *thin;
    size_t thin_len;
    int is_fat = 0;
    int rc;

    if (out_buf == NULL || out_buf_size == 0) {
        return -1;
    }
    out_buf[0] = '\0';

    fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(out_buf, out_buf_size, "{\"error\":\"fopen failed: %s\"}",
                 strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) <= 0) {
        fclose(fp);
        snprintf(out_buf, out_buf_size, "{\"error\":\"invalid file size\"}");
        return -1;
    }
    rewind(fp);
    file = (uint8_t *)malloc((size_t)sz);
    if (file == NULL) {
        fclose(fp);
        snprintf(out_buf, out_buf_size, "{\"error\":\"oom\"}");
        return -1;
    }
    if (fread(file, 1, (size_t)sz, fp) != (size_t)sz) {
        free(file);
        fclose(fp);
        snprintf(out_buf, out_buf_size, "{\"error\":\"read failed\"}");
        return -1;
    }
    fclose(fp);

    thin = file;
    thin_len = (size_t)sz;
    if (find_arm64_slice(file, (size_t)sz, &thin_off, &thin_size) == 0) {
        thin = file + thin_off;
        thin_len = thin_size;
        is_fat = 1;
    }

    jb_init(&jb);
    if (jb.data == NULL) {
        free(file);
        snprintf(out_buf, out_buf_size, "{\"error\":\"oom\"}");
        return -1;
    }

    rc = inspect_thin(thin, thin_len, is_fat, &jb);
    if (rc != 0 || jb.data == NULL) {
        free(jb.data);
        free(file);
        snprintf(out_buf, out_buf_size,
                 "{\"error\":\"not a supported Mach-O binary\"}");
        return -1;
    }

    snprintf(out_buf, out_buf_size, "%s", jb.data);
    free(jb.data);
    free(file);
    return 0;
}
