#include "DecompilerEngine.h"
#include "MachOMap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

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
#endif

#if __has_include(<capstone/capstone.h>)
#include <capstone/capstone.h>
#define DYLAB_HAVE_CAPSTONE 1
#elif __has_include("capstone/capstone.h")
#include "capstone/capstone.h"
#define DYLAB_HAVE_CAPSTONE 1
#else
#define DYLAB_HAVE_CAPSTONE 0
#endif

#if DYLAB_HAVE_CAPSTONE
#if defined(CS_ARCH_AARCH64)
#define DYLAB_CS_ARM64 CS_ARCH_AARCH64
#else
#define DYLAB_CS_ARM64 CS_ARCH_ARM64
#endif
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} SB;

static void
sb_init(SB *sb)
{
    sb->cap = 128 * 1024;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static int
sb_grow(SB *sb, size_t need)
{
    if (!sb->data) {
        return -1;
    }
    if (sb->len + need + 1 <= sb->cap) {
        return 0;
    }
    size_t ncap = sb->cap;
    while (sb->len + need + 1 > ncap) {
        ncap *= 2;
    }
    char *nd = (char *)realloc(sb->data, ncap);
    if (!nd) {
        return -1;
    }
    sb->data = nd;
    sb->cap = ncap;
    return 0;
}

static void
sb_puts(SB *sb, const char *s)
{
    size_t n;
    if (!s || sb_grow(sb, 0) != 0) {
        return;
    }
    n = strlen(s);
    if (sb_grow(sb, n) != 0) {
        return;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void
sb_printf(SB *sb, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_puts(sb, tmp);
}

static const char *
lookup_cstr(const MachOMap *map, uint64_t vma)
{
    return macho_cstr_at(map, macho_strip_ptr(vma));
}

static const char *
lookup_stub_symbol(const MachOMap *map, uint64_t vma, char *tmp, size_t tmp_size)
{
    const MachOSect *stubs;
    uint64_t addr = macho_strip_ptr(vma);
    uint32_t stub_size;
    uint64_t index;
    uint32_t ind_index;
    const uint32_t *indirect;
    uint32_t symindex;
    const struct nlist_64 *syms;
    const char *strtab;
    const char *name;

    stubs = macho_find_sect(map, "__TEXT", "__stubs");
    if (!stubs || !map->dysym_nindirectsyms || !map->dysym_indirectsymoff) {
        return NULL;
    }
    if (addr < stubs->addr || addr >= stubs->addr + stubs->size) {
        return NULL;
    }
    stub_size = stubs->reserved2 ? stubs->reserved2 : 12;
    if (stub_size == 0) {
        stub_size = 12;
    }
    index = (addr - stubs->addr) / stub_size;
    ind_index = stubs->reserved1 + (uint32_t)index;
    if (ind_index >= map->dysym_nindirectsyms) {
        return NULL;
    }
    if ((size_t)map->dysym_indirectsymoff +
            (size_t)(ind_index + 1) * sizeof(uint32_t) >
        map->thin_len) {
        return NULL;
    }
    indirect = (const uint32_t *)(map->file + map->thin_off +
                                  map->dysym_indirectsymoff);
    symindex = indirect[ind_index];
    if (symindex & 0x80000000u) {
        return NULL; /* INDIRECT_SYMBOL_LOCAL / ABS */
    }
    if (!map->nsyms || !map->symoff || !map->stroff ||
        symindex >= map->nsyms) {
        return NULL;
    }
    if ((size_t)map->symoff + (size_t)map->nsyms * sizeof(struct nlist_64) >
            map->thin_len ||
        (size_t)map->stroff + map->strsize > map->thin_len) {
        return NULL;
    }
    syms = (const struct nlist_64 *)(map->file + map->thin_off + map->symoff);
    strtab = (const char *)(map->file + map->thin_off + map->stroff);
    if (syms[symindex].n_un.n_strx >= map->strsize) {
        return NULL;
    }
    name = strtab + syms[symindex].n_un.n_strx;
    if (!name[0]) {
        return NULL;
    }
    snprintf(tmp, tmp_size, "%s", name);
    return tmp;
}

static const char *
annotate_ptr(const MachOMap *map, uint64_t vma, char *tmp, size_t tmp_size)
{
    const MachOSect *cfstring;
    const MachOSect *selrefs;
    const MachOSect *classrefs;
    const MachOSect *cstring;
    uint64_t addr = macho_strip_ptr(vma);
    const char *s;

    cstring = macho_find_sect(map, "__TEXT", "__cstring");
    if (cstring && addr >= cstring->addr &&
        addr < cstring->addr + cstring->size) {
        s = lookup_cstr(map, addr);
        if (s) {
            snprintf(tmp, tmp_size, "\"%s\"", s);
            return tmp;
        }
    }

    selrefs = macho_find_sect(map, "__DATA", "__objc_selrefs");
    if (!selrefs) {
        selrefs = macho_find_sect(map, "__DATA_CONST", "__objc_selrefs");
    }
    if (selrefs && addr >= selrefs->addr &&
        addr < selrefs->addr + selrefs->size) {
        const uint8_t *p = macho_vma_to_ptr(map, addr, 8);
        if (p) {
            uint64_t sel;
            memcpy(&sel, p, 8);
            s = lookup_cstr(map, sel);
            if (s) {
                snprintf(tmp, tmp_size, "@selector(%s)", s);
                return tmp;
            }
        }
        snprintf(tmp, tmp_size, "selref_0x%llx", (unsigned long long)addr);
        return tmp;
    }

    classrefs = macho_find_sect(map, "__DATA", "__objc_classrefs");
    if (!classrefs) {
        classrefs = macho_find_sect(map, "__DATA_CONST", "__objc_classrefs");
    }
    if (classrefs && addr >= classrefs->addr &&
        addr < classrefs->addr + classrefs->size) {
        snprintf(tmp, tmp_size, "classref_0x%llx", (unsigned long long)addr);
        return tmp;
    }

    cfstring = macho_find_sect(map, "__DATA", "__cfstring");
    if (!cfstring) {
        cfstring = macho_find_sect(map, "__DATA_CONST", "__cfstring");
    }
    if (cfstring && addr >= cfstring->addr &&
        addr < cfstring->addr + cfstring->size) {
        /* CFString literal layout: isa, flags, pointer, length */
        const uint8_t *p = macho_vma_to_ptr(map, addr, 32);
        if (p) {
            uint64_t strp;
            memcpy(&strp, p + 16, 8);
            s = lookup_cstr(map, strp);
            if (s) {
                snprintf(tmp, tmp_size, "@\"%s\"", s);
                return tmp;
            }
        }
    }

    /* Stub targets via LC_DYSYMTAB indirect symbols. */
    if (lookup_stub_symbol(map, addr, tmp, tmp_size)) {
        return tmp;
    }

    /* Exact symbol match from LC_SYMTAB. */
    if (map->nsyms && map->symoff && map->stroff) {
        const struct nlist_64 *syms =
            (const struct nlist_64 *)(map->file + map->thin_off + map->symoff);
        const char *strtab =
            (const char *)(map->file + map->thin_off + map->stroff);
        uint32_t max = map->nsyms > 4000 ? 4000 : map->nsyms;
        if ((size_t)map->symoff + (size_t)map->nsyms * sizeof(struct nlist_64) <=
                map->thin_len &&
            (size_t)map->stroff + map->strsize <= map->thin_len) {
            for (uint32_t i = 0; i < max; i++) {
                if (macho_strip_ptr(syms[i].n_value) == addr &&
                    syms[i].n_un.n_strx < map->strsize) {
                    const char *name = strtab + syms[i].n_un.n_strx;
                    if (name[0]) {
                        snprintf(tmp, tmp_size, "%s", name);
                        return tmp;
                    }
                }
            }
        }
    }

    tmp[0] = '\0';
    return NULL;
}

#if DYLAB_HAVE_CAPSTONE

static int
emit_disasm(SB *sb, const MachOMap *map, const uint8_t *code, size_t code_size,
            uint64_t address)
{
    csh handle;
    cs_insn *insn;
    size_t count;
    uint64_t xregs[32];
    int x_valid[32];
    char ann[256];

    memset(xregs, 0, sizeof(xregs));
    memset(x_valid, 0, sizeof(x_valid));

    if (cs_open(DYLAB_CS_ARM64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
        sb_puts(sb, "; error: cs_open failed\n");
        return -1;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

    count = cs_disasm(handle, code, code_size, address, 0, &insn);
    if (count == 0) {
        sb_puts(sb, "; error: cs_disasm produced no instructions\n");
        cs_close(&handle);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        const char *mnem = insn[i].mnemonic;
        const char *ops = insn[i].op_str;
        ann[0] = '\0';

        /* Track ADRP */
        if (strcmp(mnem, "adrp") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_IMM) {
                unsigned reg = a64->operands[0].reg - ARM64_REG_X0;
                if (reg < 32) {
                    xregs[reg] = (uint64_t)a64->operands[1].imm;
                    x_valid[reg] = 1;
                }
            }
        }
        /* ADD Xd, Xn, #imm after ADRP */
        if ((strcmp(mnem, "add") == 0) && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_IMM) {
                unsigned rd = a64->operands[0].reg - ARM64_REG_X0;
                unsigned rn = a64->operands[1].reg - ARM64_REG_X0;
                if (rd < 32 && rn < 32 && x_valid[rn]) {
                    xregs[rd] = xregs[rn] + (uint64_t)a64->operands[2].imm;
                    x_valid[rd] = 1;
                    annotate_ptr(map, xregs[rd], ann, sizeof(ann));
                }
            }
        }
        /* LDR Xt, [Xn, #imm] */
        if ((strcmp(mnem, "ldr") == 0) && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = a64->operands[0].reg - ARM64_REG_X0;
                unsigned rn = a64->operands[1].mem.base - ARM64_REG_X0;
                int64_t disp = a64->operands[1].mem.disp;
                if (rt < 32 && rn < 32 && x_valid[rn]) {
                    uint64_t target = xregs[rn] + (uint64_t)disp;
                    const uint8_t *p = macho_vma_to_ptr(map, target, 8);
                    if (p) {
                        uint64_t val;
                        memcpy(&val, p, 8);
                        xregs[rt] = macho_strip_ptr(val);
                        x_valid[rt] = 1;
                        annotate_ptr(map, xregs[rt], ann, sizeof(ann));
                    } else {
                        annotate_ptr(map, target, ann, sizeof(ann));
                    }
                }
            }
        }

        /* BL stub annotation */
        if (strcmp(mnem, "bl") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 1 && a64->operands[0].type == ARM64_OP_IMM) {
                uint64_t t = (uint64_t)a64->operands[0].imm;
                annotate_ptr(map, t, ann, sizeof(ann));
                if (!ann[0]) {
                    snprintf(ann, sizeof(ann), "->0x%llx",
                             (unsigned long long)t);
                }
            }
        }

        sb_printf(sb, "0x%llx:\t%s\t%s", (unsigned long long)insn[i].address,
                  mnem, ops);
        if (ann[0]) {
            sb_printf(sb, "\t; %s", ann);
        }
        sb_puts(sb, "\n");

        if (strcmp(mnem, "ret") == 0) {
            break;
        }
    }

    cs_free(insn, count);
    cs_close(&handle);
    return 0;
}

static int
emit_heuristic_c(SB *sb, const MachOMap *map, const uint8_t *code,
                 size_t code_size, uint64_t address)
{
    csh handle;
    cs_insn *insn;
    size_t count;
    uint64_t xregs[32];
    int x_valid[32];
    char ann[256];
    int indent = 1;

    memset(xregs, 0, sizeof(xregs));
    memset(x_valid, 0, sizeof(x_valid));

    sb_printf(sb, "// Heuristic pseudo-C @ 0x%llx\n",
              (unsigned long long)address);
    sb_puts(sb, "id fn(id self, SEL _cmd, ...) {\n");

    if (cs_open(DYLAB_CS_ARM64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
        sb_puts(sb, "  // cs_open failed\n}\n");
        return -1;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    count = cs_disasm(handle, code, code_size, address, 0, &insn);
    if (count == 0) {
        sb_puts(sb, "  // no instructions\n}\n");
        cs_close(&handle);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        const char *mnem = insn[i].mnemonic;
        for (int k = 0; k < indent; k++) {
            sb_puts(sb, "  ");
        }

        if (strcmp(mnem, "adrp") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_IMM) {
                unsigned reg = a64->operands[0].reg - ARM64_REG_X0;
                if (reg < 32) {
                    xregs[reg] = (uint64_t)a64->operands[1].imm;
                    x_valid[reg] = 1;
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "add") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[2].type == ARM64_OP_IMM) {
                unsigned rd = a64->operands[0].reg - ARM64_REG_X0;
                unsigned rn = a64->operands[1].reg - ARM64_REG_X0;
                if (rd < 32 && rn < 32 && x_valid[rn]) {
                    xregs[rd] = xregs[rn] + (uint64_t)a64->operands[2].imm;
                    x_valid[rd] = 1;
                    if (annotate_ptr(map, xregs[rd], ann, sizeof(ann))) {
                        sb_printf(sb, "x%u = %s;\n", rd, ann);
                    } else {
                        sb_printf(sb, "x%u = 0x%llx;\n", rd,
                                  (unsigned long long)xregs[rd]);
                    }
                } else {
                    sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                }
            } else {
                sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
            }
        } else if (strcmp(mnem, "bl") == 0) {
            /* Heuristic: objc_msgSend(self, sel, ...) */
            const char *sel = NULL;
            if (x_valid[1]) {
                sel = lookup_cstr(map, xregs[1]);
                if (!sel && annotate_ptr(map, xregs[1], ann, sizeof(ann))) {
                    sel = ann;
                }
            }
            if (sel && sel[0] == '@' && strstr(sel, "selector")) {
                sb_printf(sb, "id r = [x0 %s x2];\n", sel);
            } else if (sel) {
                sb_printf(sb, "id r = [x0 %s:x2]; // sel=%s\n",
                          (sel[0] == '@') ? "perform" : sel, sel);
            } else {
                sb_printf(sb, "r = bl(%s);\n", insn[i].op_str);
            }
        } else if (strncmp(mnem, "b.", 2) == 0 || strcmp(mnem, "cbz") == 0 ||
                   strcmp(mnem, "cbnz") == 0 || strcmp(mnem, "tbz") == 0 ||
                   strcmp(mnem, "tbnz") == 0) {
            sb_printf(sb, "if (cond) { /* %s %s */ }\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "b") == 0) {
            sb_printf(sb, "goto label_%s;\n", insn[i].op_str);
        } else if (strcmp(mnem, "ret") == 0) {
            sb_puts(sb, "return x0;\n");
            break;
        } else if (strcmp(mnem, "mov") == 0) {
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else {
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        }
    }

    sb_puts(sb, "}\n");
    cs_free(insn, count);
    cs_close(&handle);
    return 0;
}

#else /* !DYLAB_HAVE_CAPSTONE */

static int
emit_disasm(SB *sb, const MachOMap *map, const uint8_t *code, size_t code_size,
            uint64_t address)
{
    (void)map;
    (void)code;
    sb_printf(sb,
              "; Capstone headers not available at build time\n"
              "; raw bytes @ 0x%llx len=%zu\n",
              (unsigned long long)address, code_size);
    return -1;
}

static int
emit_heuristic_c(SB *sb, const MachOMap *map, const uint8_t *code,
                 size_t code_size, uint64_t address)
{
    (void)map;
    (void)code;
    sb_printf(sb,
              "// Capstone unavailable — cannot lift @ 0x%llx (%zu bytes)\n",
              (unsigned long long)address, code_size);
    return -1;
}

#endif

int
decompile_function(const char *path,
                   uint64_t file_offset,
                   uint64_t size,
                   uint64_t vma_hint,
                   DecompileMode mode,
                   char *out_buf,
                   size_t out_buf_size)
{
    MachOMap map;
    SB sb;
    const uint8_t *code;
    uint64_t address;
    size_t code_size;
    const MachOSect *text;
    int rc = -1;

    if (!out_buf || out_buf_size == 0 || !path) {
        return -1;
    }
    out_buf[0] = '\0';

    if (mode == DECOMPILE_MODE_GHIDRA_C) {
        snprintf(out_buf, out_buf_size,
                 "// Phase 3B placeholder: Ghidra/rz-ghidra C decompilation\n"
                 "// Not available in Phase 3A. Enable DECOMPILE_MODE_GHIDRA_C\n"
                 "// once librz + rz-ghidra are wired.\n"
                 "// requested: %s off=0x%llx size=%llu\n",
                 path, (unsigned long long)file_offset,
                 (unsigned long long)size);
        return 0;
    }

    if (macho_map_load(path, &map) != 0) {
        snprintf(out_buf, out_buf_size, "; failed to load Mach-O\n");
        return -1;
    }

    text = macho_find_sect(&map, "__TEXT", "__text");
    if (file_offset >= map.thin_len) {
        snprintf(out_buf, out_buf_size, "; file_offset out of range\n");
        macho_map_free(&map);
        return -1;
    }

    if (size == 0) {
        size = 0x400;
        if (text) {
            uint64_t text_end = text->offset + text->size;
            if (file_offset < text_end) {
                uint64_t remain = text_end - file_offset;
                if (remain < size) {
                    size = remain;
                }
            }
        }
    }
    if (file_offset + size > map.thin_len) {
        size = map.thin_len - file_offset;
    }

    code = map.file + map.thin_off + file_offset;
    code_size = (size_t)size;
    address = vma_hint;
    if (address == 0) {
        /* Derive VMA from segment containing file_offset. */
        for (int i = 0; i < map.nsegs; i++) {
            if (file_offset >= map.segs[i].fileoff &&
                file_offset < map.segs[i].fileoff + map.segs[i].filesize) {
                address = map.segs[i].vmaddr +
                          (file_offset - map.segs[i].fileoff);
                break;
            }
        }
    }

    sb_init(&sb);
    if (!sb.data) {
        macho_map_free(&map);
        return -1;
    }

    if (mode == DECOMPILE_MODE_DISASM) {
        rc = emit_disasm(&sb, &map, code, code_size, address);
    } else if (mode == DECOMPILE_MODE_HEURISTIC_C) {
        rc = emit_heuristic_c(&sb, &map, code, code_size, address);
    } else {
        sb_puts(&sb, "; unknown mode\n");
    }

    snprintf(out_buf, out_buf_size, "%s", sb.data ? sb.data : "");
    free(sb.data);
    macho_map_free(&map);
    return rc;
}
