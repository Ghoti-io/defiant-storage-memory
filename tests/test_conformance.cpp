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
 * Runs the port's conformance suite against the in-memory backend.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/fiber.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>

extern "C" {

static GDEFSTOR_Result to_port(GDEFSMEMORY_Result rc) {
  switch (rc) {
  case GDEFSMEMORY_OK:
    return GDEFSTOR_OK;
  case GDEFSMEMORY_ERR_IO:
    return GDEFSTOR_ERR_IO;
  case GDEFSMEMORY_ERR_FORMAT:
    return GDEFSTOR_ERR_FORMAT;
  case GDEFSMEMORY_ERR_UNSUPPORTED:
    return GDEFSTOR_ERR_UNSUPPORTED;
  case GDEFSMEMORY_ERR_LIMIT:
    return GDEFSTOR_ERR_LIMIT;
  case GDEFSMEMORY_ERR_CORRUPT:
    return GDEFSTOR_ERR_CORRUPT;
  case GDEFSMEMORY_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSMEMORY_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  case GDEFSMEMORY_ERR_INTERNAL:
    return GDEFSTOR_ERR_INTERNAL;
  case GDEFSMEMORY_ERR_BUSY:
    return GDEFSTOR_ERR_BUSY;
  case GDEFSMEMORY_RESULT_COUNT:
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

GDEFSTOR_Result ConformanceOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * allocator) {
  return to_port(gdefsmemory_store_create(allocator, NULL, out));
}

void ConformanceClose(GDEFSTOR_Store * store) {
  gdefstor_store_destroy(store);
}

} /* extern "C" */

#include "suite.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
