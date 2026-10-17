#include "MachOSigner.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#define DYLAB_SHA256_CTX CC_SHA256_CTX
#define DYLAB_SHA256_Init CC_SHA256_Init
#define DYLAB_SHA256_Update CC_SHA256_Update
#define DYLAB_SHA256_Final CC_SHA256_Final
#define DYLAB_SHA256_DIGEST_LENGTH CC_SHA256_DIGEST_LENGTH
#else
/* Portable SHA-256 (public domain style) for Linux unit tests */
#include <array>
namespace {
struct DYLAB_SHA256_CTX {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
};
static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
static void sha256_transform(DYLAB_SHA256_CTX *ctx, const uint8_t data[]) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t m[64], a,b,c,d,e,f,g,h,t1,t2;
    for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j+1] << 16) |
               ((uint32_t)data[j+2] << 8) | ((uint32_t)data[j+3]);
    for (uint32_t i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(m[i-15],7) ^ rotr(m[i-15],18) ^ (m[i-15] >> 3);
        uint32_t s1 = rotr(m[i-2],17) ^ rotr(m[i-2],19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        t1 = h + (rotr(e,6)^rotr(e,11)^rotr(e,25)) + ((e&f)^((~e)&g)) + k[i] + m[i];
        t2 = (rotr(a,2)^rotr(a,13)^rotr(a,22)) + ((a&b)^(a&c)^(b&c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}
static void DYLAB_SHA256_Init(DYLAB_SHA256_CTX *ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}
static void DYLAB_SHA256_Update(DYLAB_SHA256_CTX *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = p[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}
static void DYLAB_SHA256_Final(unsigned char *hash, DYLAB_SHA256_CTX *ctx) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0;
    ctx->bitlen += ctx->datalen * 8;
    for (int j = 0; j < 8; j++)
        ctx->data[63 - j] = (uint8_t)((ctx->bitlen >> (8 * j)) & 0xff);
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i + j * 4] = (ctx->state[j] >> (24 - i * 8)) & 0xff;
        }
    }
}
#define DYLAB_SHA256_DIGEST_LENGTH 32
} // namespace
#endif

#if !defined(__APPLE__)
#define MH_MAGIC_64 0xfeedfacf
#define LC_CODE_SIGNATURE 0x1d
#define LC_SEGMENT_64 0x19
struct mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
};
struct linkedit_data_command { uint32_t cmd, cmdsize, dataoff, datasize; };
#else
#include <mach-o/loader.h>
#endif

namespace {

constexpr uint32_t CSMAGIC_EMBEDDED_SIGNATURE = 0xfade0cc0;
constexpr uint32_t CSMAGIC_CODEDIRECTORY = 0xfade0c02;
constexpr uint32_t CS_HASHTYPE_SHA256 = 2;
constexpr uint32_t CS_ADHOC = 0x00000002;
constexpr uint32_t CS_LINKER_SIGNED = 0x00020000;
constexpr uint32_t CD_VERSION = 0x20400;

#pragma pack(push, 1)
struct SuperBlob {
    uint32_t magic, length, count;
};
struct BlobIndex {
    uint32_t type, offset;
};
struct CodeDirectory {
    uint32_t magic;
    uint32_t length;
    uint32_t version;
    uint32_t flags;
    uint32_t hashOffset;
    uint32_t identOffset;
    uint32_t nSpecialSlots;
    uint32_t nCodeSlots;
    uint32_t codeLimit;
    uint8_t hashSize;
    uint8_t hashType;
    uint8_t platform;
    uint8_t pageSize; /* log2 */
    uint32_t spare2;
    uint32_t scatterOffset;
    uint32_t teamOffset;
    uint32_t spare3;
    uint64_t codeLimit64;
    uint64_t execSegBase;
    uint64_t execSegLimit;
    uint64_t execSegFlags;
};
#pragma pack(pop)

static uint32_t be32(uint32_t v) {
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}
static uint64_t be64(uint64_t v) {
    return ((uint64_t)be32((uint32_t)v) << 32) | be32((uint32_t)(v >> 32));
}

static void set_err(char *e, size_t c, const std::string &m) {
    if (e && c) snprintf(e, c, "%s", m.c_str());
}

} // namespace

