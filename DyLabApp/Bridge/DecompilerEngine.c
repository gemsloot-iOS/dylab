#include "DecompilerEngine.h"
#include "MachOMap.h"
#include "StructuredDecompiler.h"

#include <ctype.h>
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

typedef enum {
    SV_NONE = 0,
    SV_ADDR,   /* raw address / page+off */
    SV_CSTR,   /* C string literal */
    SV_SEL,    /* ObjC selector name in label */
    SV_CLASS,  /* ObjC class name in label */
    SV_CFSTR,  /* CFString / NSString literal */
    SV_SYM     /* symbol / stub name */
} SymKind;

typedef struct {
    SymKind kind;
    uint64_t imm;
    char label[128];
} SymVal;

#define MAX_STACK_SLOTS 96

typedef struct {
    int used;
    int is_fp; /* 1 = x29, 0 = sp */
    int64_t disp;
    SymVal val;
} StackSlot;

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

static void
sym_clear(SymVal *s)
{
    memset(s, 0, sizeof(*s));
}

static void
sym_set_addr(SymVal *s, uint64_t imm)
{
    sym_clear(s);
    s->kind = SV_ADDR;
    s->imm = imm;
}

static void
sym_set_label(SymVal *s, SymKind k, const char *label, uint64_t imm)
{
    size_t j = 0;
    sym_clear(s);
    s->kind = k;
    s->imm = imm;
    if (!label) {
        return;
    }
    for (size_t i = 0; label[i] && j + 1 < sizeof(s->label); i++) {
        unsigned char c = (unsigned char)label[i];
        if (c >= 0x20 && c < 0x7f) {
            s->label[j++] = (char)c;
        }
    }
    s->label[j] = '\0';
    if (j == 0) {
        s->kind = SV_NONE;
    }
}

static int
sym_printable(const SymVal *s)
{
    return s && s->kind != SV_NONE && s->label[0] != '\0';
}

