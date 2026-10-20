#include "MachOMap.h"

#include <ctype.h>
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
#define LC_DYSYMTAB 0xb
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_FUNCTION_STARTS 0x26
#define LC_DYLD_CHAINED_FIXUPS 0x34
#define N_EXT 0x01
#define N_TYPE 0x0e
#define N_SECT 0x0e
#define N_UNDF 0x0
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
struct symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};
struct dysymtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
};
struct dyld_info_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
};
struct linkedit_data_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
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

#ifndef LC_DYLD_INFO
#define LC_DYLD_INFO 0x22
#endif
#ifndef LC_DYLD_INFO_ONLY
#define LC_DYLD_INFO_ONLY 0x80000022
#endif

static uint32_t
u32n(uint32_t v, int swap)
{
    return swap ? OSSwapInt32(v) : v;
}

uint64_t
macho_strip_ptr(uint64_t vma)
{
    return vma & DYLAB_VMA_MASK;
}

uint64_t
macho_fixup_ptr(const MachOMap *map, uint64_t raw)
{
    if (!map || !map->has_chained_fixups) {
        return macho_strip_ptr(raw);
    }
    /* dyld_chained_ptr_64_*: bit 62 is bind. */
    if ((raw >> 62) & 1ull) {
        return 0;
    }
    /* Rebase: 36-bit target in low bits (arm64 common format). */
    {
        uint64_t target = raw & 0xFFFFFFFFFull;
        if (target != 0) {
            return macho_strip_ptr(target);
        }
    }
    return macho_strip_ptr(raw);
}

