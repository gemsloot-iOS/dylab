#include "DebExtractor.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

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
            size_t used = cap;
            cap *= 2;
            uint8_t *grown = (uint8_t *)realloc(dst, cap);
            if (grown == NULL) {
                free(dst);
                inflateEnd(&strm);
                return -1;
            }
            dst = grown;
            strm.next_out = dst + used;
            strm.avail_out = (uInt)(cap - used);
        }
    }

    *out_len = strm.total_out;
    *out_buf = dst;
    inflateEnd(&strm);
    return 0;
}

#if defined(__APPLE__)
static int
lzma_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
                 size_t *out_len)
{
    size_t cap = src_len * 8 + 64 * 1024;
    for (int attempt = 0; attempt < 4; attempt++) {
        uint8_t *dst = (uint8_t *)malloc(cap);
        if (dst == NULL) {
            return -1;
        }
        size_t decoded = compression_decode_buffer(dst, cap, src, src_len, NULL,
                                                   COMPRESSION_LZMA);
        if (decoded > 0 && decoded < cap) {
            *out_buf = dst;
            *out_len = decoded;
            return 0;
        }
        free(dst);
        cap *= 2;
    }
    return -1;
}
#else
static int
lzma_decompress(const uint8_t *src, size_t src_len, uint8_t **out_buf,
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

        /* GNU string table. */
        if (strcmp(name_field, "//") == 0) {
            gnu_strtab = (const char *)member_start;
            gnu_strtab_len = (size_t)header_member_size;
            offset += (size_t)header_member_size;
            if (offset & 1) {
                offset++;
            }
            continue;
        }

        /* Skip symbol table / empty. */
        if (strcmp(name_field, "/") == 0 || name_field[0] == '\0') {
            offset += (size_t)header_member_size;
            if (offset & 1) {
                offset++;
            }
            continue;
        }

        if (strncmp(name_field, "#1/", 3) == 0) {
            /* BSD extended name: real name is first name_len bytes of body. */
            long name_len = strtol(name_field + 3, NULL, 10);
            if (name_len <= 0 || (size_t)name_len > (size_t)header_member_size ||
                (size_t)name_len > AR_NAME_MAX) {
                set_err(err_buf, err_buf_size, "invalid BSD ar extended name");
                free(members);
                return -1;
            }
            memcpy(resolved_name, member_start, (size_t)name_len);
            resolved_name[name_len] = '\0';
            /* Stop at first embedded NUL if present. */
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
        "data.tar.gz", "data.tar.xz", "data.tar.lzma", "data.tar",
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
    if (ensure_dir(workspace_dir) != 0) {
        set_err(err_buf, err_buf_size, "cannot create workspace directory");
        return -1;
    }

    deb_buf = read_entire_file(deb_path, &deb_size);
    if (deb_buf == NULL) {
        set_err(err_buf, err_buf_size, "failed to read .deb");
        return -1;
    }

    if (parse_ar((const uint8_t *)deb_buf, deb_size, &members, &member_count,
                 err_buf, err_buf_size) != 0) {
        free(deb_buf);
        return -1;
    }

    data_member = find_data_tar(members, member_count, &kind);
    if (data_member == NULL) {
        format_missing_data_tar_error(err_buf, err_buf_size, members,
                                      member_count);
        goto done;
    }

    if (strcmp(kind, "data.tar") == 0) {
        tar_buf = (uint8_t *)malloc(data_member->size);
        if (tar_buf == NULL) {
            set_err(err_buf, err_buf_size, "out of memory");
            goto done;
        }
        memcpy(tar_buf, data_member->data, data_member->size);
        tar_len = data_member->size;
    } else if (strcmp(kind, "data.tar.gz") == 0) {
        if (gzip_decompress(data_member->data, data_member->size, &tar_buf,
                            &tar_len) != 0) {
            set_err(err_buf, err_buf_size, "gzip decompress failed");
            goto done;
        }
    } else if (strcmp(kind, "data.tar.xz") == 0 ||
               strcmp(kind, "data.tar.lzma") == 0) {
        if (lzma_decompress(data_member->data, data_member->size, &tar_buf,
                            &tar_len) != 0) {
            set_err(err_buf, err_buf_size,
                    "lzma/xz decompress unavailable or failed");
            goto done;
        }
    } else {
        set_err(err_buf, err_buf_size, "unsupported data.tar compression");
        goto done;
    }

    if (extract_tar(tar_buf, tar_len, workspace_dir, err_buf, err_buf_size) != 0) {
        goto done;
    }

    rc = 0;

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
