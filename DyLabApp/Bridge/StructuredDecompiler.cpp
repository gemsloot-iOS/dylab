#include "StructuredDecompiler.h"
#include "MachOMap.h"

#include <cxxabi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

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

namespace {

std::string sanitize_printable(const char *s) {
    if (!s) {
        return {};
    }
    std::string out;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p >= 0x20 && *p < 0x7f) {
            out.push_back((char)*p);
        }
    }
    return out;
}

std::string demangle_symbol(const char *name) {
    if (!name || !name[0]) {
        return {};
    }
    const char *n = name;
    if (n[0] == '_') {
        /* Mach-O leading underscore; try both forms */
    }
    int status = 0;
    char *dem = abi::__cxa_demangle(n, nullptr, nullptr, &status);
    if ((!dem || status != 0) && n[0] == '_') {
        free(dem);
        dem = abi::__cxa_demangle(n + 1, nullptr, nullptr, &status);
    }
    if (dem && status == 0) {
        std::string r = dem;
        free(dem);
        return r;
    }
    free(dem);
    return sanitize_printable(name);
}

std::string strip_leading_underscore(const std::string &s) {
    if (!s.empty() && s[0] == '_') {
        return s.substr(1);
    }
    return s;
}

bool is_objc_msgsend(const std::string &name) {
    std::string n = strip_leading_underscore(name);
    return n == "objc_msgSend" || n == "objc_msgSendSuper2" ||
           n == "objc_msgSendSuper";
}

bool is_variadic_c_func(const std::string &name) {
    std::string n = strip_leading_underscore(name);
    return n == "syslog" || n == "NSLog" || n == "snprintf" ||
           n == "sprintf" || n == "fprintf" || n == "printf" ||
           n == "asprintf" || n == "dprintf";
}

#if DYLAB_HAVE_CAPSTONE