uint64_t
macho_read_ptr(const MachOMap *map, uint64_t vma)
{
    const uint8_t *p = macho_vma_to_ptr(map, vma, 8);
    uint64_t raw;
    if (!p) {
        return 0;
    }
    memcpy(&raw, p, 8);
    return macho_fixup_ptr(map, raw);
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
    nfat = u32n(fh->nfat_arch, swap);
    if (sizeof(struct fat_header) + (size_t)nfat * sizeof(struct fat_arch) >
        file_len) {
        return -1;
    }
    archs = (const struct fat_arch *)(file + sizeof(struct fat_header));
    for (uint32_t i = 0; i < nfat; i++) {
        cpu_type_t cputype =
            (cpu_type_t)u32n((uint32_t)archs[i].cputype, swap);
        uint32_t offset = u32n(archs[i].offset, swap);
        uint32_t size = u32n(archs[i].size, swap);
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

static void
bind_add(MachOMap *map, uint64_t vma, const char *name)
{
    MachOBind *nb;
    if (!name || !name[0] || map->nbinds >= DYLAB_MAX_BINDS) {
        return;
    }
    if (!map->binds) {
        map->binds = (MachOBind *)calloc(DYLAB_MAX_BINDS, sizeof(MachOBind));
        if (!map->binds) {
            return;
        }
    }
    /* Dedupe exact vma */
    for (int i = 0; i < map->nbinds; i++) {
        if (map->binds[i].vma == macho_strip_ptr(vma)) {
            snprintf(map->binds[i].name, sizeof(map->binds[i].name), "%s",
                     name);
            return;
        }
    }
    nb = &map->binds[map->nbinds++];
    nb->vma = macho_strip_ptr(vma);
    snprintf(nb->name, sizeof(nb->name), "%s", name);
}

/* Minimal dyld bind opcode walker (LC_DYLD_INFO). */
static void
parse_bind_opcodes(MachOMap *map, uint32_t off, uint32_t size)
{
    const uint8_t *p;
    const uint8_t *end;
    uint64_t seg_index = 0;
    uint64_t seg_offset = 0;
    char symbol[256];
    int done = 0;

    if (!off || !size || (size_t)off + size > map->thin_len) {
        return;
    }
    p = map->file + map->thin_off + off;
    end = p + size;
    symbol[0] = '\0';

    while (p < end && !done) {
        uint8_t imm = *p & 0x0f;
        uint8_t opcode = *p & 0xf0;
        p++;
        switch (opcode) {
        case 0x00: /* DONE */
            done = 1;
            break;
        case 0x10: /* SET_DYLIB_ORDINAL_IMM */
            break;
        case 0x20: { /* SET_DYLIB_ORDINAL_ULEB */
            while (p < end && (*p++ & 0x80)) {
            }
            break;
        }
        case 0x30: /* SET_DYLIB_SPECIAL_IMM */
            break;
        case 0x40: { /* SET_SYMBOL_TRAILING_FLAGS_IMM */
            size_t i = 0;
            while (p < end && *p && i + 1 < sizeof(symbol)) {
                symbol[i++] = (char)*p++;
            }
            if (p < end) {
                p++;
            }
            symbol[i] = '\0';
            break;
        }
        case 0x50: /* SET_TYPE_IMM */
            break;
        case 0x60: { /* SET_ADDEND_SLEB */
            uint64_t tmp = 0;
            int shift = 0;
            uint8_t b;
            do {
                if (p >= end) {
                    break;
                }
                b = *p++;
                tmp |= (uint64_t)(b & 0x7f) << shift;
                shift += 7;
            } while (b & 0x80);
            (void)tmp;
            break;
        }
        case 0x70: { /* SET_SEGMENT_AND_OFFSET_ULEB */
            uint64_t tmp = 0;
            int shift = 0;
            uint8_t b;
            seg_index = imm;
            do {
                if (p >= end) {
                    break;
                }
                b = *p++;
                tmp |= (uint64_t)(b & 0x7f) << shift;
                shift += 7;
            } while (b & 0x80);
            seg_offset = tmp;
            break;
        }
        case 0x80: { /* ADD_ADDR_ULEB */
            uint64_t tmp = 0;
            int shift = 0;
            uint8_t b;
            do {
                if (p >= end) {
                    break;
                }
                b = *p++;
                tmp |= (uint64_t)(b & 0x7f) << shift;
                shift += 7;
            } while (b & 0x80);
            seg_offset += tmp;
            break;
        }
        case 0x90: /* DO_BIND */
            if (seg_index < (uint64_t)map->nsegs && symbol[0]) {
                bind_add(map, map->segs[seg_index].vmaddr + seg_offset, symbol);
            }
            seg_offset += 8;
            break;
        case 0xA0: /* DO_BIND_ADD_ADDR_ULEB */
            if (seg_index < (uint64_t)map->nsegs && symbol[0]) {
                bind_add(map, map->segs[seg_index].vmaddr + seg_offset, symbol);
            }
            {
                uint64_t tmp = 0;
                int shift = 0;
                uint8_t b;
                do {
                    if (p >= end) {
                        break;
                    }
                    b = *p++;
                    tmp |= (uint64_t)(b & 0x7f) << shift;
                    shift += 7;
                } while (b & 0x80);
                seg_offset += 8 + tmp;
            }
            break;
        case 0xB0: /* DO_BIND_ADD_ADDR_IMM_SCALED */
            if (seg_index < (uint64_t)map->nsegs && symbol[0]) {
                bind_add(map, map->segs[seg_index].vmaddr + seg_offset, symbol);
            }
            seg_offset += 8 + (uint64_t)imm * 8;
            break;
        case 0xC0: { /* DO_BIND_ULEB_TIMES_SKIPPING_ULEB */
            uint64_t count = 0, skip = 0;
            int shift = 0;
            uint8_t b;
            do {
                if (p >= end) {
                    break;
                }
                b = *p++;
                count |= (uint64_t)(b & 0x7f) << shift;
                shift += 7;
            } while (b & 0x80);
            shift = 0;
            do {
                if (p >= end) {
                    break;
                }
                b = *p++;
                skip |= (uint64_t)(b & 0x7f) << shift;
                shift += 7;
            } while (b & 0x80);
            for (uint64_t i = 0; i < count; i++) {
                if (seg_index < (uint64_t)map->nsegs && symbol[0]) {
                    bind_add(map, map->segs[seg_index].vmaddr + seg_offset,
                             symbol);
                }
                seg_offset += 8 + skip;
            }
            break;
        }
        default:
            /* Unknown / threaded — stop this stream */
            done = 1;
            break;
        }
        (void)imm;
    }
}

/* Chained fixups: record bind targets for pointers in DATA sections. */
static void
parse_chained_fixups(MachOMap *map)
{
    const uint8_t *base;
    uint32_t imports_offset;
    uint32_t imports_count;
    uint32_t symbols_offset;
    uint32_t imports_format;
    uint32_t i;

    if (!map->chained_fixups_off || !map->chained_fixups_size) {
        return;
    }
    if ((size_t)map->chained_fixups_off + map->chained_fixups_size >
        map->thin_len) {
        return;
    }
    base = map->file + map->thin_off + map->chained_fixups_off;
    /* dyld_chained_fixups_header */
    if (map->chained_fixups_size < 32) {
        return;
    }
    imports_offset = *(const uint32_t *)(base + 16);
    imports_count = *(const uint32_t *)(base + 20);
    symbols_offset = *(const uint32_t *)(base + 24);
    imports_format = *(const uint32_t *)(base + 28);
    if (imports_count > 8192) {
        imports_count = 8192;
    }

    /* Walk classrefs / selrefs / got: decode each 8-byte slot as chained bind. */
    for (int si = 0; si < map->nsects; si++) {
        const MachOSect *sec = &map->sects[si];
        int interesting = 0;
        if (strncmp(sec->sectname, "__objc_classrefs", 16) == 0 ||
            strncmp(sec->sectname, "__objc_selrefs", 16) == 0 ||
            strncmp(sec->sectname, "__got", 16) == 0 ||
            strncmp(sec->sectname, "__la_symbol_ptr", 16) == 0) {
            interesting = 1;
        }
        if (!interesting || sec->size < 8) {
            continue;
        }
        for (uint64_t off = 0; off + 8 <= sec->size; off += 8) {
            const uint8_t *slot = macho_vma_to_ptr(map, sec->addr + off, 8);
            uint64_t raw;
            uint64_t bind;
            uint64_t ordinal;
            const char *sym;
            if (!slot) {
                continue;
            }
            memcpy(&raw, slot, 8);
            /* dyld_chained_ptr_64_bind: bind=1 in bit 62 (common arm64e/64) */
            bind = (raw >> 62) & 1ull;
            if (!bind) {
                continue;
            }
            ordinal = raw & 0xffffffull; /* 24-bit ordinal typical */
            if (imports_format == 1) {
                /* DYLD_CHAINED_IMPORT: 4 bytes each */
                const uint32_t *imp;
                uint32_t name_off;
                if (imports_offset + (ordinal + 1) * 4 >
                    map->chained_fixups_size) {
                    continue;
                }
                imp = (const uint32_t *)(base + imports_offset + ordinal * 4);
                name_off = (*imp >> 9) & 0x7fffffu;
                if (symbols_offset + name_off >= map->chained_fixups_size) {
                    continue;
                }
                sym = (const char *)(base + symbols_offset + name_off);
                bind_add(map, sec->addr + off, sym);
            } else if (imports_format == 2 || imports_format == 3) {
                /* DYLD_CHAINED_IMPORT_ADDEND / ADDEND64 — name offset varies;
                   try format2: lib_ordinal:8, weak:1, name_offset:23 */
                const uint32_t *imp;
                uint32_t name_off;
                size_t stride = (imports_format == 3) ? 16 : 8;
                if (imports_offset + (ordinal + 1) * stride >
                    map->chained_fixups_size) {
                    continue;
                }
                imp = (const uint32_t *)(base + imports_offset + ordinal * stride);
                name_off = (*imp >> 9) & 0x7fffffu;
                if (symbols_offset + name_off >= map->chained_fixups_size) {
                    continue;
                }
                sym = (const char *)(base + symbols_offset + name_off);
                bind_add(map, sec->addr + off, sym);
            }
        }
    }
    (void)i;
}

static void
build_bind_table(MachOMap *map)
{
    parse_bind_opcodes(map, map->dyld_info_bind_off, map->dyld_info_bind_size);
    parse_bind_opcodes(map, map->dyld_info_lazy_bind_off,
                       map->dyld_info_lazy_bind_size);
    parse_bind_opcodes(map, map->dyld_info_weak_bind_off,
                       map->dyld_info_weak_bind_size);
    if (map->has_chained_fixups) {
        parse_chained_fixups(map);
    }
}

int
macho_map_load(const char *path, MachOMap *map)
{
    FILE *fp;
    long sz;
    const uint8_t *thin;
    const struct mach_header_64 *mh;
    const uint8_t *scan;
    const uint8_t *cmds_end;
    int swap = 0;

    memset(map, 0, sizeof(*map));
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) <= 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    map->file = (uint8_t *)malloc((size_t)sz);
    if (map->file == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(map->file, 1, (size_t)sz, fp) != (size_t)sz) {
        free(map->file);
        map->file = NULL;
        fclose(fp);
        return -1;
    }
    fclose(fp);
    map->file_len = (size_t)sz;

    map->thin_off = 0;
    map->thin_len = map->file_len;
    if (find_arm64_slice(map->file, map->file_len, &map->thin_off,
                         &map->thin_len) == 0) {
        map->is_fat = 1;
    }

    thin = map->file + map->thin_off;
    if (map->thin_len < sizeof(struct mach_header_64)) {
        macho_map_free(map);
        return -1;
    }
    mh = (const struct mach_header_64 *)thin;
    if (mh->magic == MH_CIGAM_64) {
        swap = 1;
    } else if (mh->magic != MH_MAGIC_64) {
        macho_map_free(map);
        return -1;
    }

    map->ncmds = u32n(mh->ncmds, swap);
    map->sizeofcmds = u32n(mh->sizeofcmds, swap);
    map->cmds = thin + sizeof(struct mach_header_64);
    cmds_end = map->cmds + map->sizeofcmds;
    if ((size_t)(cmds_end - thin) > map->thin_len) {
        macho_map_free(map);
        return -1;
    }

    scan = map->cmds;
    for (uint32_t i = 0; i < map->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)scan;
        uint32_t cmd = u32n(lc->cmd, swap);
        uint32_t cmdsize = u32n(lc->cmdsize, swap);
        if (cmdsize < 8 || scan + cmdsize > cmds_end) {
            break;
        }
        if (cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg =
                (const struct segment_command_64 *)scan;
            uint32_t nsects = u32n(sg->nsects, swap);
            const struct section_64 *sect =
                (const struct section_64 *)(scan +
                                            sizeof(struct segment_command_64));
            if (map->nsegs < DYLAB_MAX_SEGS) {
                MachOSeg *s = &map->segs[map->nsegs++];
                memset(s, 0, sizeof(*s));
                memcpy(s->segname, sg->segname, 16);
                s->vmaddr = sg->vmaddr;
                s->vmsize = sg->vmsize;
                s->fileoff = sg->fileoff;
                s->filesize = sg->filesize;
            }
            for (uint32_t si = 0; si < nsects && map->nsects < DYLAB_MAX_SECTS;
                 si++) {
                MachOSect *sec = &map->sects[map->nsects++];
                memset(sec, 0, sizeof(*sec));
                memcpy(sec->sectname, sect[si].sectname, 16);
                memcpy(sec->segname, sect[si].segname, 16);
                sec->addr = sect[si].addr;
                sec->size = sect[si].size;
                sec->offset = sect[si].offset;
                sec->flags = sect[si].flags;
                sec->reserved1 = sect[si].reserved1;
                sec->reserved2 = sect[si].reserved2;
            }
        } else if (cmd == LC_SYMTAB) {
            const struct symtab_command *st =
                (const struct symtab_command *)scan;
            map->symoff = u32n(st->symoff, swap);
            map->nsyms = u32n(st->nsyms, swap);
            map->stroff = u32n(st->stroff, swap);
            map->strsize = u32n(st->strsize, swap);
        } else if (cmd == LC_DYSYMTAB) {
            const struct dysymtab_command *dy =
                (const struct dysymtab_command *)scan;
            map->dysym_ilocalsym = u32n(dy->ilocalsym, swap);
            map->dysym_nlocalsym = u32n(dy->nlocalsym, swap);
            map->dysym_iextdefsym = u32n(dy->iextdefsym, swap);
            map->dysym_nextdefsym = u32n(dy->nextdefsym, swap);
            map->dysym_iundefsym = u32n(dy->iundefsym, swap);
            map->dysym_nundefsym = u32n(dy->nundefsym, swap);
            map->dysym_indirectsymoff = u32n(dy->indirectsymoff, swap);
            map->dysym_nindirectsyms = u32n(dy->nindirectsyms, swap);
        } else if (cmd == LC_FUNCTION_STARTS) {
            const struct linkedit_data_command *ld =
                (const struct linkedit_data_command *)scan;
            map->function_starts_off = u32n(ld->dataoff, swap);
            map->function_starts_size = u32n(ld->datasize, swap);
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            const struct dyld_info_command *di =
                (const struct dyld_info_command *)scan;
            map->dyld_info_rebase_off = u32n(di->rebase_off, swap);
            map->dyld_info_rebase_size = u32n(di->rebase_size, swap);
            map->dyld_info_bind_off = u32n(di->bind_off, swap);
            map->dyld_info_bind_size = u32n(di->bind_size, swap);
            map->dyld_info_weak_bind_off = u32n(di->weak_bind_off, swap);
            map->dyld_info_weak_bind_size = u32n(di->weak_bind_size, swap);
            map->dyld_info_lazy_bind_off = u32n(di->lazy_bind_off, swap);
            map->dyld_info_lazy_bind_size = u32n(di->lazy_bind_size, swap);
        } else if (cmd == LC_DYLD_CHAINED_FIXUPS) {
            const struct linkedit_data_command *ld =
                (const struct linkedit_data_command *)scan;
            map->has_chained_fixups = 1;
            map->chained_fixups_off = u32n(ld->dataoff, swap);
            map->chained_fixups_size = u32n(ld->datasize, swap);
        }
        scan += cmdsize;
    }

    build_bind_table(map);
    return 0;
}

