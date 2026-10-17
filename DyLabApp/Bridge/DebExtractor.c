#define _XOPEN_SOURCE 700
#include "DebExtractor.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>

#include <zlib.h>

#if __has_include(<lzma.h>)
#include <lzma.h>
#define DYLAB_HAVE_LZMA 1
#endif

#if __has_include(<bzlib.h>)
#include <bzlib.h>
#define DYLAB_HAVE_BZ2 1
#endif

#if __has_include("zstd.h")
#include "zstd.h"
#define DYLAB_HAVE_ZSTD 1
#elif __has_include(<zstd.h>)
#include <zstd.h>
#define DYLAB_HAVE_ZSTD 1
#endif

#if defined(__APPLE__)
#include <compression.h>
#endif

#define AR_MAGIC "!<arch>\n"
#define AR_HEADER_SIZE 60
#define TAR_BLOCK 512
#define AR_NAME_MAX 255

static void
set_err(char *err_buf, size_t err_buf_size, const char *msg)
{
    if (err_buf == NULL || err_buf_size == 0) {
        return;
    }
    snprintf(err_buf, err_buf_size, "%s", msg ? msg : "unknown error");
}

static void
format_magic_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    size_t n = len < 8 ? len : 8;
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n && used + 3 < out_size; i++) {
        int w = snprintf(out + used, out_size - used, "%02x%s", data[i],
                         (i + 1 < n) ? " " : "");
        if (w < 0) {
            break;
        }
        used += (size_t)w;
    }
}

static void
set_decompress_err(char *err_buf, size_t err_buf_size, const char *kind,
                   const uint8_t *data, size_t len, const char *detail)
{
    char magic[32];
    format_magic_hex(data, len, magic, sizeof(magic));
    if (err_buf == NULL || err_buf_size == 0) {
        return;
    }
    snprintf(err_buf, err_buf_size,
             "decompress failed for %s (magic: %s)%s%s", kind,
             magic[0] ? magic : "(empty)", detail ? ": " : "",
             detail ? detail : "");
}

static int
ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    return (errno == EEXIST) ? 0 : -1;
}

static int
nftw_rm_cb(const char *fpath, const struct stat *sb, int typeflag,
           struct FTW *ftwbuf)
{
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    return remove(fpath);
}

static void
remove_tree(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return;
    }
    nftw(path, nftw_rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static int
ensure_parent_dirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (ensure_dir(path) != 0) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    return 0;
}

static int
copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = NULL;
    char buf[8192];
    size_t n;
    int rc = -1;

    if (in == NULL) {
        return -1;
    }
    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            goto done;
        }
    }
    rc = ferror(in) ? -1 : 0;
done:
    fclose(in);
    fclose(out);
    return rc;
}

static char *
read_entire_file(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[sz] = '\0';
    *out_size = (size_t)sz;
    return buf;
}

static int
grow_buf(uint8_t **buf, size_t *cap, size_t used, size_t need_more)
{
    size_t ncap = *cap ? *cap : 64 * 1024;
    while (used + need_more > ncap) {
        ncap *= 2;
    }
    if (ncap == *cap) {
        return 0;
    }
    uint8_t *grown = (uint8_t *)realloc(*buf, ncap);
    if (grown == NULL) {
        return -1;
    }
    *buf = grown;
    *cap = ncap;
    return 0;
}

static int
gzip_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                 size_t *out_len)
{
    z_stream strm;
    size_t cap = src_len * 4 + 64 * 1024;
    uint8_t *dst;
    int ret;

    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        return -1;
    }

    dst = (uint8_t *)malloc(cap);
    if (dst == NULL) {
        inflateEnd(&strm);
        return -1;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)cap;

    for (;;) {
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK) {
            free(dst);
            inflateEnd(&strm);
            return -1;
        }
        if (strm.avail_out == 0) {
            size_t used = (size_t)(strm.next_out - dst);
            if (grow_buf(&dst, &cap, used, 64 * 1024) != 0) {
                free(dst);
                inflateEnd(&strm);
                return -1;
            }
            strm.next_out = dst + used;
            strm.avail_out = (uInt)(cap - used);
        }
    }

    *out_len = strm.total_out;
    *out_buf = dst;
    inflateEnd(&strm);
    return 0;
}

