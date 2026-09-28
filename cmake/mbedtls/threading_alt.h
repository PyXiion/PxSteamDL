/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

/* Holds a Windows SRWLOCK (a single pointer, zero-initialized) without pulling windows.h into mbedTLS. */
typedef struct mbedtls_threading_mutex_t {
    void *lock;
} mbedtls_threading_mutex_t;
