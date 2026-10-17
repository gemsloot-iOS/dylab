#include "MachOMap.h"

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
#define LC_FUNCTION_STARTS 0x26
#define LC_DYLD_CHAINED_FIXUPS 0x34
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
struct linkedit_data_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
};
static uint32_t
OSSwapInt32(uint32_t x)
{
    return ((x & 0xff) << 24) | ((x & 0xff00) << 8) | ((x >> 8) & 0xff00) |
           (x >> 24);
}
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
        } else if (cmd == LC_DYLD_CHAINED_FIXUPS) {
            map->has_chained_fixups = 1;
        }
        scan += cmdsize;
    }
    return 0;
}

void
macho_map_free(MachOMap *map)
{
    if (map == NULL) {
        return;
    }
    free(map->file);
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
    /* Ensure NUL within remaining thin image. */
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
