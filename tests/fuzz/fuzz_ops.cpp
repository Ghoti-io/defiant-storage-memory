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
 * Random operations against an in-memory store. The first byte is unused
 * except to keep a one-byte input from doing nothing.
 */

#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  if (data == NULL || size < 2) {
    return 0;
  }
  if (gdefsmemory_store_create(NULL, NULL, &store) != GDEFSMEMORY_OK) {
    return 0;
  }
  if (gdefstor_write_begin(store, "t", 1, NULL, &write) == GDEFSTOR_OK) {
    gdefstor_revision_append(write, data + 1, size - 1);
    if (data[0] & 1u) {
      gdefstor_write_commit(write);
    } else {
      gdefstor_write_abort(write);
    }
  }
  gdefstor_store_destroy(store);
  return 0;
}