unsigned reg_x_index(unsigned cs_reg) {
    if (cs_reg >= ARM64_REG_X0 && cs_reg <= ARM64_REG_X28) {
        return cs_reg - ARM64_REG_X0;
    }
    if (cs_reg == ARM64_REG_FP || cs_reg == ARM64_REG_X29) {
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

bool is_fp_reg(unsigned cs_reg) {
    return cs_reg == ARM64_REG_FP || cs_reg == ARM64_REG_X29;
}

bool is_sp_reg(unsigned cs_reg) {
    return cs_reg == ARM64_REG_SP;
}

std::string lookup_stub(const MachOMap *map, uint64_t vma) {
    const MachOSect *stubs = macho_find_sect(map, "__TEXT", "__stubs");
    if (!stubs || !map->dysym_nindirectsyms || !map->dysym_indirectsymoff) {
        return {};
    }
    uint64_t addr = macho_strip_ptr(vma);
    if (addr < stubs->addr || addr >= stubs->addr + stubs->size) {
        return {};
    }
    uint32_t stub_size = stubs->reserved2 ? stubs->reserved2 : 12;
    if (stub_size == 0) {
        stub_size = 12;
    }
    uint64_t index = (addr - stubs->addr) / stub_size;
    uint32_t ind_index = stubs->reserved1 + (uint32_t)index;
    if (ind_index >= map->dysym_nindirectsyms) {
        return {};
    }
    if ((size_t)map->dysym_indirectsymoff +
            (size_t)(ind_index + 1) * sizeof(uint32_t) >
        map->thin_len) {
        return {};
    }
    const uint32_t *indirect =
        (const uint32_t *)(map->file + map->thin_off + map->dysym_indirectsymoff);
    uint32_t symindex = indirect[ind_index];
    if (symindex & 0x80000000u) {
        return {};
    }
    if (!map->nsyms || !map->symoff || !map->stroff || symindex >= map->nsyms) {
        return {};
    }
    if ((size_t)map->symoff + (size_t)map->nsyms * sizeof(struct nlist_64) >
            map->thin_len ||
        (size_t)map->stroff + map->strsize > map->thin_len) {
        return {};
    }
    const struct nlist_64 *syms =
        (const struct nlist_64 *)(map->file + map->thin_off + map->symoff);
    const char *strtab =
        (const char *)(map->file + map->thin_off + map->stroff);
    if (syms[symindex].n_un.n_strx >= map->strsize) {
        return {};
    }
    return sanitize_printable(strtab + syms[symindex].n_un.n_strx);
}

enum class ExprKind {
    None,
    Addr,
    CStr,
    CFStr,
    Sel,
    Class,
    Sym,
    Msg,
    Call,
    Raw
};

struct Expr {
    ExprKind kind = ExprKind::None;
    uint64_t imm = 0;
    std::string text;

    bool ok() const { return kind != ExprKind::None && !text.empty(); }
};

Expr make_raw(const std::string &t) {
    Expr e;
    e.kind = ExprKind::Raw;
    e.text = t;
    return e;
}

Expr resolve_addr(const MachOMap *map, uint64_t vma) {
    Expr e;
    uint64_t addr = macho_strip_ptr(vma);
    char tmp[192];

    auto in_sect = [](const MachOSect *s, uint64_t a) {
        return s && a >= s->addr && a < s->addr + s->size;
    };

    const MachOSect *selrefs =
        macho_find_sect(map, "__DATA_CONST", "__objc_selrefs");
    if (!selrefs) {
        selrefs = macho_find_sect(map, "__DATA", "__objc_selrefs");
    }
    const MachOSect *classrefs =
        macho_find_sect(map, "__DATA_CONST", "__objc_classrefs");
    if (!classrefs) {
        classrefs = macho_find_sect(map, "__DATA", "__objc_classrefs");
    }
    const MachOSect *cstring = macho_find_sect(map, "__TEXT", "__cstring");
    const MachOSect *methname =
        macho_find_sect(map, "__TEXT", "__objc_methname");
    const MachOSect *cfstring =
        macho_find_sect(map, "__DATA_CONST", "__cfstring");
    if (!cfstring) {
        cfstring = macho_find_sect(map, "__DATA", "__cfstring");
    }

    if (in_sect(selrefs, addr) &&
        macho_resolve_selref(map, addr, tmp, sizeof(tmp)) == 0) {
        e.kind = ExprKind::Sel;
        e.imm = addr;
        e.text = tmp;
        return e;
    }
    if (in_sect(classrefs, addr) &&
        macho_resolve_classref(map, addr, tmp, sizeof(tmp)) == 0) {
        e.kind = ExprKind::Class;
        e.imm = addr;
        e.text = tmp;
        return e;
    }
    if (in_sect(cstring, addr) || in_sect(methname, addr)) {
        const char *s = macho_cstr_at(map, addr);
        std::string p = sanitize_printable(s);
        if (!p.empty()) {
            e.kind = ExprKind::CStr;
            e.imm = addr;
            e.text = p;
            return e;
        }
    }
    if (in_sect(cfstring, addr)) {
        const uint8_t *p = macho_vma_to_ptr(map, addr, 32);
        if (p) {
            uint64_t strp;
            memcpy(&strp, p + 16, 8);
            std::string pstr =
                sanitize_printable(macho_cstr_at(map, macho_strip_ptr(strp)));
            if (!pstr.empty()) {
                e.kind = ExprKind::CFStr;
                e.imm = addr;
                e.text = pstr;
                return e;
            }
        }
    }
    std::string stub = lookup_stub(map, addr);
    if (!stub.empty()) {
        e.kind = ExprKind::Sym;
        e.imm = addr;
        e.text = stub;
        return e;
    }
    const char *bind = macho_bind_name_at(map, addr);
    if (bind && bind[0]) {
        e.kind = ExprKind::Sym;
        e.imm = addr;
        e.text = sanitize_printable(bind);
        return e;
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
                        e.kind = ExprKind::Sym;
                        e.imm = addr;
                        e.text = sanitize_printable(name);
                        return e;
                    }
                }
            }
        }
    }
    e.kind = ExprKind::Addr;
    e.imm = addr;
    char buf[64];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    e.text = buf;
    return e;
}

