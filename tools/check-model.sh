#!/bin/sh
#
# SPDX-License-Identifier: LGPL-3.0-only
#
# Show that each defect planted in the memory entity backend (src/model.c)
# turns a named test of the port's entity conformance suite red. These are the
# defects a backend can commit on its own, whatever the port computed: history
# rewritten in place, and current tables that drift from the truth.
#
# Usage: check-model.sh <make> <build-dir> [make args]

set -u

MAKE="${1:?usage: check-model.sh <make> <build-dir> [make args]}"
BUILD_DIR="${2:?usage: check-model.sh <make> <build-dir> [make args]}"
shift 2

. tools/plant-lib.sh
plant_setup "$MAKE" "$BUILD_DIR" "$@"
plant_control testModel 'Model*'

# Closing a version also closes every earlier version of the item again:
# a valid_to written a second time.
plant valid-to-twice src/model.c testModel \
  'ModelHistory.ValidToIsWrittenOnce ModelSweep.RandomHistoriesMatchAnIndependentModel' \
  '      if (v->group == c->group && v->item == c->item &&
          v->from == c->valid_from) {
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
  '    cur_remove(m->alloc, e, c->group, c->item);' \
  '    if (c->group == 0) {
      cur_remove(m->alloc, e, c->group, c->item);
    }'

plant_finish
