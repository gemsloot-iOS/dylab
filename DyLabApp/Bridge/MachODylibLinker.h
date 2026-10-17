#ifndef DYLAB_MACHO_DYLIB_LINKER_H
#define DYLAB_MACHO_DYLIB_LINKER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Link a TinyCC ARM64 ELF relocatable (.o) into an MH_DYLIB arm64 Mach-O.
 *
 * Emits: __TEXT,__text (+ stubs), __DATA,__data / __const / __bss,
 * __DATA,__la_symbol_ptr / __got, LC_ID_DYLIB, LC_LOAD_DYLIB
 * (libSystem, libobjc, CoreFoundation, Foundation), LC_SYMTAB/DYSYMTAB,
 * LC_DYLD_INFO_ONLY (binds), __DATA,__mod_init_func for constructors.
 *
 * Relocs handled: R_AARCH64_ABS64, PREL32/64, ADR_PREL_PG_HI21,
 * ADD_ABS_LO12_NC, CALL26, JUMP26, ADR_GOT_PAGE, LD64_GOT_LO12_NC.
 *
 * @param elf_path     Input ELF64 .o from libtcc (TCC_OUTPUT_OBJ)
 * @param dylib_path   Output path for the .dylib
 * @param install_name LC_ID_DYLIB install name (e.g. /Library/MobileSubstrate/DynamicLibraries/X.dylib)
 * @param err_buf      Error text
 * @param err_cap      Capacity
 * @return 0 on success
 */
int dylab_link_elf_to_dylib(const char *elf_path,
                            const char *dylib_path,
                            const char *install_name,
                            char *err_buf,
                            size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_MACHO_DYLIB_LINKER_H */
