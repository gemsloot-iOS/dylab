#ifndef DYLAB_DEB_PACKAGER_H
#define DYLAB_DEB_PACKAGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DYLAB_DEB_ROOTFUL = 0,  /* /Library/MobileSubstrate/..., iphoneos-arm */
    DYLAB_DEB_ROOTLESS = 1  /* /var/jb/Library/MobileSubstrate/..., iphoneos-arm64 */
} DylabDebStyle;

/**
 * Package a signed dylib into a Cydia/Sileo .deb (ar of debian-binary,
 * control.tar.gz, data.tar.gz). Writes a MobileSubstrate filter plist.
 *
 * @param dylib_path      Signed .dylib
 * @param out_deb_path    Output .deb
 * @param package_id      e.g. com.dylab.tweak
 * @param package_name    Display name
 * @param version         e.g. 1.0.0
 * @param filter_bundle   App bundle id filter (empty = com.apple.UIKit)
 * @param style           Rootful vs Rootless
 * @param err_buf         Error text
 * @param err_cap         Capacity
 * @return 0 on success
 */
int dylab_package_deb(const char *dylib_path,
                      const char *out_deb_path,
                      const char *package_id,
                      const char *package_name,
                      const char *version,
                      const char *filter_bundle,
                      DylabDebStyle style,
                      char *err_buf,
                      size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_DEB_PACKAGER_H */
