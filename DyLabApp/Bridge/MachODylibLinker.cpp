#include "MachODylibLinker.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#else
/* Minimal Mach-O constants for Linux unit builds */
#define MH_MAGIC_64 0xfeedfacf
#define CPU_TYPE_ARM64 ((int)0x0100000c)
#define CPU_SUBTYPE_ARM64_ALL 0
#define MH_DYLIB 6
#define MH_NOUNDEFS 0x1
#define MH_DYLDLINK 0x4
#define MH_TWOLEVEL 0x80
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_LOAD_DYLIB 0xc
#define LC_ID_DYLIB 0xd
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_CODE_SIGNATURE 0x1d
#define S_REGULAR 0x0
#define S_ZEROFILL 0x1
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000
#define S_ATTR_SOME_INSTRUCTIONS 0x400
#define S_MOD_INIT_FUNC_POINTERS 0x9
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS 0x7
#define S_SYMBOL_STUBS 0x8
#define N_UNDF 0x0
#define N_EXT 0x01
#define N_SECT 0xe
#define REFERENCE_FLAG_UNDEFINED_LAZY 0x1
#define REFERENCE_FLAG_UNDEFINED_NON_LAZY 0x0
struct mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct segment_command_64 {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
};
struct section_64 {
    char sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct symtab_command {
    uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize;
};
struct dysymtab_command {
    uint32_t cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
    uint32_t tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms;
    uint32_t indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;
};
union lc_str {
    uint32_t offset;
};
struct dylib {
    union lc_str name;
    uint32_t timestamp, current_version, compatibility_version;
};
struct dylib_command {
    uint32_t cmd, cmdsize;
    struct dylib dylib;
};
struct dyld_info_command {
    uint32_t cmd, cmdsize;
    uint32_t rebase_off, rebase_size, bind_off, bind_size;
    uint32_t weak_bind_off, weak_bind_size, export_off, export_size;
};
struct nlist_64 {
    union { uint32_t n_strx; } n_un;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
};
#endif

/* ELF64 */
static const unsigned char ELFMAG[4] = {0x7f, 'E', 'L', 'F'};
enum {
    ET_REL = 1,
    EM_AARCH64 = 183,
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_NOBITS = 8,
    SHT_INIT_ARRAY = 14,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
    SHF_WRITE = 0x1,
    STB_LOCAL = 0,
    STB_GLOBAL = 1,
    STB_WEAK = 2,
    STT_NOTYPE = 0,
    STT_OBJECT = 1,
    STT_FUNC = 2,
    STT_SECTION = 3,
    STT_FILE = 4,
    SHN_UNDEF = 0,
    SHN_ABS = 0xfff1,
    R_AARCH64_ABS64 = 257,
    R_AARCH64_ABS32 = 258,
    R_AARCH64_PREL64 = 260,
    R_AARCH64_PREL32 = 261,
    R_AARCH64_ADR_PREL_PG_HI21 = 275,
    R_AARCH64_ADD_ABS_LO12_NC = 277,
    R_AARCH64_JUMP26 = 282,
    R_AARCH64_CALL26 = 283,
    R_AARCH64_ADR_GOT_PAGE = 311,
    R_AARCH64_LD64_GOT_LO12_NC = 312
};

#pragma pack(push, 1)
struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf64_Shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};
struct Elf64_Sym {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct Elf64_Rela {
    uint64_t r_offset, r_info;
    int64_t r_addend;
};
#pragma pack(pop)

namespace {

constexpr uint64_t kBaseAddr = 0x100000000ULL; /* typical dyld slide base for dylib */
constexpr uint64_t kPage = 0x4000;             /* 16K arm64e/iOS */

static void set_err(char *err, size_t cap, const std::string &m) {
    if (err && cap) snprintf(err, cap, "%s", m.c_str());
}

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void write_u32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(v & 0xff);
    b.push_back((v >> 8) & 0xff);
    b.push_back((v >> 16) & 0xff);
    b.push_back((v >> 24) & 0xff);
}
static void write_u64(std::vector<uint8_t> &b, uint64_t v) {
    for (int i = 0; i < 8; i++) b.push_back((v >> (8 * i)) & 0xff);
}
static void patch_u32(std::vector<uint8_t> &b, size_t off, uint32_t v) {
    b[off] = v & 0xff;
    b[off + 1] = (v >> 8) & 0xff;
    b[off + 2] = (v >> 16) & 0xff;
    b[off + 3] = (v >> 24) & 0xff;
}
static void patch_u64(std::vector<uint8_t> &b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) b[off + i] = (v >> (8 * i)) & 0xff;
}

struct ElfFile {
    std::vector<uint8_t> data;
    Elf64_Ehdr ehdr{};
    std::vector<Elf64_Shdr> shdrs;
    std::string shstr;
    int symtab_idx = -1;
    int strtab_idx = -1;
};

