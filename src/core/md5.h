/* MD5, because a DS-10 song file is prefixed with one.
 *
 * The original calls 0x02015404 to build the 16-byte header and 0x0205D038 to
 * check it. That routine is plain RFC 1321 MD5: the digest of every song file's
 * payload matches a reference implementation exactly, all 17 files in the ROM.
 * Nothing here is reverse engineered - it is the standard algorithm, included so
 * that a song we write out is a file the original would accept.
 */
#ifndef DS10_MD5_H
#define DS10_MD5_H

#include <stddef.h>
#include <stdint.h>

void ds10_md5(const void *data, size_t len, uint8_t out[16]);

#endif /* DS10_MD5_H */
