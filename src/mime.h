/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * mime.h — MIME type detection
 */

#ifndef _MIME_H_
#define _MIME_H_


/*
 * Return the MIME type string for the given filename/path.
 * Always returns a non-NULL string (falls back to application/octet-stream).
 * Matching is case-insensitive by extension.
 */
const char *mime_from_path(const char *path);


#endif  // _MIME_H_