#if DYLAB_HAVE_LZMA
static int
lzma_lib_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                     size_t *out_len)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    size_t cap = src_len * 8 + 128 * 1024;
    uint8_t *dst;
    lzma_ret ret;

    if (lzma_auto_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) {
        return -1;
    }

    dst = (uint8_t *)malloc(cap);
    if (dst == NULL) {
        lzma_end(&strm);
        return -1;
    }

    strm.next_in = src;
    strm.avail_in = src_len;
    strm.next_out = dst;
    strm.avail_out = cap;

    for (;;) {
        ret = lzma_code(&strm, strm.avail_in ? LZMA_RUN : LZMA_FINISH);
        if (ret == LZMA_STREAM_END) {
            break;
        }
        if (ret != LZMA_OK) {
            free(dst);
            lzma_end(&strm);
            return -1;
        }
        if (strm.avail_out == 0) {
            size_t used = (size_t)(strm.next_out - dst);
            if (grow_buf(&dst, &cap, used, 128 * 1024) != 0) {
                free(dst);
                lzma_end(&strm);
                return -1;
            }
            strm.next_out = dst + used;
            strm.avail_out = cap - used;
        }
    }

    *out_len = (size_t)(strm.next_out - dst);
    *out_buf = dst;
    lzma_end(&strm);
    return 0;
}
#endif

#if defined(__APPLE__)
static int
apple_lzma_stream_decompress(const uint8_t *src, size_t src_len,
                             uint8_t **out_buf, size_t *out_len)
{
    compression_stream stream;
    size_t cap = src_len * 8 + 128 * 1024;
    uint8_t *dst;
    compression_status status;

    if (compression_stream_init(&stream, COMPRESSION_STREAM_DECODE,
                                COMPRESSION_LZMA) != COMPRESSION_STATUS_OK) {
        return -1;
    }

    dst = (uint8_t *)malloc(cap);
    if (dst == NULL) {
        compression_stream_destroy(&stream);
        return -1;
    }

    stream.src_ptr = src;
    stream.src_size = src_len;
    stream.dst_ptr = dst;
    stream.dst_size = cap;

    for (;;) {
        status = compression_stream_process(&stream, COMPRESSION_STREAM_FINALIZE);
        if (status == COMPRESSION_STATUS_END) {
            break;
        }
        if (status == COMPRESSION_STATUS_ERROR) {
            free(dst);
            compression_stream_destroy(&stream);
            return -1;
        }
        if (status == COMPRESSION_STATUS_OK && stream.dst_size == 0) {
            size_t used = (size_t)(stream.dst_ptr - dst);
            if (grow_buf(&dst, &cap, used, 128 * 1024) != 0) {
                free(dst);
                compression_stream_destroy(&stream);
                return -1;
            }
            stream.dst_ptr = dst + used;
            stream.dst_size = cap - used;
            continue;
        }
        if (status != COMPRESSION_STATUS_OK) {
            free(dst);
            compression_stream_destroy(&stream);
            return -1;
        }
    }

    *out_len = (size_t)(stream.dst_ptr - dst);
    *out_buf = dst;
    compression_stream_destroy(&stream);
    return 0;
}
#endif

static int
lzma_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                size_t *out_len)
{
#if DYLAB_HAVE_LZMA
    if (lzma_lib_decompress(src, src_len, out_buf, out_len) == 0) {
        return 0;
    }
#endif
#if defined(__APPLE__)
    if (apple_lzma_stream_decompress(src, src_len, out_buf, out_len) == 0) {
        return 0;
    }
#endif
    (void)src;
    (void)src_len;
    (void)out_buf;
    (void)out_len;
    return -1;
}

