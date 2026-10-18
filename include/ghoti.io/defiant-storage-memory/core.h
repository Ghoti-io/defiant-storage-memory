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
 * @file core.h
 * @brief Result codes for the in-memory storage backend.
 * @stability free
 *
 * The vocabulary matches the port's, name for name. Numeric values are not
 * a contract: a caller maps with a switch, it does not cast.
 */

#ifndef GHOTI_IO_GDEFSMEMORY_CORE_H
#define GHOTI_IO_GDEFSMEMORY_CORE_H

#include <ghoti.io/defiant-storage-memory/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of a memory-backend call.
 *
 * Same names as the port, including ::GDEFSMEMORY_ERR_BUSY, so a backend and
 * the port can be talked about with one vocabulary.
 */
typedef enum GDEFSMEMORY_Result {
  GDEFSMEMORY_OK = 0,          ///< Succeeded.
  GDEFSMEMORY_ERR_IO,          ///< A stream or sink reported an error.
  GDEFSMEMORY_ERR_FORMAT,      ///< Well formed input that is not this format.
  GDEFSMEMORY_ERR_UNSUPPORTED, ///< Unknown format name, or a newer version.
  GDEFSMEMORY_ERR_LIMIT,       ///< A limit was exceeded. Nothing is truncated.
  GDEFSMEMORY_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GDEFSMEMORY_ERR_OOM,         ///< The allocator returned NULL.
  GDEFSMEMORY_ERR_INVALID,     ///< A caller argument is wrong, or a missing row.
  GDEFSMEMORY_ERR_INTERNAL,    ///< A library invariant failed.
  GDEFSMEMORY_ERR_BUSY,        ///< The tenant's writer is busy and this is not a fiber.
  GDEFSMEMORY_ERR_CONFLICT,    ///< A write named a base revision that is no longer the newest.
  GDEFSMEMORY_RESULT_COUNT     ///< Not a result. Closes the string table.
} GDEFSMEMORY_Result;

/**
 * @brief A static description of a result. Never NULL.
 *
 * @param result The code, including out-of-range values.
 * @return A string the caller does not free.
 */
GDEFSMEMORY_API const char * gdefsmemory_result_string(GDEFSMEMORY_Result result);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GDEFSMEMORY_CORE_H */