static void
stack_store(StackSlot *slots, int nslots, int is_fp, int64_t disp,
            const SymVal *val)
{
    int free_i = -1;
    for (int i = 0; i < nslots; i++) {
        if (slots[i].used && slots[i].is_fp == is_fp && slots[i].disp == disp) {
            slots[i].val = *val;
            return;
        }
        if (!slots[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i >= 0) {
        slots[free_i].used = 1;
        slots[free_i].is_fp = is_fp;
        slots[free_i].disp = disp;
        slots[free_i].val = *val;
    }
}

static int
stack_load(const StackSlot *slots, int nslots, int is_fp, int64_t disp,
           SymVal *out)
{
    for (int i = 0; i < nslots; i++) {
        if (slots[i].used && slots[i].is_fp == is_fp && slots[i].disp == disp) {
            *out = slots[i].val;
            return 1;
        }
    }
    return 0;
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
        return NULL;
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

static int
in_sect(const MachOSect *s, uint64_t addr)
{
    return s && addr >= s->addr && addr < s->addr + s->size;
}

/** Classify / annotate an address; fills SymVal when possible. */
static int
resolve_addr(const MachOMap *map, uint64_t vma, SymVal *out, char *ann,
             size_t ann_sz)
{
    uint64_t addr = macho_strip_ptr(vma);
    const MachOSect *selrefs;
    const MachOSect *classrefs;
    const MachOSect *cstring;
    const MachOSect *cfstring;
    const MachOSect *methname;
    char tmp[192];
    const char *s;

    sym_clear(out);
    if (ann && ann_sz) {
        ann[0] = '\0';
    }

    selrefs = macho_find_sect(map, "__DATA_CONST", "__objc_selrefs");
    if (!selrefs) {
        selrefs = macho_find_sect(map, "__DATA", "__objc_selrefs");
    }
    classrefs = macho_find_sect(map, "__DATA_CONST", "__objc_classrefs");
    if (!classrefs) {
        classrefs = macho_find_sect(map, "__DATA", "__objc_classrefs");
    }
    cstring = macho_find_sect(map, "__TEXT", "__cstring");
    methname = macho_find_sect(map, "__TEXT", "__objc_methname");
    cfstring = macho_find_sect(map, "__DATA_CONST", "__cfstring");
    if (!cfstring) {
        cfstring = macho_find_sect(map, "__DATA", "__cfstring");
    }

    if (in_sect(selrefs, addr)) {
        if (macho_resolve_selref(map, addr, tmp, sizeof(tmp)) == 0) {
            sym_set_label(out, SV_SEL, tmp, addr);
            if (ann) {
                snprintf(ann, ann_sz, "@selector(%s)", tmp);
            }
            return 1;
        }
        if (ann) {
            snprintf(ann, ann_sz, "selref_0x%llx", (unsigned long long)addr);
        }
        sym_set_addr(out, addr);
        return 1;
    }

    if (in_sect(classrefs, addr)) {
        if (macho_resolve_classref(map, addr, tmp, sizeof(tmp)) == 0) {
            sym_set_label(out, SV_CLASS, tmp, addr);
            if (ann) {
                snprintf(ann, ann_sz, "cls %s", tmp);
            }
            return 1;
        }
        if (ann) {
            snprintf(ann, ann_sz, "classref_0x%llx", (unsigned long long)addr);
        }
        sym_set_addr(out, addr);
        return 1;
    }

    if (in_sect(cstring, addr) || in_sect(methname, addr)) {
        s = macho_cstr_at(map, addr);
        if (s && s[0]) {
            size_t j = 0;
            for (size_t i = 0; s[i] && j + 1 < sizeof(tmp); i++) {
                unsigned char c = (unsigned char)s[i];
                if (c >= 0x20 && c < 0x7f) {
                    tmp[j++] = (char)c;
                }
            }
            tmp[j] = '\0';
            if (j > 0) {
                sym_set_label(out, SV_CSTR, tmp, addr);
                if (ann) {
                    snprintf(ann, ann_sz, "\"%s\"", tmp);
                }
                return 1;
            }
        }
    }

    if (in_sect(cfstring, addr)) {
        const uint8_t *p = macho_vma_to_ptr(map, addr, 32);
        if (p) {
            uint64_t strp;
            memcpy(&strp, p + 16, 8);
            s = macho_cstr_at(map, macho_strip_ptr(strp));
            if (s && s[0]) {
                size_t j = 0;
                for (size_t i = 0; s[i] && j + 1 < sizeof(tmp); i++) {
                    unsigned char c = (unsigned char)s[i];
                    if (c >= 0x20 && c < 0x7f) {
                        tmp[j++] = (char)c;
                    }
                }
                tmp[j] = '\0';
                if (j > 0) {
                    sym_set_label(out, SV_CFSTR, tmp, addr);
                    if (ann) {
                        snprintf(ann, ann_sz, "@\"%s\"", tmp);
                    }
                    return 1;
                }
            }
        }
    }

    if (lookup_stub_symbol(map, addr, tmp, sizeof(tmp))) {
        sym_set_label(out, SV_SYM, tmp, addr);
        if (ann) {
            snprintf(ann, ann_sz, "%s", tmp);
        }
        return 1;
    }

    {
        const char *bind = macho_bind_name_at(map, addr);
        if (bind && bind[0]) {
            sym_set_label(out, SV_SYM, bind, addr);
            if (ann) {
                snprintf(ann, ann_sz, "%s", bind);
            }
            return 1;
        }
    }

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
                        sym_set_label(out, SV_SYM, name, addr);
                        if (ann) {
                            snprintf(ann, ann_sz, "%s", name);
                        }
                        return 1;
                    }
                }
            }
        }
    }

    sym_set_addr(out, addr);
    return 0;
}

/** Load through a pointer: if addr is selref/classref, propagate metadata. */
static void
load_through(const MachOMap *map, uint64_t addr, SymVal *dst, char *ann,
             size_t ann_sz)
{
    SymVal box;
    char local[192];

    if (resolve_addr(map, addr, &box, local, sizeof(local))) {
        if (box.kind == SV_SEL || box.kind == SV_CLASS || box.kind == SV_CFSTR ||
            box.kind == SV_CSTR || box.kind == SV_SYM) {
            *dst = box;
            if (ann && ann_sz) {
                snprintf(ann, ann_sz, "%s", local);
            }
            return;
        }
    }

    /* Plain pointer load (may be chained garbage — prefer annotate box addr). */
    {
        const uint8_t *p = macho_vma_to_ptr(map, addr, 8);
        if (p && box.kind != SV_SEL && box.kind != SV_CLASS) {
            uint64_t val;
            SymVal pointed;
            memcpy(&val, p, 8);
            /* Only follow if not a chained bind */
            if (((val >> 62) & 1ull) == 0) {
                val = macho_strip_ptr(val);
                if (resolve_addr(map, val, &pointed, local, sizeof(local)) &&
                    pointed.kind != SV_ADDR) {
                    *dst = pointed;
                    if (ann && ann_sz) {
                        snprintf(ann, ann_sz, "%s", local);
                    }
                    return;
                }
                sym_set_addr(dst, val);
                if (ann && ann_sz && local[0]) {
                    snprintf(ann, ann_sz, "%s", local);
                }
                return;
            }
        }
    }

    if (local[0]) {
        sym_set_addr(dst, addr);
        if (ann && ann_sz) {
            snprintf(ann, ann_sz, "%s", local);
        }
    } else {
        sym_set_addr(dst, addr);
    }
}

