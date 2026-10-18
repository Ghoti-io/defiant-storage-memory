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
 * Cross-file declarations of the memory backend. Not installed.
 */

#ifndef GHOTI_IO_GDEFSMEMORY_SRC_MODEL_INTERNAL_H
#define GHOTI_IO_GDEFSMEMORY_SRC_MODEL_INTERNAL_H

#include <ghoti.io/defiant-storage-memory/macros.h>
#include <ghoti.io/defiant-storage-memory/defiant-storage-memory.h>
#include <ghoti.io/defiant-storage/model_backend.h>

#define gdefsmemory_model_backend GHOTIIO_DEFSMEMORY(gdefsmemory_model_backend)

/* Fill @p out with a fresh entity backend (model.c). */
GDEFSTOR_Result gdefsmemory_model_backend(const GDEFSMEMORY_Allocator * alloc,
    GDEFSTOR_ModelBackend * out);

#endif /* GHOTI_IO_GDEFSMEMORY_SRC_MODEL_INTERNAL_H */
