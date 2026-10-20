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
 * The entity backend (model_backend.h) for one process. The port decides what
 * every write means; this file keeps the rows it is handed.
 *
 * Per tenant, each entity owns its truth (the entity facts, its revision and
 * state rows, and every item version) and its current table (a copy of each
 * open version) and newest state, which are derived. A transaction copies an
 * entity the first time it touches it and works on the copy; commit swaps the
 * copies in, abort drops them, and a read outside the transaction keeps seeing
 * the committed entity throughout (AD-13: a read sees a snapshot). Declarations
 * and drafts are copied whole on first write the same way. A lookup from
 * public id to row is derived too.
 */

#include "model_internal.h"

#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* ---- owned rows ---------------------------------------------------------- */

static void val_free(const GDEFSMEMORY_Allocator * a, GDEFSTOR_Value * v) {
  gcu_allocator_free(a, (void *)v->bytes);
  gcu_allocator_free(a, (void *)v->subject);
  memset(v, 0, sizeof *v);
}

static int bytes_copy(const GDEFSMEMORY_Allocator * a, const uint8_t * p,
    size_t n, const uint8_t ** out) {
  uint8_t * c;
  *out = NULL;
  if (n == 0) {
    return 1;
  }
  c = (uint8_t *)gcu_allocator_malloc(a, n);
  if (c == NULL) {
    return 0;
  }
  memcpy(c, p, n);
  *out = c;
  return 1;
}

static GDEFSTOR_Result val_copy(const GDEFSMEMORY_Allocator * a,
    GDEFSTOR_Value * dst, const GDEFSTOR_Value * src) {
  GDEFSTOR_Value v = *src;
  if (!bytes_copy(a, src->bytes, src->len, &v.bytes)) {
    return GDEFSTOR_ERR_OOM;
  }
  if (!bytes_copy(a, src->subject, src->subject_len, &v.subject)) {
    gcu_allocator_free(a, (void *)v.bytes);
    return GDEFSTOR_ERR_OOM;
  }
  *dst = v;
  return GDEFSTOR_OK;
}