extern "C" int
dylab_ldid_sign(const char *path, char *err_buf, size_t err_cap)
{
    if (!path) {
        set_err(err_buf, err_cap, "null path");
        return -1;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_err(err_buf, err_cap, "cannot open");
        return -1;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), {});
    in.close();
    if (buf.size() < sizeof(mach_header_64)) {
        set_err(err_buf, err_cap, "too small");
        return -1;
    }
    auto *mh = (mach_header_64 *)buf.data();
    if (mh->magic != MH_MAGIC_64) {
        set_err(err_buf, err_cap, "not mach-o 64");
        return -1;
    }

    /* Strip existing LC_CODE_SIGNATURE and shrink __LINKEDIT if present */
    uint32_t code_limit = (uint32_t)buf.size();
    uint8_t *p = buf.data() + sizeof(mach_header_64);
    uint8_t *end = buf.data() + sizeof(mach_header_64) + mh->sizeofcmds;
    std::vector<uint8_t> newcmds;
    uint32_t ncmds = 0;
    linkedit_data_command *found_cs = nullptr;
    segment_command_64 *linkedit = nullptr;

    while (p + 8 <= end) {
        auto *lc = (load_command *)p;
        if (lc->cmdsize < 8 || p + lc->cmdsize > end) break;
        if (lc->cmd == LC_CODE_SIGNATURE) {
            found_cs = (linkedit_data_command *)lc;
            code_limit = found_cs->dataoff;
        } else {
            if (lc->cmd == LC_SEGMENT_64) {
                auto *sg = (segment_command_64 *)lc;
                if (strncmp(sg->segname, "__LINKEDIT", 16) == 0) linkedit = sg;
            }
            newcmds.insert(newcmds.end(), p, p + lc->cmdsize);
            ncmds++;
        }
        p += lc->cmdsize;
    }

    if (found_cs) {
        buf.resize(code_limit);
        mh = (mach_header_64 *)buf.data();
    } else {
        code_limit = (uint32_t)buf.size();
    }

    /* Rebuild header with space for LC_CODE_SIGNATURE */
    linkedit_data_command cscmd{};
    cscmd.cmd = LC_CODE_SIGNATURE;
    cscmd.cmdsize = sizeof(cscmd);
    /* dataoff/datasize filled later */

    std::vector<uint8_t> header;
    header.resize(sizeof(mach_header_64));
    mach_header_64 nh = *mh;
    nh.ncmds = ncmds + 1;
    nh.sizeofcmds = (uint32_t)(newcmds.size() + sizeof(cscmd));
    memcpy(header.data(), &nh, sizeof(nh));
    header.insert(header.end(), newcmds.begin(), newcmds.end());

    /* Page hashes over [0, code_limit) but code_limit is old file size.
       After we rebuild, signature is appended; codeLimit = offset of SuperBlob. */
    /* For ad-hoc: hash pages of the file excluding signature blob.
       We'll construct: [macho without sig][SuperBlob] and set codeLimit to start of SuperBlob.
       Header changed size — need to rebuild file carefully.

       Simpler approach used by many adhoc signers:
       1. Keep body as-is (already stripped of old sig)
       2. Append SuperBlob at end
       3. Add LC_CODE_SIGNATURE pointing at it
       4. Extend __LINKEDIT filesize/vmsize

       But modifying sizeofcmds requires rewriting the header in place without shifting
       if there's slack, OR rebuilding.

       Rebuild: keep file content from end of load commands to code_limit as "body_after_header".
       Actually original file layout: header+cmds | segments...
       We must not shift segment fileoffs. So we can only ADD the LC if there's padding
       in the cmds region, or put signature cmd by reclaiming space.

       Practical approach: write LC_CODE_SIGNATURE into unused cmd space if sizeofcmds has slack;
       else append a new linkedit-only signature and patch an existing empty area.

       For DyLab-produced dylibs, header+cmds are padded to page — there is slack between
       end of cmds and first section fileoff.
     */

    uint32_t old_sizeofcmds = mh->sizeofcmds;
    uint32_t first_seg_off = (uint32_t)buf.size();
    {
        uint8_t *q = buf.data() + sizeof(mach_header_64);
        uint8_t *qe = q + mh->sizeofcmds;
        while (q + 8 <= qe) {
            auto *lc = (load_command *)q;
            if (lc->cmd == LC_SEGMENT_64) {
                auto *sg = (segment_command_64 *)lc;
                if (sg->fileoff > 0 && sg->fileoff < first_seg_off) first_seg_off = (uint32_t)sg->fileoff;
            }
            q += lc->cmdsize;
        }
    }

    uint32_t cmds_end = (uint32_t)sizeof(mach_header_64) + old_sizeofcmds;
    uint32_t slack = (first_seg_off > cmds_end) ? (first_seg_off - cmds_end) : 0;

    /* Rebuild cmds into buffer start without moving segments */
    if (sizeof(cscmd) > slack + (found_cs ? sizeof(linkedit_data_command) : 0) &&
        newcmds.size() + sizeof(cscmd) + sizeof(mach_header_64) > first_seg_off) {
        set_err(err_buf, err_cap, "no room for LC_CODE_SIGNATURE in header");
        return -2;
    }

    /* Clear old cmd area and rewrite */
    uint32_t new_sizeofcmds = (uint32_t)(newcmds.size() + sizeof(cscmd));
    memset(buf.data() + sizeof(mach_header_64), 0, first_seg_off - sizeof(mach_header_64));
    memcpy(buf.data() + sizeof(mach_header_64), newcmds.data(), newcmds.size());

    uint32_t sig_off = (uint32_t)buf.size(); /* append */
    /* Build CodeDirectory */
    const char *ident = "DyLabAdHoc";
    uint32_t page_shift = 12; /* 4 KiB */
    uint32_t page_size = 1u << page_shift;
    uint32_t n_pages = (sig_off + page_size - 1) / page_size;

    uint32_t ident_off = sizeof(CodeDirectory);
    uint32_t hash_off = ident_off + (uint32_t)strlen(ident) + 1;
    hash_off = (hash_off + 7) & ~7u;
    uint32_t cd_length = hash_off + n_pages * DYLAB_SHA256_DIGEST_LENGTH;

    std::vector<uint8_t> cd(cd_length, 0);
    CodeDirectory cdr{};
    cdr.magic = be32(CSMAGIC_CODEDIRECTORY);
    cdr.length = be32(cd_length);
    cdr.version = be32(CD_VERSION);
    cdr.flags = be32(CS_ADHOC | CS_LINKER_SIGNED);
    cdr.hashOffset = be32(hash_off);
    cdr.identOffset = be32(ident_off);
    cdr.nSpecialSlots = be32(0);
    cdr.nCodeSlots = be32(n_pages);
    cdr.codeLimit = be32(sig_off);
    cdr.hashSize = DYLAB_SHA256_DIGEST_LENGTH;
    cdr.hashType = CS_HASHTYPE_SHA256;
    cdr.platform = 0;
    cdr.pageSize = (uint8_t)page_shift;
    cdr.codeLimit64 = be64(sig_off);
    memcpy(cd.data(), &cdr, sizeof(cdr));
    memcpy(cd.data() + ident_off, ident, strlen(ident) + 1);

    for (uint32_t i = 0; i < n_pages; i++) {
        uint32_t start = i * page_size;
        uint32_t len = std::min(page_size, sig_off - start);
        DYLAB_SHA256_CTX ctx;
        DYLAB_SHA256_Init(&ctx);
        DYLAB_SHA256_Update(&ctx, buf.data() + start, len);
        DYLAB_SHA256_Final(cd.data() + hash_off + i * DYLAB_SHA256_DIGEST_LENGTH, &ctx);
    }

    uint32_t sb_length = (uint32_t)(sizeof(SuperBlob) + sizeof(BlobIndex) + cd.size());
    std::vector<uint8_t> sb(sb_length, 0);
    SuperBlob sbr{};
    sbr.magic = be32(CSMAGIC_EMBEDDED_SIGNATURE);
    sbr.length = be32(sb_length);
    sbr.count = be32(1);
    memcpy(sb.data(), &sbr, sizeof(sbr));
    BlobIndex bi{};
    bi.type = be32(0); /* CSSLOT_CODEDIRECTORY */
    bi.offset = be32(sizeof(SuperBlob) + sizeof(BlobIndex));
    memcpy(sb.data() + sizeof(SuperBlob), &bi, sizeof(bi));
    memcpy(sb.data() + sizeof(SuperBlob) + sizeof(BlobIndex), cd.data(), cd.size());

    cscmd.dataoff = sig_off;
    cscmd.datasize = sb_length;
    memcpy(buf.data() + sizeof(mach_header_64) + newcmds.size(), &cscmd, sizeof(cscmd));

    mh = (mach_header_64 *)buf.data();
    mh->ncmds = ncmds + 1;
    mh->sizeofcmds = new_sizeofcmds;

    /* Extend __LINKEDIT */
    {
        uint8_t *q = buf.data() + sizeof(mach_header_64);
        uint8_t *qe = q + mh->sizeofcmds;
        while (q + 8 <= qe) {
            auto *lc = (load_command *)q;
            if (lc->cmd == LC_SEGMENT_64) {
                auto *sg = (segment_command_64 *)lc;
                if (strncmp(sg->segname, "__LINKEDIT", 16) == 0) {
                    uint64_t end_off = sig_off + sb_length;
                    sg->filesize = end_off - sg->fileoff;
                    sg->vmsize = (sg->filesize + 0x3fff) & ~0x3fffULL;
                }
            }
            q += lc->cmdsize;
        }
    }

    buf.insert(buf.end(), sb.begin(), sb.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_err(err_buf, err_cap, "cannot write");
        return -3;
    }
    out.write((const char *)buf.data(), (std::streamsize)buf.size());
    if (err_buf && err_cap) err_buf[0] = '\0';
    return 0;
}
