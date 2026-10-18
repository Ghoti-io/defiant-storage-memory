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
 * Runs the port's entity conformance suite (model.inc) against the memory
 * backend.
 */

#include <gtest/gtest.h>

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/defiant-storage/defiant-storage.h>
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>

static GDEFSTOR_Result to_port(GDEFSMEMORY_Result rc) {
  switch (rc) {
  case GDEFSMEMORY_OK:
    return GDEFSTOR_OK;
  case GDEFSMEMORY_ERR_OOM:
    return GDEFSTOR_ERR_OOM;
  case GDEFSMEMORY_ERR_INVALID:
    return GDEFSTOR_ERR_INVALID;
  default:
    return GDEFSTOR_ERR_INTERNAL;
  }
}

extern "C" GDEFSTOR_Result ModelOpen(GDEFSTOR_Store ** out,
    const GDEFSTOR_Allocator * alloc) {
  return to_port(gdefsmemory_store_create(alloc, nullptr, out));
}

extern "C" void ModelClose(GDEFSTOR_Store * store) {
  gdefstor_store_destroy(store);
}

#include "model.inc"

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