static char * str_copy(const GDEFSMEMORY_Allocator * a, const char * p,
    size_t n) {
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

static int same(const char * a, size_t an, const char * b, size_t bn) {
  return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

/* Room for one more of @p n elements of @p size bytes: the array, moved if
 * it had to grow, or NULL (the old one still valid) when the allocator
 * refused. */
static void * grown(const GDEFSMEMORY_Allocator * a, void * v, size_t n,
    size_t * cap, size_t size) {
  void * next;
  size_t c;
  if (n < *cap) {
    return v;
  }
  c = *cap == 0 ? 8 : *cap * 2;
  next = gcu_allocator_realloc(a, v, c * size);
  if (next == NULL) {
    return NULL;
  }
  *cap = c;
  return next;
}

/* grown() into @p field, or return ERR_OOM from the caller. */
#define GROW_OR_OOM(a, field, n, cap)                                          \
  do {                                                                         \
    void * grown_ = grown((a), (field), (n), &(cap), sizeof *(field));         \
    if (grown_ == NULL) {                                                      \
      return GDEFSTOR_ERR_OOM;                                                 \
    }                                                                          \
    (field) = grown_;                                                          \
  } while (0)

typedef struct MVer {
  uint32_t group;
  uint64_t item;
  uint64_t parent;
  double order;
  uint64_t from;
  uint64_t to;
  GDEFSTOR_Value * vals;
  size_t nvals;
  /* The language of its tree: a column of the row, held in place. */
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
} MVer;

static void ver_free(const GDEFSMEMORY_Allocator * a, MVer * v) {
  size_t k;
  for (k = 0; k < v->nvals; k++) {
    val_free(a, &v->vals[k]);
  }
  gcu_allocator_free(a, v->vals);
  memset(v, 0, sizeof *v);
}

static GDEFSTOR_Result ver_set(const GDEFSMEMORY_Allocator * a, MVer * dst,
    const MVer * src) {
  size_t k;
  *dst = *src;
  dst->vals = NULL;
  dst->nvals = 0;
  if (src->nvals == 0) {
    return GDEFSTOR_OK;
  }
  dst->vals = (GDEFSTOR_Value *)gcu_allocator_calloc(a, src->nvals,
      sizeof *dst->vals);
  if (dst->vals == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (k = 0; k < src->nvals; k++) {
    if (val_copy(a, &dst->vals[k], &src->vals[k]) != GDEFSTOR_OK) {
      dst->nvals = k;
      ver_free(a, dst);
      return GDEFSTOR_ERR_OOM;
    }
  }
  dst->nvals = src->nvals;
  return GDEFSTOR_OK;
}

typedef struct MRev {
  uint64_t rev;
  uint64_t seq;
  char * who;
  size_t who_len;
  int64_t when;
  char * why;
  size_t why_len;
  char * langs; /* the languages it changed, as the port gave them */
  size_t langs_len;
} MRev;

typedef struct MSt {
  uint64_t seq;
  int pub;
  int del;
  char * actor;
  size_t actor_len;
  int64_t when;
} MSt;

/* A blind row (AD-14): truth while its subject exists, kept per entity. */
typedef struct MBlind {
  uint64_t item;
  uint64_t from;
  uint32_t field;
  uint8_t * subject;
  size_t subject_len;
  uint8_t mac[GDEFSTOR_BLIND_MAC_BYTES];
  uint32_t key_id;
  uint32_t version;
  char lang[GDEFSTOR_MODEL_MAX_LANGUAGE_BYTES];
  size_t lang_len;
} MBlind;

typedef struct MEnt {
  uint64_t row;
  char * type;
  size_t type_len;
  char * pid;
  size_t pid_len;
  char * creator;
  size_t creator_len;
  int64_t created;
  uint64_t revision;
  uint64_t last_item;
  MRev * revs;
  size_t nrev;
  size_t caprev;
  MSt * sts;
  size_t nst;
  size_t capst;
  MVer * vers; /* the truth: every version, appended */
  size_t nver;
  size_t capver;
  MVer * cur; /* derived: the current table */
  size_t ncur;
  size_t capcur;
  MBlind * blinds; /* the blind table's rows of this entity: never derived */
  size_t nblind;
  size_t capblind;
  int pub;     /* derived: the newest state */
  int del;
} MEnt;

static void ent_free(const GDEFSMEMORY_Allocator * a, MEnt * e) {
  size_t i;
  if (e == NULL) {
    return;
  }
  for (i = 0; i < e->nrev; i++) {
    gcu_allocator_free(a, e->revs[i].who);
    gcu_allocator_free(a, e->revs[i].why);
    gcu_allocator_free(a, e->revs[i].langs);
  }
  for (i = 0; i < e->nst; i++) {
    gcu_allocator_free(a, e->sts[i].actor);
  }
  for (i = 0; i < e->nver; i++) {
    ver_free(a, &e->vers[i]);
  }
  for (i = 0; i < e->ncur; i++) {
    ver_free(a, &e->cur[i]);
  }
  for (i = 0; i < e->nblind; i++) {
    gcu_allocator_free(a, e->blinds[i].subject);
  }
  gcu_allocator_free(a, e->blinds);
  gcu_allocator_free(a, e->revs);
  gcu_allocator_free(a, e->sts);
  gcu_allocator_free(a, e->vers);
  gcu_allocator_free(a, e->cur);
  gcu_allocator_free(a, e->type);
  gcu_allocator_free(a, e->pid);
  gcu_allocator_free(a, e->creator);
  gcu_allocator_free(a, e);
}

static GDEFSTOR_Result ent_copy(const GDEFSMEMORY_Allocator * a,
    const MEnt * e, MEnt ** out) {
  MEnt * c = (MEnt *)gcu_allocator_calloc(a, 1, sizeof *c);
  size_t i;
  if (c == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  c->row = e->row;
  c->created = e->created;
  c->revision = e->revision;
  c->last_item = e->last_item;
  c->pub = e->pub;
  c->del = e->del;
  c->type = str_copy(a, e->type, e->type_len);
  c->pid = str_copy(a, e->pid, e->pid_len);
  c->creator = str_copy(a, e->creator, e->creator_len);
  c->type_len = e->type_len;
  c->pid_len = e->pid_len;
  c->creator_len = e->creator_len;
  if (c->type == NULL || c->pid == NULL || c->creator == NULL) {
    ent_free(a, c);
    return GDEFSTOR_ERR_OOM;
  }
  if (e->nrev > 0) {
    c->revs = (MRev *)gcu_allocator_calloc(a, e->nrev, sizeof *c->revs);
    if (c->revs == NULL) {
      ent_free(a, c);
      return GDEFSTOR_ERR_OOM;
    }
    c->caprev = e->nrev;
    for (i = 0; i < e->nrev; i++) {
      c->revs[i] = e->revs[i];
      c->revs[i].who = str_copy(a, e->revs[i].who, e->revs[i].who_len);
      c->revs[i].why = str_copy(a, e->revs[i].why, e->revs[i].why_len);
      c->revs[i].langs = str_copy(a, e->revs[i].langs, e->revs[i].langs_len);
      c->nrev = i + 1;
      if (c->revs[i].who == NULL || c->revs[i].why == NULL ||
          c->revs[i].langs == NULL) {
        ent_free(a, c);
        return GDEFSTOR_ERR_OOM;
      }
    }
  }
  if (e->nst > 0) {
    c->sts = (MSt *)gcu_allocator_calloc(a, e->nst, sizeof *c->sts);
    if (c->sts == NULL) {
      ent_free(a, c);
      return GDEFSTOR_ERR_OOM;
    }
    c->capst = e->nst;
    for (i = 0; i < e->nst; i++) {
      c->sts[i] = e->sts[i];
      c->sts[i].actor = str_copy(a, e->sts[i].actor, e->sts[i].actor_len);
      c->nst = i + 1;
      if (c->sts[i].actor == NULL) {
        ent_free(a, c);
        return GDEFSTOR_ERR_OOM;
      }
    }
  }
  if (e->nver > 0) {
    c->vers = (MVer *)gcu_allocator_calloc(a, e->nver, sizeof *c->vers);
    if (c->vers == NULL) {
      ent_free(a, c);
      return GDEFSTOR_ERR_OOM;
    }
    c->capver = e->nver;
    for (i = 0; i < e->nver; i++) {
      if (ver_set(a, &c->vers[i], &e->vers[i]) != GDEFSTOR_OK) {
        ent_free(a, c);
        return GDEFSTOR_ERR_OOM;
      }
      c->nver = i + 1;
    }
  }
  if (e->ncur > 0) {
    c->cur = (MVer *)gcu_allocator_calloc(a, e->ncur, sizeof *c->cur);
    if (c->cur == NULL) {
      ent_free(a, c);
      return GDEFSTOR_ERR_OOM;
    }
    c->capcur = e->ncur;
    for (i = 0; i < e->ncur; i++) {
      if (ver_set(a, &c->cur[i], &e->cur[i]) != GDEFSTOR_OK) {
        ent_free(a, c);
        return GDEFSTOR_ERR_OOM;
      }
      c->ncur = i + 1;
    }
  }
  if (e->nblind > 0) {
    c->blinds = (MBlind *)gcu_allocator_calloc(a, e->nblind, sizeof *c->blinds);
    if (c->blinds == NULL) {
      ent_free(a, c);
      return GDEFSTOR_ERR_OOM;
    }
    c->capblind = e->nblind;
    for (i = 0; i < e->nblind; i++) {
      c->blinds[i] = e->blinds[i];
      c->blinds[i].subject = (uint8_t *)str_copy(a,
          (const char *)e->blinds[i].subject, e->blinds[i].subject_len);
      c->nblind = i + 1;
      if (c->blinds[i].subject == NULL) {
        ent_free(a, c);
        return GDEFSTOR_ERR_OOM;
      }
    }
  }
  *out = c;
  return GDEFSTOR_OK;
}

/* Derived data from the truth: the open versions and the newest state. */
static GDEFSTOR_Result ent_derive(const GDEFSMEMORY_Allocator * a, MEnt * e) {
  MVer * cur = NULL;
  size_t ncur = 0;
  size_t i;
  for (i = 0; i < e->nver; i++) {
    if (e->vers[i].to == 0) {
      ncur += 1;
    }
  }
  if (ncur > 0) {
    size_t at = 0;
    cur = (MVer *)gcu_allocator_calloc(a, ncur, sizeof *cur);
    if (cur == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    for (i = 0; i < e->nver; i++) {
      if (e->vers[i].to != 0) {
        continue;
      }
      if (ver_set(a, &cur[at], &e->vers[i]) != GDEFSTOR_OK) {
        size_t k;
        for (k = 0; k < at; k++) {
          ver_free(a, &cur[k]);
        }
        gcu_allocator_free(a, cur);
        return GDEFSTOR_ERR_OOM;
      }
      at += 1;
    }
  }
  for (i = 0; i < e->ncur; i++) {
    ver_free(a, &e->cur[i]);
  }
  gcu_allocator_free(a, e->cur);
  e->cur = cur;
  e->ncur = ncur;
  e->capcur = ncur;
  e->pub = e->nst == 0 ? 0 : e->sts[e->nst - 1].pub;
  e->del = e->nst == 0 ? 0 : e->sts[e->nst - 1].del;
  return GDEFSTOR_OK;
}

typedef struct MType {
  char * name;
  size_t name_len;
  uint8_t * text;
  size_t text_len;
} MType;

typedef struct MDraft {
  char * pid;
  size_t pid_len;
  char * type;
  size_t type_len;
  uint64_t base;
  uint8_t * json;
  size_t json_len;
} MDraft;

/* A record: committed (in the tenant, sorted by namespace then key), or a
 * transaction's put or delete of one (dead), which commit applies. */
typedef struct MRec {
  char * ns;
  size_t ns_len;
  char * key;
  size_t key_len;
  uint8_t * bytes;
  size_t len;
  int dead;
} MRec;

/* A counter's value (committed, or a transaction's), or a counter's floor. */
typedef struct MCnt {
  int kind;
  char * name;
  size_t name_len;
  uint64_t value;
} MCnt;

/* An outbox row. */
typedef struct MOb {
  char * stream;
  size_t stream_len;
  uint64_t seq;
  char * pid;
  size_t pid_len;
  char * fmt;
  size_t fmt_len;
  uint8_t * payload;
  size_t payload_len;
  int64_t until;
  uint32_t fails;
  int parked;
} MOb;

typedef struct MTen {
  char * name;
  size_t name_len;
  /* Records, counters and the outbox: truth, which rebuild never touches. */
  MRec * recs; /* sorted by ns, then key */
  size_t nrec;
  size_t caprec;
  MCnt * cnts; /* committed values */
  size_t ncnt;
  size_t capcnt;
  /* The highest value each counter ever handed out, committed or not:
   * written at once, never in a transaction, so an aborted write's value
   * stays taken. */
  MCnt * floors;
  size_t nfloor;
  size_t capfloor;
  MOb * obs; /* sorted by stream, then sequence */
  size_t nob;
  size_t capob;
  MEnt ** ents; /* by row - 1 */
  size_t nent;
  size_t capent;
  MType * types;
  size_t ntype;
  size_t captype;
  MDraft * drafts;
  size_t ndraft;
  size_t capdraft;
  uint64_t commit_seq;
  uint64_t * slots; /* derived: public id -> row, open addressing; 0 empty */
  size_t nslot;
  struct MTen * next;
} MTen;

typedef struct MTxn {
  MTen * ten;
  MEnt ** touched; /* copies of committed rows, and the new rows */
  size_t ntouch;
  size_t captouch;
  uint64_t next_row;
  MType * types; /* a copy, once written */
  size_t ntype;
  size_t captype;
  int types_copied;
  MDraft * drafts; /* a copy, once written */
  size_t ndraft;
  size_t capdraft;
  int drafts_copied;
  uint64_t seq;
  /* The largest commit sequence an import restored in this transaction
   * (m_restore_revision): the tenant's is raised to it at commit. */
  uint64_t restored;
  MRec * rput; /* records put or deleted, one entry per key */
  size_t nrput;
  size_t caprput;
  MCnt * cset; /* counters advanced */
  size_t ncset;
  size_t capcset;
  MOb * obs; /* a copy of the outbox, once written */
  size_t nob;
  size_t capob;
  int obs_copied;
  struct MTxn * next;
} MTxn;

typedef struct MModel {
  const GDEFSMEMORY_Allocator * alloc;
  MTen * tenants;
  MTxn * open;
} MModel;

static void types_free(const GDEFSMEMORY_Allocator * a, MType * v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    gcu_allocator_free(a, v[i].name);
    gcu_allocator_free(a, v[i].text);
  }
  gcu_allocator_free(a, v);
}

static void drafts_free(const GDEFSMEMORY_Allocator * a, MDraft * v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    gcu_allocator_free(a, v[i].pid);
    gcu_allocator_free(a, v[i].type);
    gcu_allocator_free(a, v[i].json);
  }
  gcu_allocator_free(a, v);
}

static GDEFSTOR_Result types_copy(const GDEFSMEMORY_Allocator * a,
    const MType * src, size_t n, MType ** out) {
  MType * v;
  size_t i;
  *out = NULL;
  if (n == 0) {
    return GDEFSTOR_OK;
  }
  v = (MType *)gcu_allocator_calloc(a, n, sizeof *v);
  if (v == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    v[i].name = str_copy(a, src[i].name, src[i].name_len);
    v[i].text = (uint8_t *)str_copy(a, (const char *)src[i].text,
        src[i].text_len);
    v[i].name_len = src[i].name_len;
    v[i].text_len = src[i].text_len;
    if (v[i].name == NULL || v[i].text == NULL) {
      types_free(a, v, i + 1);
      return GDEFSTOR_ERR_OOM;
    }
  }
  *out = v;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result drafts_copy(const GDEFSMEMORY_Allocator * a,
    const MDraft * src, size_t n, MDraft ** out) {
  MDraft * v;
  size_t i;
  *out = NULL;
  if (n == 0) {
    return GDEFSTOR_OK;
  }
  v = (MDraft *)gcu_allocator_calloc(a, n, sizeof *v);
  if (v == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < n; i++) {
    v[i] = src[i];
    v[i].pid = str_copy(a, src[i].pid, src[i].pid_len);
    v[i].type = str_copy(a, src[i].type, src[i].type_len);
    v[i].json = (uint8_t *)str_copy(a, (const char *)src[i].json,
        src[i].json_len);
    if (v[i].pid == NULL || v[i].type == NULL || v[i].json == NULL) {
      drafts_free(a, v, i + 1);
      return GDEFSTOR_ERR_OOM;
    }
  }
  *out = v;
  return GDEFSTOR_OK;
}

/* ---- the public-id lookup (derived) -------------------------------------- */

static uint64_t hash(const char * p, size_t n) {
  uint64_t h = UINT64_C(1469598103934665603);
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= (unsigned char)p[i];
    h *= UINT64_C(1099511628211);
  }
  return h;
}

static uint64_t slot_find(const MTen * t, const char * pid, size_t len) {
  size_t i;
  if (t->nslot == 0) {
    return 0;
  }
  i = (size_t)(hash(pid, len) % t->nslot);
  while (t->slots[i] != 0) {
    const MEnt * e = t->ents[t->slots[i] - 1];
    if (same(e->pid, e->pid_len, pid, len)) {
      return t->slots[i];
    }
    i = (i + 1) % t->nslot;
  }
  return 0;
}

static void slot_put(uint64_t * slots, size_t n, const MEnt * e) {
  size_t i = (size_t)(hash(e->pid, e->pid_len) % n);
  while (slots[i] != 0) {
    i = (i + 1) % n;
  }
  slots[i] = e->row;
}

/* A lookup for the committed rows and the @p nextra rows of @p extra, which
 * are about to be. Nothing is changed when the allocator refuses. */
static GDEFSTOR_Result slots_make(const GDEFSMEMORY_Allocator * a,
    const MTen * t, MEnt * const * extra, size_t nextra, uint64_t ** out,
    size_t * out_n) {
  size_t n = 16;
  uint64_t * slots;
  size_t r;
  while (n < (t->nent + nextra) * 2 + 1) {
    n *= 2;
  }
  slots = (uint64_t *)gcu_allocator_calloc(a, n, sizeof *slots);
  if (slots == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (r = 0; r < t->nent; r++) {
    slot_put(slots, n, t->ents[r]);
  }
  for (r = 0; r < nextra; r++) {
    if (extra[r]->row > t->nent) {
      slot_put(slots, n, extra[r]);
    }
  }
  *out = slots;
  *out_n = n;
  return GDEFSTOR_OK;
}

/* Rebuild the lookup for every committed row. */
static GDEFSTOR_Result slots_build(const GDEFSMEMORY_Allocator * a, MTen * t) {
  uint64_t * slots = NULL;
  size_t n = 0;
  GDEFSTOR_Result rc = slots_make(a, t, NULL, 0, &slots, &n);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  gcu_allocator_free(a, t->slots);
  t->slots = slots;
  t->nslot = n;
  return GDEFSTOR_OK;
}

/* ---- records, counters and outbox rows, owned ----------------------------- */

static void rec_free(const GDEFSMEMORY_Allocator * a, MRec * r) {
  gcu_allocator_free(a, r->ns);
  gcu_allocator_free(a, r->key);
  gcu_allocator_free(a, r->bytes);
  memset(r, 0, sizeof *r);
}

static void recs_free(const GDEFSMEMORY_Allocator * a, MRec * v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    rec_free(a, &v[i]);
  }
  gcu_allocator_free(a, v);
}

static void cnts_free(const GDEFSMEMORY_Allocator * a, MCnt * v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    gcu_allocator_free(a, v[i].name);
  }
  gcu_allocator_free(a, v);
}

static void ob_free(const GDEFSMEMORY_Allocator * a, MOb * o) {
  gcu_allocator_free(a, o->stream);
  gcu_allocator_free(a, o->pid);
  gcu_allocator_free(a, o->fmt);
  gcu_allocator_free(a, o->payload);
  memset(o, 0, sizeof *o);
}

static void obs_free(const GDEFSMEMORY_Allocator * a, MOb * v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    ob_free(a, &v[i]);
  }
  gcu_allocator_free(a, v);
}

/* ---- tenants and transactions -------------------------------------------- */

static MTen * ten_find(MModel * m, const char * name, size_t len) {
  MTen * t;
  for (t = m->tenants; t != NULL; t = t->next) {
    if (same(t->name, t->name_len, name, len)) {
      return t;
    }
  }
  return NULL;
}

static void ten_free(const GDEFSMEMORY_Allocator * a, MTen * t) {
  size_t i;
  for (i = 0; i < t->nent; i++) {
    ent_free(a, t->ents[i]);
  }
  gcu_allocator_free(a, t->ents);
  types_free(a, t->types, t->ntype);
  drafts_free(a, t->drafts, t->ndraft);
  recs_free(a, t->recs, t->nrec);
  cnts_free(a, t->cnts, t->ncnt);
  cnts_free(a, t->floors, t->nfloor);
  obs_free(a, t->obs, t->nob);
  gcu_allocator_free(a, t->slots);
  gcu_allocator_free(a, t->name);
  gcu_allocator_free(a, t);
}

static void txn_free(const GDEFSMEMORY_Allocator * a, MTxn * x) {
  size_t i;
  for (i = 0; i < x->ntouch; i++) {
    ent_free(a, x->touched[i]);
  }
  gcu_allocator_free(a, x->touched);
  if (x->types_copied) {
    types_free(a, x->types, x->ntype);
  }
  if (x->drafts_copied) {
    drafts_free(a, x->drafts, x->ndraft);
  }
  recs_free(a, x->rput, x->nrput);
  cnts_free(a, x->cset, x->ncset);
  if (x->obs_copied) {
    obs_free(a, x->obs, x->nob);
  }
  gcu_allocator_free(a, x);
}

/* The entity with this row as the transaction (or, without one, the last
 * commit) sees it; NULL when there is none. */
static MEnt * ent_view(MTen * t, MTxn * x, uint64_t row) {
  size_t i;
  if (x != NULL) {
    for (i = 0; i < x->ntouch; i++) {
      if (x->touched[i]->row == row) {
        return x->touched[i];
      }
    }
  }
  if (t == NULL || row == 0 || row > t->nent) {
    return NULL;
  }
  return t->ents[row - 1];
}

/* The transaction's own copy of a row, made on first touch. */
static GDEFSTOR_Result ent_touch(MModel * m, MTxn * x, uint64_t row,
    MEnt ** out) {
  size_t i;
  MEnt * c;
  GDEFSTOR_Result rc;
  for (i = 0; i < x->ntouch; i++) {
    if (x->touched[i]->row == row) {
      *out = x->touched[i];
      return GDEFSTOR_OK;
    }
  }
  if (row == 0 || row > x->ten->nent) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  GROW_OR_OOM(m->alloc, x->touched, x->ntouch, x->captouch);
  rc = ent_copy(m->alloc, x->ten->ents[row - 1], &c);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  x->touched[x->ntouch] = c;
  x->ntouch += 1;
  *out = c;
  return GDEFSTOR_OK;
}

static MTen * view_tenant(MModel * m, MTxn * x, const char * tenant,
    size_t tenant_len) {
  return x != NULL ? x->ten : ten_find(m, tenant, tenant_len);
}

static uint64_t pid_row(MTen * t, MTxn * x, const char * pid, size_t len) {
  size_t i;
  if (x != NULL) {
    /* Rows made in this transaction are not in the lookup yet. */
    for (i = 0; i < x->ntouch; i++) {
      const MEnt * e = x->touched[i];
      if (e->row > x->ten->nent && same(e->pid, e->pid_len, pid, len)) {
        return e->row;
      }
    }
  }
  return t == NULL ? 0 : slot_find(t, pid, len);
}

static GDEFSTOR_Result m_begin(void * ctx, const char * tenant,
    size_t tenant_len, void ** out_txn) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  MTxn * x;
  for (x = m->open; x != NULL; x = x->next) {
    if (x->ten != NULL && same(x->ten->name, x->ten->name_len, tenant,
                              tenant_len)) {
      return GDEFSTOR_ERR_INTERNAL;
    }
  }
  if (t == NULL) {
    t = (MTen *)gcu_allocator_calloc(m->alloc, 1, sizeof *t);
    if (t == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    t->name = str_copy(m->alloc, tenant, tenant_len);
    if (t->name == NULL) {
      gcu_allocator_free(m->alloc, t);
      return GDEFSTOR_ERR_OOM;
    }
    t->name_len = tenant_len;
    t->next = m->tenants;
    m->tenants = t;
  }
  x = (MTxn *)gcu_allocator_calloc(m->alloc, 1, sizeof *x);
  if (x == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  x->ten = t;
  x->next_row = t->nent + 1;
  x->next = m->open;
  m->open = x;
  *out_txn = x;
  return GDEFSTOR_OK;
}

static void txn_unlink(MModel * m, MTxn * x) {
  MTxn ** pp = &m->open;
  while (*pp != NULL) {
    if (*pp == x) {
      *pp = x->next;
      return;
    }
    pp = &(*pp)->next;
  }
}

static GDEFSTOR_Result m_abort(void * ctx, void * txn) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  txn_unlink(m, x);
  txn_free(m->alloc, x);
  return GDEFSTOR_OK;
}

static void sys_apply(const GDEFSMEMORY_Allocator * a, MTen * t, MTxn * x);

static GDEFSTOR_Result m_commit(void * ctx, void * txn) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MTen * t;
  size_t added = 0;
  size_t i;
  uint64_t * slots = NULL;
  size_t nslot = 0;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  t = x->ten;
  /* Everything that can fail first: the lookup with the new rows in it, and
   * room for them; after that nothing fails and the swap is whole. */
  for (i = 0; i < x->ntouch; i++) {
    if (x->touched[i]->row > t->nent) {
      added += 1;
    }
  }
  if ((added > 0 || t->nslot == 0) &&
      slots_make(m->alloc, t, x->touched, x->ntouch, &slots, &nslot) !=
          GDEFSTOR_OK) {
    m_abort(ctx, txn);
    return GDEFSTOR_ERR_OOM;
  }
  /* Room for every record and counter the transaction adds, so that
   * applying them below cannot fail either. */
  if (t->nrec + x->nrput > t->caprec) {
    size_t cap = t->nrec + x->nrput;
    MRec * next = (MRec *)gcu_allocator_realloc(m->alloc, t->recs,
        cap * sizeof *next);
    if (next == NULL) {
      gcu_allocator_free(m->alloc, slots);
      m_abort(ctx, txn);
      return GDEFSTOR_ERR_OOM;
    }
    t->recs = next;
    t->caprec = cap;
  }
  if (t->ncnt + x->ncset > t->capcnt) {
    size_t cap = t->ncnt + x->ncset;
    MCnt * next = (MCnt *)gcu_allocator_realloc(m->alloc, t->cnts,
        cap * sizeof *next);
    if (next == NULL) {
      gcu_allocator_free(m->alloc, slots);
      m_abort(ctx, txn);
      return GDEFSTOR_ERR_OOM;
    }
    t->cnts = next;
    t->capcnt = cap;
  }
  if (t->nent + added > t->capent) {
    size_t cap = t->capent == 0 ? 16 : t->capent;
    MEnt ** next;
    while (cap < t->nent + added) {
      cap *= 2;
    }
    next = (MEnt **)gcu_allocator_realloc(m->alloc, t->ents, cap * sizeof *next);
    if (next == NULL) {
      gcu_allocator_free(m->alloc, slots);
      m_abort(ctx, txn);
      return GDEFSTOR_ERR_OOM;
    }
    t->ents = next;
    t->capent = cap;
  }
  if (slots != NULL) {
    gcu_allocator_free(m->alloc, t->slots);
    t->slots = slots;
    t->nslot = nslot;
  }
  {
    size_t base = t->nent;
    for (i = 0; i < x->ntouch; i++) {
      MEnt * e = x->touched[i];
      if (e->row <= base) {
        ent_free(m->alloc, t->ents[e->row - 1]);
        t->ents[e->row - 1] = e;
      } else {
        t->ents[e->row - 1] = e;
      }
    }
    t->nent = base + added;
    x->ntouch = 0;
  }
  if (x->types_copied) {
    types_free(m->alloc, t->types, t->ntype);
    t->types = x->types;
    t->ntype = x->ntype;
    t->captype = x->captype;
    x->types_copied = 0;
  }
  if (x->drafts_copied) {
    drafts_free(m->alloc, t->drafts, t->ndraft);
    t->drafts = x->drafts;
    t->ndraft = x->ndraft;
    t->capdraft = x->capdraft;
    x->drafts_copied = 0;
  }
  if (x->seq != 0) {
    t->commit_seq = x->seq;
  }
  if (x->restored > t->commit_seq) {
    t->commit_seq = x->restored;
  }
  sys_apply(m->alloc, t, x);
  txn_unlink(m, x);
  txn_free(m->alloc, x);
  return GDEFSTOR_OK;
}