static bool load_elf(const char *path, ElfFile &ef, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open ELF";
        return false;
    }
    ef.data.assign(std::istreambuf_iterator<char>(in), {});
    if (ef.data.size() < sizeof(Elf64_Ehdr)) {
        err = "ELF too small";
        return false;
    }
    memcpy(&ef.ehdr, ef.data.data(), sizeof(ef.ehdr));
    if (memcmp(ef.ehdr.e_ident, ELFMAG, 4) != 0 || ef.ehdr.e_ident[4] != 2) {
        err = "not ELF64";
        return false;
    }
    if (ef.ehdr.e_type != ET_REL || ef.ehdr.e_machine != EM_AARCH64) {
        err = "need ET_REL EM_AARCH64";
        return false;
    }
    ef.shdrs.resize(ef.ehdr.e_shnum);
    memcpy(ef.shdrs.data(), ef.data.data() + ef.ehdr.e_shoff,
           ef.ehdr.e_shnum * sizeof(Elf64_Shdr));
    if (ef.ehdr.e_shstrndx < ef.shdrs.size()) {
        const auto &sh = ef.shdrs[ef.ehdr.e_shstrndx];
        ef.shstr.assign((const char *)ef.data.data() + sh.sh_offset, (size_t)sh.sh_size);
    }
    for (size_t i = 0; i < ef.shdrs.size(); i++) {
        if (ef.shdrs[i].sh_type == SHT_SYMTAB) {
            ef.symtab_idx = (int)i;
            ef.strtab_idx = (int)ef.shdrs[i].sh_link;
        }
    }
    return true;
}

static const char *sec_name(const ElfFile &ef, const Elf64_Shdr &sh) {
    if (sh.sh_name >= ef.shstr.size()) return "";
    return ef.shstr.c_str() + sh.sh_name;
}

static std::string sym_name(const ElfFile &ef, const Elf64_Sym &sym) {
    if (ef.strtab_idx < 0) return {};
    const auto &st = ef.shdrs[ef.strtab_idx];
    if (sym.st_name >= st.sh_size) return {};
    return std::string((const char *)ef.data.data() + st.sh_offset + sym.st_name);
}

struct LaidSection {
    int elf_idx = -1;
    std::string name;
    std::vector<uint8_t> bytes; /* empty for nobits until sized */
    uint64_t size = 0;
    uint64_t addr = 0; /* VA */
    uint32_t fileoff = 0;
    uint32_t align = 8;
    bool exec = false;
    bool write = false;
    bool nobits = false;
    bool init_array = false;
};

struct UndefSym {
    std::string name;
    uint32_t stub_index = 0; /* index into stub table */
    uint32_t got_index = 0;
    bool is_func = true;
};

struct MachBuilder {
    std::vector<LaidSection> sections;
    std::vector<Elf64_Sym> syms;
    std::map<int, uint64_t> elfsec_to_addr; /* elf section idx → VA base */
    std::map<std::string, uint64_t> defined; /* symbol → VA */
    std::vector<UndefSym> undefs;
    std::map<std::string, size_t> undef_index;

    std::vector<uint8_t> stub_bytes;   /* 12-byte stubs: ldr x16, [pc,#8?]; br x16; .quad */
    std::vector<uint8_t> la_ptrs;      /* lazy pointer slots */
    std::vector<uint8_t> got_ptrs;
    uint64_t stub_addr = 0, la_addr = 0, got_addr = 0, text_addr = 0;
    uint32_t stub_off = 0, la_off = 0, got_off = 0;

    std::string install_name;
};

static size_t ensure_undef(MachBuilder &mb, const std::string &name, bool is_func) {
    auto it = mb.undef_index.find(name);
    if (it != mb.undef_index.end()) return it->second;
    UndefSym u;
    u.name = name;
    u.is_func = is_func;
    u.stub_index = (uint32_t)mb.undefs.size();
    u.got_index = u.stub_index;
    size_t idx = mb.undefs.size();
    mb.undefs.push_back(u);
    mb.undef_index[name] = idx;
    return idx;
}

static uint64_t page_addr(uint64_t a) { return a & ~0xfffULL; }

