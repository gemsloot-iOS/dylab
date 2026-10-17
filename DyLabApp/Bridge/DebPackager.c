#include "DebPackager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

/* Minimal ustar tar + gzip + ar (.deb) writer */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

static void
buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static int
buf_reserve(Buf *b, size_t need)
{
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) ncap *= 2;
    unsigned char *p = (unsigned char *)realloc(b->data, ncap);
    if (!p) return -1;
    b->data = p;
    b->cap = ncap;
    return 0;
}

static int
buf_append(Buf *b, const void *p, size_t n)
{
    if (buf_reserve(b, b->len + n) != 0) return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static void
buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

static void
set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

static void
tar_octal(char *dst, size_t dstlen, unsigned long long v)
{
    /* NUL-terminated octal, space-padded classic */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%0*llo", (int)dstlen - 1, v);
    memset(dst, '0', dstlen);
    size_t n = strlen(tmp);
    if (n >= dstlen) n = dstlen - 1;
    memcpy(dst + (dstlen - 1 - n), tmp, n);
    dst[dstlen - 1] = '\0';
}

static int
tar_add_file(Buf *tar, const char *path, const void *data, size_t len, int mode)
{
    char hdr[512];
    memset(hdr, 0, sizeof(hdr));
    snprintf(hdr + 0, 100, "%s", path);
    tar_octal(hdr + 100, 8, (unsigned)mode);
    tar_octal(hdr + 108, 8, 0); /* uid */
    tar_octal(hdr + 116, 8, 0); /* gid */
    tar_octal(hdr + 124, 12, (unsigned long long)len);
    tar_octal(hdr + 136, 12, (unsigned long long)time(NULL));
    memset(hdr + 148, ' ', 8);
    memcpy(hdr + 257, "ustar", 5);
    hdr[262] = '0';
    hdr[263] = '0';
    /* checksum */
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += (unsigned char)hdr[i];
    snprintf(hdr + 148, 8, "%06o", sum);

    if (buf_append(tar, hdr, 512) != 0) return -1;
    if (len && buf_append(tar, data, len) != 0) return -1;
    size_t pad = (512 - (len % 512)) % 512;
    if (pad) {
        char z[512];
        memset(z, 0, pad);
        if (buf_append(tar, z, pad) != 0) return -1;
    }
    return 0;
}

static int
gzip_buf(const unsigned char *in, size_t inlen, Buf *out)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + 15, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        return -1;
    }
    size_t bound = deflateBound(&zs, inlen) + 64;
    if (buf_reserve(out, bound) != 0) {
        deflateEnd(&zs);
        return -1;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)inlen;
    zs.next_out = out->data;
    zs.avail_out = (uInt)bound;
    int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        return -1;
    }
    out->len = zs.total_out;
    deflateEnd(&zs);
    return 0;
}

static int
ar_append(FILE *f, const char *name, const void *data, size_t len)
{
    char hdr[60];
    memset(hdr, ' ', 60);
    /* name / length fields */
    size_t nl = strlen(name);
    if (nl > 16) nl = 16;
    memcpy(hdr, name, nl);
    /* mtime */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%ld", (long)time(NULL));
    memcpy(hdr + 16, tmp, strlen(tmp));
    memcpy(hdr + 28, "0", 1);  /* uid */
    memcpy(hdr + 34, "0", 1);  /* gid */
    memcpy(hdr + 40, "100644", 6);
    snprintf(tmp, sizeof(tmp), "%zu", len);
    memcpy(hdr + 48, tmp, strlen(tmp));
    hdr[58] = '`';
    hdr[59] = '\n';
    if (fwrite(hdr, 1, 60, f) != 60) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;
    if (len & 1) {
        char pad = '\n';
        if (fwrite(&pad, 1, 1, f) != 1) return -1;
    }
    return 0;
}

static int
read_file(const char *path, Buf *b)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return -1;
    }
    if (buf_reserve(b, (size_t)sz) != 0) {
        fclose(f);
        return -1;
    }
    if (sz && fread(b->data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return -1;
    }
    b->len = (size_t)sz;
    fclose(f);
    return 0;
}

