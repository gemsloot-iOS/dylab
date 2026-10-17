#ifndef DYLAB_MACHO_MAP_H
#define DYLAB_MACHO_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DYLAB_VMA_MASK 0x7FFFFFFFFFULL
#define DYLAB_MAX_SEGS 64
#define DYLAB_MAX_SECTS 256

typedef struct {
    char segname[16];
    char sectname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t flags;
    uint32_t reserved1; /* stubs: index into indirect symbol table */
    uint32_t reserved2; /* stubs: stub size in bytes */
} MachOSect;

typedef struct {
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
} MachOSeg;

typedef struct {
    uint8_t *file;
    size_t file_len;
    size_t thin_off;
    size_t thin_len;
    int is_fat;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    const uint8_t *cmds;
    MachOSeg segs[DYLAB_MAX_SEGS];
    int nsegs;
    MachOSect sects[DYLAB_MAX_SECTS];
    int nsects;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
    uint32_t dysym_ilocalsym;
    uint32_t dysym_nlocalsym;
    uint32_t dysym_iextdefsym;
    uint32_t dysym_nextdefsym;
    uint32_t dysym_iundefsym;
    uint32_t dysym_nundefsym;
    uint32_t dysym_indirectsymoff;
    uint32_t dysym_nindirectsyms;
    uint32_t function_starts_off;
    uint32_t function_starts_size;
    int has_chained_fixups;
} MachOMap;

int macho_map_load(const char *path, MachOMap *map);
void macho_map_free(MachOMap *map);

uint64_t macho_strip_ptr(uint64_t vma);
const uint8_t *macho_vma_to_ptr(const MachOMap *map, uint64_t vma, size_t need);
int macho_vma_to_fileoff(const MachOMap *map, uint64_t vma, uint64_t *out_off);
const MachOSect *macho_find_sect(const MachOMap *map, const char *seg,
                                 const char *sect);
const char *macho_cstr_at(const MachOMap *map, uint64_t vma);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_MACHO_MAP_H */