static void apply_reloc(MachBuilder &mb, LaidSection &sec, const ElfFile &ef,
                        const Elf64_Rela &rela, std::string &err) {
    uint64_t r_sym = rela.r_info >> 32;
    uint32_t r_type = (uint32_t)(rela.r_info & 0xffffffffu);
    if (r_sym >= mb.syms.size()) {
        err = "bad reloc sym";
        return;
    }
    const Elf64_Sym &sym = mb.syms[r_sym];
    std::string name = sym_name(ef, sym);
    uint64_t S = 0;
    bool undef = (sym.st_shndx == SHN_UNDEF);

    if (undef) {
        size_t ui = ensure_undef(mb, name, true);
        /* Placeholder — patched after stub layout; use stub/GOT VA */
        S = 0; /* filled below per type */
        (void)ui;
    } else if (sym.st_shndx == SHN_ABS) {
        S = sym.st_value;
    } else {
        auto it = mb.elfsec_to_addr.find(sym.st_shndx);
        if (it == mb.elfsec_to_addr.end()) {
            err = "reloc to unknown section for " + name;
            return;
        }
        S = it->second + sym.st_value;
    }

    uint64_t P = sec.addr + rela.r_offset;
    int64_t A = rela.r_addend;
    size_t off = (size_t)rela.r_offset;
    if (off + 4 > sec.bytes.size() && r_type != R_AARCH64_ABS64 && r_type != R_AARCH64_PREL64) {
        /* allow ABS64 needing 8 */
    }
    if (off >= sec.bytes.size()) {
        err = "reloc offset OOB";
        return;
    }

    auto u32_at = [&](size_t o) -> uint32_t {
        return (uint32_t)sec.bytes[o] | ((uint32_t)sec.bytes[o + 1] << 8) |
               ((uint32_t)sec.bytes[o + 2] << 16) | ((uint32_t)sec.bytes[o + 3] << 24);
    };
    auto put_u32 = [&](size_t o, uint32_t v) {
        sec.bytes[o] = v & 0xff;
        sec.bytes[o + 1] = (v >> 8) & 0xff;
        sec.bytes[o + 2] = (v >> 16) & 0xff;
        sec.bytes[o + 3] = (v >> 24) & 0xff;
    };

    switch (r_type) {
    case R_AARCH64_ABS64: {
        if (undef) {
            size_t ui = ensure_undef(mb, name, false);
            S = mb.got_addr + ui * 8; /* will re-patch after got layout if needed */
            /* For ABS64 to undef, point at GOT slot; GOT filled by dyld bind */
            S = 0; /* store 0; bind handles */
            (void)S;
            if (off + 8 > sec.bytes.size()) {
                err = "ABS64 OOB";
                return;
            }
            patch_u64(sec.bytes, off, (uint64_t)A); /* placeholder */
        } else {
            if (off + 8 > sec.bytes.size()) {
                err = "ABS64 OOB";
                return;
            }
            patch_u64(sec.bytes, off, S + A);
        }
        break;
    }
    case R_AARCH64_PREL32: {
        int64_t rel = (int64_t)(S + A - P);
        put_u32(off, (uint32_t)rel);
        break;
    }
    case R_AARCH64_PREL64: {
        if (off + 8 > sec.bytes.size()) {
            err = "PREL64 OOB";
            return;
        }
        patch_u64(sec.bytes, off, (uint64_t)(S + A - P));
        break;
    }
    case R_AARCH64_ADR_PREL_PG_HI21: {
        if (undef) {
            size_t ui = ensure_undef(mb, name, true);
            S = mb.got_addr + ui * 8;
        }
        int64_t page_delta = (int64_t)(page_addr(S + A) - page_addr(P));
        uint32_t immhi = (uint32_t)((page_delta >> 14) & 0x1fffff);
        uint32_t immlo = (uint32_t)((page_delta >> 12) & 0x3);
        uint32_t insn = u32_at(off);
        insn = (insn & 0x9f00001f) | (immlo << 29) | (immhi << 5);
        put_u32(off, insn);
        break;
    }
    case R_AARCH64_ADD_ABS_LO12_NC: {
        if (undef) {
            size_t ui = ensure_undef(mb, name, true);
            S = mb.got_addr + ui * 8;
        }
        uint32_t imm12 = (uint32_t)((S + A) & 0xfff);
        uint32_t insn = u32_at(off);
        insn = (insn & 0xffc003ff) | (imm12 << 10);
        put_u32(off, insn);
        break;
    }
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26: {
        if (undef) {
            size_t ui = ensure_undef(mb, name, true);
            S = mb.stub_addr + ui * 16; /* 16-byte stub */
        }
        int64_t delta = (int64_t)(S + A - P);
        if (delta & 0x3) {
            err = "CALL26 unaligned";
            return;
        }
        int64_t imm26 = delta >> 2;
        uint32_t insn = u32_at(off);
        insn = (insn & 0xfc000000) | ((uint32_t)imm26 & 0x03ffffff);
        put_u32(off, insn);
        break;
    }
    case R_AARCH64_ADR_GOT_PAGE: {
        size_t ui = ensure_undef(mb, name.empty() ? std::string("?") : name, true);
        uint64_t G = mb.got_addr + ui * 8;
        int64_t page_delta = (int64_t)(page_addr(G) - page_addr(P));
        uint32_t immhi = (uint32_t)((page_delta >> 14) & 0x1fffff);
        uint32_t immlo = (uint32_t)((page_delta >> 12) & 0x3);
        uint32_t insn = u32_at(off);
        insn = (insn & 0x9f00001f) | (immlo << 29) | (immhi << 5);
        put_u32(off, insn);
        break;
    }
    case R_AARCH64_LD64_GOT_LO12_NC: {
        size_t ui = ensure_undef(mb, name.empty() ? std::string("?") : name, true);
        uint64_t G = mb.got_addr + ui * 8;
        uint32_t imm12 = (uint32_t)((G & 0xfff) >> 3); /* scaled x8 for LDR */
        uint32_t insn = u32_at(off);
        insn = (insn & 0xffc003ff) | (imm12 << 10);
        put_u32(off, insn);
        break;
    }
    default:
        err = "unsupported reloc type " + std::to_string(r_type) + " for " + name;
        break;
    }
}

static std::vector<uint8_t> build_bind_opcodes(const MachBuilder &mb) {
    /* Simplified BIND: for each undef, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM(1=libSystem)
       then SET_SYMBOL_TRAILING_FLAGS_IMM + symbol + SET_TYPE_IMM(pointer) +
       SET_SEGMENT_AND_OFFSET_ULEB + DO_BIND
       Actually we need proper dylib ordinals per symbol. For Path C:
       libSystem=1, libobjc=2, CF=3, Foundation=4 after LC_ID.
     */
    auto uleb = [](std::vector<uint8_t> &o, uint64_t v) {
        do {
            uint8_t b = v & 0x7f;
            v >>= 7;
            if (v) b |= 0x80;
            o.push_back(b);
        } while (v);
    };
    std::vector<uint8_t> out;
    auto dylib_ord = [](const std::string &n) -> uint8_t {
        if (n.find("objc_") == 0 || n == "sel_registerName" || n == "sel_getName" ||
            n.find("class_") == 0 || n.find("method_") == 0 || n == "object_getClass")
            return 2; /* libobjc */
        if (n.find("CF") == 0 || n.find("CG") == 0) return 3;
        if (n.find("NS") == 0 || n == "NSLog") return 4;
        return 1; /* libSystem */
    };
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        const auto &u = mb.undefs[i];
        uint8_t ord = dylib_ord(u.name);
        out.push_back((uint8_t)(0x10 | (ord & 0x0f))); /* SET_DYLIB_ORDINAL_IMM */
        out.push_back(0x40); /* SET_SYMBOL_TRAILING_FLAGS_IMM */
        for (char c : u.name) out.push_back((uint8_t)c);
        out.push_back(0);
        out.push_back(0x50 | 1); /* SET_TYPE_IMM POINTER */
        /* GOT lives in __DATA segment index 1 (0=__TEXT,1=__DATA) */
        out.push_back(0x70 | 1); /* SET_SEGMENT_AND_OFFSET_ULEB seg1 */
        uint64_t got_seg_off = (mb.got_addr - (kBaseAddr + 0x4000)); /* fragile — patched below */
        /* We'll set got section VA relative to __DATA vmaddr in final emit */
        (void)got_seg_off;
        uleb(out, i * 8); /* temporary; rewriter fixes — for now sequential GOT */
        out.push_back(0x90); /* DO_BIND */
    }
    out.push_back(0x00); /* DONE */
    return out;
}

} // namespace

