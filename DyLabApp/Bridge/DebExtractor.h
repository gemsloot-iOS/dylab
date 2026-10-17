#ifndef DYLAB_DEB_EXTRACTOR_H
#define DYLAB_DEB_EXTRACTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extract a .deb (ar + data.tar.*) into workspaces_root/<workspace_name>/.
 * Creates the workspace directory as needed.
 *
 * @return 0 on success, non-zero on failure (message in err_buf when provided)
 */
int extract_deb_to_workspace(const char *deb_path,
                             const char *workspaces_root,
                             const char *workspace_name,
                             char *err_buf,
                             size_t err_buf_size);

/**
 * Copy a standalone .dylib into an existing workspace directory.
 *
 * @return 0 on success, non-zero on failure
 */
int import_dylib_to_workspace(const char *dylib_path,
                              const char *workspace_dir,
                              char *err_buf,
                              size_t err_buf_size);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_DEB_EXTRACTOR_H */