static int
is_objc_msgsend_name(const char *name)
{
    const char *n = name;
    if (!n || !n[0]) {
        return 0;
    }
    if (n[0] == '_') {
        n++;
    }
    return strcmp(n, "objc_msgSend") == 0 ||
           strcmp(n, "objc_msgSendSuper2") == 0 ||
           strcmp(n, "objc_msgSendSuper") == 0;
}

static const char *
c_func_name(const char *sym)
{
    if (!sym) {
        return "fn";
    }
    if (sym[0] == '_') {
        return sym + 1;
    }
    return sym;
}

#if DYLAB_HAVE_CAPSTONE

static unsigned
reg_x_index(unsigned cs_reg)
{
    if (cs_reg >= ARM64_REG_X0 && cs_reg <= ARM64_REG_X28) {
        return cs_reg - ARM64_REG_X0;
    }
    if (cs_reg == ARM64_REG_FP) {
        return 29;
    }
    if (cs_reg == ARM64_REG_LR) {
        return 30;
    }
    if (cs_reg == ARM64_REG_SP) {
        return 31;
    }
    if (cs_reg >= ARM64_REG_W0 && cs_reg <= ARM64_REG_W28) {
        return cs_reg - ARM64_REG_W0;
    }
    return 255;
}

static int
is_fp_reg(unsigned cs_reg)
{
    return cs_reg == ARM64_REG_FP || cs_reg == ARM64_REG_X29;
}

static int
is_sp_reg(unsigned cs_reg)
{
    return cs_reg == ARM64_REG_SP;
}

