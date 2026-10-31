#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A

// Like mbedtls, writes a terminating NUL, so dst needs one byte more than the
// text. When it is too small, *olen says how many bytes are needed.
static inline int mbedtls_base64_encode(unsigned char* dst, size_t dlen, size_t* olen,
                                        const unsigned char* src, size_t slen) {
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t needed = (slen + 2) / 3 * 4;
  *olen = needed;
  if (slen == 0) {
    *olen = 0;
    if (dlen > 0) dst[0] = '\0';
    return dlen > 0 ? 0 : MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  if (dlen < needed + 1) {
    *olen = needed + 1;
    return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  }
  size_t n = 0;
  for (size_t i = 0; i < slen; i += 3) {
    const unsigned v = (unsigned)src[i] << 16 | (i + 1 < slen ? (unsigned)src[i + 1] << 8 : 0u) |
                       (i + 2 < slen ? (unsigned)src[i + 2] : 0u);
    dst[n++] = (unsigned char)table[v >> 18 & 63];
    dst[n++] = (unsigned char)table[v >> 12 & 63];
    dst[n++] = (unsigned char)(i + 1 < slen ? table[v >> 6 & 63] : '=');
    dst[n++] = (unsigned char)(i + 2 < slen ? table[v & 63] : '=');
  }
  dst[n] = '\0';
  return 0;
}

#ifdef __cplusplus
}
#endif
