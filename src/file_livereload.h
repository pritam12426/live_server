/*
 * file_livereload.h — LiveReload script injection
 */

#ifndef _FILE_LIVERELOAD_H_
#define _FILE_LIVERELOAD_H_

#include <stddef.h>
#include "types.h"

void file_livereload_inject(const char *body, size_t body_len,
                             char *out, size_t out_len,
                             size_t *out_len_ptr);

int file_livereload_should_inject(const char *mime, LivereloadMode mode);

#endif  // _FILE_LIVERELOAD_H_