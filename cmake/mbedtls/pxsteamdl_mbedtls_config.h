/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Appended to the default mbedTLS configuration (MBEDTLS_USER_CONFIG_FILE).
 * TLS 1.3 handshakes go through PSA's global key store, which download threads use concurrently. */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
/* mbedTLS 3.6 only ships a pthread backend; src/http.cpp installs SRWLOCK-based mutexes. */
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif
