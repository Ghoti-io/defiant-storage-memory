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
 * @file memory.h
 * @brief Open a port store whose rows live in the process.
 * @stability free
 *
 * The returned store is the port's. Close it with gdefstor_store_destroy().
 * Isolation, the writer queue and the formats stay in the port; this library
 * only keeps the committed bytes and, for the entity port (model.h), the
 * committed rows. It also has a key store (keystore.h) for tests.
 */

#ifndef GHOTI_IO_GDEFSMEMORY_MEMORY_H
#define GHOTI_IO_GDEFSMEMORY_MEMORY_H

#include <ghoti.io/defiant-storage-memory/allocator.h>
#include <ghoti.io/defiant-storage-memory/core.h>
#include <ghoti.io/defiant-storage-memory/macros.h>
#include <ghoti.io/defiant-storage/store.h>
#include <ghoti.io/defiant-storage/keystore.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open an in-memory store, with the entity port (model.h).
 *
 * Queries over entities are answered by the port's reference scan
 * (gdefstor_model_scan()) over the current tables of the last commit. It
 * keeps the blind index too (model_backend.h): blind rows beside each
 * entity's versions, never derived, read by scanning, so the history and
 * subject lookups and a subject's deletion visit every entity of the tenant.
 *
 * @param allocator NULL uses gdefsmemory_allocator_default().
 * @param limits NULL uses the port's defaults. Passed through to the port.
 * @param out_store Receives the port store on success. Not written on failure.
 * @return ::GDEFSMEMORY_OK, ::GDEFSMEMORY_ERR_INVALID, or ::GDEFSMEMORY_ERR_OOM.
 */
GDEFSMEMORY_API GDEFSMEMORY_Result gdefsmemory_store_create(
    const GDEFSMEMORY_Allocator * allocator, const GDEFSTOR_Limits * limits,
    GDEFSTOR_Store ** out_store);

/**
 * @brief Open an in-memory key store over a caller's cipher.
 *
 * Keys are made with @p cipher's randomness and never leave the key store;
 * wrap and unwrap seal and open with @p cipher under them. It also keeps
 * sealed material, tenant secrets and the key set (keystore.h). Nothing
 * survives the process: this is for tests and development, never for a
 * deployment.
 *
 * @param allocator NULL uses gdefsmemory_allocator_default().
 * @param cipher Copied. Required, with every member set.
 * @param out_keystore Receives the key store; close it with
 *   gdefstor_keystore_destroy().
 * @return ::GDEFSMEMORY_OK, ::GDEFSMEMORY_ERR_INVALID, or ::GDEFSMEMORY_ERR_OOM.
 */
GDEFSMEMORY_API GDEFSMEMORY_Result gdefsmemory_keystore_create(
    const GDEFSMEMORY_Allocator * allocator, const GDEFSTOR_KeyCipher * cipher,
    GDEFSTOR_KeyStore ** out_keystore);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSMEMORY_MEMORY_H */
