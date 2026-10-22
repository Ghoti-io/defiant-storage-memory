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
 * Every public struct of this library that begins with a `size` member. There
 * is none, and tools/check-size-structs.sh fails, naming the struct, when a
 * header declares one: add it here, with the file that runs it, and a probe
 * (defiant-storage's size_probe.inc has the helper).
 */

#ifndef GHOTI_GDEFSMEMORY_TEST_SIZE_STRUCTS_H
#define GHOTI_GDEFSMEMORY_TEST_SIZE_STRUCTS_H

#define GDEFSMEMORY_SIZE_STRUCTS(X)

#endif
