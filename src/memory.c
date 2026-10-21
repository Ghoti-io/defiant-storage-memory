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
 * The store over the entity backend of model.c. The port owns isolation and
 * the writer queue; model.c keeps the committed rows.
 */

#include "model_internal.h"
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/defiant-storage/model.h>

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
  case GDEFSTOR_ERR_CONFLICT:
    return GDEFSMEMORY_ERR_CONFLICT;
  case GDEFSTOR_RESULT_COUNT:
  default:
    return GDEFSMEMORY_ERR_INTERNAL;
  }
}

GDEFSMEMORY_Result gdefsmemory_store_create(
    const GDEFSMEMORY_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store) {
  const GDEFSMEMORY_Allocator * use = gdefsmemory_allocator_default();
  GDEFSTOR_ModelBackend model;
  GDEFSTOR_Result rc;
  if (allocator != NULL) {
    use = allocator;
  }
  if (out_store == NULL) {
    return GDEFSMEMORY_ERR_INVALID;
  }
  rc = gdefsmemory_model_backend(use, &model);
  if (rc != GDEFSTOR_OK) {
    return from_port(rc);
  }
  rc = gdefstor_store_open(&model, use, limits, out_store);
  if (rc != GDEFSTOR_OK) {
    model.destroy(model.ctx);
  }
  return from_port(rc);
}