void
macho_map_free(MachOMap *map)
{
    if (map == NULL) {
        return;
    }
    free(map->file);
    free(map->binds);
    memset(map, 0, sizeof(*map));
}

int
macho_vma_to_fileoff(const MachOMap *map, uint64_t vma, uint64_t *out_off)
{
    uint64_t addr = macho_strip_ptr(vma);
    for (int i = 0; i < map->nsegs; i++) {
        const MachOSeg *s = &map->segs[i];
        if (addr >= s->vmaddr && addr < s->vmaddr + s->vmsize) {
            uint64_t delta = addr - s->vmaddr;
            if (delta >= s->filesize) {
                return -1;
            }
            *out_off = s->fileoff + delta;
            return 0;
        }
    }
    return -1;
}

const uint8_t *
macho_vma_to_ptr(const MachOMap *map, uint64_t vma, size_t need)
{
    uint64_t off;
    if (macho_vma_to_fileoff(map, vma, &off) != 0) {
        return NULL;
    }
    if (off + need > map->thin_len) {
        return NULL;
    }
    return map->file + map->thin_off + off;
}

const MachOSect *
macho_find_sect(const MachOMap *map, const char *seg, const char *sect)
{
    for (int i = 0; i < map->nsects; i++) {
        if (strncmp(map->sects[i].segname, seg, 16) == 0 &&
            strncmp(map->sects[i].sectname, sect, 16) == 0) {
            return &map->sects[i];
        }
    }
    return NULL;
}