Expr load_through(const MachOMap *map, uint64_t addr) {
    Expr box = resolve_addr(map, addr);
    if (box.kind == ExprKind::Sel || box.kind == ExprKind::Class ||
        box.kind == ExprKind::CFStr || box.kind == ExprKind::CStr ||
        box.kind == ExprKind::Sym) {
        return box;
    }
    const uint8_t *p = macho_vma_to_ptr(map, addr, 8);
    if (p) {
        uint64_t val;
        memcpy(&val, p, 8);
        if (((val >> 62) & 1ull) == 0) {
            return resolve_addr(map, macho_strip_ptr(val));
        }
    }
    return box;
}

std::string render_expr(const Expr &e) {
    switch (e.kind) {
    case ExprKind::CStr:
        return "\"" + e.text + "\"";
    case ExprKind::CFStr:
        return "@\"" + e.text + "\"";
    case ExprKind::Sel:
        return "@selector(" + e.text + ")";
    case ExprKind::Class:
        return e.text;
    case ExprKind::Sym:
        return strip_leading_underscore(e.text);
    case ExprKind::Msg:
    case ExprKind::Call:
    case ExprKind::Raw:
    case ExprKind::Addr:
        return e.text;
    default:
        return "/*?*/";
    }
}

std::string format_objc_msg(const Expr &recv, const Expr &sel, const Expr &a2) {
    std::string r = "self";
    if (recv.kind == ExprKind::Class || recv.kind == ExprKind::Msg ||
        recv.kind == ExprKind::Call || recv.kind == ExprKind::Raw ||
        recv.kind == ExprKind::Sym) {
        r = render_expr(recv);
    } else if (recv.ok()) {
        r = render_expr(recv);
    }
    std::string s = sel.kind == ExprKind::Sel || sel.kind == ExprKind::CStr
                        ? sel.text
                        : "unknown";
    if (s.find(':') == std::string::npos) {
        return "[" + r + " " + s + "]";
    }
    std::string head = s.substr(0, s.find(':'));
    if (a2.ok() && (a2.kind == ExprKind::CFStr || a2.kind == ExprKind::CStr ||
                    a2.kind == ExprKind::Msg || a2.kind == ExprKind::Call ||
                    a2.kind == ExprKind::Class)) {
        return "[" + r + " " + head + ":" + render_expr(a2) + "]";
    }
    return "[" + r + " " + head + ":x2]";
}

std::string format_c_call(const std::string &fname, const Expr regs[32],
                          const std::map<int64_t, Expr> &sp_slots,
                          const std::map<int64_t, Expr> &fp_slots) {
    std::string fn = strip_leading_underscore(fname);
    std::vector<std::string> args;
    for (int i = 0; i < 8; i++) {
        if (regs[i].ok()) {
            args.push_back(render_expr(regs[i]));
        } else if (i < 3) {
            char b[16];
            snprintf(b, sizeof(b), "x%d", i);
            args.push_back(b);
        } else {
            break;
        }
    }
    if (is_variadic_c_func(fname)) {
        /* Pull stack args at 0,8,16,... from SP then FP */
        for (int64_t off = 0; off <= 64; off += 8) {
            auto it = sp_slots.find(off);
            if (it != sp_slots.end() && it->second.ok()) {
                args.push_back(render_expr(it->second));
            } else {
                auto jt = fp_slots.find(-off);
                if (jt == fp_slots.end()) {
                    jt = fp_slots.find(off);
                }
                if (jt != fp_slots.end() && jt->second.ok()) {
                    args.push_back(render_expr(jt->second));
                }
            }
            if (args.size() >= 6) {
                break;
            }
        }
    }
    std::ostringstream oss;
    oss << fn << "(";
    for (size_t i = 0; i < args.size(); i++) {
        if (i) {
            oss << ", ";
        }
        oss << args[i];
    }
    oss << ")";
    return oss.str();
}

struct InsnRec {
    uint64_t addr = 0;
    size_t size = 0;
    std::string mnem;
    std::string ops;
    bool is_ret = false;
    bool is_call = false;
    bool is_uncond = false;
    bool is_cond = false;
    bool has_fallthrough = true;
    uint64_t branch_target = 0;
    bool has_branch_target = false;
};

