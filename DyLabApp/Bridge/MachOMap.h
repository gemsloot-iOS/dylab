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
#define DYLAB_MAX_BINDS 4096
#define DYLAB_MAX_TEXT_FNS 4096

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
    uint64_t vma;
    char name[192];
} MachOBind;

typedef struct {
    uint64_t vma;
    uint64_t file_offset;
    uint64_t size; /* bytes until next start or end of __text */
    char name[160];
    char kind[32]; /* function_start | symbol | method */
} MachOTextFn;

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
    uint32_t dyld_info_rebase_off;
    uint32_t dyld_info_rebase_size;
    uint32_t dyld_info_bind_off;
    uint32_t dyld_info_bind_size;
    uint32_t dyld_info_lazy_bind_off;
    uint32_t dyld_info_lazy_bind_size;
    uint32_t dyld_info_weak_bind_off;
    uint32_t dyld_info_weak_bind_size;
    uint32_t chained_fixups_off;
    uint32_t chained_fixups_size;
    int has_chained_fixups;
    MachOBind *binds;
    int nbinds;
} MachOMap;

int macho_map_load(const char *path, MachOMap *map);
void macho_map_free(MachOMap *map);

uint64_t macho_strip_ptr(uint64_t vma);

/**
 * Decode a pointer value read from __DATA* when the image may use dyld
 * chained fixups. For chained rebases, strip to the 36-bit target; for
 * plain images, behave like macho_strip_ptr. Bind entries return 0.
 */
uint64_t macho_fixup_ptr(const MachOMap *map, uint64_t raw);

/** Read an 8-byte pointer at vma and run it through macho_fixup_ptr. */
uint64_t macho_read_ptr(const MachOMap *map, uint64_t vma);

const uint8_t *macho_vma_to_ptr(const MachOMap *map, uint64_t vma, size_t need);
int macho_vma_to_fileoff(const MachOMap *map, uint64_t vma, uint64_t *out_off);
const MachOSect *macho_find_sect(const MachOMap *map, const char *seg,
                                 const char *sect);
const char *macho_cstr_at(const MachOMap *map, uint64_t vma);

/** Look up dyld bind / chained import name at a data VMA (e.g. classref slot). */
const char *macho_bind_name_at(const MachOMap *map, uint64_t vma);

/** Resolve __objc_selrefs slot → selector string (printable). */
int macho_resolve_selref(const MachOMap *map, uint64_t selref_vma, char *out,
                         size_t out_sz);

/** Resolve __objc_classrefs slot → class name (e.g. NSBundle). */
int macho_resolve_classref(const MachOMap *map, uint64_t classref_vma, char *out,
                           size_t out_sz);

/**
 * Collect callable starts in __TEXT,__text from function_starts, symtab,
 * and optional extra VMAs (e.g. ObjC method imps). Sorts and fills sizes.
 * @return number of entries written (<= max_out)
 */
int macho_collect_text_functions(const MachOMap *map,
                                 const uint64_t *extra_vmas, int n_extra,
                                 MachOTextFn *out, int max_out);

/** Size of function containing vma (0 if unknown). */
uint64_t macho_function_size_at(const MachOMap *map, uint64_t vma);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_MACHO_MAP_H */