#if DYLAB_HAVE_ZSTD
static int
zstd_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                size_t *out_len)
{
    unsigned long long frame_size = ZSTD_getFrameContentSize(src, src_len);
    size_t cap;
    uint8_t *dst;
    size_t result;

    if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
        return -1;
    }
    if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        cap = src_len * 8 + 128 * 1024;
    } else {
        cap = (size_t)frame_size + 64;
    }

    dst = (uint8_t *)malloc(cap);
    if (dst == NULL) {
        return -1;
    }

    result = ZSTD_decompress(dst, cap, src, src_len);
    if (ZSTD_isError(result)) {
        /* Retry with a larger buffer if bound was unknown/too small. */
        free(dst);
        cap *= 4;
        dst = (uint8_t *)malloc(cap);
        if (dst == NULL) {
            return -1;
        }
        result = ZSTD_decompress(dst, cap, src, src_len);
        if (ZSTD_isError(result)) {
            free(dst);
            return -1;
        }
    }

    *out_buf = dst;
    *out_len = result;
    return 0;
}
#else
static int
zstd_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                size_t *out_len)
{
    (void)src;
    (void)src_len;
    (void)out_buf;
    (void)out_len;
    return -1;
}
#endif

#if DYLAB_HAVE_BZ2
static int
bz2_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
               size_t *out_len)
{
    size_t cap = src_len * 8 + 128 * 1024;
    for (int attempt = 0; attempt < 6; attempt++) {
        uint8_t *dst = (uint8_t *)malloc(cap);
        unsigned int dest_len;
        int ret;

        if (dst == NULL) {
            return -1;
        }
        dest_len = (unsigned int)cap;
        ret = BZ2_bzBuffToBuffDecompress((char *)dst, &dest_len, (char *)src,
                                         (unsigned int)src_len, 0, 0);
        if (ret == BZ_OK) {
            *out_buf = dst;
            *out_len = dest_len;
            return 0;
        }
        free(dst);
        if (ret != BZ_OUTBUFF_FULL) {
            return -1;
        }
        cap *= 2;
    }
    return -1;
}
#else
static int
bz2_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
               size_t *out_len)
{
    (void)src;
    (void)src_len;
    (void)out_buf;
    (void)out_len;
    return -1;
}
#endif

static int
is_safe_tar_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '/') {
        return 0;
    }
    if (strstr(name, "..") != NULL) {
        return 0;
    }
    return 1;
}

static unsigned long
tar_octal(const char *field, size_t len)
{
    unsigned long value = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        i++;
    }
    for (; i < len && field[i] >= '0' && field[i] <= '7'; i++) {
        value = (value << 3) + (unsigned long)(field[i] - '0');
    }
    return value;
}

