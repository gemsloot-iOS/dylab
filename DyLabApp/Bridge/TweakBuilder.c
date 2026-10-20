#include "TweakBuilder.h"

#include "DebPackager.h"
#include "MachODylibLinker.h"
#include "MachOSigner.h"
#include "ObjCLowering.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if __has_include(<libtcc.h>)
#include <libtcc.h>
#define DYLAB_HAVE_TCC 1
#elif __has_include("libtcc.h")
#include "libtcc.h"
#define DYLAB_HAVE_TCC 1
#else
#define DYLAB_HAVE_TCC 0
#endif

static void
log_line(DylabBuildLogFn fn, void *ctx, const char *line)
{
    if (fn) fn(ctx, line);
}

static void
set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

/* Directory containing path (mutates a copy into out). */
static void
parent_dir(const char *path, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!path || !path[0]) {
        snprintf(out, cap, ".");
        return;
    }
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    if (slash == out) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
}

#if DYLAB_HAVE_TCC
typedef struct {
    DylabBuildLogFn fn;
    void *ctx;
    char *err;
    size_t err_cap;
} TccLog;

static void
tcc_err(void *opaque, const char *msg)
{
    TccLog *L = (TccLog *)opaque;
    if (L && L->fn) {
        char line[512];
        snprintf(line, sizeof(line), "  tcc: %s", msg ? msg : "");
        L->fn(L->ctx, line);
    }
    if (L && L->err && L->err_cap && msg) {
        snprintf(L->err, L->err_cap, "%s", msg);
    }
}
#endif

int
dylab_build_tweak(const char *tweak_m_path,
                  const char *sdk_include_dir,
                  const char *out_dir,
                  const char *dylib_basename,
                  const char *package_id,
                  const char *package_name,
                  const char *version,
                  const char *filter_bundle,
                  DylabDebStyle style,
                  DylabBuildLogFn log_fn,
                  void *log_ctx,
                  char *out_dylib_path,
                  size_t out_dylib_cap,
                  char *out_deb_path,
                  size_t out_deb_cap,
                  char *err_buf,
                  size_t err_cap)
{
    if (!tweak_m_path || !out_dir || !dylib_basename || !sdk_include_dir) {
        set_err(err_buf, err_cap, "invalid arguments");
        return -1;
    }

    mkdir(out_dir, 0755);

    char lowered_path[1024], obj_path[1024], dylib_path[1024], deb_path[1024];
    snprintf(lowered_path, sizeof(lowered_path), "%s/Tweak.lowered.c", out_dir);
    snprintf(obj_path, sizeof(obj_path), "%s/Tweak.o", out_dir);
    snprintf(dylib_path, sizeof(dylib_path), "%s/%s", out_dir, dylib_basename);

    char deb_base[256];
    snprintf(deb_base, sizeof(deb_base), "%s", dylib_basename);
    char *dot = strrchr(deb_base, '.');
    if (dot) *dot = '\0';
    snprintf(deb_path, sizeof(deb_path), "%s/%s.deb", out_dir, deb_base);

    char err[1024];
    err[0] = '\0';

    /* [1/4] Lower */
    log_line(log_fn, log_ctx, "[1/4] ObjC → C lowering…");
    if (dylab_objc_lower_file(tweak_m_path, lowered_path, err, sizeof(err)) != 0) {
        set_err(err_buf, err_cap, err[0] ? err : "lowering failed");
        log_line(log_fn, log_ctx, "  FAIL lowering");
        return -2;
    }
    log_line(log_fn, log_ctx, "  wrote Tweak.lowered.c");

#if !DYLAB_HAVE_TCC
    set_err(err_buf, err_cap, "libtcc not available in this build");
    log_line(log_fn, log_ctx, "  FAIL: libtcc missing");
    return -3;
#else
    /* [2/4] TCC */
    log_line(log_fn, log_ctx, "[2/4] TinyCC compile (ELF .o)…");
    {
        TCCState *s = tcc_new();
        if (!s) {
            set_err(err_buf, err_cap, "tcc_new failed");
            return -3;
        }
        TccLog L = {log_fn, log_ctx, err, sizeof(err)};
        tcc_set_error_func(s, &L, tcc_err);
        tcc_set_output_type(s, TCC_OUTPUT_OBJ);
        /* Prefer the tweak source dir so any remaining #include "…" resolve
         * next to Tweak.m (Generate Source writes TweakClasses.h there). */
        {
            char src_dir[1024];
            parent_dir(tweak_m_path, src_dir, sizeof(src_dir));
            tcc_add_include_path(s, src_dir);
        }
        tcc_add_include_path(s, sdk_include_dir);
        tcc_define_symbol(s, "__APPLE__", NULL);
        tcc_define_symbol(s, "__arm64__", NULL);
        tcc_define_symbol(s, "__aarch64__", NULL);
        tcc_define_symbol(s, "TARGET_OS_IPHONE", "1");
        if (tcc_add_file(s, lowered_path) < 0) {
            tcc_delete(s);
            set_err(err_buf, err_cap, err[0] ? err : "tcc compile failed");
            log_line(log_fn, log_ctx, "  FAIL tcc");
            return -3;
        }
        if (tcc_output_file(s, obj_path) < 0) {
            tcc_delete(s);
            set_err(err_buf, err_cap, "tcc output failed");
            return -3;
        }
        tcc_delete(s);
    }
    log_line(log_fn, log_ctx, "  wrote Tweak.o");

    /* [3/4] Link + sign */
    log_line(log_fn, log_ctx, "[3/4] ELF → Mach-O dylib + ad-hoc sign…");
    {
        const char *id_prefix =
            (style == DYLAB_DEB_ROOTLESS)
                ? "/var/jb/Library/MobileSubstrate/DynamicLibraries/"
                : "/Library/MobileSubstrate/DynamicLibraries/";
        char install_name[1024];
        snprintf(install_name, sizeof(install_name), "%s%s", id_prefix, dylib_basename);
        if (dylab_link_elf_to_dylib(obj_path, dylib_path, install_name, err, sizeof(err)) != 0) {
            set_err(err_buf, err_cap, err[0] ? err : "link failed");
            log_line(log_fn, log_ctx, "  FAIL link");
            return -4;
        }
        if (dylab_ldid_sign(dylib_path, err, sizeof(err)) != 0) {
            set_err(err_buf, err_cap, err[0] ? err : "sign failed");
            log_line(log_fn, log_ctx, "  FAIL sign");
            return -4;
        }
    }
    log_line(log_fn, log_ctx, "  wrote + signed dylib");

    /* [4/4] Deb */
    log_line(log_fn, log_ctx, "[4/4] Package .deb…");
    if (dylab_package_deb(dylib_path, deb_path, package_id ? package_id : "com.dylab.tweak",
                          package_name ? package_name : "DyLabTweak",
                          version ? version : "1.0.0", filter_bundle, style, err,
                          sizeof(err)) != 0) {
        set_err(err_buf, err_cap, err[0] ? err : "deb package failed");
        log_line(log_fn, log_ctx, "  FAIL deb");
        return -5;
    }
    log_line(log_fn, log_ctx, "  wrote .deb");
    log_line(log_fn, log_ctx, "BUILD OK");

    if (out_dylib_path && out_dylib_cap)
        snprintf(out_dylib_path, out_dylib_cap, "%s", dylib_path);
    if (out_deb_path && out_deb_cap)
        snprintf(out_deb_path, out_deb_cap, "%s", deb_path);
    if (err_buf && err_cap) err_buf[0] = '\0';
    return 0;
#endif
}
