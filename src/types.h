/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * types.h — Shared type definitions used across the project
 */

#ifndef _TYPES_H_
#define _TYPES_H_


/* ------------------------------------------------------------------ */
/*  Live-reload mode                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
	LIVERELOAD_OFF         = 0, /* No live-reload injection              */
	LIVERELOAD_SOFT_RELOAD = 1, /* SSE event triggers window.location.reload() */
	LIVERELOAD_HARD_RELOAD = 2, /* SSE event triggers cache-busting reload via URL param */
} LivereloadMode;


#endif  // _TYPES_H_
