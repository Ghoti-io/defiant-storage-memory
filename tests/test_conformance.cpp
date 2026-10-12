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

#include <cstring>

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

extern "C" int gdefsmemory_plant_fail_put;

namespace {

GDEFSTOR_Result put_title(GDEFSTOR_Store * store, const char * value) {
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Column column;
  GDEFSTOR_Result rc = gdefstor_write_begin(store, "t", 1, NULL, &write);
  if (rc != GDEFSTOR_OK) {
    return rc;
  }
  memset(&column, 0, sizeof column);
  column.name = "title";
  column.name_len = 5;
  column.value = (const uint8_t *)value;
  column.value_len = strlen(value);
  rc = gdefstor_projection_put(write, "page", 4, "e1", 2, &column, 1);
  if (rc != GDEFSTOR_OK) {
    gdefstor_write_abort(write);
    return rc;
  }
  return gdefstor_write_commit(write);
}

} /* namespace */

TEST(Commit, FailedUpdateKeepsPreviousBytes) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Column column;
  GDEFSTOR_Projection * row = NULL;
  GDEFSTOR_Column got;
  const uint8_t blob[] = {9};
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(NULL, NULL, &store));
  ASSERT_EQ(GDEFSTOR_OK, put_title(store, "Hello"));
  gdefsmemory_plant_fail_put = 2;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t", 1, NULL, &write));
  memset(&column, 0, sizeof column);
  column.name = "title";
  column.name_len = 5;
  column.value = (const uint8_t *)"There";
  column.value_len = 5;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_put(write, "page", 4, "e1", 2, &column, 1));
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_blob_put(write, "ab", 2, blob, sizeof blob));
  EXPECT_EQ(GDEFSTOR_ERR_IO, gdefstor_write_commit(write));
  gdefsmemory_plant_fail_put = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_get(store, "t", 1, "page", 4, "e1", 2, &row));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_column(row, 0, &got));
  ASSERT_EQ(got.value_len, 5u);
  EXPECT_EQ(0, memcmp(got.value, "Hello", 5));
  gdefstor_projection_free(row);
  {
    uint8_t * bytes = NULL;
    size_t len = 0;
    EXPECT_EQ(GDEFSTOR_ERR_INVALID,
        gdefstor_blob_get(store, "t", 1, "ab", 2, &bytes, &len));
  }
  gdefstor_store_destroy(store);
}

TEST(Commit, FailedDeleteKeepsRow) {
  GDEFSTOR_Store * store = NULL;
  GDEFSTOR_Write * write = NULL;
  GDEFSTOR_Projection * row = NULL;
  GDEFSTOR_Column got;
  const uint8_t blob[] = {9};
  ASSERT_EQ(GDEFSMEMORY_OK, gdefsmemory_store_create(NULL, NULL, &store));
  ASSERT_EQ(GDEFSTOR_OK, put_title(store, "Hello"));
  gdefsmemory_plant_fail_put = 1;
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_write_begin(store, "t", 1, NULL, &write));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_delete(write, "page", 4, "e1", 2));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_blob_put(write, "ab", 2, blob, sizeof blob));
  EXPECT_EQ(GDEFSTOR_ERR_IO, gdefstor_write_commit(write));
  gdefsmemory_plant_fail_put = 0;
  ASSERT_EQ(GDEFSTOR_OK,
      gdefstor_projection_get(store, "t", 1, "page", 4, "e1", 2, &row));
  ASSERT_EQ(GDEFSTOR_OK, gdefstor_projection_column(row, 0, &got));
  ASSERT_EQ(got.value_len, 5u);
  EXPECT_EQ(0, memcmp(got.value, "Hello", 5));
  gdefstor_projection_free(row);
  gdefstor_store_destroy(store);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