int
dylab_package_deb(const char *dylib_path,
                  const char *out_deb_path,
                  const char *package_id,
                  const char *package_name,
                  const char *version,
                  const char *filter_bundle,
                  DylabDebStyle style,
                  char *err_buf,
                  size_t err_cap)
{
    if (!dylib_path || !out_deb_path || !package_id || !package_name || !version) {
        set_err(err_buf, err_cap, "invalid arguments");
        return -1;
    }
    const char *bundle = (filter_bundle && filter_bundle[0]) ? filter_bundle : "com.apple.UIKit";
    const char *arch = (style == DYLAB_DEB_ROOTLESS) ? "iphoneos-arm64" : "iphoneos-arm";
    const char *prefix =
        (style == DYLAB_DEB_ROOTLESS) ? "/var/jb/Library/MobileSubstrate/DynamicLibraries"
                                      : "/Library/MobileSubstrate/DynamicLibraries";

    /* Derive dylib basename */
    const char *base = strrchr(dylib_path, '/');
    base = base ? base + 1 : dylib_path;
    char dylib_dest[512];
    snprintf(dylib_dest, sizeof(dylib_dest), "%s/%s", prefix + 1, base); /* tar paths relative */

    char plist_name[512];
    snprintf(plist_name, sizeof(plist_name), "%s", base);
    char *dot = strrchr(plist_name, '.');
    if (dot) strcpy(dot, ".plist");
    else strcat(plist_name, ".plist");
    char plist_dest[512];
    snprintf(plist_dest, sizeof(plist_dest), "%s/%s", prefix + 1, plist_name);

    char control[1024];
    snprintf(control, sizeof(control),
             "Package: %s\n"
             "Name: %s\n"
             "Version: %s\n"
             "Architecture: %s\n"
             "Description: Built with DyLab Path C\n"
             "Maintainer: DyLab\n"
             "Author: DyLab\n"
             "Section: Tweaks\n"
             "Depends: mobilesubstrate\n",
             package_id, package_name, version, arch);

    char plist[1024];
    snprintf(plist, sizeof(plist),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n"
             "<dict>\n"
             "  <key>Filter</key>\n"
             "  <dict>\n"
             "    <key>Bundles</key>\n"
             "    <array>\n"
             "      <string>%s</string>\n"
             "    </array>\n"
             "  </dict>\n"
             "</dict>\n"
             "</plist>\n",
             bundle);

    Buf dylib;
    buf_init(&dylib);
    if (read_file(dylib_path, &dylib) != 0) {
        set_err(err_buf, err_cap, "cannot read dylib");
        return -1;
    }

    Buf control_tar, data_tar;
    buf_init(&control_tar);
    buf_init(&data_tar);
    if (tar_add_file(&control_tar, "control", control, strlen(control), 0644) != 0) {
        set_err(err_buf, err_cap, "control tar failed");
        buf_free(&dylib);
        return -1;
    }
    /* two zero blocks end tar */
    char z[1024];
    memset(z, 0, sizeof(z));
    buf_append(&control_tar, z, 1024);

    if (tar_add_file(&data_tar, dylib_dest, dylib.data, dylib.len, 0755) != 0 ||
        tar_add_file(&data_tar, plist_dest, plist, strlen(plist), 0644) != 0) {
        set_err(err_buf, err_cap, "data tar failed");
        buf_free(&dylib);
        buf_free(&control_tar);
        return -1;
    }
    buf_append(&data_tar, z, 1024);

    Buf control_gz, data_gz;
    buf_init(&control_gz);
    buf_init(&data_gz);
    if (gzip_buf(control_tar.data, control_tar.len, &control_gz) != 0 ||
        gzip_buf(data_tar.data, data_tar.len, &data_gz) != 0) {
        set_err(err_buf, err_cap, "gzip failed");
        buf_free(&dylib);
        buf_free(&control_tar);
        buf_free(&data_tar);
        return -1;
    }

    FILE *out = fopen(out_deb_path, "wb");
    if (!out) {
        set_err(err_buf, err_cap, "cannot write deb");
        buf_free(&dylib);
        buf_free(&control_tar);
        buf_free(&data_tar);
        buf_free(&control_gz);
        buf_free(&data_gz);
        return -1;
    }
    if (fwrite("!<arch>\n", 1, 8, out) != 8) {
        fclose(out);
        set_err(err_buf, err_cap, "ar magic failed");
        return -1;
    }
    const char *deb_bin = "2.0\n";
    if (ar_append(out, "debian-binary", deb_bin, strlen(deb_bin)) != 0 ||
        ar_append(out, "control.tar.gz", control_gz.data, control_gz.len) != 0 ||
        ar_append(out, "data.tar.gz", data_gz.data, data_gz.len) != 0) {
        fclose(out);
        set_err(err_buf, err_cap, "ar append failed");
        return -1;
    }
    fclose(out);

    buf_free(&dylib);
    buf_free(&control_tar);
    buf_free(&data_tar);
    buf_free(&control_gz);
    buf_free(&data_gz);
    if (err_buf && err_cap) err_buf[0] = '\0';
    return 0;
}