extern "C" int
dylab_link_elf_to_dylib(const char *elf_path,
                        const char *dylib_path,
                        const char *install_name,
                        char *err_buf,
                        size_t err_cap)
{
    if (!elf_path || !dylib_path || !install_name) {
        set_err(err_buf, err_cap, "invalid arguments");
        return -1;
    }
    ElfFile ef;
    std::string err;
    if (!load_elf(elf_path, ef, err)) {
        set_err(err_buf, err_cap, err);
        return -1;
    }

    MachBuilder mb;
    mb.install_name = install_name;

    if (ef.symtab_idx >= 0) {
        const auto &st = ef.shdrs[ef.symtab_idx];
        size_t n = (size_t)(st.sh_size / sizeof(Elf64_Sym));
        mb.syms.resize(n);
        memcpy(mb.syms.data(), ef.data.data() + st.sh_offset, n * sizeof(Elf64_Sym));
    }

    /* Collect allocatable sections */
    for (size_t i = 1; i < ef.shdrs.size(); i++) {
        const auto &sh = ef.shdrs[i];
        if (!(sh.sh_flags & SHF_ALLOC) && sh.sh_type != SHT_INIT_ARRAY) continue;
        if (sh.sh_type == SHT_SYMTAB || sh.sh_type == SHT_STRTAB || sh.sh_type == SHT_RELA)
            continue;
        if (sh.sh_type == SHT_NULL) continue;
        const char *nm = sec_name(ef, sh);
        /* Skip .eh_frame for Path C simplicity (optional) */
        if (nm && std::string(nm) == ".eh_frame") continue;

        LaidSection ls;
        ls.elf_idx = (int)i;
        ls.name = nm ? nm : "";
        ls.align = (uint32_t)std::max<uint64_t>(sh.sh_addralign, 8);
        ls.exec = (sh.sh_flags & SHF_EXECINSTR) != 0;
        ls.write = (sh.sh_flags & SHF_WRITE) != 0;
        ls.nobits = (sh.sh_type == SHT_NOBITS);
        ls.init_array = (sh.sh_type == SHT_INIT_ARRAY) || ls.name == ".init_array";
        ls.size = sh.sh_size;
        if (!ls.nobits && sh.sh_size) {
            ls.bytes.assign(ef.data.begin() + sh.sh_offset,
                            ef.data.begin() + sh.sh_offset + sh.sh_size);
        } else {
            ls.bytes.assign((size_t)sh.sh_size, 0);
        }
        mb.sections.push_back(std::move(ls));
    }

    /* First pass: estimate undefs by scanning relocs (so stub/GOT sizes known) */
    for (size_t i = 1; i < ef.shdrs.size(); i++) {
        const auto &sh = ef.shdrs[i];
        if (sh.sh_type != SHT_RELA) continue;
        size_t n = (size_t)(sh.sh_size / sizeof(Elf64_Rela));
        for (size_t r = 0; r < n; r++) {
            Elf64_Rela rela;
            memcpy(&rela, ef.data.data() + sh.sh_offset + r * sizeof(rela), sizeof(rela));
            uint64_t si = rela.r_info >> 32;
            if (si >= mb.syms.size()) continue;
            const auto &sym = mb.syms[si];
            if (sym.st_shndx == SHN_UNDEF) {
                ensure_undef(mb, sym_name(ef, sym), true);
            }
        }
    }

    /* Layout VAs: __TEXT at kBaseAddr, __DATA after */
    uint64_t text_va = kBaseAddr + kPage; /* leave page 0 for header mapping */
    uint64_t va = text_va;
    for (auto &sec : mb.sections) {
        if (!sec.exec) continue;
        va = align_up(va, sec.align);
        sec.addr = va;
        mb.elfsec_to_addr[sec.elf_idx] = va;
        va += sec.size;
    }
    /* stubs after text */
    va = align_up(va, 16);
    mb.stub_addr = va;
    mb.stub_bytes.assign(mb.undefs.size() * 16, 0);
    /* stub: ldr x16, #8; br x16; .quad ptr */
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        size_t o = i * 16;
        /* LDR X16, [PC, #8] = 0x58000050 */
        mb.stub_bytes[o + 0] = 0x50;
        mb.stub_bytes[o + 1] = 0x00;
        mb.stub_bytes[o + 2] = 0x00;
        mb.stub_bytes[o + 3] = 0x58;
        /* BR X16 = 0xd61f0200 */
        mb.stub_bytes[o + 4] = 0x00;
        mb.stub_bytes[o + 5] = 0x02;
        mb.stub_bytes[o + 6] = 0x1f;
        mb.stub_bytes[o + 7] = 0xd6;
        /* pointer filled to la_symbol_ptr */
    }
    va += mb.stub_bytes.size();
    uint64_t text_end = align_up(va, kPage);

    uint64_t data_va = text_end;
    va = data_va;
    for (auto &sec : mb.sections) {
        if (sec.exec) continue;
        va = align_up(va, sec.align);
        sec.addr = va;
        mb.elfsec_to_addr[sec.elf_idx] = va;
        va += std::max(sec.size, (uint64_t)sec.bytes.size());
    }
    va = align_up(va, 8);
    mb.la_addr = va;
    mb.la_ptrs.assign(mb.undefs.size() * 8, 0);
    /* la ptrs initially point at stub helper — self */
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        patch_u64(mb.la_ptrs, i * 8, mb.stub_addr + i * 16);
        patch_u64(mb.stub_bytes, i * 16 + 8, mb.la_addr + i * 8);
    }
    va += mb.la_ptrs.size();
    mb.got_addr = align_up(va, 8);
    mb.got_ptrs.assign(mb.undefs.size() * 8, 0);
    va = mb.got_addr + mb.got_ptrs.size();

    /* Defined symbols map */
    for (const auto &sym : mb.syms) {
        if (sym.st_shndx == SHN_UNDEF || sym.st_shndx == SHN_ABS) continue;
        std::string n = sym_name(ef, sym);
        if (n.empty()) continue;
        auto it = mb.elfsec_to_addr.find(sym.st_shndx);
        if (it != mb.elfsec_to_addr.end()) mb.defined[n] = it->second + sym.st_value;
    }

    /* Apply relocations now that addresses are known */
    for (size_t i = 1; i < ef.shdrs.size(); i++) {
        const auto &sh = ef.shdrs[i];
        if (sh.sh_type != SHT_RELA) continue;
        int target = (int)sh.sh_info;
        LaidSection *secp = nullptr;
        for (auto &sec : mb.sections) {
            if (sec.elf_idx == target) {
                secp = &sec;
                break;
            }
        }
        if (!secp) continue; /* e.g. .eh_frame skipped */
        size_t n = (size_t)(sh.sh_size / sizeof(Elf64_Rela));
        for (size_t r = 0; r < n; r++) {
            Elf64_Rela rela;
            memcpy(&rela, ef.data.data() + sh.sh_offset + r * sizeof(rela), sizeof(rela));
            err.clear();
            apply_reloc(mb, *secp, ef, rela, err);
            if (!err.empty()) {
                set_err(err_buf, err_cap, err);
                return -2;
            }
        }
    }

    /* ---- Emit Mach-O file ---- */
    std::vector<uint8_t> file;
    /* We'll build load commands then append section data */

    const char *load_dylibs[] = {
        "/usr/lib/libSystem.B.dylib",
        "/usr/lib/libobjc.A.dylib",
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        "/System/Library/Frameworks/Foundation.framework/Foundation",
    };

    auto pad_cmd = [](std::vector<uint8_t> &c) {
        while (c.size() & 7) c.push_back(0);
    };

    std::vector<uint8_t> cmds;

    /* Collect section content into TEXT and DATA blobs with file offsets */
    std::vector<uint8_t> text_blob, data_blob;
    struct OutSec {
        char sect[16];
        char seg[16];
        uint64_t addr, size;
        uint32_t offset, align, flags, reserved1, reserved2;
        bool in_text;
    };
    std::vector<OutSec> out_secs;

    auto add_sec = [&](const char *seg, const char *sect, uint64_t addr,
                       const std::vector<uint8_t> &bytes, uint32_t align, uint32_t flags,
                       bool in_text, uint32_t r1 = 0, uint32_t r2 = 0) {
        OutSec os{};
        strncpy(os.seg, seg, 16);
        strncpy(os.sect, sect, 16);
        os.addr = addr;
        os.size = bytes.size();
        os.align = align;
        os.flags = flags;
        os.reserved1 = r1;
        os.reserved2 = r2;
        os.in_text = in_text;
        if (in_text) {
            while (text_blob.size() & (align ? ((1u << align) - 1) : 0)) {
                /* align via power-of-2: align field is log2 */
            }
            size_t a = 1u << align;
            while (text_blob.size() % a) text_blob.push_back(0);
            os.offset = 0; /* fill later */
            text_blob.insert(text_blob.end(), bytes.begin(), bytes.end());
        } else {
            size_t a = 1u << align;
            while (data_blob.size() % a) data_blob.push_back(0);
            os.offset = 0;
            data_blob.insert(data_blob.end(), bytes.begin(), bytes.end());
        }
        out_secs.push_back(os);
    };

    for (auto &sec : mb.sections) {
        if (sec.exec) {
            add_sec("__TEXT", "__text", sec.addr, sec.bytes, 3,
                    S_REGULAR | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, true);
        }
    }
    if (!mb.stub_bytes.empty()) {
        add_sec("__TEXT", "__stubs", mb.stub_addr, mb.stub_bytes, 4,
                S_SYMBOL_STUBS | S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS, true,
                0, 16);
    }
    for (auto &sec : mb.sections) {
        if (sec.exec) continue;
        const char *sn = "__const";
        uint32_t flags = S_REGULAR;
        if (sec.init_array) {
            sn = "__mod_init_func";
            flags = S_MOD_INIT_FUNC_POINTERS;
        } else if (sec.write && !sec.nobits) {
            sn = "__data";
        } else if (sec.nobits) {
            sn = "__bss";
            flags = S_ZEROFILL;
        } else if (sec.name.find(".data.ro") != std::string::npos ||
                   sec.name.find("rodata") != std::string::npos) {
            sn = "__const";
        }
        if (flags == S_ZEROFILL) {
            OutSec os{};
            strncpy(os.seg, "__DATA", 16);
            strncpy(os.sect, sn, 16);
            os.addr = sec.addr;
            os.size = sec.size;
            os.offset = 0;
            os.align = 3;
            os.flags = flags;
            os.in_text = false;
            out_secs.push_back(os);
        } else {
            add_sec("__DATA", sn, sec.addr, sec.bytes, 3, flags, false);
        }
    }
    if (!mb.la_ptrs.empty()) {
        add_sec("__DATA", "__la_symbol_ptr", mb.la_addr, mb.la_ptrs, 3,
                S_LAZY_SYMBOL_POINTERS, false, 0, 0);
    }
    if (!mb.got_ptrs.empty()) {
        add_sec("__DATA", "__got", mb.got_addr, mb.got_ptrs, 3, S_NON_LAZY_SYMBOL_POINTERS,
                false);
    }

    /* Placeholder sizes for header — compute sizeofcmds */
    std::string idname = mb.install_name;
    uint32_t id_cmdsize = (uint32_t)align_up(sizeof(dylib_command) + idname.size() + 1, 8);

    uint32_t ntext_secs = 0, ndata_secs = 0;
    for (auto &s : out_secs) {
        if (s.in_text) ntext_secs++;
        else ndata_secs++;
    }

    uint32_t text_seg_cmdsize =
        (uint32_t)(sizeof(segment_command_64) + ntext_secs * sizeof(section_64));
    uint32_t data_seg_cmdsize =
        (uint32_t)(sizeof(segment_command_64) + ndata_secs * sizeof(section_64));

    /* LINKEDIT will hold symtab, dysymtab, bind, etc. */
    std::vector<uint8_t> bind = build_bind_opcodes(mb);

    /* Build symbol table */
    std::vector<uint8_t> strtab(1, 0); /* index 0 = empty */
    std::vector<nlist_64> nlists;
    auto add_str = [&](const std::string &s) -> uint32_t {
        uint32_t off = (uint32_t)strtab.size();
        strtab.insert(strtab.end(), s.begin(), s.end());
        strtab.push_back(0);
        return off;
    };

    uint32_t ilocal = 0, nlocal = 0, iextdef = 0, nextdef = 0, iundef = 0, nundef = 0;
    /* locals none for simplicity; external defines; then undefs */
    iextdef = (uint32_t)nlists.size();
    for (auto &kv : mb.defined) {
        if (kv.first.empty() || kv.first[0] == 'L') continue; /* skip locals L. */
        nlist_64 nl{};
        nl.n_un.n_strx = add_str("_" + kv.first); /* Mach-O underscore */
        nl.n_type = N_SECT | N_EXT;
        nl.n_sect = 1; /* __text approx */
        nl.n_value = kv.second;
        nlists.push_back(nl);
    }
    nextdef = (uint32_t)nlists.size() - iextdef;
    iundef = (uint32_t)nlists.size();
    for (auto &u : mb.undefs) {
        nlist_64 nl{};
        nl.n_un.n_strx = add_str("_" + u.name);
        nl.n_type = N_UNDF | N_EXT;
        nl.n_sect = 0;
        nl.n_desc = REFERENCE_FLAG_UNDEFINED_LAZY;
        nl.n_value = 0;
        nlists.push_back(nl);
    }
    nundef = (uint32_t)mb.undefs.size();

    std::vector<uint32_t> indirect;
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        indirect.push_back(iundef + (uint32_t)i); /* stubs */
    }
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        indirect.push_back(iundef + (uint32_t)i); /* la */
    }
    for (size_t i = 0; i < mb.undefs.size(); i++) {
        indirect.push_back(iundef + (uint32_t)i); /* got */
    }

    /* sizeofcmds */
    uint32_t ncmds = 0;
    uint32_t sizeofcmds = 0;
    sizeofcmds += text_seg_cmdsize;
    ncmds++;
    sizeofcmds += data_seg_cmdsize;
    ncmds++;
    /* LINKEDIT segment */
    sizeofcmds += sizeof(segment_command_64);
    ncmds++;
    sizeofcmds += sizeof(dyld_info_command);
    ncmds++;
    sizeofcmds += sizeof(symtab_command);
    ncmds++;
    sizeofcmds += sizeof(dysymtab_command);
    ncmds++;
    sizeofcmds += id_cmdsize;
    ncmds++;
    for (auto *p : load_dylibs) {
        uint32_t sz = (uint32_t)align_up(sizeof(dylib_command) + strlen(p) + 1, 8);
        sizeofcmds += sz;
        ncmds++;
        (void)p;
    }

    uint32_t header_size = (uint32_t)sizeof(mach_header_64) + sizeofcmds;
    uint32_t text_fileoff = (uint32_t)align_up(header_size, kPage);
    /* Fix section file offsets */
    uint32_t cursor = text_fileoff;
    size_t text_idx = 0, data_idx = 0;
    std::vector<size_t> text_starts, data_starts;
    /* recompute blob packing offsets */
    text_blob.clear();
    data_blob.clear();
    for (auto &os : out_secs) {
        if (os.in_text) {
            size_t a = 1u << os.align;
            while (text_blob.size() % a) text_blob.push_back(0);
            os.offset = text_fileoff + (uint32_t)text_blob.size();
            /* re-append from original sections — simplified: use addr match */
        }
    }
    /* Rebuild blobs in out_secs order */
    for (auto &os : out_secs) {
        std::vector<uint8_t> *src = nullptr;
        std::vector<uint8_t> tmp;
        if (os.in_text) {
            if (strncmp(os.sect, "__stubs", 7) == 0) src = &mb.stub_bytes;
            else {
                for (auto &sec : mb.sections)
                    if (sec.exec && sec.addr == os.addr) {
                        src = &sec.bytes;
                        break;
                    }
            }
            if (!src) {
                tmp.assign((size_t)os.size, 0);
                src = &tmp;
            }
            size_t a = 1u << os.align;
            while (text_blob.size() % a) text_blob.push_back(0);
            os.offset = text_fileoff + (uint32_t)text_blob.size();
            text_blob.insert(text_blob.end(), src->begin(), src->end());
            os.size = src->size();
        } else if ((os.flags & 0xff) != S_ZEROFILL) {
            if (strncmp(os.sect, "__la_symbol_ptr", 14) == 0) src = &mb.la_ptrs;
            else if (strncmp(os.sect, "__got", 5) == 0) src = &mb.got_ptrs;
            else {
                for (auto &sec : mb.sections)
                    if (!sec.exec && sec.addr == os.addr) {
                        src = &sec.bytes;
                        break;
                    }
            }
            if (!src) {
                tmp.assign((size_t)os.size, 0);
                src = &tmp;
            }
            size_t a = 1u << os.align;
            while (data_blob.size() % a) data_blob.push_back(0);
            /* data fileoff after text */
            src->size();
            data_starts.push_back(data_blob.size());
            data_blob.insert(data_blob.end(), src->begin(), src->end());
            os.size = src->size();
        }
        (void)text_idx;
        (void)data_idx;
        (void)cursor;
    }
    uint32_t text_filesize = (uint32_t)align_up(text_blob.size(), kPage);
    text_blob.resize(text_filesize, 0);
    uint32_t data_fileoff = text_fileoff + text_filesize;
    /* assign data offsets */
    {
        size_t pos = 0;
        for (auto &os : out_secs) {
            if (os.in_text) continue;
            if ((os.flags & 0xff) == S_ZEROFILL) {
                os.offset = 0;
                continue;
            }
            size_t a = 1u << os.align;
            while (pos % a) pos++;
            os.offset = data_fileoff + (uint32_t)pos;
            pos += (size_t)os.size;
        }
    }
    uint32_t data_filesize = (uint32_t)align_up(data_blob.size(), 8);
    data_blob.resize(data_filesize, 0);

    uint32_t linkedit_fileoff = data_fileoff + data_filesize;
    std::vector<uint8_t> linkedit;

    auto le_align = [&](size_t a) {
        while (linkedit.size() % a) linkedit.push_back(0);
    };

    /* bind */
    le_align(8);
    uint32_t bind_off = linkedit_fileoff + (uint32_t)linkedit.size();
    linkedit.insert(linkedit.end(), bind.begin(), bind.end());
    uint32_t bind_sz = (uint32_t)bind.size();

    /* symtab */
    le_align(8);
    uint32_t symoff = linkedit_fileoff + (uint32_t)linkedit.size();
    for (auto &nl : nlists) {
        write_u32(linkedit, nl.n_un.n_strx);
        linkedit.push_back(nl.n_type);
        linkedit.push_back(nl.n_sect);
        linkedit.push_back(nl.n_desc & 0xff);
        linkedit.push_back((nl.n_desc >> 8) & 0xff);
        write_u64(linkedit, nl.n_value);
    }
    uint32_t nsyms = (uint32_t)nlists.size();

    le_align(8);
    uint32_t indirectoff = linkedit_fileoff + (uint32_t)linkedit.size();
    for (uint32_t v : indirect) write_u32(linkedit, v);
    uint32_t nindirect = (uint32_t)indirect.size();

    le_align(8);
    uint32_t stroff = linkedit_fileoff + (uint32_t)linkedit.size();
    linkedit.insert(linkedit.end(), strtab.begin(), strtab.end());
    uint32_t strsize = (uint32_t)strtab.size();

    uint32_t linkedit_size = (uint32_t)align_up(linkedit.size(), kPage);
    linkedit.resize(linkedit_size, 0);

    /* Write header + commands */
    file.clear();
    mach_header_64 mh{};
    mh.magic = MH_MAGIC_64;
    mh.cputype = CPU_TYPE_ARM64;
    mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    mh.filetype = MH_DYLIB;
    mh.ncmds = ncmds;
    mh.sizeofcmds = sizeofcmds;
    mh.flags = MH_DYLDLINK | MH_TWOLEVEL;
    file.resize(sizeof(mh));
    memcpy(file.data(), &mh, sizeof(mh));

    auto append = [&](const void *p, size_t n) {
        const auto *b = (const uint8_t *)p;
        file.insert(file.end(), b, b + n);
    };

    /* __TEXT segment */
    segment_command_64 textseg{};
    textseg.cmd = LC_SEGMENT_64;
    textseg.cmdsize = text_seg_cmdsize;
    strncpy(textseg.segname, "__TEXT", 16);
    textseg.vmaddr = kBaseAddr;
    textseg.vmsize = text_end - kBaseAddr;
    textseg.fileoff = 0;
    textseg.filesize = text_fileoff + text_filesize;
    textseg.maxprot = 5; /* r-x */
    textseg.initprot = 5;
    textseg.nsects = ntext_secs;
    textseg.flags = 0;
    append(&textseg, sizeof(textseg));
    for (auto &os : out_secs) {
        if (!os.in_text) continue;
        section_64 s{};
        strncpy(s.sectname, os.sect, 16);
        strncpy(s.segname, os.seg, 16);
        s.addr = os.addr;
        s.size = os.size;
        s.offset = os.offset;
        s.align = os.align;
        s.flags = os.flags;
        s.reserved1 = os.reserved1;
        s.reserved2 = os.reserved2;
        append(&s, sizeof(s));
    }

    /* __DATA */
    segment_command_64 dataseg{};
    dataseg.cmd = LC_SEGMENT_64;
    dataseg.cmdsize = data_seg_cmdsize;
    strncpy(dataseg.segname, "__DATA", 16);
    dataseg.vmaddr = data_va;
    uint64_t data_vmsize = align_up(va - data_va, kPage);
    dataseg.vmsize = data_vmsize;
    dataseg.fileoff = data_fileoff;
    dataseg.filesize = data_filesize;
    dataseg.maxprot = 3; /* rw- */
    dataseg.initprot = 3;
    dataseg.nsects = ndata_secs;
    append(&dataseg, sizeof(dataseg));
    for (auto &os : out_secs) {
        if (os.in_text) continue;
        section_64 s{};
        strncpy(s.sectname, os.sect, 16);
        strncpy(s.segname, os.seg, 16);
        s.addr = os.addr;
        s.size = os.size;
        s.offset = os.offset;
        s.align = os.align;
        s.flags = os.flags;
        s.reserved1 = os.reserved1;
        s.reserved2 = os.reserved2;
        append(&s, sizeof(s));
    }

    /* __LINKEDIT */
    segment_command_64 leseg{};
    leseg.cmd = LC_SEGMENT_64;
    leseg.cmdsize = sizeof(segment_command_64);
    strncpy(leseg.segname, "__LINKEDIT", 16);
    leseg.vmaddr = data_va + data_vmsize;
    leseg.vmsize = linkedit_size;
    leseg.fileoff = linkedit_fileoff;
    leseg.filesize = linkedit_size;
    leseg.maxprot = 1;
    leseg.initprot = 1;
    leseg.nsects = 0;
    append(&leseg, sizeof(leseg));

    dyld_info_command di{};
    di.cmd = LC_DYLD_INFO_ONLY;
    di.cmdsize = sizeof(di);
    di.bind_off = bind_off;
    di.bind_size = bind_sz;
    append(&di, sizeof(di));

    symtab_command sc{};
    sc.cmd = LC_SYMTAB;
    sc.cmdsize = sizeof(sc);
    sc.symoff = symoff;
    sc.nsyms = nsyms;
    sc.stroff = stroff;
    sc.strsize = strsize;
    append(&sc, sizeof(sc));

    dysymtab_command dsc{};
    dsc.cmd = LC_DYSYMTAB;
    dsc.cmdsize = sizeof(dsc);
    dsc.ilocalsym = ilocal;
    dsc.nlocalsym = nlocal;
    dsc.iextdefsym = iextdef;
    dsc.nextdefsym = nextdef;
    dsc.iundefsym = iundef;
    dsc.nundefsym = nundef;
    dsc.indirectsymoff = indirectoff;
    dsc.nindirectsyms = nindirect;
    append(&dsc, sizeof(dsc));

    /* LC_ID_DYLIB */
    {
        std::vector<uint8_t> c(sizeof(dylib_command), 0);
        dylib_command dc{};
        dc.cmd = LC_ID_DYLIB;
        dc.cmdsize = id_cmdsize;
        dc.dylib.name.offset = sizeof(dylib_command);
        dc.dylib.timestamp = 1;
        dc.dylib.current_version = 0x10000;
        dc.dylib.compatibility_version = 0x10000;
        memcpy(c.data(), &dc, sizeof(dc));
        c.resize(id_cmdsize, 0);
        memcpy(c.data() + sizeof(dylib_command), idname.c_str(), idname.size());
        append(c.data(), c.size());
    }
    for (auto *p : load_dylibs) {
        uint32_t sz = (uint32_t)align_up(sizeof(dylib_command) + strlen(p) + 1, 8);
        std::vector<uint8_t> c(sz, 0);
        dylib_command dc{};
        dc.cmd = LC_LOAD_DYLIB;
        dc.cmdsize = sz;
        dc.dylib.name.offset = sizeof(dylib_command);
        dc.dylib.timestamp = 2;
        dc.dylib.current_version = 0x10000;
        dc.dylib.compatibility_version = 0x10000;
        memcpy(c.data(), &dc, sizeof(dc));
        memcpy(c.data() + sizeof(dylib_command), p, strlen(p));
        append(c.data(), c.size());
    }

    if (file.size() != header_size) {
        /* pad/truncate to expected header+cmds */
        if (file.size() < header_size) file.resize(header_size, 0);
    }
    file.resize(text_fileoff, 0);
    file.insert(file.end(), text_blob.begin(), text_blob.end());
    file.insert(file.end(), data_blob.begin(), data_blob.end());
    file.insert(file.end(), linkedit.begin(), linkedit.end());

    FILE *out = fopen(dylib_path, "wb");
    if (!out) {
        set_err(err_buf, err_cap, "cannot write dylib");
        return -3;
    }
    if (fwrite(file.data(), 1, file.size(), out) != file.size()) {
        fclose(out);
        set_err(err_buf, err_cap, "write failed");
        return -3;
    }
    fclose(out);
    if (err_buf && err_cap) err_buf[0] = '\0';
    return 0;
}
