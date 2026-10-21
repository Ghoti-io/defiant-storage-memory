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
 * A tree read from random JSON and saved into an in-memory store's entity
 * port. The first byte says whether the write commits or aborts.
 */

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Type * type = NULL;
  GDEFSTOR_Tree * tree = NULL;
  GDEFSTOR_FieldSpec field;
  GDEFSTOR_TypeSpec spec;
  if (data == NULL || size < 2) {
    return 0;
  }
  if (gdefsmemory_store_create(NULL, NULL, &store) != GDEFSMEMORY_OK) {
    return 0;
  }
  memset(&field, 0, sizeof field);
  field.size = sizeof field;
  field.name = "a";
  field.name_len = 1;
  field.type = GDEFSTOR_FIELD_TEXT;
  field.flags = GDEFSTOR_FIELD_MULTIPLE;
  memset(&spec, 0, sizeof spec);
  spec.size = sizeof spec;
  spec.name = "f.t";
  spec.name_len = 3;
  spec.fields = &field;
  spec.field_count = 1;
  if (gdefstor_write_begin(store, "t", 1, NULL, &write) != GDEFSTOR_OK) {
    gdefstor_store_destroy(store);
    return 0;
  }
  if (gdefstor_type_declare(write, &spec) != GDEFSTOR_OK ||
      gdefstor_write_commit(write) != GDEFSTOR_OK ||
      gdefstor_type_get(store, "t", 1, "f.t", 3, &type) != GDEFSTOR_OK) {
    gdefstor_store_destroy(store);
    return 0;
  }
  if (gdefstor_tree_from_json(type, data + 1, size - 1, NULL, &tree) ==
          GDEFSTOR_OK &&
      gdefstor_write_begin(store, "t", 1, NULL, &write) == GDEFSTOR_OK) {
    GDEFSTOR_RevisionMeta meta;
    memset(&meta, 0, sizeof meta);
    meta.size = sizeof meta;
    meta.who = "";
    meta.when_ms = 1;
    meta.why = "";
    if (gdefstor_entity_create(write, "f.t", 3, "e", 1, "", 0, 1, NULL) ==
        GDEFSTOR_OK) {
      gdefstor_entity_save(write, "e", 1, 0, &meta, "en", 2, tree, NULL);
    }
    if (data[0] & 1u) {
      gdefstor_write_commit(write);
    } else {
      gdefstor_write_abort(write);
    }
  }
  gdefstor_tree_free(tree);
  gdefstor_type_free(type);
  gdefstor_store_destroy(store);
  return 0;
}