/* ---- declarations and drafts --------------------------------------------- */

static const MType * types_of(MTen * t, MTxn * x, size_t * n) {
  if (x != NULL && x->types_copied) {
    *n = x->ntype;
    return x->types;
  }
  if (t == NULL) {
    *n = 0;
    return NULL;
  }
  *n = t->ntype;
  return t->types;
}

static const MDraft * drafts_of(MTen * t, MTxn * x, size_t * n) {
  if (x != NULL && x->drafts_copied) {
    *n = x->ndraft;
    return x->drafts;
  }
  if (t == NULL) {
    *n = 0;
    return NULL;
  }
  *n = t->ndraft;
  return t->drafts;
}

static GDEFSTOR_Result m_type_load(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, const char * type, size_t type_len, uint8_t ** out_text,
    size_t * out_len) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MTen * t = view_tenant(m, x, tenant, tenant_len);
  size_t n = 0;
  const MType * v = types_of(t, x, &n);
  size_t i;
  for (i = 0; i < n; i++) {
    if (same(v[i].name, v[i].name_len, type, type_len)) {
      uint8_t * c = (uint8_t *)str_copy(m->alloc, (const char *)v[i].text,
          v[i].text_len);
      if (c == NULL) {
        return GDEFSTOR_ERR_OOM;
      }
      *out_text = c;
      *out_len = v[i].text_len;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result m_type_store(void * ctx, void * txn,
    const GDEFSTOR_Type * decl, const uint8_t * text, size_t text_len) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  GDEFSTOR_FieldInfo info;
  const char * name = NULL;
  size_t name_len = 0;
  uint8_t * copy;
  size_t i;
  (void)info;
  if (x == NULL || decl == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  /* The name is the declaration's; a SQL backend would make its tables here. */
  {
    size_t at;
    const char * s = (const char *)text;
    const char * key = "\"name\":\"";
    size_t klen = strlen(key);
    for (at = 0; at + klen <= text_len; at++) {
      if (memcmp(s + at, key, klen) == 0) {
        size_t end = at + klen;
        while (end < text_len && s[end] != '"') {
          end += 1;
        }
        name = s + at + klen;
        name_len = end - at - klen;
        break;
      }
    }
  }
  if (name == NULL) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  if (!x->types_copied) {
    GDEFSTOR_Result rc = types_copy(m->alloc, x->ten->types, x->ten->ntype,
        &x->types);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    x->ntype = x->ten->ntype;
    x->captype = x->ten->ntype;
    x->types_copied = 1;
  }
  copy = (uint8_t *)str_copy(m->alloc, (const char *)text, text_len);
  if (copy == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  for (i = 0; i < x->ntype; i++) {
    if (same(x->types[i].name, x->types[i].name_len, name, name_len)) {
      gcu_allocator_free(m->alloc, x->types[i].text);
      x->types[i].text = copy;
      x->types[i].text_len = text_len;
      return GDEFSTOR_OK;
    }
  }
  {
    void * more = grown(m->alloc, x->types, x->ntype, &x->captype,
        sizeof *x->types);
    if (more == NULL) {
      gcu_allocator_free(m->alloc, copy);
      return GDEFSTOR_ERR_OOM;
    }
    x->types = more;
  }
  x->types[x->ntype].name = str_copy(m->alloc, name, name_len);
  if (x->types[x->ntype].name == NULL) {
    gcu_allocator_free(m->alloc, copy);
    return GDEFSTOR_ERR_OOM;
  }
  x->types[x->ntype].name_len = name_len;
  x->types[x->ntype].text = copy;
  x->types[x->ntype].text_len = text_len;
  x->ntype += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result drafts_touch(MModel * m, MTxn * x) {
  GDEFSTOR_Result rc;
  if (x->drafts_copied) {
    return GDEFSTOR_OK;
  }
  rc = drafts_copy(m->alloc, x->ten->drafts, x->ten->ndraft, &x->drafts);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  x->ndraft = x->ten->ndraft;
  x->capdraft = x->ten->ndraft;
  x->drafts_copied = 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_draft_put(void * ctx, void * txn,
    const GDEFSTOR_ModelDraft * d) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MDraft n;
  size_t i;
  GDEFSTOR_Result rc = drafts_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&n, 0, sizeof n);
  n.pid = str_copy(m->alloc, d->public_id, d->public_id_len);
  n.type = str_copy(m->alloc, d->type, d->type_len);
  n.json = (uint8_t *)str_copy(m->alloc, (const char *)d->json, d->json_len);
  if (n.pid == NULL || n.type == NULL || n.json == NULL) {
    gcu_allocator_free(m->alloc, n.pid);
    gcu_allocator_free(m->alloc, n.type);
    gcu_allocator_free(m->alloc, n.json);
    return GDEFSTOR_ERR_OOM;
  }
  n.pid_len = d->public_id_len;
  n.type_len = d->type_len;
  n.json_len = d->json_len;
  n.base = d->base_revision;
  for (i = 0; i < x->ndraft; i++) {
    if (same(x->drafts[i].pid, x->drafts[i].pid_len, n.pid, n.pid_len)) {
      gcu_allocator_free(m->alloc, x->drafts[i].pid);
      gcu_allocator_free(m->alloc, x->drafts[i].type);
      gcu_allocator_free(m->alloc, x->drafts[i].json);
      x->drafts[i] = n;
      return GDEFSTOR_OK;
    }
  }
  {
    void * more = grown(m->alloc, x->drafts, x->ndraft, &x->capdraft,
        sizeof *x->drafts);
    if (more == NULL) {
      gcu_allocator_free(m->alloc, n.pid);
      gcu_allocator_free(m->alloc, n.type);
      gcu_allocator_free(m->alloc, n.json);
      return GDEFSTOR_ERR_OOM;
    }
    x->drafts = more;
  }
  x->drafts[x->ndraft] = n;
  x->ndraft += 1;
  return GDEFSTOR_OK;
}

static void lend_draft(const MDraft * d, GDEFSTOR_ModelDraft * out) {
  memset(out, 0, sizeof *out);
  out->public_id = d->pid;
  out->public_id_len = d->pid_len;
  out->type = d->type;
  out->type_len = d->type_len;
  out->base_revision = d->base;
  out->json = d->json;
  out->json_len = d->json_len;
}

static GDEFSTOR_Result m_draft_get(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, const char * public_id, size_t public_id_len,
    const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  size_t n = 0;
  const MDraft * v = drafts_of(view_tenant(m, x, tenant, tenant_len), x, &n);
  size_t i;
  for (i = 0; i < n; i++) {
    if (same(v[i].pid, v[i].pid_len, public_id, public_id_len)) {
      GDEFSTOR_ModelDraft d;
      lend_draft(&v[i], &d);
      return visitor->draft(visitor->ctx, &d);
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

static GDEFSTOR_Result m_draft_delete(void * ctx, void * txn,
    const char * public_id, size_t public_id_len) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  size_t i;
  GDEFSTOR_Result rc = drafts_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  for (i = 0; i < x->ndraft; i++) {
    if (same(x->drafts[i].pid, x->drafts[i].pid_len, public_id,
            public_id_len)) {
      gcu_allocator_free(m->alloc, x->drafts[i].pid);
      gcu_allocator_free(m->alloc, x->drafts[i].type);
      gcu_allocator_free(m->alloc, x->drafts[i].json);
      memmove(&x->drafts[i], &x->drafts[i + 1],
          (x->ndraft - i - 1) * sizeof *x->drafts);
      x->ndraft -= 1;
      return GDEFSTOR_OK;
    }
  }
  return GDEFSTOR_ERR_INVALID;
}

/* ---- entities ------------------------------------------------------------ */

static void lend_entity(const MEnt * e, GDEFSTOR_ModelEntity * out) {
  memset(out, 0, sizeof *out);
  out->row = e->row;
  out->type = e->type;
  out->type_len = e->type_len;
  out->public_id = e->pid;
  out->public_id_len = e->pid_len;
  out->creator = e->creator;
  out->creator_len = e->creator_len;
  out->created_ms = e->created;
  out->revision = e->revision;
  out->last_item = e->last_item;
  out->published = e->pub;
  out->deleted = e->del;
}

static GDEFSTOR_Result visit_ver(const GDEFSTOR_ModelVisitor * v, uint64_t row,
    const MVer * x) {
  GDEFSTOR_ModelItem i;
  memset(&i, 0, sizeof i);
  i.group = x->group;
  i.item = x->item;
  i.parent = x->parent;
  i.order = x->order;
  i.valid_from = x->from;
  i.valid_to = x->to;
  i.values = x->vals;
  i.value_count = x->nvals;
  i.language = x->lang;
  i.language_len = x->lang_len;
  return v->item(v->ctx, row, &i);
}

/* @p lang_len 0: every language. */
static int in_lang(const char * have, size_t have_len, const char * lang,
    size_t lang_len) {
  return lang_len == 0 || same(have, have_len, lang, lang_len);
}

static GDEFSTOR_Result visit_blinds(const GDEFSTOR_ModelVisitor * v,
    const MEnt * e, const char * lang, size_t lang_len) {
  size_t i;
  for (i = 0; v->blind != NULL && i < e->nblind; i++) {
    const MBlind * x = &e->blinds[i];
    GDEFSTOR_ModelBlind b;
    GDEFSTOR_Result rc;
    if (!in_lang(x->lang, x->lang_len, lang, lang_len)) {
      continue;
    }
    memset(&b, 0, sizeof b);
    b.item = x->item;
    b.valid_from = x->from;
    b.field = x->field;
    b.subject = x->subject;
    b.subject_len = x->subject_len;
    b.mac = x->mac;
    b.key_id = x->key_id;
    b.version = x->version;
    b.language = x->lang;
    b.language_len = x->lang_len;
    rc = v->blind(v->ctx, e->row, &b);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_entity_find(void * ctx, void * txn,
    const char * tenant, size_t tenant_len, const char * public_id,
    size_t public_id_len, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MTen * t = view_tenant(m, x, tenant, tenant_len);
  uint64_t row = pid_row(t, x, public_id, public_id_len);
  MEnt * e = row == 0 ? NULL : ent_view(t, x, row);
  GDEFSTOR_ModelEntity out;
  if (e == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  lend_entity(e, &out);
  return visitor->entity(visitor->ctx, &out);
}

static GDEFSTOR_Result m_entity_insert(void * ctx, void * txn,
    const GDEFSTOR_ModelEntity * e, uint64_t * out_row) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * n;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  GROW_OR_OOM(m->alloc, x->touched, x->ntouch, x->captouch);
  n = (MEnt *)gcu_allocator_calloc(m->alloc, 1, sizeof *n);
  if (n == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  n->type = str_copy(m->alloc, e->type, e->type_len);
  n->pid = str_copy(m->alloc, e->public_id, e->public_id_len);
  n->creator = str_copy(m->alloc, e->creator, e->creator_len);
  if (n->type == NULL || n->pid == NULL || n->creator == NULL) {
    ent_free(m->alloc, n);
    return GDEFSTOR_ERR_OOM;
  }
  n->type_len = e->type_len;
  n->pid_len = e->public_id_len;
  n->creator_len = e->creator_len;
  n->created = e->created_ms;
  /* Rows increase and are never reused. */
  n->row = x->next_row;
  x->next_row += 1;
  x->touched[x->ntouch] = n;
  x->ntouch += 1;
  *out_row = n->row;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_state_append(void * ctx, void * txn, uint64_t row,
    const GDEFSTOR_ModelState * s) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = NULL;
  MSt * n;
  GDEFSTOR_Result rc;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = ent_touch(m, x, row, &e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  GROW_OR_OOM(m->alloc, e->sts, e->nst, e->capst);
  n = &e->sts[e->nst];
  memset(n, 0, sizeof *n);
  n->actor = str_copy(m->alloc, s->actor, s->actor_len);
  if (n->actor == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  n->actor_len = s->actor_len;
  n->seq = e->nst + 1;
  n->pub = s->published != 0;
  n->del = s->deleted != 0;
  n->when = s->when_ms;
  e->nst += 1;
  e->pub = n->pub;
  e->del = n->del;
  return GDEFSTOR_OK;
}

/* Take an item version out of the current table. */
static void cur_remove(const GDEFSMEMORY_Allocator * a, MEnt * e,
    uint32_t group, uint64_t item, const char * lang, size_t lang_len) {
  size_t i;
  for (i = 0; i < e->ncur; i++) {
    if (e->cur[i].group == group && e->cur[i].item == item &&
        same(e->cur[i].lang, e->cur[i].lang_len, lang, lang_len)) {
      ver_free(a, &e->cur[i]);
      memmove(&e->cur[i], &e->cur[i + 1], (e->ncur - i - 1) * sizeof *e->cur);
      e->ncur -= 1;
      return;
    }
  }
}

static GDEFSTOR_Result m_apply(void * ctx, void * txn,
    const GDEFSTOR_ModelApply * a) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = NULL;
  uint64_t rev;
  size_t i;
  MRev * r;
  GDEFSTOR_Result rc;
  if (x == NULL || a == NULL || a->revision == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = ent_touch(m, x, a->row, &e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  rev = a->revision->revision;
  /* The copy is this transaction's: an error part way leaves only a copy
   * that the port's abort throws away. */
  for (i = 0; i < a->close_count; i++) {
    const GDEFSTOR_ModelClose * c = &a->closes[i];
    size_t k;
    int found = 0;
    for (k = 0; k < e->nver; k++) {
      MVer * v = &e->vers[k];
      if (v->group == c->group && v->item == c->item &&
          v->from == c->valid_from &&
          same(v->lang, v->lang_len, c->language, c->language_len)) {
        /* valid_to is written once. */
        if (v->to != 0) {
          return GDEFSTOR_ERR_INTERNAL;
        }
        v->to = rev;
        found = 1;
      }
    }
    if (!found) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    cur_remove(m->alloc, e, c->group, c->item, c->language, c->language_len);
  }
  for (i = 0; i < a->insert_count; i++) {
    const GDEFSTOR_ModelItem * in = &a->inserts[i];
    MVer n;
    memset(&n, 0, sizeof n);
    n.group = in->group;
    n.item = in->item;
    n.parent = in->parent;
    n.order = in->order;
    n.from = rev;
    n.to = 0;
    n.vals = (GDEFSTOR_Value *)in->values;
    n.nvals = in->value_count;
    if (in->language_len == 0 || in->language_len > sizeof n.lang) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    memcpy(n.lang, in->language, in->language_len);
    n.lang_len = in->language_len;
    GROW_OR_OOM(m->alloc, e->vers, e->nver, e->capver);
    GROW_OR_OOM(m->alloc, e->cur, e->ncur, e->capcur);
    rc = ver_set(m->alloc, &e->vers[e->nver], &n);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    e->nver += 1;
    rc = ver_set(m->alloc, &e->cur[e->ncur], &n);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    e->ncur += 1;
  }
  for (i = 0; i < a->blind_count; i++) {
    const GDEFSTOR_ModelBlind * b = &a->blinds[i];
    MBlind * n;
    size_t k;
    int indexed = 0;
    /* A blind row indexes a version this same apply writes. */
    for (k = 0; k < a->insert_count; k++) {
      indexed |= a->inserts[k].item == b->item;
    }
    if (!indexed || b->valid_from != rev || b->mac == NULL ||
        (b->subject == NULL && b->subject_len != 0) ||
        b->language_len == 0 || b->language_len > sizeof n->lang) {
      return GDEFSTOR_ERR_INTERNAL;
    }
    GROW_OR_OOM(m->alloc, e->blinds, e->nblind, e->capblind);
    n = &e->blinds[e->nblind];
    memset(n, 0, sizeof *n);
    n->subject = (uint8_t *)str_copy(m->alloc, (const char *)b->subject,
        b->subject_len);
    if (n->subject == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    n->item = b->item;
    n->from = b->valid_from;
    n->field = b->field;
    n->subject_len = b->subject_len;
    memcpy(n->mac, b->mac, GDEFSTOR_BLIND_MAC_BYTES);
    n->key_id = b->key_id;
    n->version = b->version;
    memcpy(n->lang, b->language, b->language_len);
    n->lang_len = b->language_len;
    e->nblind += 1;
  }
  GROW_OR_OOM(m->alloc, e->revs, e->nrev, e->caprev);
  if (x->seq == 0) {
    x->seq = (x->ten->commit_seq > x->restored ? x->ten->commit_seq
                                               : x->restored) +
             1;
  }
  r = &e->revs[e->nrev];
  memset(r, 0, sizeof *r);
  r->who = str_copy(m->alloc, a->revision->who, a->revision->who_len);
  r->why = str_copy(m->alloc, a->revision->why, a->revision->why_len);
  r->langs = str_copy(m->alloc, a->revision->languages,
      a->revision->languages_len);
  if (r->who == NULL || r->why == NULL || r->langs == NULL) {
    gcu_allocator_free(m->alloc, r->who);
    gcu_allocator_free(m->alloc, r->why);
    gcu_allocator_free(m->alloc, r->langs);
    return GDEFSTOR_ERR_OOM;
  }
  r->langs_len = a->revision->languages_len;
  r->rev = rev;
  r->seq = x->seq;
  r->who_len = a->revision->who_len;
  r->why_len = a->revision->why_len;
  r->when = a->revision->when_ms;
  e->nrev += 1;
  e->revision = rev;
  e->last_item = a->last_item;
  return GDEFSTOR_OK;
}

/* An import's revision (model_backend.h): an apply whose revision row keeps
 * the commit sequence it is given; the tenant's is raised to it at commit. */
static GDEFSTOR_Result m_restore_revision(void * ctx, void * txn,
    const GDEFSTOR_ModelApply * a) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = NULL;
  uint64_t seq;
  GDEFSTOR_Result rc;
  if (x == NULL || a == NULL || a->revision == NULL ||
      a->revision->commit_seq == 0) {
    return GDEFSTOR_ERR_INVALID;
  }
  seq = x->seq;
  rc = m_apply(ctx, txn, a);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  x->seq = seq;
  rc = ent_touch(m, x, a->row, &e);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  e->revs[e->nrev - 1].seq = a->revision->commit_seq;
  if (a->revision->commit_seq > x->restored) {
    x->restored = a->revision->commit_seq;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result visit_history(const MEnt * e,
    const GDEFSTOR_ModelVisitor * v) {
  size_t i;
  for (i = 0; v->revision != NULL && i < e->nrev; i++) {
    GDEFSTOR_ModelRevision r;
    GDEFSTOR_Result rc;
    memset(&r, 0, sizeof r);
    r.revision = e->revs[i].rev;
    r.commit_seq = e->revs[i].seq;
    r.who = e->revs[i].who;
    r.who_len = e->revs[i].who_len;
    r.when_ms = e->revs[i].when;
    r.why = e->revs[i].why;
    r.why_len = e->revs[i].why_len;
    r.languages = e->revs[i].langs;
    r.languages_len = e->revs[i].langs_len;
    rc = v->revision(v->ctx, e->row, &r);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (i = 0; v->state != NULL && i < e->nst; i++) {
    GDEFSTOR_ModelState s;
    GDEFSTOR_Result rc;
    memset(&s, 0, sizeof s);
    s.seq = e->sts[i].seq;
    s.published = e->sts[i].pub;
    s.deleted = e->sts[i].del;
    s.actor = e->sts[i].actor;
    s.actor_len = e->sts[i].actor_len;
    s.when_ms = e->sts[i].when;
    rc = v->state(v->ctx, e->row, &s);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_history(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, uint64_t row, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = ent_view(view_tenant(m, x, tenant, tenant_len), x, row);
  if (e == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  return visit_history(e, visitor);
}

static GDEFSTOR_Result m_items(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, uint64_t row, uint64_t as_of, const char * lang,
    size_t lang_len, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = ent_view(view_tenant(m, x, tenant, tenant_len), x, row);
  size_t i;
  if (e == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (as_of == 0) {
    for (i = 0; i < e->ncur; i++) {
      GDEFSTOR_Result rc;
      if (!in_lang(e->cur[i].lang, e->cur[i].lang_len, lang, lang_len)) {
        continue;
      }
      rc = visit_ver(visitor, row, &e->cur[i]);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return visit_blinds(visitor, e, lang, lang_len);
  }
  for (i = 0; i < e->nver; i++) {
    const MVer * v = &e->vers[i];
    if (v->from <= as_of && (v->to == 0 || v->to > as_of) &&
        in_lang(v->lang, v->lang_len, lang, lang_len)) {
      GDEFSTOR_Result rc = visit_ver(visitor, row, v);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return visit_blinds(visitor, e, lang, lang_len);
}

/* The languages of the entity's trees: one per root item (group 0), current
 * or valid at @p as_of. */
static GDEFSTOR_Result m_languages(void * ctx, void * txn, const char * tenant,
    size_t tenant_len, uint64_t row, uint64_t as_of,
    const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MEnt * e = ent_view(view_tenant(m, x, tenant, tenant_len), x, row);
  size_t i;
  if (e == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (as_of == 0) {
    for (i = 0; i < e->ncur; i++) {
      GDEFSTOR_Result rc;
      if (e->cur[i].group != 0) {
        continue;
      }
      rc = visitor->language(visitor->ctx, row, e->cur[i].lang,
          e->cur[i].lang_len);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    return GDEFSTOR_OK;
  }
  for (i = 0; i < e->nver; i++) {
    const MVer * v = &e->vers[i];
    if (v->group == 0 && v->from <= as_of && (v->to == 0 || v->to > as_of)) {
      GDEFSTOR_Result rc = visitor->language(visitor->ctx, row, v->lang,
          v->lang_len);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

/* ---- queries, rebuild, dump ---------------------------------------------- */

typedef struct ScanCtx {
  MTen * ten;
} ScanCtx;

static GDEFSTOR_Result m_scan(void * ctx, const char * type, size_t type_len,
    const GDEFSTOR_ModelVisitor * visitor) {
  MTen * t = ((ScanCtx *)ctx)->ten;
  size_t r;
  if (t == NULL) {
    return GDEFSTOR_OK;
  }
  for (r = 0; r < t->nent; r++) {
    const MEnt * e = t->ents[r];
    GDEFSTOR_ModelEntity out;
    GDEFSTOR_Result rc;
    size_t i;
    if (!same(e->type, e->type_len, type, type_len)) {
      continue;
    }
    lend_entity(e, &out);
    rc = visitor->entity(visitor->ctx, &out);
    for (i = 0; rc == GDEFSTOR_OK && i < e->ncur; i++) {
      rc = visit_ver(visitor, e->row, &e->cur[i]);
    }
    if (rc == GDEFSTOR_OK) {
      rc = visit_blinds(visitor, e, NULL, 0);
    }
    if (rc == GDEFSTOR_OK && visitor->revision != NULL && e->nrev > 0) {
      /* The newest revision, for a query that reads who wrote it. */
      const MRev * nr = &e->revs[e->nrev - 1];
      GDEFSTOR_ModelRevision rv;
      memset(&rv, 0, sizeof rv);
      rv.revision = nr->rev;
      rv.commit_seq = nr->seq;
      rv.who = nr->who;
      rv.who_len = nr->who_len;
      rv.when_ms = nr->when;
      rv.why = nr->why;
      rv.why_len = nr->why_len;
      rv.languages = nr->langs;
      rv.languages_len = nr->langs_len;
      rc = visitor->revision(visitor->ctx, e->row, &rv);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

/* No engine of its own: the port's reference scan, over the current tables
 * of the last commit. */
static GDEFSTOR_Result m_query(void * ctx, const char * tenant,
    size_t tenant_len, const GDEFSTOR_ModelPlan * plan,
    GDEFSTOR_ModelHits * out) {
  MModel * m = (MModel *)ctx;
  ScanCtx s;
  s.ten = ten_find(m, tenant, tenant_len);
  return gdefstor_model_scan(plan, m_scan, &s, out);
}

static GDEFSTOR_Result m_rebuild(void * ctx, void * txn) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  uint64_t row;
  size_t i;
  if (x == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  /* Every row, through the transaction, from its truth alone. */
  for (row = 1; row <= x->ten->nent; row++) {
    MEnt * e = NULL;
    GDEFSTOR_Result rc = ent_touch(m, x, row, &e);
    if (rc == GDEFSTOR_OK) {
      rc = ent_derive(m->alloc, e);
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  for (i = 0; i < x->ntouch; i++) {
    GDEFSTOR_Result rc = ent_derive(m->alloc, x->touched[i]);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  /* The public-id lookup covers committed rows only, and is derived from
   * them; rows this transaction made are found through it until commit. */
  return slots_build(m->alloc, x->ten);
}

static GDEFSTOR_Result sys_dump(const MTen * t,
    const GDEFSTOR_ModelVisitor * v);

static GDEFSTOR_Result m_dump(void * ctx, const char * tenant,
    size_t tenant_len, int which, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  size_t i;
  if (t == NULL) {
    return GDEFSTOR_OK;
  }
  if (which == GDEFSTOR_MODEL_TRUTH) {
    for (i = 0; i < t->ntype; i++) {
      GDEFSTOR_Result rc = visitor->type(visitor->ctx, t->types[i].name,
          t->types[i].name_len, t->types[i].text, t->types[i].text_len);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    for (i = 0; i < t->ndraft; i++) {
      GDEFSTOR_ModelDraft d;
      GDEFSTOR_Result rc;
      lend_draft(&t->drafts[i], &d);
      rc = visitor->draft(visitor->ctx, &d);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    {
      GDEFSTOR_Result rc = sys_dump(t, visitor);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  for (i = 0; i < t->nent; i++) {
    const MEnt * e = t->ents[i];
    GDEFSTOR_ModelEntity out;
    GDEFSTOR_Result rc;
    size_t k;
    lend_entity(e, &out);
    rc = visitor->entity(visitor->ctx, &out);
    if (rc == GDEFSTOR_OK && which == GDEFSTOR_MODEL_TRUTH) {
      rc = visit_history(e, visitor);
      for (k = 0; rc == GDEFSTOR_OK && k < e->nver; k++) {
        rc = visit_ver(visitor, e->row, &e->vers[k]);
      }
      if (rc == GDEFSTOR_OK) {
        rc = visit_blinds(visitor, e, NULL, 0);
      }
    } else {
      for (k = 0; rc == GDEFSTOR_OK && k < e->ncur; k++) {
        rc = visit_ver(visitor, e->row, &e->cur[k]);
      }
    }
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
  }
  return GDEFSTOR_OK;
}

static void m_destroy(void * ctx) {
  MModel * m = (MModel *)ctx;
  if (m == NULL) {
    return;
  }
  while (m->open != NULL) {
    MTxn * x = m->open;
    m->open = x->next;
    txn_free(m->alloc, x);
  }
  while (m->tenants != NULL) {
    MTen * t = m->tenants;
    m->tenants = t->next;
    ten_free(m->alloc, t);
  }
  gcu_allocator_free(m->alloc, m);
}

/* An indexed field's index: the memory backend keeps none apart from its
 * rows (every query is the reference scan), so there is never one to build
 * and every indexed field is ready the moment its declaration commits. */
static GDEFSTOR_Result m_index_state(void * ctx, const char * tenant,
    size_t tenant_len, const GDEFSTOR_Type * type, size_t field,
    int * out_state) {
  (void)tenant;
  (void)tenant_len;
  if (ctx == NULL || type == NULL || out_state == NULL ||
      field >= gdefstor_type_field_count(type)) {
    return GDEFSTOR_ERR_INVALID;
  }
  *out_state = GDEFSTOR_INDEX_READY;
  return GDEFSTOR_OK;
}

/* ---- the blind index ---------------------------------------------------- */

/* The valid_to of version (item, from) of @p e: 0 open, UINT64_MAX none. */
static uint64_t version_to(const MEnt * e, uint64_t item, uint64_t from) {
  size_t i;
  for (i = 0; i < e->nver; i++) {
    if (e->vers[i].item == item && e->vers[i].from == from) {
      return e->vers[i].to;
    }
  }
  return UINT64_MAX;
}

static GDEFSTOR_Result m_blind_history(void * ctx, const char * tenant,
    size_t tenant_len, const GDEFSTOR_Type * type, size_t field,
    const uint8_t * mac, GDEFSTOR_ModelBlindFn fn, void * fn_ctx) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  GDEFSTOR_FieldInfo f;
  const char * name = NULL;
  size_t name_len = 0;
  size_t r;
  memset(&f, 0, sizeof f);
  f.size = sizeof f;
  if (type == NULL || mac == NULL || fn == NULL ||
      gdefstor_type_name(type, &name, &name_len) != GDEFSTOR_OK ||
      gdefstor_type_field(type, field, &f) != GDEFSTOR_OK) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (r = 0; t != NULL && r < t->nent; r++) {
    const MEnt * e = t->ents[r];
    size_t i;
    if (!same(e->type, e->type_len, name, name_len)) {
      continue;
    }
    for (i = 0; i < e->nblind; i++) {
      const MBlind * b = &e->blinds[i];
      GDEFSTOR_ModelBlindHit h;
      GDEFSTOR_Result rc;
      if (b->field != f.id ||
          memcmp(b->mac, mac, GDEFSTOR_BLIND_MAC_BYTES) != 0) {
        continue;
      }
      memset(&h, 0, sizeof h);
      h.valid_to = version_to(e, b->item, b->from);
      if (h.valid_to == UINT64_MAX) {
        return GDEFSTOR_ERR_CORRUPT; /* a blind row with no version */
      }
      h.row = e->row;
      h.public_id = e->pid;
      h.public_id_len = e->pid_len;
      h.item = b->item;
      h.valid_from = b->from;
      h.subject = b->subject;
      h.subject_len = b->subject_len;
      h.key_id = b->key_id;
      h.version = b->version;
      h.language = b->lang;
      h.language_len = b->lang_len;
      rc = fn(fn_ctx, &h);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
  }
  return GDEFSTOR_OK;
}

/* Every version's subject columns. The memory backend has no index and no
 * blind table to confuse with them: a value carries its subject exactly when
 * it is personal. */
static GDEFSTOR_Result m_subject_lookup(void * ctx, const char * tenant,
    size_t tenant_len, const uint8_t * subject, size_t subject_len,
    GDEFSTOR_ModelSubjectFn fn, void * fn_ctx) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  size_t r;
  if (fn == NULL || (subject == NULL && subject_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (r = 0; t != NULL && r < t->nent; r++) {
    const MEnt * e = t->ents[r];
    size_t i;
    for (i = 0; i < e->nver; i++) {
      const MVer * v = &e->vers[i];
      size_t k;
      for (k = 0; k < v->nvals; k++) {
        GDEFSTOR_ModelSubjectHit h;
        GDEFSTOR_Result rc;
        if (v->vals[k].type == 0 ||
            !same((const char *)v->vals[k].subject, v->vals[k].subject_len,
                (const char *)subject, subject_len)) {
          continue;
        }
        memset(&h, 0, sizeof h);
        h.type = e->type;
        h.type_len = e->type_len;
        h.row = e->row;
        h.public_id = e->pid;
        h.public_id_len = e->pid_len;
        h.group = v->group;
        h.column = (uint32_t)k;
        h.item = v->item;
        h.valid_from = v->from;
        h.valid_to = v->to;
        h.language = v->lang;
        h.language_len = v->lang_len;
        rc = fn(fn_ctx, &h);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
      }
    }
  }
  return GDEFSTOR_OK;
}

static int names_subject(const MEnt * e, const uint8_t * subject,
    size_t subject_len) {
  size_t i;
  for (i = 0; i < e->nblind; i++) {
    if (same((const char *)e->blinds[i].subject, e->blinds[i].subject_len,
            (const char *)subject, subject_len)) {
      return 1;
    }
  }
  return 0;
}

/* Every entity of the tenant, committed or made in this transaction, whose
 * blind rows name the subject: its own copy, with those rows gone. */
static GDEFSTOR_Result m_blind_delete(void * ctx, void * txn,
    const uint8_t * subject, size_t subject_len) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  uint64_t row;
  if (x == NULL || (subject == NULL && subject_len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (row = 1; row < x->next_row; row++) {
    MEnt * e = ent_view(x->ten, x, row);
    size_t i;
    size_t kept = 0;
    GDEFSTOR_Result rc;
    if (e == NULL || !names_subject(e, subject, subject_len)) {
      continue;
    }
    if (row <= x->ten->nent) {
      rc = ent_touch(m, x, row, &e);
      if (rc != GDEFSTOR_OK) {
        return rc;
      }
    }
    for (i = 0; i < e->nblind; i++) {
      if (same((const char *)e->blinds[i].subject, e->blinds[i].subject_len,
              (const char *)subject, subject_len)) {
        gcu_allocator_free(m->alloc, e->blinds[i].subject);
      } else {
        e->blinds[kept++] = e->blinds[i];
      }
    }
    e->nblind = kept;
  }
  return GDEFSTOR_OK;
}

/* ---- records, counters and the outbox ------------------------------------ */

/* Byte order, a prefix first. */
static int span_cmp(const char * a, size_t an, const char * b, size_t bn) {
  size_t n = an < bn ? an : bn;
  int c = n == 0 ? 0 : memcmp(a, b, n);
  if (c != 0) {
    return c;
  }
  return an < bn ? -1 : (an > bn ? 1 : 0);
}

/* The index of the first committed record of @p t not below (ns, key), and
 * whether it is that record. */
static size_t rec_lower(const MTen * t, const char * ns, size_t ns_len,
    const char * key, size_t key_len, int * out_found) {
  size_t lo = 0;
  size_t hi = t == NULL ? 0 : t->nrec;
  *out_found = 0;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const MRec * r = &t->recs[mid];
    int c = span_cmp(r->ns, r->ns_len, ns, ns_len);
    if (c == 0) {
      c = span_cmp(r->key, r->key_len, key, key_len);
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (t != NULL && lo < t->nrec &&
      same(t->recs[lo].ns, t->recs[lo].ns_len, ns, ns_len) &&
      same(t->recs[lo].key, t->recs[lo].key_len, key, key_len)) {
    *out_found = 1;
  }
  return lo;
}

/* The transaction's put or delete of (ns, key), or NULL. */
static MRec * rput_find(MTxn * x, const char * ns, size_t ns_len,
    const char * key, size_t key_len) {
  size_t i;
  for (i = 0; i < x->nrput; i++) {
    if (same(x->rput[i].ns, x->rput[i].ns_len, ns, ns_len) &&
        same(x->rput[i].key, x->rput[i].key_len, key, key_len)) {
      return &x->rput[i];
    }
  }
  return NULL;
}

/* A new entry of the transaction's records for (ns, key), taking @p bytes. */
static GDEFSTOR_Result rput_add(MModel * m, MTxn * x, const char * ns,
    size_t ns_len, const char * key, size_t key_len, uint8_t * bytes,
    size_t len, int dead) {
  MRec n;
  void * more = grown(m->alloc, x->rput, x->nrput, &x->caprput,
      sizeof *x->rput);
  memset(&n, 0, sizeof n);
  if (more != NULL) {
    x->rput = more;
    n.ns = str_copy(m->alloc, ns, ns_len);
    n.key = str_copy(m->alloc, key, key_len);
  }
  if (more == NULL || n.ns == NULL || n.key == NULL) {
    gcu_allocator_free(m->alloc, n.ns);
    gcu_allocator_free(m->alloc, n.key);
    gcu_allocator_free(m->alloc, bytes);
    return GDEFSTOR_ERR_OOM;
  }
  n.ns_len = ns_len;
  n.key_len = key_len;
  n.bytes = bytes;
  n.len = len;
  n.dead = dead;
  x->rput[x->nrput] = n;
  x->nrput += 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_record_put(void * ctx, void * txn,
    const GDEFSTOR_ModelRecord * r) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  uint8_t * bytes = NULL;
  MRec * o;
  if (x == NULL || r == NULL || (r->bytes == NULL && r->len != 0)) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (r->len > 0) {
    bytes = (uint8_t *)gcu_allocator_malloc(m->alloc, r->len);
    if (bytes == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    memcpy(bytes, r->bytes, r->len);
  }
  /* Kept in the transaction until it commits (sys_apply). */
  o = rput_find(x, r->ns, r->ns_len, r->key, r->key_len);
  if (o != NULL) {
    gcu_allocator_free(m->alloc, o->bytes);
    o->bytes = bytes;
    o->len = r->len;
    o->dead = 0;
    return GDEFSTOR_OK;
  }
  return rput_add(m, x, r->ns, r->ns_len, r->key, r->key_len, bytes, r->len,
      0);
}

static GDEFSTOR_Result m_record_delete(void * ctx, void * txn, const char * ns,
    size_t ns_len, const char * key, size_t key_len, int * out_deleted) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MRec * o;
  int found = 0;
  GDEFSTOR_Result rc;
  if (x == NULL || out_deleted == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  o = rput_find(x, ns, ns_len, key, key_len);
  if (o != NULL) {
    *out_deleted = !o->dead;
    gcu_allocator_free(m->alloc, o->bytes);
    o->bytes = NULL;
    o->len = 0;
    o->dead = 1;
    return GDEFSTOR_OK;
  }
  rec_lower(x->ten, ns, ns_len, key, key_len, &found);
  if (!found) {
    *out_deleted = 0;
    return GDEFSTOR_OK;
  }
  rc = rput_add(m, x, ns, ns_len, key, key_len, NULL, 0, 1);
  if (rc == GDEFSTOR_OK) {
    *out_deleted = 1;
  }
  return rc;
}

static GDEFSTOR_Result visit_record(const GDEFSTOR_ModelVisitor * v,
    const MRec * r) {
  GDEFSTOR_ModelRecord out;
  memset(&out, 0, sizeof out);
  out.ns = r->ns;
  out.ns_len = r->ns_len;
  out.key = r->key;
  out.key_len = r->key_len;
  out.bytes = r->bytes;
  out.len = r->len;
  return v->record(v->ctx, &out);
}

static GDEFSTOR_Result m_record_get(void * ctx, void * txn,
    const char * tenant, size_t tenant_len, const char * ns, size_t ns_len,
    const char * key, size_t key_len, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MTen * t = view_tenant(m, x, tenant, tenant_len);
  int found = 0;
  size_t at;
  if (visitor == NULL || visitor->record == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (x != NULL) {
    const MRec * o = rput_find(x, ns, ns_len, key, key_len);
    if (o != NULL) {
      return o->dead ? GDEFSTOR_OK : visit_record(visitor, o);
    }
  }
  at = rec_lower(t, ns, ns_len, key, key_len, &found);
  return found ? visit_record(visitor, &t->recs[at]) : GDEFSTOR_OK;
}

static GDEFSTOR_Result m_record_list(void * ctx, const char * tenant,
    size_t tenant_len, const char * ns, size_t ns_len, const char * prefix,
    size_t prefix_len, uint64_t offset, size_t limit,
    const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  uint64_t skipped = 0;
  size_t given = 0;
  int found = 0;
  size_t i;
  if (visitor == NULL || visitor->record == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  if (t == NULL) {
    return GDEFSTOR_OK;
  }
  for (i = rec_lower(t, ns, ns_len, prefix, prefix_len, &found);
       i < t->nrec && given < limit; i++) {
    const MRec * r = &t->recs[i];
    GDEFSTOR_Result rc;
    if (!same(r->ns, r->ns_len, ns, ns_len) || r->key_len < prefix_len ||
        (prefix_len > 0 && memcmp(r->key, prefix, prefix_len) != 0)) {
      break; /* sorted: past the namespace or the prefix */
    }
    if (skipped < offset) {
      skipped++;
      continue;
    }
    rc = visit_record(visitor, r);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    given++;
  }
  return GDEFSTOR_OK;
}

static MCnt * cnt_find(MCnt * v, size_t n, int kind, const char * name,
    size_t name_len) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (v[i].kind == kind && same(v[i].name, v[i].name_len, name, name_len)) {
      return &v[i];
    }
  }
  return NULL;
}

static GDEFSTOR_Result m_counter_next(void * ctx, void * txn, int kind,
    const char * name, size_t name_len, uint64_t * out_value) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MTen * t;
  MCnt * ov;
  MCnt * cm;
  MCnt * fl = NULL;
  uint64_t cur;
  uint64_t flv;
  uint64_t base;
  char * ov_name = NULL;
  char * fl_name = NULL;
  if (x == NULL || out_value == NULL ||
      (kind != GDEFSTOR_MODEL_COUNTER && kind != GDEFSTOR_MODEL_STREAM)) {
    return GDEFSTOR_ERR_INVALID;
  }
  t = x->ten;
  ov = cnt_find(x->cset, x->ncset, kind, name, name_len);
  cm = cnt_find(t->cnts, t->ncnt, kind, name, name_len);
  if (kind == GDEFSTOR_MODEL_COUNTER) {
    fl = cnt_find(t->floors, t->nfloor, kind, name, name_len);
  }
  cur = ov != NULL ? ov->value : (cm != NULL ? cm->value : 0);
  flv = fl != NULL ? fl->value : 0;
  base = cur > flv ? cur : flv;
  if (base >= (uint64_t)INT64_MAX) {
    return GDEFSTOR_ERR_LIMIT;
  }
  /* Every allocation first, so that a value handed out is recorded in both
   * places or in neither. */
  if (ov == NULL) {
    void * more = grown(m->alloc, x->cset, x->ncset, &x->capcset,
        sizeof *x->cset);
    if (more == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->cset = more;
    ov_name = str_copy(m->alloc, name, name_len);
    if (ov_name == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
  }
  if (kind == GDEFSTOR_MODEL_COUNTER && fl == NULL) {
    void * more = grown(m->alloc, t->floors, t->nfloor, &t->capfloor,
        sizeof *t->floors);
    if (more != NULL) {
      t->floors = more;
      fl_name = str_copy(m->alloc, name, name_len);
    }
    if (fl_name == NULL) {
      gcu_allocator_free(m->alloc, ov_name);
      return GDEFSTOR_ERR_OOM;
    }
  }
  if (ov == NULL) {
    ov = &x->cset[x->ncset];
    x->ncset += 1;
    ov->kind = kind;
    ov->name = ov_name;
    ov->name_len = name_len;
  }
  ov->value = base + 1;
  /* The floor is the committed tenant's, written now, outside the
   * transaction: whatever becomes of it, this value stays taken. */
  if (kind == GDEFSTOR_MODEL_COUNTER) {
    if (fl == NULL) {
      fl = &t->floors[t->nfloor];
      t->nfloor += 1;
      fl->kind = kind;
      fl->name = fl_name;
      fl->name_len = name_len;
    }
    fl->value = base + 1;
  }
  *out_value = base + 1;
  return GDEFSTOR_OK;
}

/* An import's counter: its value in this transaction, as a committed one
 * would be once the transaction commits. Its floor is left as it is. */
static GDEFSTOR_Result m_restore_counter(void * ctx, void * txn, int kind,
    const char * name, size_t name_len, uint64_t value) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MCnt * ov;
  if (x == NULL || name == NULL ||
      (kind != GDEFSTOR_MODEL_COUNTER && kind != GDEFSTOR_MODEL_STREAM)) {
    return GDEFSTOR_ERR_INVALID;
  }
  ov = cnt_find(x->cset, x->ncset, kind, name, name_len);
  if (ov == NULL) {
    char * n;
    void * more = grown(m->alloc, x->cset, x->ncset, &x->capcset,
        sizeof *x->cset);
    if (more == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    x->cset = more;
    n = str_copy(m->alloc, name, name_len);
    if (n == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    ov = &x->cset[x->ncset];
    x->ncset += 1;
    ov->kind = kind;
    ov->name = n;
    ov->name_len = name_len;
  }
  ov->value = value;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_counter_get(void * ctx, const char * tenant,
    size_t tenant_len, int kind, const char * name, size_t name_len,
    uint64_t * out_value) {
  MModel * m = (MModel *)ctx;
  MTen * t = ten_find(m, tenant, tenant_len);
  const MCnt * c;
  if (out_value == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  c = t == NULL ? NULL : cnt_find(t->cnts, t->ncnt, kind, name, name_len);
  *out_value = c == NULL ? 0 : c->value;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result ob_copy(const GDEFSMEMORY_Allocator * a, MOb * dst,
    const MOb * src) {
  MOb n = *src;
  n.stream = str_copy(a, src->stream, src->stream_len);
  n.pid = str_copy(a, src->pid, src->pid_len);
  n.fmt = str_copy(a, src->fmt, src->fmt_len);
  n.payload = NULL;
  if (src->payload_len > 0) {
    n.payload = (uint8_t *)gcu_allocator_malloc(a, src->payload_len);
    if (n.payload != NULL) {
      memcpy(n.payload, src->payload, src->payload_len);
    }
  }
  if (n.stream == NULL || n.pid == NULL || n.fmt == NULL ||
      (src->payload_len > 0 && n.payload == NULL)) {
    ob_free(a, &n);
    return GDEFSTOR_ERR_OOM;
  }
  *dst = n;
  return GDEFSTOR_OK;
}

/* The transaction's own copy of the outbox, made on first touch. */
static GDEFSTOR_Result obs_touch(MModel * m, MTxn * x) {
  const MTen * t = x->ten;
  size_t i;
  if (x->obs_copied) {
    return GDEFSTOR_OK;
  }
  if (t->nob > 0) {
    x->obs = (MOb *)gcu_allocator_calloc(m->alloc, t->nob, sizeof *x->obs);
    if (x->obs == NULL) {
      return GDEFSTOR_ERR_OOM;
    }
    for (i = 0; i < t->nob; i++) {
      if (ob_copy(m->alloc, &x->obs[i], &t->obs[i]) != GDEFSTOR_OK) {
        obs_free(m->alloc, x->obs, i);
        x->obs = NULL;
        return GDEFSTOR_ERR_OOM;
      }
    }
  }
  x->nob = t->nob;
  x->capob = t->nob;
  x->obs_copied = 1;
  return GDEFSTOR_OK;
}

static size_t ob_find(const MOb * v, size_t n, const char * pid,
    size_t pid_len) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (same(v[i].pid, v[i].pid_len, pid, pid_len)) {
      return i;
    }
  }
  return n;
}

static GDEFSTOR_Result m_outbox_insert(void * ctx, void * txn,
    const GDEFSTOR_ModelOutbox * o) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  MOb src;
  MOb n;
  size_t at;
  void * more;
  GDEFSTOR_Result rc;
  if (x == NULL || o == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = obs_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  if (ob_find(x->obs, x->nob, o->public_id, o->public_id_len) < x->nob) {
    return GDEFSTOR_ERR_INVALID;
  }
  memset(&src, 0, sizeof src);
  src.stream = (char *)o->stream;
  src.stream_len = o->stream_len;
  src.seq = o->seq;
  src.pid = (char *)o->public_id;
  src.pid_len = o->public_id_len;
  src.fmt = (char *)o->format;
  src.fmt_len = o->format_len;
  src.payload = (uint8_t *)o->payload;
  src.payload_len = o->payload_len;
  more = grown(m->alloc, x->obs, x->nob, &x->capob, sizeof *x->obs);
  if (more == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  x->obs = more;
  rc = ob_copy(m->alloc, &n, &src);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* In order: by stream, then sequence. */
  for (at = x->nob; at > 0; at--) {
    const MOb * p = &x->obs[at - 1];
    int c = span_cmp(p->stream, p->stream_len, n.stream, n.stream_len);
    if (c < 0 || (c == 0 && p->seq < n.seq)) {
      break;
    }
  }
  memmove(&x->obs[at + 1], &x->obs[at], (x->nob - at) * sizeof *x->obs);
  x->obs[at] = n;
  x->nob += 1;
  return GDEFSTOR_OK;
}

/* An import's outbox row: inserted as an append's is, then given the lease,
 * failures and parked flag it had. */
static GDEFSTOR_Result m_restore_outbox(void * ctx, void * txn,
    const GDEFSTOR_ModelOutbox * o) {
  MTxn * x = (MTxn *)txn;
  size_t at;
  GDEFSTOR_Result rc = m_outbox_insert(ctx, txn, o);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  at = ob_find(x->obs, x->nob, o->public_id, o->public_id_len);
  if (at == x->nob) {
    return GDEFSTOR_ERR_INTERNAL;
  }
  x->obs[at].until = o->lease_until_ms;
  x->obs[at].fails = o->failures;
  x->obs[at].parked = o->parked != 0;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result visit_ob(const GDEFSTOR_ModelVisitor * v,
    const MOb * o) {
  GDEFSTOR_ModelOutbox out;
  memset(&out, 0, sizeof out);
  out.stream = o->stream;
  out.stream_len = o->stream_len;
  out.seq = o->seq;
  out.public_id = o->pid;
  out.public_id_len = o->pid_len;
  out.format = o->fmt;
  out.format_len = o->fmt_len;
  out.payload = o->payload;
  out.payload_len = o->payload_len;
  out.lease_until_ms = o->until;
  out.failures = o->fails;
  out.parked = o->parked;
  return v->outbox(v->ctx, &out);
}

static GDEFSTOR_Result m_outbox_claim(void * ctx, void * txn,
    const char * stream, size_t stream_len, int64_t now_ms, int64_t until_ms,
    size_t max, const GDEFSTOR_ModelVisitor * visitor) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  size_t taken = 0;
  size_t i;
  GDEFSTOR_Result rc;
  if (x == NULL || visitor == NULL || visitor->outbox == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = obs_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  /* One live claim per stream: while a lease holds, nothing is taken. */
  for (i = 0; i < x->nob; i++) {
    const MOb * o = &x->obs[i];
    if (same(o->stream, o->stream_len, stream, stream_len) && !o->parked &&
        o->until > now_ms) {
      return GDEFSTOR_OK;
    }
  }
  for (i = 0; i < x->nob && taken < max; i++) {
    MOb * o = &x->obs[i];
    if (!same(o->stream, o->stream_len, stream, stream_len) || o->parked) {
      continue;
    }
    o->until = until_ms;
    rc = visit_ob(visitor, o);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    taken++;
  }
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_outbox_delete(void * ctx, void * txn,
    const char * public_id, size_t public_id_len, int * out_deleted) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  size_t at;
  GDEFSTOR_Result rc;
  if (x == NULL || out_deleted == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = obs_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  *out_deleted = 0;
  at = ob_find(x->obs, x->nob, public_id, public_id_len);
  if (at == x->nob || x->obs[at].parked) {
    return GDEFSTOR_OK; /* gone already, or parked: never deleted */
  }
  ob_free(m->alloc, &x->obs[at]);
  memmove(&x->obs[at], &x->obs[at + 1], (x->nob - at - 1) * sizeof *x->obs);
  x->nob -= 1;
  *out_deleted = 1;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_outbox_fail(void * ctx, void * txn,
    const char * public_id, size_t public_id_len, int64_t now_ms,
    uint32_t park_at, int * out_found, int * out_parked) {
  MModel * m = (MModel *)ctx;
  MTxn * x = (MTxn *)txn;
  size_t at;
  MOb * o;
  GDEFSTOR_Result rc;
  if (x == NULL || out_found == NULL || out_parked == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  rc = obs_touch(m, x);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  *out_parked = 0;
  at = ob_find(x->obs, x->nob, public_id, public_id_len);
  *out_found = at < x->nob;
  if (at == x->nob) {
    return GDEFSTOR_OK;
  }
  o = &x->obs[at];
  if (!o->parked) {
    o->fails += 1;
    if (o->fails >= park_at) {
      /* Parked: kept, counted, never claimed again. */
      o->parked = 1;
      o->until = 0;
    } else {
      o->until = now_ms;
    }
  }
  *out_parked = o->parked;
  return GDEFSTOR_OK;
}

static GDEFSTOR_Result m_outbox_parked(void * ctx, const char * tenant,
    size_t tenant_len, const char * stream, size_t stream_len,
    uint64_t * out_count) {
  MModel * m = (MModel *)ctx;
  const MTen * t = ten_find(m, tenant, tenant_len);
  uint64_t n = 0;
  size_t i;
  if (out_count == NULL) {
    return GDEFSTOR_ERR_INVALID;
  }
  for (i = 0; t != NULL && i < t->nob; i++) {
    const MOb * o = &t->obs[i];
    n += o->parked &&
         (stream_len == 0 || same(o->stream, o->stream_len, stream, stream_len));
  }
  *out_count = n;
  return GDEFSTOR_OK;
}

/* Commit's part for these families, after room was made for them: nothing
 * here can fail. The transaction's entries move into the tenant. */
static void sys_apply(const GDEFSMEMORY_Allocator * a, MTen * t, MTxn * x) {
  size_t i;
  for (i = 0; i < x->nrput; i++) {
    MRec * o = &x->rput[i];
    int found = 0;
    size_t at = rec_lower(t, o->ns, o->ns_len, o->key, o->key_len, &found);
    if (o->dead) {
      if (found) {
        rec_free(a, &t->recs[at]);
        memmove(&t->recs[at], &t->recs[at + 1],
            (t->nrec - at - 1) * sizeof *t->recs);
        t->nrec -= 1;
      }
      rec_free(a, o);
    } else if (found) {
      gcu_allocator_free(a, t->recs[at].bytes);
      t->recs[at].bytes = o->bytes;
      t->recs[at].len = o->len;
      o->bytes = NULL;
      rec_free(a, o);
    } else {
      memmove(&t->recs[at + 1], &t->recs[at], (t->nrec - at) * sizeof *t->recs);
      t->recs[at] = *o;
      t->nrec += 1;
      memset(o, 0, sizeof *o);
    }
  }
  x->nrput = 0;
  for (i = 0; i < x->ncset; i++) {
    MCnt * c = &x->cset[i];
    MCnt * cm = cnt_find(t->cnts, t->ncnt, c->kind, c->name, c->name_len);
    if (cm != NULL) {
      cm->value = c->value;
      gcu_allocator_free(a, c->name);
    } else {
      t->cnts[t->ncnt] = *c;
      t->ncnt += 1;
    }
    memset(c, 0, sizeof *c);
  }
  x->ncset = 0;
  if (x->obs_copied) {
    obs_free(a, t->obs, t->nob);
    t->obs = x->obs;
    t->nob = x->nob;
    t->capob = x->capob;
    x->obs = NULL;
    x->nob = 0;
    x->obs_copied = 0;
  }
}

static GDEFSTOR_Result sys_dump(const MTen * t,
    const GDEFSTOR_ModelVisitor * v) {
  size_t i;
  GDEFSTOR_Result rc = GDEFSTOR_OK;
  for (i = 0; v->record != NULL && rc == GDEFSTOR_OK && i < t->nrec; i++) {
    rc = visit_record(v, &t->recs[i]);
  }
  for (i = 0; v->counter != NULL && rc == GDEFSTOR_OK && i < t->ncnt; i++) {
    rc = v->counter(v->ctx, t->cnts[i].kind, t->cnts[i].name,
        t->cnts[i].name_len, t->cnts[i].value);
  }
  for (i = 0; v->outbox != NULL && rc == GDEFSTOR_OK && i < t->nob; i++) {
    rc = visit_ob(v, &t->obs[i]);
  }
  return rc;
}

GDEFSTOR_Result gdefsmemory_model_backend(const GDEFSMEMORY_Allocator * alloc,
    GDEFSTOR_ModelBackend * out) {
  MModel * m = (MModel *)gcu_allocator_calloc(alloc, 1, sizeof *m);
  if (m == NULL) {
    return GDEFSTOR_ERR_OOM;
  }
  m->alloc = alloc;
  memset(out, 0, sizeof *out);
  out->size = sizeof *out;
  out->ctx = m;
  out->destroy = m_destroy;
  out->begin = m_begin;
  out->commit = m_commit;
  out->abort = m_abort;
  out->type_load = m_type_load;
  out->type_store = m_type_store;
  out->entity_find = m_entity_find;
  out->entity_insert = m_entity_insert;
  out->state_append = m_state_append;
  out->apply = m_apply;
  out->history = m_history;
  out->items = m_items;
  out->draft_put = m_draft_put;
  out->draft_get = m_draft_get;
  out->draft_delete = m_draft_delete;
  out->query = m_query;
  out->rebuild = m_rebuild;
  out->dump = m_dump;
  out->index_ctx = m;
  out->index_state = m_index_state;
  out->blind_ctx = m;
  out->blind_history = m_blind_history;
  out->subject_lookup = m_subject_lookup;
  out->blind_delete = m_blind_delete;
  out->sys_ctx = m;
  out->record_put = m_record_put;
  out->record_delete = m_record_delete;
  out->record_get = m_record_get;
  out->record_list = m_record_list;
  out->counter_next = m_counter_next;
  out->counter_get = m_counter_get;
  out->outbox_insert = m_outbox_insert;
  out->outbox_claim = m_outbox_claim;
  out->outbox_delete = m_outbox_delete;
  out->outbox_fail = m_outbox_fail;
  out->outbox_parked = m_outbox_parked;
  out->lang_ctx = m;
  out->languages = m_languages;
  out->restore_ctx = m;
  out->restore_revision = m_restore_revision;
  out->restore_counter = m_restore_counter;
  out->restore_outbox = m_restore_outbox;
  return GDEFSTOR_OK;
}