static int
emit_disasm(SB *sb, const MachOMap *map, const uint8_t *code, size_t code_size,
            uint64_t address)
{
    csh handle;
    cs_insn *insn;
    size_t count;
    SymVal regs[32];
    char ann[256];

    memset(regs, 0, sizeof(regs));

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

        if (strcmp(mnem, "adrp") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_IMM) {
                unsigned reg = reg_x_index(a64->operands[0].reg);
                if (reg < 32) {
                    sym_set_addr(&regs[reg], (uint64_t)a64->operands[1].imm);
                }
            }
        }
        if (strcmp(mnem, "add") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_IMM) {
                unsigned rd = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].reg);
                if (rd < 32 && rn < 32 && regs[rn].kind == SV_ADDR) {
                    uint64_t target =
                        regs[rn].imm + (uint64_t)a64->operands[2].imm;
                    resolve_addr(map, target, &regs[rd], ann, sizeof(ann));
                }
            }
        }
        if ((strcmp(mnem, "ldr") == 0 || strcmp(mnem, "ldur") == 0) &&
            insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].mem.base);
                int64_t disp = a64->operands[1].mem.disp;
                if (rt < 32 && rn < 32 && regs[rn].kind != SV_NONE) {
                    uint64_t target = regs[rn].imm + (uint64_t)disp;
                    load_through(map, target, &regs[rt], ann, sizeof(ann));
                }
            }
        }
        if (strcmp(mnem, "bl") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 1 && a64->operands[0].type == ARM64_OP_IMM) {
                SymVal t;
                resolve_addr(map, (uint64_t)a64->operands[0].imm, &t, ann,
                             sizeof(ann));
                if (!ann[0]) {
                    snprintf(ann, sizeof(ann), "->0x%llx",
                             (unsigned long long)a64->operands[0].imm);
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

static void
emit_objc_call(SB *sb, const SymVal *recv, const SymVal *sel, const SymVal *a2)
{
    const char *r = "x0";
    const char *s = "sel";
    if (recv && recv->kind == SV_CLASS && recv->label[0]) {
        r = recv->label;
    } else if (recv && sym_printable(recv) && recv->kind == SV_SYM) {
        r = recv->label;
    }
    if (sel && sel->kind == SV_SEL && sel->label[0]) {
        s = sel->label;
    } else if (sel && sel->kind == SV_CSTR && sel->label[0]) {
        s = sel->label;
    }

    /* Simple unary vs keyword */
    if (strchr(s, ':') == NULL) {
        if (recv && recv->kind == SV_CLASS) {
            sb_printf(sb, "id r = [%s %s];\n", r, s);
        } else {
            sb_printf(sb, "id r = [%s %s];\n", r, s);
        }
    } else {
        /* First segment only for heuristic */
        char head[128];
        size_t j = 0;
        for (size_t i = 0; s[i] && s[i] != ':' && j + 1 < sizeof(head); i++) {
            head[j++] = s[i];
        }
        head[j] = '\0';
        if (a2 && a2->kind == SV_CFSTR && a2->label[0]) {
            sb_printf(sb, "id r = [%s %s:@\"%s\"];\n", r, head[0] ? head : s,
                      a2->label);
        } else if (a2 && a2->kind == SV_CSTR && a2->label[0]) {
            sb_printf(sb, "id r = [%s %s:\"%s\"];\n", r, head[0] ? head : s,
                      a2->label);
        } else {
            sb_printf(sb, "id r = [%s %s:x2];\n", r, head[0] ? head : s);
        }
    }
}

static void
emit_c_call(SB *sb, const char *fname, const SymVal *regs)
{
    const char *fn = c_func_name(fname);
    /* Heuristic args: w0/x0, x1, x2 */
    char a0[160] = "x0";
    char a1[160] = "x1";
    char a2[160] = "x2";

    if (regs[0].kind == SV_CSTR && regs[0].label[0]) {
        snprintf(a0, sizeof(a0), "\"%s\"", regs[0].label);
    } else if (regs[0].kind == SV_CFSTR && regs[0].label[0]) {
        snprintf(a0, sizeof(a0), "@\"%s\"", regs[0].label);
    } else if (regs[0].kind == SV_CLASS && regs[0].label[0]) {
        snprintf(a0, sizeof(a0), "%s", regs[0].label);
    }

    if (regs[1].kind == SV_CSTR && regs[1].label[0]) {
        snprintf(a1, sizeof(a1), "\"%s\"", regs[1].label);
    } else if (regs[1].kind == SV_CFSTR && regs[1].label[0]) {
        snprintf(a1, sizeof(a1), "@\"%s\"", regs[1].label);
    } else if (regs[1].kind == SV_SEL && regs[1].label[0]) {
        snprintf(a1, sizeof(a1), "@selector(%s)", regs[1].label);
    }

    if (regs[2].kind == SV_CSTR && regs[2].label[0]) {
        snprintf(a2, sizeof(a2), "\"%s\"", regs[2].label);
    } else if (regs[2].kind == SV_CFSTR && regs[2].label[0]) {
        snprintf(a2, sizeof(a2), "@\"%s\"", regs[2].label);
    }

    sb_printf(sb, "%s(%s, %s, %s);\n", fn, a0, a1, a2);
}

static int
emit_heuristic_c(SB *sb, const MachOMap *map, const uint8_t *code,
                 size_t code_size, uint64_t address)
{
    csh handle;
    cs_insn *insn;
    size_t count;
    SymVal regs[32];
    StackSlot slots[MAX_STACK_SLOTS];
    char ann[256];
    int indent = 1;

    memset(regs, 0, sizeof(regs));
    memset(slots, 0, sizeof(slots));

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
                unsigned reg = reg_x_index(a64->operands[0].reg);
                if (reg < 32) {
                    sym_set_addr(&regs[reg], (uint64_t)a64->operands[1].imm);
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "add") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[2].type == ARM64_OP_IMM) {
                unsigned rd = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].reg);
                if (rd < 32 && rn < 32 && regs[rn].kind == SV_ADDR) {
                    uint64_t target =
                        regs[rn].imm + (uint64_t)a64->operands[2].imm;
                    resolve_addr(map, target, &regs[rd], ann, sizeof(ann));
                    if (ann[0]) {
                        sb_printf(sb, "x%u = %s;\n", rd, ann);
                    } else {
                        sb_printf(sb, "x%u = 0x%llx;\n", rd,
                                  (unsigned long long)target);
                    }
                } else {
                    sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                }
            } else {
                sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
            }
        } else if ((strcmp(mnem, "ldr") == 0 || strcmp(mnem, "ldur") == 0) &&
                   insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = reg_x_index(a64->operands[0].reg);
                unsigned base = a64->operands[1].mem.base;
                int64_t disp = a64->operands[1].mem.disp;
                SymVal loaded;
                ann[0] = '\0';
                sym_clear(&loaded);

                if (is_fp_reg(base) || is_sp_reg(base)) {
                    if (stack_load(slots, MAX_STACK_SLOTS, is_fp_reg(base),
                                   disp, &loaded)) {
                        if (rt < 32) {
                            regs[rt] = loaded;
                        }
                        if (sym_printable(&loaded)) {
                            if (loaded.kind == SV_SEL) {
                                sb_printf(sb, "x%u = @selector(%s); // spill\n",
                                          rt, loaded.label);
                            } else if (loaded.kind == SV_CLASS) {
                                sb_printf(sb, "x%u = %s; // spill\n", rt,
                                          loaded.label);
                            } else {
                                sb_printf(sb, "x%u = %s; // spill\n", rt,
                                          loaded.label);
                            }
                        } else {
                            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                        }
                    } else {
                        sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                    }
                } else {
                    unsigned rn = reg_x_index(base);
                    if (rt < 32 && rn < 32 && regs[rn].kind != SV_NONE) {
                        uint64_t target = regs[rn].imm + (uint64_t)disp;
                        load_through(map, target, &regs[rt], ann, sizeof(ann));
                        if (ann[0]) {
                            sb_printf(sb, "x%u = %s;\n", rt, ann);
                        } else {
                            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                        }
                    } else {
                        sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
                    }
                }
            } else {
                sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
            }
        } else if ((strcmp(mnem, "str") == 0 || strcmp(mnem, "stur") == 0) &&
                   insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = reg_x_index(a64->operands[0].reg);
                unsigned base = a64->operands[1].mem.base;
                int64_t disp = a64->operands[1].mem.disp;
                if (rt < 32 && (is_fp_reg(base) || is_sp_reg(base))) {
                    stack_store(slots, MAX_STACK_SLOTS, is_fp_reg(base), disp,
                                &regs[rt]);
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "stp") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_MEM) {
                unsigned r0 = reg_x_index(a64->operands[0].reg);
                unsigned r1 = reg_x_index(a64->operands[1].reg);
                unsigned base = a64->operands[2].mem.base;
                int64_t disp = a64->operands[2].mem.disp;
                if ((is_fp_reg(base) || is_sp_reg(base))) {
                    if (r0 < 32) {
                        stack_store(slots, MAX_STACK_SLOTS, is_fp_reg(base),
                                    disp, &regs[r0]);
                    }
                    if (r1 < 32) {
                        stack_store(slots, MAX_STACK_SLOTS, is_fp_reg(base),
                                    disp + 8, &regs[r1]);
                    }
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "ldp") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_MEM) {
                unsigned r0 = reg_x_index(a64->operands[0].reg);
                unsigned r1 = reg_x_index(a64->operands[1].reg);
                unsigned base = a64->operands[2].mem.base;
                int64_t disp = a64->operands[2].mem.disp;
                SymVal a, b;
                if ((is_fp_reg(base) || is_sp_reg(base))) {
                    if (r0 < 32 &&
                        stack_load(slots, MAX_STACK_SLOTS, is_fp_reg(base),
                                   disp, &a)) {
                        regs[r0] = a;
                    }
                    if (r1 < 32 &&
                        stack_load(slots, MAX_STACK_SLOTS, is_fp_reg(base),
                                   disp + 8, &b)) {
                        regs[r1] = b;
                    }
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "mov") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG) {
                unsigned rd = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].reg);
                if (rd < 32 && rn < 32) {
                    regs[rd] = regs[rn];
                }
            }
            sb_printf(sb, "// %s %s\n", mnem, insn[i].op_str);
        } else if (strcmp(mnem, "bl") == 0 && insn[i].detail) {
            cs_arm64 *a64 = &insn[i].detail->arm64;
            SymVal target;
            char tname[192];
            tname[0] = '\0';
            sym_clear(&target);
            if (a64->op_count >= 1 && a64->operands[0].type == ARM64_OP_IMM) {
                resolve_addr(map, (uint64_t)a64->operands[0].imm, &target,
                             tname, sizeof(tname));
            }
            if (is_objc_msgsend_name(target.label) ||
                is_objc_msgsend_name(tname)) {
                emit_objc_call(sb, &regs[0], &regs[1], &regs[2]);
            } else if (target.kind == SV_SYM && target.label[0]) {
                emit_c_call(sb, target.label, regs);
            } else if (tname[0] && tname[0] != '@' && tname[0] != '"') {
                emit_c_call(sb, tname, regs);
            } else {
                sb_printf(sb, "bl(%s);\n", insn[i].op_str);
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
        /* Phase 3B: Capstone-based structured decompiler (CFG + folding). */
        return structured_decompile(path, file_offset, size, vma_hint, out_buf,
                                    out_buf_size);
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

    address = vma_hint;
    if (address == 0) {
        for (int i = 0; i < map.nsegs; i++) {
            if (file_offset >= map.segs[i].fileoff &&
                file_offset < map.segs[i].fileoff + map.segs[i].filesize) {
                address = map.segs[i].vmaddr +
                          (file_offset - map.segs[i].fileoff);
                break;
            }
        }
    }

    if (size == 0 && address != 0) {
        size = macho_function_size_at(&map, address);
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
