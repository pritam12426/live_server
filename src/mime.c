/*
 * mime.c — MIME type detection by file extension
 */

#include "mime.h"

#include <string.h>

#include "log.h"

/* ------------------------------------------------------------------ */
/*  MIME table                                                          */
/* ------------------------------------------------------------------ */

// Ordered list of extension → MIME type mappings.
// The fallback (NULL extension) MUST be last.
static const struct {
	const char *ext;
	const char *mime;
} mime_map[] = {
	/* HTML */
	{ ".html",  "text/html; charset=utf-8"       },
	{ ".htm",   "text/html; charset=utf-8"        },

	/* Stylesheets */
	{ ".css",   "text/css"                        },

	/* Scripts */
	{ ".js",    "application/javascript"          },
	{ ".mjs",   "application/javascript"          },
	{ ".ts",    "application/typescript"          },

	/* Data */
	{ ".json",  "application/json"                },
	{ ".xml",   "application/xml"                 },

	/* Raster images */
	{ ".png",   "image/png"                       },
	{ ".jpg",   "image/jpeg"                      },
	{ ".jpeg",  "image/jpeg"                      },
	{ ".gif",   "image/gif"                       },
	{ ".ico",   "image/x-icon"                    },
	{ ".webp",  "image/webp"                      },
	{ ".avif",  "image/avif"                      },
	{ ".bmp",   "image/bmp"                       },
	{ ".tiff",  "image/tiff"                      },

	/* Vector */
	{ ".svg",   "image/svg+xml"                   },

	/* Fonts */
	{ ".woff",  "font/woff"                       },
	{ ".woff2", "font/woff2"                      },
	{ ".ttf",   "font/ttf"                        },
	{ ".otf",   "font/otf"                        },

	/* Audio */
	{ ".mp3",   "audio/mpeg"                      },
	{ ".ogg",   "audio/ogg"                       },
	{ ".opus",  "audio/opus"                      },
	{ ".wav",   "audio/wav"                       },
	{ ".flac",  "audio/flac"                      },
	{ ".aac",   "audio/aac"                       },

	/* Video */
	{ ".mp4",   "video/mp4"                       },
	{ ".webm",  "video/webm"                      },
	{ ".ogv",   "video/ogg"                       },
	{ ".mov",   "video/quicktime"                 },
	{ ".avi",   "video/x-msvideo"                 },

	/* Text */
	{ ".txt",   "text/plain; charset=utf-8"       },
	{ ".md",    "text/plain; charset=utf-8"       },
	{ ".csv",   "text/csv; charset=utf-8"         },

	/* Documents */
	{ ".pdf",   "application/pdf"                 },

	/* Archives */
	{ ".zip",   "application/zip"                 },
	{ ".gz",    "application/gzip"                },
	{ ".tar",   "application/x-tar"               },

	/* WASM */
	{ ".wasm",  "application/wasm"                },

	/* Fallback — must be last */
	{ NULL,     "application/octet-stream"        },
};

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

// Look up the MIME type for a file path by its extension.
// Extension matching is case-insensitive.
// Always returns a valid string (falls back to application/octet-stream).
const char *mime_from_path(const char *path)
{
	if (!path) return "application/octet-stream";

	// Find the last dot in the filename (after the last /)
	const char *slash = strrchr(path, '/');
	const char *base  = slash ? slash + 1 : path;
	const char *dot   = strrchr(base, '.');

	if (!dot || dot == base) return "application/octet-stream";

	// Walk the table until we find a match
	for (int i = 0; mime_map[i].ext != NULL; i++) {
		if (strcasecmp(dot, mime_map[i].ext) == 0) {
			LOG_DEBUG("MIME lookup: %s → %s", path, mime_map[i].mime);
			return mime_map[i].mime;
		}
	}

	LOG_WARN("Unknown file extension for '%s' — falling back to application/octet-stream", path);
	return "application/octet-stream";
}