struct BasicBlock {
    int id = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    size_t begin_idx = 0;
    size_t end_idx = 0; /* exclusive */
    std::vector<int> succs;
    std::vector<int> preds;
    bool is_loop_header = false;
};

bool is_terminator_mnem(const char *m) {
    return strcmp(m, "b") == 0 || strncmp(m, "b.", 2) == 0 ||
           strcmp(m, "cbz") == 0 || strcmp(m, "cbnz") == 0 ||
           strcmp(m, "tbz") == 0 || strcmp(m, "tbnz") == 0 ||
           strcmp(m, "ret") == 0 || strcmp(m, "br") == 0 ||
           strcmp(m, "blr") == 0;
}

int structured_decompile_impl(const MachOMap *map, const uint8_t *code,
                              size_t code_size, uint64_t address,
                              std::string &out) {
    csh handle;
    cs_insn *insn = nullptr;
    size_t count = 0;

    if (cs_open(DYLAB_CS_ARM64, CS_MODE_LITTLE_ENDIAN, &handle) != CS_ERR_OK) {
        out = "// structured: cs_open failed\n";
        return -1;
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    count = cs_disasm(handle, code, code_size, address, 0, &insn);
    if (count == 0) {
        out = "// structured: no instructions\n";
        cs_close(&handle);
        return -1;
    }

    std::vector<InsnRec> recs;
    recs.reserve(count);
    for (size_t i = 0; i < count; i++) {
        InsnRec r;
        r.addr = insn[i].address;
        r.size = insn[i].size;
        r.mnem = insn[i].mnemonic;
        r.ops = insn[i].op_str;
        const char *m = insn[i].mnemonic;
        if (strcmp(m, "ret") == 0) {
            r.is_ret = true;
            r.has_fallthrough = false;
        } else if (strcmp(m, "bl") == 0 || strcmp(m, "blr") == 0) {
            r.is_call = true;
            if (insn[i].detail && strcmp(m, "bl") == 0) {
                cs_arm64 *a64 = &insn[i].detail->arm64;
                if (a64->op_count >= 1 &&
                    a64->operands[0].type == ARM64_OP_IMM) {
                    r.has_branch_target = true;
                    r.branch_target = (uint64_t)a64->operands[0].imm;
                }
            }
        } else if (strcmp(m, "b") == 0) {
            r.is_uncond = true;
            r.has_fallthrough = false;
            if (insn[i].detail) {
                cs_arm64 *a64 = &insn[i].detail->arm64;
                if (a64->op_count >= 1 &&
                    a64->operands[0].type == ARM64_OP_IMM) {
                    r.has_branch_target = true;
                    r.branch_target = (uint64_t)a64->operands[0].imm;
                }
            }
        } else if (strncmp(m, "b.", 2) == 0 || strcmp(m, "cbz") == 0 ||
                   strcmp(m, "cbnz") == 0 || strcmp(m, "tbz") == 0 ||
                   strcmp(m, "tbnz") == 0) {
            r.is_cond = true;
            if (insn[i].detail) {
                cs_arm64 *a64 = &insn[i].detail->arm64;
                for (int oi = 0; oi < a64->op_count; oi++) {
                    if (a64->operands[oi].type == ARM64_OP_IMM) {
                        r.has_branch_target = true;
                        r.branch_target = (uint64_t)a64->operands[oi].imm;
                    }
                }
            }
        }
        recs.push_back(r);
        if (r.is_ret) {
            /* keep going for CFG completeness up to size, but leaders stop */
        }
    }

    /* --- Leaders / basic blocks --- */
    std::set<uint64_t> leaders;
    if (!recs.empty()) {
        leaders.insert(recs[0].addr);
    }
    for (size_t i = 0; i < recs.size(); i++) {
        if (recs[i].has_branch_target &&
            (recs[i].is_cond || recs[i].is_uncond)) {
            leaders.insert(recs[i].branch_target);
        }
        if ((recs[i].is_cond || recs[i].is_uncond || recs[i].is_ret ||
             recs[i].is_call == false) &&
            is_terminator_mnem(recs[i].mnem.c_str()) && !recs[i].is_call) {
            if (i + 1 < recs.size() && recs[i].has_fallthrough) {
                leaders.insert(recs[i + 1].addr);
            } else if (i + 1 < recs.size() && recs[i].is_cond) {
                leaders.insert(recs[i + 1].addr);
            }
        }
        if (recs[i].is_cond && i + 1 < recs.size()) {
            leaders.insert(recs[i + 1].addr);
        }
        if ((recs[i].is_uncond || recs[i].is_ret) && i + 1 < recs.size()) {
            leaders.insert(recs[i + 1].addr);
        }
    }

    std::map<uint64_t, size_t> addr_to_idx;
    for (size_t i = 0; i < recs.size(); i++) {
        addr_to_idx[recs[i].addr] = i;
    }

    std::vector<uint64_t> leader_list(leaders.begin(), leaders.end());
    std::sort(leader_list.begin(), leader_list.end());
    /* Keep only leaders inside our insn range */
    {
        std::vector<uint64_t> filtered;
        for (uint64_t a : leader_list) {
            if (addr_to_idx.count(a)) {
                filtered.push_back(a);
            }
        }
        leader_list.swap(filtered);
    }

    std::vector<BasicBlock> blocks;
    for (size_t li = 0; li < leader_list.size(); li++) {
        BasicBlock bb;
        bb.id = (int)blocks.size();
        bb.start = leader_list[li];
        bb.begin_idx = addr_to_idx[bb.start];
        size_t end_idx = recs.size();
        if (li + 1 < leader_list.size()) {
            end_idx = addr_to_idx[leader_list[li + 1]];
        }
        bb.end_idx = end_idx;
        if (end_idx > bb.begin_idx) {
            bb.end = recs[end_idx - 1].addr + recs[end_idx - 1].size;
        } else {
            bb.end = bb.start;
        }
        blocks.push_back(bb);
    }

    std::map<uint64_t, int> start_to_block;
    for (auto &bb : blocks) {
        start_to_block[bb.start] = bb.id;
    }

    auto block_at = [&](uint64_t a) -> int {
        auto it = start_to_block.find(a);
        if (it != start_to_block.end()) {
            return it->second;
        }
        /* find containing */
        for (auto &bb : blocks) {
            if (a >= bb.start && a < bb.end) {
                return bb.id;
            }
        }
        return -1;
    };

    for (auto &bb : blocks) {
        if (bb.begin_idx >= bb.end_idx) {
            continue;
        }
        const InsnRec &last = recs[bb.end_idx - 1];
        if (last.is_uncond && last.has_branch_target) {
            int t = block_at(last.branch_target);
            if (t >= 0) {
                bb.succs.push_back(t);
            }
        } else if (last.is_cond && last.has_branch_target) {
            int t = block_at(last.branch_target);
            if (t >= 0) {
                bb.succs.push_back(t);
            }
            if (bb.end_idx < recs.size()) {
                int f = block_at(recs[bb.end_idx].addr);
                if (f >= 0) {
                    bb.succs.push_back(f);
                }
            }
        } else if (!last.is_ret && bb.end_idx < recs.size()) {
            int f = block_at(recs[bb.end_idx].addr);
            if (f >= 0) {
                bb.succs.push_back(f);
            }
        }
    }
    for (auto &bb : blocks) {
        for (int s : bb.succs) {
            blocks[s].preds.push_back(bb.id);
            if (blocks[s].start <= bb.start) {
                blocks[s].is_loop_header = true;
            }
        }
    }

    /* --- Symbolic lift + structured emit --- */
    Expr regs[32];
    std::map<int64_t, Expr> sp_slots;
    std::map<int64_t, Expr> fp_slots;
    std::ostringstream oss;

    /* Function name from symbol at entry */
    Expr entry_sym = resolve_addr(map, address);
    std::string fname = "fn";
    if (entry_sym.kind == ExprKind::Sym) {
        std::string dem = demangle_symbol(entry_sym.text.c_str());
        fname = dem.empty() ? strip_leading_underscore(entry_sym.text) : dem;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "sub_%llx", (unsigned long long)address);
        fname = buf;
    }

    oss << "// dylab structured decompile @ 0x" << std::hex << address
        << std::dec << "\n";
    oss << "// CFG: " << blocks.size() << " basic blocks\n";
    oss << "id " << fname << "(id self, SEL _cmd, ...) {\n";

    std::set<int> emitted;
    std::vector<int> order;
    for (auto &bb : blocks) {
        order.push_back(bb.id);
    }

    auto emit_indent = [&](int n) {
        for (int i = 0; i < n; i++) {
            oss << "  ";
        }
    };

    auto simulate_insn = [&](size_t idx, int indent, bool skip_branch_emit) {
        const InsnRec &r = recs[idx];
        cs_insn *ci = &insn[idx];
        const char *m = r.mnem.c_str();

        if (strcmp(m, "adrp") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_IMM) {
                unsigned reg = reg_x_index(a64->operands[0].reg);
                if (reg < 32) {
                    regs[reg].kind = ExprKind::Addr;
                    regs[reg].imm = (uint64_t)a64->operands[1].imm;
                    char b[64];
                    snprintf(b, sizeof(b), "0x%llx",
                             (unsigned long long)regs[reg].imm);
                    regs[reg].text = b;
                }
            }
            return;
        }
        if (strcmp(m, "add") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 3 && a64->operands[2].type == ARM64_OP_IMM) {
                unsigned rd = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].reg);
                if (rd < 32 && rn < 32 && regs[rn].kind == ExprKind::Addr) {
                    uint64_t target =
                        regs[rn].imm + (uint64_t)a64->operands[2].imm;
                    regs[rd] = resolve_addr(map, target);
                    emit_indent(indent);
                    oss << "x" << rd << " = " << render_expr(regs[rd]) << ";\n";
                }
            }
            return;
        }
        if ((strcmp(m, "ldr") == 0 || strcmp(m, "ldur") == 0) && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = reg_x_index(a64->operands[0].reg);
                unsigned base = a64->operands[1].mem.base;
                int64_t disp = a64->operands[1].mem.disp;
                if (rt >= 32) {
                    return;
                }
                if (is_fp_reg(base)) {
                    auto it = fp_slots.find(disp);
                    if (it != fp_slots.end()) {
                        regs[rt] = it->second;
                        emit_indent(indent);
                        oss << "x" << rt << " = " << render_expr(regs[rt])
                            << "; // reload\n";
                    }
                } else if (is_sp_reg(base)) {
                    auto it = sp_slots.find(disp);
                    if (it != sp_slots.end()) {
                        regs[rt] = it->second;
                        emit_indent(indent);
                        oss << "x" << rt << " = " << render_expr(regs[rt])
                            << "; // reload\n";
                    }
                } else {
                    unsigned rn = reg_x_index(base);
                    if (rn < 32 && regs[rn].kind != ExprKind::None) {
                        uint64_t target = regs[rn].imm + (uint64_t)disp;
                        regs[rt] = load_through(map, target);
                        emit_indent(indent);
                        oss << "x" << rt << " = " << render_expr(regs[rt])
                            << ";\n";
                    }
                }
            }
            return;
        }
        if ((strcmp(m, "str") == 0 || strcmp(m, "stur") == 0) && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_MEM) {
                unsigned rt = reg_x_index(a64->operands[0].reg);
                unsigned base = a64->operands[1].mem.base;
                int64_t disp = a64->operands[1].mem.disp;
                if (rt < 32) {
                    if (is_fp_reg(base)) {
                        fp_slots[disp] = regs[rt];
                    } else if (is_sp_reg(base)) {
                        sp_slots[disp] = regs[rt];
                    }
                }
            }
            return;
        }
        if (strcmp(m, "stp") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_MEM) {
                unsigned r0 = reg_x_index(a64->operands[0].reg);
                unsigned r1 = reg_x_index(a64->operands[1].reg);
                unsigned base = a64->operands[2].mem.base;
                int64_t disp = a64->operands[2].mem.disp;
                if (is_fp_reg(base)) {
                    if (r0 < 32) {
                        fp_slots[disp] = regs[r0];
                    }
                    if (r1 < 32) {
                        fp_slots[disp + 8] = regs[r1];
                    }
                } else if (is_sp_reg(base)) {
                    if (r0 < 32) {
                        sp_slots[disp] = regs[r0];
                    }
                    if (r1 < 32) {
                        sp_slots[disp + 8] = regs[r1];
                    }
                }
            }
            return;
        }
        if (strcmp(m, "ldp") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 3 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG &&
                a64->operands[2].type == ARM64_OP_MEM) {
                unsigned r0 = reg_x_index(a64->operands[0].reg);
                unsigned r1 = reg_x_index(a64->operands[1].reg);
                unsigned base = a64->operands[2].mem.base;
                int64_t disp = a64->operands[2].mem.disp;
                auto &slots = is_fp_reg(base) ? fp_slots : sp_slots;
                if ((is_fp_reg(base) || is_sp_reg(base))) {
                    if (r0 < 32 && slots.count(disp)) {
                        regs[r0] = slots[disp];
                    }
                    if (r1 < 32 && slots.count(disp + 8)) {
                        regs[r1] = slots[disp + 8];
                    }
                }
            }
            return;
        }
        if (strcmp(m, "mov") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            if (a64->op_count >= 2 && a64->operands[0].type == ARM64_OP_REG &&
                a64->operands[1].type == ARM64_OP_REG) {
                unsigned rd = reg_x_index(a64->operands[0].reg);
                unsigned rn = reg_x_index(a64->operands[1].reg);
                if (rd < 32 && rn < 32) {
                    regs[rd] = regs[rn];
                }
            }
            return;
        }
        if (strcmp(m, "bl") == 0 && ci->detail) {
            cs_arm64 *a64 = &ci->detail->arm64;
            Expr target;
            if (a64->op_count >= 1 && a64->operands[0].type == ARM64_OP_IMM) {
                target = resolve_addr(map, (uint64_t)a64->operands[0].imm);
            }
            emit_indent(indent);
            if (is_objc_msgsend(target.text)) {
                Expr folded;
                folded.kind = ExprKind::Msg;
                folded.text = format_objc_msg(regs[0], regs[1], regs[2]);
                oss << "id r = " << folded.text << ";\n";
                regs[0] = folded;
            } else if (target.kind == ExprKind::Sym) {
                std::string call =
                    format_c_call(target.text, regs, sp_slots, fp_slots);
                Expr folded;
                folded.kind = ExprKind::Call;
                folded.text = call;
                oss << call << ";\n";
                regs[0] = folded;
            } else {
                oss << "bl(" << r.ops << ");\n";
            }
            return;
        }
        if (r.is_ret) {
            emit_indent(indent);
            if (regs[0].ok()) {
                oss << "return " << render_expr(regs[0]) << ";\n";
            } else {
                oss << "return x0;\n";
            }
            return;
        }
        if ((r.is_cond || r.is_uncond) && !skip_branch_emit) {
            /* handled at block level */
            return;
        }
    };

    for (int bid : order) {
        if (emitted.count(bid)) {
            continue;
        }
        BasicBlock &bb = blocks[bid];
        emitted.insert(bid);

        int indent = 1;
        bool structured = false;

        /* while-loop: header with back-edge successor */
        if (bb.is_loop_header && bb.end_idx > bb.begin_idx) {
            const InsnRec &last = recs[bb.end_idx - 1];
            if (last.is_cond) {
                emit_indent(indent);
                oss << "while (cond /* " << last.mnem << " " << last.ops
                    << " */) {\n";
                for (size_t i = bb.begin_idx; i + 1 < bb.end_idx; i++) {
                    simulate_insn(i, indent + 1, true);
                }
                emit_indent(indent);
                oss << "}\n";
                structured = true;
            }
        }

        /* if/else: conditional branch */
        if (!structured && bb.end_idx > bb.begin_idx) {
            const InsnRec &last = recs[bb.end_idx - 1];
            if (last.is_cond && bb.succs.size() >= 1) {
                for (size_t i = bb.begin_idx; i + 1 < bb.end_idx; i++) {
                    simulate_insn(i, indent, true);
                }
                emit_indent(indent);
                oss << "if (cond /* " << last.mnem << " " << last.ops
                    << " */) {\n";
                /* then-block = branch target if not fallthrough */
                int then_id = -1;
                int else_id = -1;
                if (last.has_branch_target) {
                    then_id = block_at(last.branch_target);
                }
                if (bb.end_idx < recs.size()) {
                    else_id = block_at(recs[bb.end_idx].addr);
                }
                if (then_id >= 0 && then_id != bid && !emitted.count(then_id)) {
                    BasicBlock &tb = blocks[then_id];
                    for (size_t i = tb.begin_idx; i < tb.end_idx; i++) {
                        if (recs[i].is_cond || recs[i].is_uncond) {
                            continue;
                        }
                        simulate_insn(i, indent + 1, true);
                    }
                    emitted.insert(then_id);
                }
                if (else_id >= 0 && else_id != then_id && else_id != bid &&
                    !emitted.count(else_id)) {
                    emit_indent(indent);
                    oss << "} else {\n";
                    BasicBlock &eb = blocks[else_id];
                    for (size_t i = eb.begin_idx; i < eb.end_idx; i++) {
                        if (recs[i].is_cond || recs[i].is_uncond) {
                            continue;
                        }
                        simulate_insn(i, indent + 1, true);
                    }
                    emitted.insert(else_id);
                }
                emit_indent(indent);
                oss << "}\n";
                structured = true;
            }
        }

        if (!structured) {
            if (bb.is_loop_header) {
                emit_indent(indent);
                oss << "/* loop header bb_" << bb.id << " */\n";
            }
            for (size_t i = bb.begin_idx; i < bb.end_idx; i++) {
                const InsnRec &r = recs[i];
                if (r.is_uncond && r.has_branch_target) {
                    emit_indent(indent);
                    oss << "goto label_" << std::hex << r.branch_target
                        << std::dec << ";\n";
                    continue;
                }
                if (r.is_cond) {
                    emit_indent(indent);
                    oss << "if (cond /* " << r.mnem << " */) goto label_"
                        << std::hex << r.branch_target << std::dec << ";\n";
                    continue;
                }
                simulate_insn(i, indent, false);
            }
        }
    }

    oss << "}\n";
    out = oss.str();

    cs_free(insn, count);
    cs_close(&handle);
    return 0;
}

