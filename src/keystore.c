/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage Memory.
 *
 * Ghoti.io Defiant Storage Memory is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage Memory is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * A key store (keystore.h) for one process: subject keys and the erasure
 * ledger in memory, sealing with the cipher the caller supplies. For tests and
 * development: nothing here survives the process, which for a key store is the
 * one property a deployment must not have.
 */

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

typedef struct KSubject {
  char * tenant;
  size_t tenant_len;
  char * subject;
  size_t subject_len;
  uint8_t * key; /* NULL once erased */
  int erased;
  struct KSubject * next;
} KSubject;

typedef struct KLedger {
  char * tenant;
  size_t tenant_len;
  char * subject;
  size_t subject_len;
  int64_t when;
  struct KLedger * next;
} KLedger;

typedef struct KStore {
  const GDEFSMEMORY_Allocator * alloc;
  GDEFSTOR_KeyCipher cipher;
  KSubject * subjects;
  KLedger * ledger; /* oldest first */
  KLedger * tail;
} KStore;

static int same(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

static char * copy(const GDEFSMEMORY_Allocator * a, const char * p, size_t n) {
  char * c = (char *)gcu_allocator_malloc(a, n + 1);
  if (c == NULL) {
    return NULL;
  }
  if (n > 0) {
    memcpy(c, p, n);
  }
  c[n] = '\0';
  return c;
}

/* Overwrite key bytes before they go back to the allocator. */
static void wipe(uint8_t * p, size_t n) {
  volatile uint8_t * v = p;
  size_t i;
  for (i = 0; i < n; i++) {
    v[i] = 0;
  }
}

static KSubject * find(KStore * k, const char * t, size_t tl, const char * s,
    size_t sl) {
  KSubject * x;
  for (x = k->subjects; x != NULL; x = x->next) {
    if (same(x->tenant, x->tenant_len, t, tl) &&
        same(x->subject, x->subject_len, s, sl)) {
      return x;
    }
  }
  return NULL;
}

static void k_destroy(void * ctx) {
  KStore * k = (KStore *)ctx;
  if (k == NULL) {
    return;
  }
  while (k->subjects != NULL) {
    KSubject * x = k->subjects;
    k->subjects = x->next;
    if (x->key != NULL) {
      wipe(x->key, k->cipher.key_bytes);
    }
    gcu_allocator_free(k->alloc, x->key);
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->subject);
    gcu_allocator_free(k->alloc, x);
  }
  while (k->ledger != NULL) {
    KLedger * l = k->ledger;
    k->ledger = l->next;
    gcu_allocator_free(k->alloc, l->tenant);
    gcu_allocator_free(k->alloc, l->subject);
    gcu_allocator_free(k->alloc, l);
  }
  gcu_allocator_free(k->alloc, k);
}

static GDEFSTOR_Result k_create(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl) {
  KStore * k = (KStore *)ctx;
  KSubject * x;
  GDEFSTOR_Result rc;
  if (find(k, t, tl, s, sl) != NULL) {
    return GDEFSTOR_ERR_INVALID; /* has a key, or was erased */
  }
  x = (KSubject *)gcu_allocator_calloc(k->alloc, 1, sizeof *x);
  if (x == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant = copy(k->alloc, t, tl);
  x->subject = copy(k->alloc, s, sl);
  x->key = (uint8_t *)gcu_allocator_malloc(k->alloc, k->cipher.key_bytes);
  if (x->tenant == NULL || x->subject == NULL || x->key == NULL) {
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->subject);
    gcu_allocator_free(k->alloc, x->key);
    gcu_allocator_free(k->alloc, x);
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant_len = tl;
  x->subject_len = sl;
  rc = k->cipher.random(k->cipher.ctx, x->key, k->cipher.key_bytes);
  if (rc != GDEFSTOR_OK) {
    wipe(x->key, k->cipher.key_bytes);
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->subject);
    gcu_allocator_free(k->alloc, x->key);
    gcu_allocator_free(k->alloc, x);
    return rc;
  }
  x->next = k->subjects;
  k->subjects = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result k_seal_or_open(KStore * k, int seal, const char * t,
    size_t tl, const char * s, size_t sl, const uint8_t * in, size_t n,
    int * out_erased, uint8_t ** out, size_t * out_len) {
  KSubject * x = find(k, t, tl, s, sl);
  *out_erased = 0;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (x->erased) {
    *out_erased = 1;
    return GDEFSTOR_OK;
  }
  return (seal ? k->cipher.seal : k->cipher.open)(k->cipher.ctx, k->alloc,
      x->key, in, n, out, out_len);
}

static GDEFSTOR_Result k_wrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * out_erased,
    uint8_t ** out, size_t * out_len) {
  return k_seal_or_open((KStore *)ctx, 1, t, tl, s, sl, in, n, out_erased,
      out, out_len);
}

