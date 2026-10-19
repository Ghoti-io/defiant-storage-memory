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
 * A key store (keystore.h) for one process: subject keys, sealed material,
 * tenant secrets and the erasure ledger in memory, sealing with the cipher the
 * caller supplies. For tests and development: nothing here survives the
 * process, which for a key store is the one property a deployment must not
 * have.
 */

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

typedef struct KSubject {
  char * tenant;
  size_t tenant_len;
  char * subject;
  size_t subject_len;
  uint8_t * key; /* NULL once erased, or when the subject has material */
  uint8_t * sealed; /* material a caller put; NULL otherwise */
  size_t sealed_len;
  int erased;
  struct KSubject * next;
} KSubject;

typedef struct KSecret {
  char * tenant;
  size_t tenant_len;
  char * name;
  size_t name_len;
  uint8_t * bytes;
  size_t len;
  struct KSecret * next;
} KSecret;

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
  KSecret * secrets;
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

/* Wipe and free a subject's sealed material. */
static void drop_sealed(KStore * k, KSubject * x) {
  if (x->sealed != NULL) {
    wipe(x->sealed, x->sealed_len);
  }
  gcu_allocator_free(k->alloc, x->sealed);
  x->sealed = NULL;
  x->sealed_len = 0;
}

static void k_destroy(void * ctx) {
  KStore * k = (KStore *)ctx;
  if (k == NULL) {
    return;
  }
  while (k->secrets != NULL) {
    KSecret * x = k->secrets;
    k->secrets = x->next;
    wipe(x->bytes, x->len);
    gcu_allocator_free(k->alloc, x->bytes);
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->name);
    gcu_allocator_free(k->alloc, x);
  }
  while (k->subjects != NULL) {
    KSubject * x = k->subjects;
    k->subjects = x->next;
    if (x->key != NULL) {
      wipe(x->key, k->cipher.key_bytes);
    }
    drop_sealed(k, x);
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
  if (x->key == NULL) {
    return GDEFSTOR_ERR_INVALID; /* sealed material: the store never opens it */
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
  drop_sealed(k, x);
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

/* ---- sealed material, secrets, the key set ---------------------------------- */

static uint8_t * copy_bytes(const GDEFSMEMORY_Allocator * a, const uint8_t * p,
    size_t n) {
  uint8_t * c = (uint8_t *)gcu_allocator_malloc(a, n == 0 ? 1 : n);
  if (c != NULL && n > 0) {
    memcpy(c, p, n);
  }
  return c;
}

/* A new subject row holding @p bytes as its key (@p sealed 0) or its
 * material, linked in front. */
static GDEFSTOR_Result add_subject(KStore * k, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * bytes, size_t n, int sealed) {
  KSubject * x = (KSubject *)gcu_allocator_calloc(k->alloc, 1, sizeof *x);
  uint8_t * b;
  if (x == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant = copy(k->alloc, t, tl);
  x->subject = copy(k->alloc, s, sl);
  b = copy_bytes(k->alloc, bytes, n);
  if (x->tenant == NULL || x->subject == NULL || b == NULL) {
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->subject);
    gcu_allocator_free(k->alloc, b);
    gcu_allocator_free(k->alloc, x);
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant_len = tl;
  x->subject_len = sl;
  if (sealed) {
    x->sealed = b;
    x->sealed_len = n;
  } else {
    x->key = b;
  }
  x->next = k->subjects;
  k->subjects = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result k_sealed_put(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, const uint8_t * bytes, size_t n) {
  KStore * k = (KStore *)ctx;
  if (find(k, t, tl, s, sl) != NULL) {
    return GDEFSTOR_ERR_INVALID; /* has a key or material, or was erased */
  }
  return add_subject(k, t, tl, s, sl, bytes, n, 1);
}

static GDEFSTOR_Result k_sealed_get(void * ctx, const char * t, size_t tl,
    const char * s, size_t sl, int * out_erased, uint8_t ** out,
    size_t * out_len) {
  KStore * k = (KStore *)ctx;
  KSubject * x = find(k, t, tl, s, sl);
  *out_erased = 0;
  if (x != NULL && x->erased) {
    *out_erased = 1;
    return GDEFSTOR_OK;
  }
  if (x == NULL || x->sealed == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out = copy_bytes(k->alloc, x->sealed, x->sealed_len);
  if (*out == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  *out_len = x->sealed_len;
  return GDEFSTOR_OK;
}

static KSecret * find_secret(KStore * k, const char * t, size_t tl,
    const char * n, size_t nl) {
  KSecret * x;
  for (x = k->secrets; x != NULL; x = x->next) {
    if (same(x->tenant, x->tenant_len, t, tl) &&
        same(x->name, x->name_len, n, nl)) {
      return x;
    }
  }
  return NULL;
}

static GDEFSTOR_Result k_secret_put(void * ctx, const char * t, size_t tl,
    const char * n, size_t nl, const uint8_t * bytes, size_t len) {
  KStore * k = (KStore *)ctx;
  KSecret * x = find_secret(k, t, tl, n, nl);
  if (x != NULL) {
    return x->len == len && memcmp(x->bytes, bytes, len) == 0
               ? GDEFSTOR_OK
               : GDEFSTOR_ERR_INVALID;
  }
  x = (KSecret *)gcu_allocator_calloc(k->alloc, 1, sizeof *x);
  if (x == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant = copy(k->alloc, t, tl);
  x->name = copy(k->alloc, n, nl);
  x->bytes = copy_bytes(k->alloc, bytes, len);
  if (x->tenant == NULL || x->name == NULL || x->bytes == NULL) {
    gcu_allocator_free(k->alloc, x->tenant);
    gcu_allocator_free(k->alloc, x->name);
    gcu_allocator_free(k->alloc, x->bytes);
    gcu_allocator_free(k->alloc, x);
    return GDEFSTOR_ERR_OOM;
  }
  x->tenant_len = tl;
  x->name_len = nl;
  x->len = len;
  x->next = k->secrets;
  k->secrets = x;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result k_secret_get(void * ctx, const char * t, size_t tl,
    const char * n, size_t nl, int * out_found, uint8_t ** out,
    size_t * out_len) {
  KStore * k = (KStore *)ctx;
  KSecret * x = find_secret(k, t, tl, n, nl);
  *out_found = 0;
  if (x == NULL) {
    return GDEFSTOR_OK;
  }
  *out = copy_bytes(k->alloc, x->bytes, x->len);
  if (*out == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  *out_found = 1;
  *out_len = x->len;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result k_dump(void * ctx, const char * t, size_t tl,
    GDEFSTOR_Result (*visit)(void * vctx, const GDEFSTOR_KeyRow * row),
    void * vctx) {
  KStore * k = (KStore *)ctx;
  GDEFSTOR_KeyRow row;
  KSecret * s;
  KSubject * x;
  KLedger * l;
  int kind;
  GDEFSTOR_Result rc;
  for (s = k->secrets; s != NULL; s = s->next) {
    if (!same(s->tenant, s->tenant_len, t, tl)) {
      continue;
    }
    memset(&row, 0, sizeof row);
    row.kind = GDEFSTOR_KEYROW_SECRET;
    row.name = s->name;
    row.name_len = s->name_len;
    row.bytes = s->bytes;
    row.len = s->len;
    rc = visit(vctx, &row);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (kind = GDEFSTOR_KEYROW_KEY; kind <= GDEFSTOR_KEYROW_SEALED; kind++) {
    for (x = k->subjects; x != NULL; x = x->next) {
      if (!same(x->tenant, x->tenant_len, t, tl) ||
          (kind == GDEFSTOR_KEYROW_KEY ? x->key : x->sealed) == NULL) {
        continue;
      }
      memset(&row, 0, sizeof row);
      row.kind = kind;
      row.name = x->subject;
      row.name_len = x->subject_len;
      row.bytes = kind == GDEFSTOR_KEYROW_KEY ? x->key : x->sealed;
      row.len = kind == GDEFSTOR_KEYROW_KEY ? k->cipher.key_bytes : x->sealed_len;
      rc = visit(vctx, &row);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  for (l = k->ledger; l != NULL; l = l->next) {
    if (!same(l->tenant, l->tenant_len, t, tl)) {
      continue;
    }
    memset(&row, 0, sizeof row);
    row.kind = GDEFSTOR_KEYROW_LEDGER;
    row.name = l->subject;
    row.name_len = l->subject_len;
    row.when_ms = l->when;
    rc = visit(vctx, &row);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* Whether a ledger row in @p rows names the subject. */
static int rows_erase(const GDEFSTOR_KeyRow * rows, size_t n, const char * s,
    size_t sl) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (rows[i].kind == GDEFSTOR_KEYROW_LEDGER &&
        same(rows[i].name, rows[i].name_len, s, sl)) {
      return 1;
    }
  }
  return 0;
}

/* All or none: every conflict is found before anything changes. What can still
 * fail after that is an allocation, and every row applied before it is one
 * the same import, retried, applies as a no-op. */
static GDEFSTOR_Result k_restore(void * ctx, const char * t, size_t tl,
    const GDEFSTOR_KeyRow * rows, size_t n) {
  KStore * k = (KStore *)ctx;
  size_t i;
  GDEFSTOR_Result rc;
  for (i = 0; i < n; i++) {
    const GDEFSTOR_KeyRow * r = &rows[i];
    if (r->kind == GDEFSTOR_KEYROW_SECRET) {
      KSecret * x = find_secret(k, t, tl, r->name, r->name_len);
      if (x != NULL && (x->len != r->len || memcmp(x->bytes, r->bytes, r->len) != 0)) {
        return GDEFSTOR_ERR_INVALID;
      }
    } else if (r->kind == GDEFSTOR_KEYROW_KEY ||
               r->kind == GDEFSTOR_KEYROW_SEALED) {
      KSubject * x = find(k, t, tl, r->name, r->name_len);
      int key = r->kind == GDEFSTOR_KEYROW_KEY;
      if (key && r->len != k->cipher.key_bytes) {
        return GDEFSTOR_ERR_INVALID; /* not a key of this store's cipher */
      }
      if (x == NULL || x->erased || rows_erase(rows, n, r->name, r->name_len)) {
        continue;
      }
      if (key ? (x->key == NULL || memcmp(x->key, r->bytes, r->len) != 0)
              : (x->sealed == NULL || x->sealed_len != r->len ||
                    memcmp(x->sealed, r->bytes, r->len) != 0)) {
        return GDEFSTOR_ERR_INVALID;
      }
    }
  }
  for (i = 0; i < n; i++) {
    const GDEFSTOR_KeyRow * r = &rows[i];
    if (r->kind == GDEFSTOR_KEYROW_LEDGER) {
      rc = k_erase(k, t, tl, r->name, r->name_len, r->when_ms);
    } else if (r->kind == GDEFSTOR_KEYROW_SECRET) {
      rc = k_secret_put(k, t, tl, r->name, r->name_len, r->bytes, r->len);
    } else if (find(k, t, tl, r->name, r->name_len) != NULL ||
               rows_erase(rows, n, r->name, r->name_len)) {
      rc = GDEFSTOR_OK; /* equal, or erased: nothing to write */
    } else {
      rc = add_subject(k, t, tl, r->name, r->name_len, r->bytes, r->len,
          r->kind == GDEFSTOR_KEYROW_SEALED);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
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
  b.sealed_put = k_sealed_put;
  b.sealed_get = k_sealed_get;
  b.secret_put = k_secret_put;
  b.secret_get = k_secret_get;
  b.dump = k_dump;
  b.restore = k_restore;
  rc = gdefstor_keystore_create(&b, use, out_keystore);
  if (rc != GDEFSTOR_OK) {
    k_destroy(k);
    return rc == GDEFSTOR_ERR_OOM ? GDEFSMEMORY_ERR_OOM
                                  : GDEFSMEMORY_ERR_INVALID;
  }
  return GDEFSMEMORY_OK;
}
