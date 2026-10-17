#ifndef DYLAB_MACHO_SIGNER_H
#define DYLAB_MACHO_SIGNER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ad-hoc codesign a Mach-O (adds/updates LC_CODE_SIGNATURE).
 * SuperBlob + CodeDirectory v0x20400, flags adhoc|linkerSigned, pageSize=12 (4K).
 * Hashes pages with SHA-256 (CommonCrypto on Apple; portable SHA-256 elsewhere).
 *
 * @return 0 on success
 */
int dylab_ldid_sign(const char *path, char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif

#endif /* DYLAB_MACHO_SIGNER_H */
