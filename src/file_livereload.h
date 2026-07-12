/*
 * Copyright (c) 2026 Pritam
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * file_livereload.h — LiveReload script injection
 */

#ifndef _FILE_LIVERELOAD_H_
#define _FILE_LIVERELOAD_H_


#include <stddef.h>
#include "types.h"

/**
 * Inject LiveReload script into HTML body before </body> tag.
 * @param body       original HTML body (must be null-terminated)
 * @param body_len   length of body in bytes
 * @param out        output buffer (must have space for body_len + script_len + 32)
 * @param out_len    size of output buffer
 * @param out_len_ptr receives length of modified body
 */
void file_livereload_inject(const char *body, size_t body_len,
                             char *out, size_t out_len,
                             size_t *out_len_ptr);

/**
 * Check if LiveReload injection should be applied.
 * @param mime  MIME type of response (e.g., "text/html")
 * @param mode  LiveReload mode from server config
 * @return 1 if injection should occur, 0 otherwise
 */
int file_livereload_should_inject(const char *mime, LivereloadMode mode);


#endif  // _FILE_LIVERELOAD_H_