static GDEFSTOR_Result k_unwrap(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * in, size_t n, int * out_erased,
    uint8_t ** out, size_t * out_len) {
  return k_seal_or_open((KStore *)ctx, 0, t, tl, s, sl, in, n, out_erased,
      out, out_len);
}

static GDEFSTOR_Result k_erase(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, int64_t when) {
  KStore * k = (KStore *)ctx;
  KSubject * x = find(k, t, tl, s, sl);
  KLedger * l;
  if (x != NULL && x->erased) {
    return GDEFSTOR_OK; /* once is enough */
  }
  /* Allocate everything first, so the step cannot stop half way. */
  l = (KLedger *)gcu_allocator_calloc(k->alloc, 1, sizeof *l);
  if (l == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  l->tenant = copy(k->alloc, t, tl);
  l->subject = copy(k->alloc, s, sl);
  if (l->tenant == NULL || l->subject == NULL) {
    gcu_allocator_free(k->alloc, l->tenant);
    gcu_allocator_free(k->alloc, l->subject);
    gcu_allocator_free(k->alloc, l);
    return GDEFSTOR_ERR_OOM;
  }
  if (x == NULL) {
    /* Never had a key here: record it, so a key cannot be made for it later. */
    x = (KSubject *)gcu_allocator_calloc(k->alloc, 1, sizeof *x);
    if (x != NULL) {
      x->tenant = copy(k->alloc, t, tl);
      x->subject = copy(k->alloc, s, sl);
    }
    if (x == NULL || x->tenant == NULL || x->subject == NULL) {
      if (x != NULL) {
        gcu_allocator_free(k->alloc, x->tenant);
        gcu_allocator_free(k->alloc, x->subject);
        gcu_allocator_free(k->alloc, x);
      }
      gcu_allocator_free(k->alloc, l->tenant);
      gcu_allocator_free(k->alloc, l->subject);
      gcu_allocator_free(k->alloc, l);
      return GDEFSTOR_ERR_OOM;
    }
    x->tenant_len = tl;
    x->subject_len = sl;
    x->next = k->subjects;
    k->subjects = x;
  }
  if (x->key != NULL) {
    wipe(x->key, k->cipher.key_bytes);
    gcu_allocator_free(k->alloc, x->key);
    x->key = NULL;
  }
  x->erased = 1;
  l->tenant_len = tl;
  l->subject_len = sl;
  l->when = when;
  if (k->tail == NULL) {
    k->ledger = l;
  } else {
    k->tail->next = l;
  }
  k->tail = l;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result k_ledger(void * ctx, const char * t, size_t tl,
    GDEFSTOR_Result (*visit)(void * vctx, const char * subject,
        size_t subject_len, int64_t when_ms),
    void * vctx) {
  KStore * k = (KStore *)ctx;
  KLedger * l;
  for (l = k->ledger; l != NULL; l = l->next) {
    if (same(l->tenant, l->tenant_len, t, tl)) {
      GDEFSTOR_Result rc = visit(vctx, l->subject, l->subject_len, l->when);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

GDEFSMEMORY_Result gdefsmemory_keystore_create(
    const GDEFSMEMORY_Allocator * allocator, const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out_keystore) {
  const GDEFSMEMORY_Allocator * use =
      allocator != NULL ? allocator : gdefsmemory_allocator_default();
  GDEFSTOR_KeyStoreBackend b;
  KStore * k;
  GDEFSTOR_Result rc;
  if (out_keystore == NULL || cipher == NULL ||
      cipher->size != sizeof(GDEFSTOR_KeyCipher) || cipher->random == NULL ||
      cipher->seal == NULL || cipher->open == NULL || cipher->key_bytes == 0 ||
      cipher->key_bytes > 1024) {
    return GDEFSMEMORY_ERR_INVALID;
  }
  k = (KStore *)gcu_allocator_calloc(use, 1, sizeof *k);
  if (k == NULL) {
    return GDEFSMEMORY_ERR_OOM;
  }
  k->alloc = use;
  k->cipher = *cipher;
  memset(&b, 0, sizeof b);
  b.size = sizeof b;
  b.ctx = k;
  b.destroy = k_destroy;
  b.create = k_create;
  b.wrap = k_wrap;
  b.unwrap = k_unwrap;
  b.erase = k_erase;
  b.ledger = k_ledger;
  rc = gdefstor_keystore_create(&b, use, out_keystore);
  if (rc != GDEFSTOR_OK) {
    k_destroy(k);
    return rc == GDEFSTOR_ERR_OOM ? GDEFSMEMORY_ERR_OOM
                                  : GDEFSMEMORY_ERR_INVALID;
  }
  return GDEFSMEMORY_OK;
}
