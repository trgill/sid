/*
 * This file is part of SID.
 *
 * Copyright (C) 2017-2020 Red Hat, Inc. All rights reserved.
 *
 * SID is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * SID is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SID.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _SID_BUFFER_COMMON_H
#define _SID_BUFFER_COMMON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSG_SIZE_PREFIX_TYPE uint32_t
#define MSG_SIZE_PREFIX_LEN  (sizeof(uint32_t))

typedef enum
{
	BUFFER_BACKEND_MALLOC,
	BUFFER_BACKEND_MEMFD,
} buffer_backend_t;

typedef enum
{
	BUFFER_TYPE_LINEAR,
	BUFFER_TYPE_VECTOR,
} buffer_type_t;

typedef enum
{
	BUFFER_MODE_PLAIN,       /* plain buffer */
	BUFFER_MODE_SIZE_PREFIX, /* has uint32_t size prefix */
} buffer_mode_t;

struct buffer_spec {
	buffer_backend_t backend;
	buffer_type_t    type;
	buffer_mode_t    mode;
};

struct buffer_init {
	size_t size;
	size_t alloc_step;
	size_t limit;
};

struct buffer_usage {
	size_t allocated;
	size_t used;
};

struct buffer_stat {
	struct buffer_spec  spec;
	struct buffer_init  init;
	struct buffer_usage usage;
};

#ifdef __cplusplus
}
#endif

#endif