const char *
macho_cstr_at(const MachOMap *map, uint64_t vma)
{
    const uint8_t *p = macho_vma_to_ptr(map, vma, 1);
    if (p == NULL) {
        return NULL;
    }
    {
        uint64_t off;
        size_t remain;
        if (macho_vma_to_fileoff(map, vma, &off) != 0) {
            return NULL;
        }
        remain = map->thin_len - (size_t)off;
        if (memchr(p, 0, remain) == NULL) {
            return NULL;
        }
    }
    return (const char *)p;
}

const char *
macho_bind_name_at(const MachOMap *map, uint64_t vma)
{
    uint64_t addr = macho_strip_ptr(vma);
    if (!map->binds) {
        return NULL;
    }
    for (int i = 0; i < map->nbinds; i++) {
        if (map->binds[i].vma == addr) {
            return map->binds[i].name;
        }
    }
    return NULL;
}

static int
sanitize_ident(const char *in, char *out, size_t out_sz)
{
    size_t j = 0;
    int any = 0;
    if (!in || !out || out_sz == 0) {
        return -1;
    }
    for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c < 0x7f) {
            out[j++] = (char)c;
            any = 1;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '$') {
            out[j++] = (char)c;
            any = 1;
        }
    }
    out[j] = '\0';
    return any ? 0 : -1;
}