#else /* !DYLAB_HAVE_CAPSTONE */

int structured_decompile_impl(const MachOMap *, const uint8_t *, size_t,
                              uint64_t address, std::string &out) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "// Capstone unavailable — structured decompile @ 0x%llx skipped\n",
             (unsigned long long)address);
    out = buf;
    return -1;
}

#endif

} // namespace

extern "C" int
structured_decompile(const char *path, uint64_t file_offset, uint64_t size,
                     uint64_t vma_hint, char *out_buf, size_t out_buf_size) {
    MachOMap map;
    const MachOSect *text;
    uint64_t address;
    std::string result;

    if (!out_buf || out_buf_size == 0 || !path) {
        return -1;
    }
    out_buf[0] = '\0';

    if (macho_map_load(path, &map) != 0) {
        snprintf(out_buf, out_buf_size, "// failed to load Mach-O\n");
        return -1;
    }

    text = macho_find_sect(&map, "__TEXT", "__text");
    if (file_offset >= map.thin_len) {
        snprintf(out_buf, out_buf_size, "// file_offset out of range\n");
        macho_map_free(&map);
        return -1;
    }

    address = vma_hint;
    if (address == 0) {
        for (int i = 0; i < map.nsegs; i++) {
            if (file_offset >= map.segs[i].fileoff &&
                file_offset < map.segs[i].fileoff + map.segs[i].filesize) {
                address =
                    map.segs[i].vmaddr + (file_offset - map.segs[i].fileoff);
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

    const uint8_t *code = map.file + map.thin_off + file_offset;
    int rc = structured_decompile_impl(&map, code, (size_t)size, address, result);
    snprintf(out_buf, out_buf_size, "%s", result.c_str());
    macho_map_free(&map);
    return rc;
}
