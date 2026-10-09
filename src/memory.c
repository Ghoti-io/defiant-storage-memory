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
 * Committed bytes for one process. The port owns isolation; this list only
 * stores what a commit handed it.
 */

#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/cutil/allocator.h>
#include <stdint.h>
#include <string.h>

typedef struct MemNode {
  char * tenant;
  size_t tenant_len;
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  struct MemNode * next;
} MemNode;

typedef struct MemBag {
  const GDEFSMEMORY_Allocator * alloc;
  MemNode * head;
} MemBag;

static int same(const char * a, size_t a_len, const char * b, size_t b_len) {
  return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

static char * dup_span(const GDEFSMEMORY_Allocator * alloc, const char * p,
    size_t n) {
  char * out = (char *)gcu_allocator_malloc(alloc, n + 1);
  if (out == NULL) {
    return NULL;
  }
  if (n > 0 && p != NULL) {
    memcpy(out, p, n);
  }
  out[n] = '\0';
  return out;
}

static MemNode * find_node(MemBag * bag, const char * tenant, size_t tenant_len,
    const char * key, size_t key_len) {
  MemNode * n;
  for (n = bag->head; n != NULL; n = n->next) {
    if (same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        same(n->key, n->key_len, key, key_len)) {
      return n;
    }
  }
  return NULL;
}

static void bag_destroy(void * ctx) {
  MemBag * bag = (MemBag *)ctx;
  MemNode * n;
  if (bag == NULL) {
    return;
  }
  n = bag->head;
  while (n != NULL) {
    MemNode * next = n->next;
    gcu_allocator_free(bag->alloc, n->tenant);
    gcu_allocator_free(bag->alloc, n->key);
    gcu_allocator_free(bag->alloc, n->bytes);
    gcu_allocator_free(bag->alloc, n);
    n = next;
  }
  gcu_allocator_free(bag->alloc, bag);
}

static GDEFSTOR_Result bag_put(void * ctx, const char * tenant, size_t tenant_len,
    const char * key, size_t key_len, const uint8_t * bytes, size_t len) {
  MemBag * bag = (MemBag *)ctx;
  MemNode * n;
  uint8_t * copy;
  if (bag == NULL || tenant == NULL || key == NULL ||
      (bytes == NULL && len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, len == 0 ? 1 : len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (len > 0) {
    memcpy(copy, bytes, len);
  }
  n = find_node(bag, tenant, tenant_len, key, key_len);
  if (n != NULL) {
    gcu_allocator_free(bag->alloc, n->bytes);
    n->bytes = copy;
    n->len = len;
    return GDEFSTOR_OK;
  }
  n = (MemNode *)gcu_allocator_calloc(bag->alloc, 1, sizeof *n);
  if (n == NULL) {
    gcu_allocator_free(bag->alloc, copy);
    return GDEFSTOR_ERR_OOM;
  }
  n->tenant = dup_span(bag->alloc, tenant, tenant_len);
  n->key = dup_span(bag->alloc, key, key_len);
  if (n->tenant == NULL || n->key == NULL) {
    gcu_allocator_free(bag->alloc, n->tenant);
    gcu_allocator_free(bag->alloc, n->key);
    gcu_allocator_free(bag->alloc, n);
    gcu_allocator_free(bag->alloc, copy);
    return GDEFSTOR_ERR_OOM;
  }
  n->tenant_len = tenant_len;
  n->key_len = key_len;
  n->bytes = copy;
  n->len = len;
  n->next = bag->head;
  bag->head = n;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_get(void * ctx, const char * tenant, size_t tenant_len,
    const char * key, size_t key_len, uint8_t ** out_bytes, size_t * out_len) {
  MemBag * bag = (MemBag *)ctx;
  MemNode * n;
  uint8_t * copy;
  if (bag == NULL || out_bytes == NULL || out_len == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  n = find_node(bag, tenant, tenant_len, key, key_len);
  if (n == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  copy = (uint8_t *)gcu_allocator_malloc(bag->alloc, n->len == 0 ? 1 : n->len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  if (n->len > 0) {
    memcpy(copy, n->bytes, n->len);
  }
  *out_bytes = copy;
  *out_len = n->len;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result bag_remove(void * ctx, const char * tenant,
    size_t tenant_len, const char * key, size_t key_len) {
  MemBag * bag = (MemBag *)ctx;
  MemNode ** pp;
  if (bag == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (pp = &bag->head; *pp != NULL; pp = &(*pp)->next) {
    MemNode * n = *pp;
    if (same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        same(n->key, n->key_len, key, key_len)) {
      *pp = n->next;
      gcu_allocator_free(bag->alloc, n->tenant);
      gcu_allocator_free(bag->alloc, n->key);
      gcu_allocator_free(bag->alloc, n->bytes);
      gcu_allocator_free(bag->alloc, n);
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result bag_list(void * ctx, const char * tenant,
    size_t tenant_len, const char * prefix, size_t prefix_len, char *** out_keys,
    size_t ** out_lens, size_t * out_count) {
  MemBag * bag = (MemBag *)ctx;
  MemNode * n;
  size_t count = 0;
  size_t i = 0;
  char ** keys;
  size_t * lens;
  if (bag == NULL || out_keys == NULL || out_lens == NULL || out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (n = bag->head; n != NULL; n = n->next) {
    if (same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        n->key_len >= prefix_len &&
        (prefix_len == 0 || memcmp(n->key, prefix, prefix_len) == 0)) {
      count += 1;
    }
  }
  if (count == 0) {
    *out_keys = NULL;
    *out_lens = NULL;
    *out_count = 0;
    return GDEFSTOR_OK;
  }
  keys = (char **)gcu_allocator_calloc(bag->alloc, count, sizeof *keys);
  lens = (size_t *)gcu_allocator_calloc(bag->alloc, count, sizeof *lens);
  if (keys == NULL || lens == NULL) {
    gcu_allocator_free(bag->alloc, keys);
    gcu_allocator_free(bag->alloc, lens);
    return GDEFSTOR_ERR_OOM;
  }
  for (n = bag->head; n != NULL; n = n->next) {
    if (same(n->tenant, n->tenant_len, tenant, tenant_len) &&
        n->key_len >= prefix_len &&
        (prefix_len == 0 || memcmp(n->key, prefix, prefix_len) == 0)) {
      keys[i] = dup_span(bag->alloc, n->key, n->key_len);
      if (keys[i] == NULL) {
        size_t j;
        for (j = 0; j < i; j++) {
          gcu_allocator_free(bag->alloc, keys[j]);
        }
        gcu_allocator_free(bag->alloc, keys);
        gcu_allocator_free(bag->alloc, lens);
        return GDEFSTOR_ERR_OOM;
      }
      lens[i] = n->key_len;
      i += 1;
    }
  }
  *out_keys = keys;
  *out_lens = lens;
  *out_count = count;
  return GDEFSTOR_OK;
}

static GDEFSMEMORY_Result from_port(GDEFSTOR_Result rc) {
  switch (rc) {
  case GDEFSTOR_OK:
    return GDEFSMEMORY_OK;
  case GDEFSTOR_ERR_IO:
    return GDEFSMEMORY_ERR_IO;
  case GDEFSTOR_ERR_FORMAT:
    return GDEFSMEMORY_ERR_FORMAT;
  case GDEFSTOR_ERR_UNSUPPORTED:
    return GDEFSMEMORY_ERR_UNSUPPORTED;
  case GDEFSTOR_ERR_LIMIT:
    return GDEFSMEMORY_ERR_LIMIT;
  case GDEFSTOR_ERR_CORRUPT:
    return GDEFSMEMORY_ERR_CORRUPT;
  case GDEFSTOR_ERR_OOM:
    return GDEFSMEMORY_ERR_OOM;
  case GDEFSTOR_ERR_INVALID:
    return GDEFSMEMORY_ERR_INVALID;
  case GDEFSTOR_ERR_INTERNAL:
    return GDEFSMEMORY_ERR_INTERNAL;
  case GDEFSTOR_ERR_BUSY:
    return GDEFSMEMORY_ERR_BUSY;
  case GDEFSTOR_RESULT_COUNT:
  default:
    return GDEFSMEMORY_ERR_INTERNAL;
  }
}

GDEFSMEMORY_Result gdefsmemory_store_create(
    const GDEFSMEMORY_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  const GDEFSMEMORY_Allocator * use = gdefsmemory_allocator_default();
  MemBag * bag;
  GDEFSTOR_Backend backend;
  GDEFSTOR_Result rc;
  if (allocator != NULL) {
    use = allocator;
  }
  if (out_store == NULL) {
    return GDEFSMEMORY_ERR_INVALID;
  }
  bag = (MemBag *)gcu_allocator_calloc(use, 1, sizeof *bag);
  if (bag == NULL) {
    return GDEFSMEMORY_ERR_OOM;
  }
  bag->alloc = use;
  memset(&backend, 0, sizeof backend);
  backend.ctx = bag;
  backend.destroy = bag_destroy;
  backend.put = bag_put;
  backend.get = bag_get;
  backend.remove = bag_remove;
  backend.list = bag_list;
  rc = gdefstor_store_create(&backend, use, limits, out_store);
  if (rc != GDEFSTOR_OK) {
    bag_destroy(bag);
  }
  return from_port(rc);
}