static int
extract_tar(const uint8_t *tar, size_t tar_len, const char *dest_root,
            char *err_buf, size_t err_buf_size)
{
    size_t offset = 0;

    while (offset + TAR_BLOCK <= tar_len) {
        const char *block = (const char *)(tar + offset);
        char name[256];
        char typeflag;
        unsigned long size;
        int all_zero = 1;

        for (int i = 0; i < TAR_BLOCK; i++) {
            if (block[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            break;
        }

        memset(name, 0, sizeof(name));
        memcpy(name, block, 100);
        typeflag = block[156];
        size = tar_octal(block + 124, 12);
        offset += TAR_BLOCK;

        if (!is_safe_tar_name(name)) {
            offset += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
            continue;
        }

        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s", dest_root, name);

        if (typeflag == '5' || (size == 0 && name[strlen(name) - 1] == '/')) {
            if (ensure_dir(out_path) != 0) {
                set_err(err_buf, err_buf_size, "failed to create tar directory");
                return -1;
            }
        } else if (typeflag == '0' || typeflag == '\0') {
            if (ensure_parent_dirs(out_path) != 0) {
                set_err(err_buf, err_buf_size, "failed to create parent dirs");
                return -1;
            }
            if (offset + size > tar_len) {
                set_err(err_buf, err_buf_size, "truncated tar member");
                return -1;
            }
            FILE *fp = fopen(out_path, "wb");
            if (fp == NULL) {
                set_err(err_buf, err_buf_size, "failed to write tar member");
                return -1;
            }
            if (fwrite(tar + offset, 1, size, fp) != size) {
                fclose(fp);
                set_err(err_buf, err_buf_size, "short write extracting tar");
                return -1;
            }
            fclose(fp);
            offset += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
            continue;
        }

        offset += ((size + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    }

    return 0;
}

typedef struct {
    char name[AR_NAME_MAX + 1];
    const uint8_t *data;
    size_t size;
} ArMember;

static void
trim_trailing_spaces(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static void
trim_trailing_slash(char *s)
{
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '/') {
        s[n - 1] = '\0';
    }
}

static int
is_digits(const char *s)
{
    if (s == NULL || *s == '\0') {
        return 0;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
    }
    return 1;
}

static int
lookup_gnu_string_table(const char *strtab, size_t strtab_len, unsigned long off,
                        char *out, size_t out_size)
{
    size_t i;
    size_t n = 0;

    if (strtab == NULL || off >= strtab_len || out_size == 0) {
        return -1;
    }
    i = (size_t)off;
    while (i < strtab_len && strtab[i] != '\0' && strtab[i] != '\n' &&
           n + 1 < out_size) {
        if (strtab[i] == '/' && (i + 1 >= strtab_len || strtab[i + 1] == '\n' ||
                                 strtab[i + 1] == '\0')) {
            break;
        }
        out[n++] = strtab[i++];
    }
    out[n] = '\0';
    return n > 0 ? 0 : -1;
}

static int
push_member(ArMember **members, size_t *count, size_t *cap, const char *name,
            const uint8_t *data, size_t size, char *err_buf, size_t err_buf_size)
{
    if (*count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        ArMember *nm = (ArMember *)realloc(*members, ncap * sizeof(ArMember));
        if (nm == NULL) {
            set_err(err_buf, err_buf_size, "out of memory");
            return -1;
        }
        *members = nm;
        *cap = ncap;
    }
    memset(&(*members)[*count], 0, sizeof(ArMember));
    strncpy((*members)[*count].name, name, AR_NAME_MAX);
    (*members)[*count].data = data;
    (*members)[*count].size = size;
    (*count)++;
    return 0;
}

static int
parse_ar(const uint8_t *buf, size_t len, ArMember **out_members, size_t *out_count,
         char *err_buf, size_t err_buf_size)
{
    size_t offset = 0;
    ArMember *members = NULL;
    size_t count = 0;
    size_t cap = 0;
    const char *gnu_strtab = NULL;
    size_t gnu_strtab_len = 0;

    if (len < 8 || memcmp(buf, AR_MAGIC, 8) != 0) {
        set_err(err_buf, err_buf_size, "not a UNIX ar archive");
        return -1;
    }
    offset = 8;

    while (offset + AR_HEADER_SIZE <= len) {
        const char *hdr = (const char *)(buf + offset);
        char name_field[17];
        char size_field[11];
        long header_member_size;
        const uint8_t *member_start;
        size_t name_prefix = 0;
        size_t payload_size;
        char resolved_name[AR_NAME_MAX + 1];

        memset(name_field, 0, sizeof(name_field));
        memset(size_field, 0, sizeof(size_field));
        memset(resolved_name, 0, sizeof(resolved_name));
        memcpy(name_field, hdr, 16);
        memcpy(size_field, hdr + 48, 10);
        trim_trailing_spaces(name_field);
        trim_trailing_spaces(size_field);

        header_member_size = strtol(size_field, NULL, 10);
        if (header_member_size < 0) {
            set_err(err_buf, err_buf_size, "invalid ar member size");
            free(members);
            return -1;
        }

        offset += AR_HEADER_SIZE;
        if (offset + (size_t)header_member_size > len) {
            set_err(err_buf, err_buf_size, "truncated ar member");
            free(members);
            return -1;
        }
        member_start = buf + offset;

        if (strcmp(name_field, "//") == 0) {
            gnu_strtab = (const char *)member_start;
            gnu_strtab_len = (size_t)header_member_size;
            offset += (size_t)header_member_size;
            if (offset & 1) {
                offset++;
            }
            continue;
        }

        if (strcmp(name_field, "/") == 0 || name_field[0] == '\0') {
            offset += (size_t)header_member_size;
            if (offset & 1) {
                offset++;
            }
            continue;
        }

        if (strncmp(name_field, "#1/", 3) == 0) {
            long name_len = strtol(name_field + 3, NULL, 10);
            if (name_len <= 0 || (size_t)name_len > (size_t)header_member_size ||
                (size_t)name_len > AR_NAME_MAX) {
                set_err(err_buf, err_buf_size, "invalid BSD ar extended name");
                free(members);
                return -1;
            }
            memcpy(resolved_name, member_start, (size_t)name_len);
            resolved_name[name_len] = '\0';
            resolved_name[strnlen(resolved_name, (size_t)name_len)] = '\0';
            trim_trailing_spaces(resolved_name);
            trim_trailing_slash(resolved_name);
            name_prefix = (size_t)name_len;
        } else if (name_field[0] == '/' && is_digits(name_field + 1)) {
            unsigned long stroff = strtoul(name_field + 1, NULL, 10);
            if (lookup_gnu_string_table(gnu_strtab, gnu_strtab_len, stroff,
                                        resolved_name, sizeof(resolved_name)) !=
                0) {
                snprintf(resolved_name, sizeof(resolved_name), "gnu_long_%lu",
                         stroff);
            }
            trim_trailing_slash(resolved_name);
        } else {
            strncpy(resolved_name, name_field, sizeof(resolved_name) - 1);
            trim_trailing_slash(resolved_name);
        }

        payload_size = (size_t)header_member_size - name_prefix;
        if (resolved_name[0] != '\0') {
            if (push_member(&members, &count, &cap, resolved_name,
                            member_start + name_prefix, payload_size, err_buf,
                            err_buf_size) != 0) {
                free(members);
                return -1;
            }
        }

        offset += (size_t)header_member_size;
        if (offset & 1) {
            offset++;
        }
    }

    *out_members = members;
    *out_count = count;
    return 0;
}

static const ArMember *
find_data_tar(const ArMember *members, size_t count, const char **kind)
{
    static const char *names[] = {
        "data.tar.gz",  "data.tar.xz", "data.tar.lzma", "data.tar.zst",
        "data.tar.bz2", "data.tar",
    };
    for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(members[i].name, names[n]) == 0) {
                *kind = names[n];
                return &members[i];
            }
        }
    }
    return NULL;
}

static void
format_missing_data_tar_error(char *err_buf, size_t err_buf_size,
                              const ArMember *members, size_t count)
{
    size_t used = 0;
    int n;

    if (err_buf == NULL || err_buf_size == 0) {
        return;
    }
    n = snprintf(err_buf, err_buf_size,
                 "data.tar.* not found in .deb; members:");
    if (n < 0) {
        return;
    }
    used = (size_t)n;
    for (size_t i = 0; i < count && used + 1 < err_buf_size; i++) {
        n = snprintf(err_buf + used, err_buf_size - used, " [%zu]=%s", i,
                     members[i].name);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
    if (count == 0 && used + 1 < err_buf_size) {
        snprintf(err_buf + used, err_buf_size - used, " (none)");
    }
}

int
extract_deb_to_workspace(const char *deb_path,
                         const char *workspaces_root,
                         const char *workspace_name,
                         char *err_buf,
                         size_t err_buf_size)
{
    size_t deb_size = 0;
    char *deb_buf = NULL;
    ArMember *members = NULL;
    size_t member_count = 0;
    const ArMember *data_member = NULL;
    const char *kind = NULL;
    uint8_t *tar_buf = NULL;
    size_t tar_len = 0;
    char workspace_dir[1024];
    int workspace_created = 0;
    int rc = -1;

    if (deb_path == NULL || workspaces_root == NULL || workspace_name == NULL) {
        set_err(err_buf, err_buf_size, "null argument");
        return -1;
    }

    if (ensure_dir(workspaces_root) != 0) {
        set_err(err_buf, err_buf_size, "cannot create workspaces root");
        return -1;
    }

    snprintf(workspace_dir, sizeof(workspace_dir), "%s/%s", workspaces_root,
             workspace_name);
    {
        struct stat st;
        if (stat(workspace_dir, &st) != 0) {
            if (ensure_dir(workspace_dir) != 0) {
                set_err(err_buf, err_buf_size, "cannot create workspace directory");
                return -1;
            }
            workspace_created = 1;
        }
    }

    deb_buf = read_entire_file(deb_path, &deb_size);
    if (deb_buf == NULL) {
        set_err(err_buf, err_buf_size, "failed to read .deb");
        goto fail_cleanup;
    }

    if (parse_ar((const uint8_t *)deb_buf, deb_size, &members, &member_count,
                 err_buf, err_buf_size) != 0) {
        goto fail_cleanup;
    }

    data_member = find_data_tar(members, member_count, &kind);
    if (data_member == NULL) {
        format_missing_data_tar_error(err_buf, err_buf_size, members,
                                      member_count);
        goto fail_cleanup;
    }

    if (strcmp(kind, "data.tar") == 0) {
        tar_buf = (uint8_t *)malloc(data_member->size);
        if (tar_buf == NULL) {
            set_err(err_buf, err_buf_size, "out of memory");
            goto fail_cleanup;
        }
        memcpy(tar_buf, data_member->data, data_member->size);
        tar_len = data_member->size;
    } else if (strcmp(kind, "data.tar.gz") == 0) {
        if (gzip_decompress(data_member->data, data_member->size, &tar_buf,
                            &tar_len) != 0) {
            set_decompress_err(err_buf, err_buf_size, kind, data_member->data,
                               data_member->size, "gzip");
            goto fail_cleanup;
        }
    } else if (strcmp(kind, "data.tar.xz") == 0 ||
               strcmp(kind, "data.tar.lzma") == 0) {
        if (lzma_decompress(data_member->data, data_member->size, &tar_buf,
                            &tar_len) != 0) {
            set_decompress_err(err_buf, err_buf_size, kind, data_member->data,
                               data_member->size, "lzma/xz");
            goto fail_cleanup;
        }
    } else if (strcmp(kind, "data.tar.zst") == 0) {
        if (zstd_decompress(data_member->data, data_member->size, &tar_buf,
                            &tar_len) != 0) {
            set_decompress_err(err_buf, err_buf_size, kind, data_member->data,
                               data_member->size, "zstd");
            goto fail_cleanup;
        }
    } else if (strcmp(kind, "data.tar.bz2") == 0) {
        if (bz2_decompress(data_member->data, data_member->size, &tar_buf,
                           &tar_len) != 0) {
            set_decompress_err(err_buf, err_buf_size, kind, data_member->data,
                               data_member->size, "bz2");
            goto fail_cleanup;
        }
    } else {
        set_err(err_buf, err_buf_size, "unsupported data.tar compression");
        goto fail_cleanup;
    }

    if (extract_tar(tar_buf, tar_len, workspace_dir, err_buf, err_buf_size) != 0) {
        goto fail_cleanup;
    }

    rc = 0;
    goto done;

fail_cleanup:
    if (workspace_created) {
        remove_tree(workspace_dir);
    }

done:
    free(tar_buf);
    free(members);
    free(deb_buf);
    return rc;
}

int
import_dylib_to_workspace(const char *dylib_path,
                          const char *workspace_dir,
                          char *err_buf,
                          size_t err_buf_size)
{
    const char *base;
    char dst[1024];

    if (dylib_path == NULL || workspace_dir == NULL) {
        set_err(err_buf, err_buf_size, "null argument");
        return -1;
    }
    if (ensure_dir(workspace_dir) != 0) {
        set_err(err_buf, err_buf_size, "workspace dir missing");
        return -1;
    }

    base = strrchr(dylib_path, '/');
    base = base ? base + 1 : dylib_path;
    snprintf(dst, sizeof(dst), "%s/%s", workspace_dir, base);

    if (copy_file(dylib_path, dst) != 0) {
        set_err(err_buf, err_buf_size, "failed to copy dylib");
        return -1;
    }
    return 0;
}
