#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
struct mbedtls_sha256_context {
  CC_SHA256_CTX state;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return CC_SHA256_Init(&ctx->state) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* data, size_t size) {
  return CC_SHA256_Update(&ctx->state, data, static_cast<CC_LONG>(size)) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32]) {
  return CC_SHA256_Final(output, &ctx->state) == 1 ? 0 : -1;
}
#else
#include <openssl/sha.h>
struct mbedtls_sha256_context {
  SHA256_CTX state;
};
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx, int) { return SHA256_Init(&ctx->state) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx, const unsigned char* data, size_t size) {
  return SHA256_Update(&ctx->state, data, size) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx, unsigned char output[32]) {
  return SHA256_Final(output, &ctx->state) == 1 ? 0 : -1;
}
#endif
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