static uint64_t
decode_chained_or_ptr(const MachOMap *map, uint64_t raw)
{
    return macho_fixup_ptr(map, raw);
}

int
macho_resolve_selref(const MachOMap *map, uint64_t selref_vma, char *out,
                     size_t out_sz)
{
    const uint8_t *p;
    uint64_t raw, target;
    const char *s;
    const MachOSect *methname;
    char tmp[256];

    if (!out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';
    p = macho_vma_to_ptr(map, selref_vma, 8);
    if (!p) {
        return -1;
    }
    memcpy(&raw, p, 8);
    target = decode_chained_or_ptr(map, raw);
    s = macho_cstr_at(map, target);
    methname = macho_find_sect(map, "__TEXT", "__objc_methname");
    if ((!s || !s[0]) && methname) {
        /* Try strip differently */
        target = macho_strip_ptr(raw);
        s = macho_cstr_at(map, target);
    }
    if (s && s[0] && sanitize_ident(s, tmp, sizeof(tmp)) == 0) {
        snprintf(out, out_sz, "%s", tmp);
        return 0;
    }
    return -1;
}

int
macho_resolve_classref(const MachOMap *map, uint64_t classref_vma, char *out,
                       size_t out_sz)
{
    const char *bind;
    const uint8_t *p;
    uint64_t raw, class_vma;
    const uint8_t *cls;
    uint64_t data_ptr;
    const uint8_t *rw;
    uint64_t ro_vma;
    const uint8_t *ro;
    const char *cname;
    char tmp[256];

    if (!out || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

    bind = macho_bind_name_at(map, classref_vma);
    if (bind && bind[0]) {
        const char *pfx = "_OBJC_CLASS_$_";
        if (strncmp(bind, pfx, strlen(pfx)) == 0) {
            bind += strlen(pfx);
        } else if (bind[0] == '_') {
            bind++;
        }
        if (sanitize_ident(bind, tmp, sizeof(tmp)) == 0) {
            snprintf(out, out_sz, "%s", tmp);
            return 0;
        }
    }

    p = macho_vma_to_ptr(map, classref_vma, 8);
    if (!p) {
        return -1;
    }
    memcpy(&raw, p, 8);
    if ((raw >> 62) & 1ull) {
        /* Bind without name in table — fail gracefully */
        return -1;
    }
    class_vma = decode_chained_or_ptr(map, raw);
    cls = macho_vma_to_ptr(map, class_vma, 40);
    if (!cls) {
        return -1;
    }
    memcpy(&data_ptr, cls + 32, 8);
    data_ptr = macho_fixup_ptr(map, data_ptr);
    rw = macho_vma_to_ptr(map, data_ptr, 16);
    if (!rw) {
        return -1;
    }
    memcpy(&ro_vma, rw + 8, 8);
    ro_vma = macho_fixup_ptr(map, ro_vma);
    if (!macho_vma_to_ptr(map, ro_vma, 48)) {
        ro_vma = data_ptr;
    }
    ro = macho_vma_to_ptr(map, ro_vma, 48);
    if (!ro) {
        return -1;
    }
    {
        uint64_t name_vma;
        memcpy(&name_vma, ro + 24, 8);
        cname = macho_cstr_at(map, macho_fixup_ptr(map, name_vma));
    }
    if (cname && cname[0] && sanitize_ident(cname, tmp, sizeof(tmp)) == 0) {
        snprintf(out, out_sz, "%s", tmp);
        return 0;
    }
    return -1;
}

static int
cmp_text_fn(const void *a, const void *b)
{
    const MachOTextFn *fa = (const MachOTextFn *)a;
    const MachOTextFn *fb = (const MachOTextFn *)b;
    if (fa->vma < fb->vma) {
        return -1;
    }
    if (fa->vma > fb->vma) {
        return 1;
    }
    return 0;
}

static void
push_fn(MachOTextFn *out, int *n, int max_out, uint64_t vma, uint64_t foff,
        const char *name, const char *kind)
{
    int i;
    if (*n >= max_out || vma == 0) {
        return;
    }
    for (i = 0; i < *n; i++) {
        if (out[i].vma == vma) {
            /* Prefer named symbol over anonymous */
            if (name && name[0] &&
                (out[i].name[0] == '\0' ||
                 strncmp(out[i].name, "sub_", 4) == 0)) {
                snprintf(out[i].name, sizeof(out[i].name), "%s", name);
                if (kind) {
                    snprintf(out[i].kind, sizeof(out[i].kind), "%s", kind);
                }
            }
            return;
        }
    }
    memset(&out[*n], 0, sizeof(out[*n]));
    out[*n].vma = vma;
    out[*n].file_offset = foff;
    if (name) {
        snprintf(out[*n].name, sizeof(out[*n].name), "%s", name);
    }
    if (kind) {
        snprintf(out[*n].kind, sizeof(out[*n].kind), "%s", kind);
    }
    (*n)++;
}

int
macho_collect_text_functions(const MachOMap *map, const uint64_t *extra_vmas,
                             int n_extra, MachOTextFn *out, int max_out)
{
    const MachOSect *text;
    int n = 0;
    uint64_t text_end;

    if (!map || !out || max_out <= 0) {
        return 0;
    }
    text = macho_find_sect(map, "__TEXT", "__text");
    if (!text) {
        return 0;
    }
    text_end = text->addr + text->size;

    if (map->function_starts_off && map->function_starts_size) {
        const uint8_t *p =
            map->file + map->thin_off + map->function_starts_off;
        const uint8_t *end = p + map->function_starts_size;
        uint64_t addr = text->addr;
        while (p < end && n < max_out) {
            uint64_t delta = 0;
            int shift = 0;
            while (p < end) {
                uint8_t b = *p++;
                delta |= (uint64_t)(b & 0x7f) << shift;
                if ((b & 0x80) == 0) {
                    break;
                }
                shift += 7;
            }
            if (delta == 0 && addr != text->addr) {
                break;
            }
            addr += delta;
            if (addr >= text_end) {
                break;
            }
            {
                uint64_t foff = 0;
                char name[64];
                macho_vma_to_fileoff(map, addr, &foff);
                snprintf(name, sizeof(name), "sub_%llx",
                         (unsigned long long)addr);
                push_fn(out, &n, max_out, addr, foff, name, "function_start");
            }
        }
    }

    if (map->nsyms && map->symoff && map->stroff) {
        const struct nlist_64 *syms =
            (const struct nlist_64 *)(map->file + map->thin_off + map->symoff);
        const char *strtab =
            (const char *)(map->file + map->thin_off + map->stroff);
        uint32_t max = map->nsyms > 4000 ? 4000 : map->nsyms;
        if ((size_t)map->symoff + (size_t)map->nsyms * sizeof(struct nlist_64) >
                map->thin_len ||
            (size_t)map->stroff + map->strsize > map->thin_len) {
            max = 0;
        }
        for (uint32_t i = 0; i < max && n < max_out; i++) {
            uint32_t strx = syms[i].n_un.n_strx;
            uint64_t val = macho_strip_ptr(syms[i].n_value);
            const char *name;
            uint64_t foff = 0;
            if (strx >= map->strsize) {
                continue;
            }
            if ((syms[i].n_type & N_TYPE) != N_SECT) {
                continue;
            }
            if (val < text->addr || val >= text_end) {
                continue;
            }
            name = strtab + strx;
            if (!name[0]) {
                continue;
            }
            macho_vma_to_fileoff(map, val, &foff);
            push_fn(out, &n, max_out, val, foff, name, "symbol");
        }
    }

    for (int i = 0; i < n_extra && n < max_out; i++) {
        uint64_t v = macho_strip_ptr(extra_vmas[i]);
        uint64_t foff = 0;
        char name[64];
        if (v < text->addr || v >= text_end) {
            continue;
        }
        macho_vma_to_fileoff(map, v, &foff);
        snprintf(name, sizeof(name), "sub_%llx", (unsigned long long)v);
        push_fn(out, &n, max_out, v, foff, name, "method");
    }

    qsort(out, (size_t)n, sizeof(MachOTextFn), cmp_text_fn);
    for (int i = 0; i < n; i++) {
        uint64_t next = (i + 1 < n) ? out[i + 1].vma : text_end;
        if (next > out[i].vma) {
            out[i].size = next - out[i].vma;
        } else {
            out[i].size = 0;
        }
        /* Clamp to remaining __text */
        if (out[i].vma + out[i].size > text_end) {
            out[i].size = text_end - out[i].vma;
        }
    }
    return n;
}

uint64_t
macho_function_size_at(const MachOMap *map, uint64_t vma)
{
    MachOTextFn *fns;
    int n;
    uint64_t addr = macho_strip_ptr(vma);
    uint64_t size = 0;

    fns = (MachOTextFn *)calloc(DYLAB_MAX_TEXT_FNS, sizeof(MachOTextFn));
    if (!fns) {
        return 0;
    }
    n = macho_collect_text_functions(map, NULL, 0, fns, DYLAB_MAX_TEXT_FNS);
    for (int i = 0; i < n; i++) {
        if (fns[i].vma == addr) {
            size = fns[i].size;
            break;
        }
        if (i + 1 < n && addr >= fns[i].vma && addr < fns[i + 1].vma) {
            size = fns[i].vma + fns[i].size - addr;
            break;
        }
    }
    free(fns);
    return size;
}
