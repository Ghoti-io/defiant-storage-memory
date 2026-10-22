#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the memory entity backend (src/model.c)
# turns a named test of the port's entity conformance suite red. These are the
# defects a backend can commit on its own, whatever the port computed: history
# rewritten in place, current tables that drift from the truth, and records,
# counters and outbox rows kept outside the rules of story S6a.
#
# Usage: check-model.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-model.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-model.sh <make> <build-dir> [make args]}"
shift 2

# The helpers are defiant-storage's, installed with its conformance suites;
# the Makefile names the file.
PLANT_LIB="${GHOTI_PLANT_LIB:-}"
if [ ! -r "$PLANT_LIB" ]; then
  printf '%s: defiant-storage installs the plant helpers (plant-lib.sh) with its conformance suites and none was found at "%s": install defiant-storage first, then run this through make (GHOTI_PLANT_LIB names the file)\n' \
    "$(basename "$0")" "$PLANT_LIB" >&2
  exit 1
fi
. "$PLANT_LIB"
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testModel 'Model*'

# Closing a version also closes every earlier version of the item again:
# a valid_to written a second time.
plant valid-to-twice src/model.c testModel \
  'ModelHistory.ValidToIsWrittenOnce ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '      if (v->group == c->group && v->item == c->item &&
          v->from == c->valid_from &&
          same(v->lang, v->lang_len, c->language, c->language_len)) {
        /* valid_to is written once. */
        if (v->to != 0) {
          return GDEFSTOR_ERR_INTERNAL;
        }
        v->to = rev;' \
  '      if (v->group == c->group && v->item == c->item) {
        v->to = rev;'

# A revert that "saves space" by reopening the old version it copies instead
# of writing a new one: old rows updated in place.
plant revert-updates-old-rows src/model.c testModel \
  'ModelHistory.RevertAddsRowsAndLeavesHistory ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '    rc = ver_set(m->alloc, &e->vers[e->nver], &n);
    if (rc != GDEFSTOR_OK) {
      return rc;
    }
    e->nver += 1;' \
  '    {
      size_t k2;
      int reopened = 0;
      for (k2 = 0; k2 < e->nver && !reopened; k2++) {
        MVer * o = &e->vers[k2];
        size_t q;
        int eq = o->group == n.group && o->item == n.item && o->to != 0 &&
                 o->parent == n.parent && o->order == n.order &&
                 o->nvals == n.nvals;
        for (q = 0; eq && q < n.nvals; q++) {
          eq = o->vals[q].type == n.vals[q].type &&
               o->vals[q].integer == n.vals[q].integer &&
               o->vals[q].real == n.vals[q].real &&
               o->vals[q].len == n.vals[q].len &&
               (n.vals[q].len == 0 ||
                   memcmp(o->vals[q].bytes, n.vals[q].bytes, n.vals[q].len) == 0);
        }
        if (eq) {
          o->to = 0;
          reopened = 1;
        }
      }
      if (!reopened) {
        rc = ver_set(m->alloc, &e->vers[e->nver], &n);
        if (rc != GDEFSTOR_OK) {
          return rc;
        }
        e->nver += 1;
      }
    }'

# The current table keeps the closed versions of nested items: it no longer
# matches what a rebuild from the truth gives.
plant current-diverges src/model.c testModel \
  'ModelDerived.RebuildGivesTheSameCurrentState ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '    cur_remove(m->alloc, e, c->group, c->item, c->language, c->language_len);' \
  '    if (c->group == 0) {
      cur_remove(m->alloc, e, c->group, c->item, c->language, c->language_len);
    }'

# Records, counters and the outbox (story S6a) --------------------------------

# A record replaced in place in the committed tenant instead of in the
# transaction: written outside the write, it survives the abort.
plant record-outside-transaction src/model.c testModel \
  'ModelRecords.AnAbortedWriteLeavesNoRecord' \
  '  /* Kept in the transaction until it commits (sys_apply). */
  o = rput_find(x, r->ns, r->ns_len, r->key, r->key_len);' \
  '  {
    int here = 0;
    size_t at = rec_lower(x->ten, r->ns, r->ns_len, r->key, r->key_len, &here);
    if (here) {
      gcu_allocator_free(m->alloc, x->ten->recs[at].bytes);
      x->ten->recs[at].bytes = bytes;
      x->ten->recs[at].len = r->len;
      return GDEFSTOR_OK;
    }
  }
  o = rput_find(x, r->ns, r->ns_len, r->key, r->key_len);'

# The floor never read: the next value comes from the committed one alone,
# so the value an aborted write took is handed out again.
plant counter-reuses-aborted-value src/model.c testModel \
  'ModelCounters.AnAbortedWritesValueIsSkippedNotReused' \
  '  flv = fl != NULL ? fl->value : 0;' \
  '  flv = 0;'

# A claim that walks the stream from its newest row: out of order.
plant claim-out-of-order src/model.c testModel \
  'ModelOutbox.ThreeAppendsAreClaimedInSequence' \
  '  for (i = 0; i < x->nob && taken < max; i++) {
    MOb * o = &x->obs[i];' \
  '  for (i = x->nob; i-- > 0 && taken < max;) {
    MOb * o = &x->obs[i];'

# A live lease not looked at: a second claimer takes the stream too.
plant double-claim-under-live-lease src/model.c testModel \
  'ModelOutbox.ASecondClaimerDuringALiveLeaseGetsNothing' \
  '        o->until > now_ms) {
      return GDEFSTOR_OK;' \
  '        o->until > now_ms && 0) {
      return GDEFSTOR_OK;'

# A row that reaches the threshold dropped instead of parked.
plant parked-row-deleted src/model.c testModel \
  'ModelOutbox.AFailingRowIsParkedCountedNeverClaimedAndKept' \
  '      o->parked = 1;
      o->until = 0;' \
  '      ob_free(m->alloc, o);
      memmove(&x->obs[at], &x->obs[at + 1],
          (x->nob - at - 1) * sizeof *x->obs);
      x->nob -= 1;
      *out_parked = 1;
      return GDEFSTOR_OK;'

# Records treated as derived: a rebuild deletes them.
plant rebuild-clears-records src/model.c testModel \
  'ModelSystem.RebuildLeavesRecordsCountersAndTheOutboxAlone' \
  'static GDEFSTOR_Result m_rebuild(void * ctx, void * txn) {' \
  'static GDEFSTOR_Result m_record_delete(void * ctx, void * txn, const char * ns,
    size_t ns_len, const char * key, size_t key_len, int * out_deleted);

static GDEFSTOR_Result m_rebuild(void * ctx, void * txn) {' \
  '  /* Every row, through the transaction, from its truth alone. */' \
  '  for (i = x->ten->nrec; i > 0; i--) {
    int gone = 0;
    GDEFSTOR_Result rc0 = m_record_delete(m, x, x->ten->recs[i - 1].ns,
        x->ten->recs[i - 1].ns_len, x->ten->recs[i - 1].key,
        x->ten->recs[i - 1].key_len, &gone);
    if (rc0 != GDEFSTOR_OK) {
      return rc0;
    }
  }
  /* Every row, through the transaction, from its truth alone. */'

# The scan hands the reference engine the oldest revision, not the newest,
# so who wrote it is who wrote the first.
plant scan-hands-the-oldest-revision src/model.c testModel \
  'ModelQuery.RevisionWhoIsTheNewestRevisionsAuthor' \
  '      const MRev * nr = &e->revs[e->nrev - 1];' \
  '      const MRev * nr = &e->revs[0];'

# A blind row put back beside the row the version already has: a rebuild run
# twice leaves two rows per version (story S6c).
plant blind-put-keeps-the-old-row src/model.c testModel \
  'ModelBlindPut.PuttingTheRowsBackRebuildsTheBlindIndex' \
  '    if (e->blinds[i].item == b->item && e->blinds[i].from == b->valid_from &&
        e->blinds[i].field == b->field) {' \
  '    if (0 && e->blinds[i].item == b->item &&
        e->blinds[i].from == b->valid_from && e->blinds[i].field == b->field) {'

# The stale-object plant. A plant's copy starts from the finished build, so the
# edit has to be compiled into it; this one is made not to be (the object that
# depends on the edit is left looking up to date), and the gate has to refuse it
# as a STALE OBJECT. If it ever passes instead, a warm copy could pass with the
# defect never built in.
plant_stale stale-object src/model.c testModel \
  'ModelQuery.RevisionWhoIsTheNewestRevisionsAuthor' \
  '      const MRev * nr = &e->revs[e->nrev - 1];' \
  '      const MRev * nr = &e->revs[0];'

plant_finish
