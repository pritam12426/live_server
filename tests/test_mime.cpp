#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

extern "C" {
#include "log.h"
#include "mime.h"
}

#include <string.h>

// Initialise logger once so log calls from C modules don't complain
struct LogInit {
	LogInit()
	{
		log_init(NULL, LOG_LEVEL_ERROR);
	}
} log_init_guard;

TEST_CASE("mime_from_path — known extensions")
{
	CHECK(strcmp(mime_from_path("index.html"), "text/html; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("page.htm"), "text/html; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("style.css"), "text/css") == 0);
	CHECK(strcmp(mime_from_path("app.js"), "application/javascript") == 0);
	CHECK(strcmp(mime_from_path("script.mjs"), "application/javascript") == 0);
	CHECK(strcmp(mime_from_path("main.ts"), "application/typescript") == 0);
}

TEST_CASE("mime_from_path — images")
{
	CHECK(strcmp(mime_from_path("photo.jpg"), "image/jpeg") == 0);
	CHECK(strcmp(mime_from_path("photo.jpeg"), "image/jpeg") == 0);
	CHECK(strcmp(mime_from_path("img.png"), "image/png") == 0);
	CHECK(strcmp(mime_from_path("icon.gif"), "image/gif") == 0);
	CHECK(strcmp(mime_from_path("logo.svg"), "image/svg+xml") == 0);
	CHECK(strcmp(mime_from_path("favicon.ico"), "image/x-icon") == 0);
	CHECK(strcmp(mime_from_path("pic.webp"), "image/webp") == 0);
	CHECK(strcmp(mime_from_path("photo.avif"), "image/avif") == 0);
	CHECK(strcmp(mime_from_path("img.bmp"), "image/bmp") == 0);
	CHECK(strcmp(mime_from_path("img.tiff"), "image/tiff") == 0);
}

TEST_CASE("mime_from_path — fonts")
{
	CHECK(strcmp(mime_from_path("font.woff2"), "font/woff2") == 0);
	CHECK(strcmp(mime_from_path("font.woff"), "font/woff") == 0);
	CHECK(strcmp(mime_from_path("font.ttf"), "font/ttf") == 0);
	CHECK(strcmp(mime_from_path("font.otf"), "font/otf") == 0);
}

TEST_CASE("mime_from_path — audio")
{
	CHECK(strcmp(mime_from_path("song.mp3"), "audio/mpeg") == 0);
	CHECK(strcmp(mime_from_path("song.ogg"), "audio/ogg") == 0);
	CHECK(strcmp(mime_from_path("song.opus"), "audio/opus") == 0);
	CHECK(strcmp(mime_from_path("song.wav"), "audio/wav") == 0);
	CHECK(strcmp(mime_from_path("song.flac"), "audio/flac") == 0);
	CHECK(strcmp(mime_from_path("song.aac"), "audio/aac") == 0);
}

TEST_CASE("mime_from_path — video")
{
	CHECK(strcmp(mime_from_path("vid.mp4"), "video/mp4") == 0);
	CHECK(strcmp(mime_from_path("vid.webm"), "video/webm") == 0);
	CHECK(strcmp(mime_from_path("vid.ogv"), "video/ogg") == 0);
	CHECK(strcmp(mime_from_path("vid.mov"), "video/quicktime") == 0);
	CHECK(strcmp(mime_from_path("vid.avi"), "video/x-msvideo") == 0);
}

TEST_CASE("mime_from_path — text / data")
{
	CHECK(strcmp(mime_from_path("readme.txt"), "text/plain; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("README.md"), "text/plain; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("data.csv"), "text/csv; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("data.json"), "application/json") == 0);
	CHECK(strcmp(mime_from_path("data.xml"), "application/xml") == 0);
}

TEST_CASE("mime_from_path — documents / archives / wasm")
{
	CHECK(strcmp(mime_from_path("doc.pdf"), "application/pdf") == 0);
	CHECK(strcmp(mime_from_path("arc.zip"), "application/zip") == 0);
	CHECK(strcmp(mime_from_path("file.gz"), "application/gzip") == 0);
	CHECK(strcmp(mime_from_path("file.tar"), "application/x-tar") == 0);
	CHECK(strcmp(mime_from_path("app.wasm"), "application/wasm") == 0);
}

TEST_CASE("mime_from_path — case insensitive")
{
	CHECK(strcmp(mime_from_path("INDEX.HTML"), "text/html; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("Style.CSS"), "text/css") == 0);
	CHECK(strcmp(mime_from_path("App.JS"), "application/javascript") == 0);
}

TEST_CASE("mime_from_path — fallback for unknown extensions")
{
	CHECK(strcmp(mime_from_path("file.xyz"), "application/octet-stream") == 0);
	CHECK(strcmp(mime_from_path("file.zzz"), "application/octet-stream") == 0);
}

TEST_CASE("mime_from_path — edge cases")
{
	CHECK(strcmp(mime_from_path("Makefile"), "application/octet-stream") == 0);
	CHECK(strcmp(mime_from_path(".htaccess"), "application/octet-stream") == 0);
	CHECK(strcmp(mime_from_path("file."), "application/octet-stream") == 0);
	CHECK(strcmp(mime_from_path(""), "application/octet-stream") == 0);
	CHECK(strcmp(mime_from_path((const char *) 0), "application/octet-stream") == 0);
}

TEST_CASE("mime_from_path — path with directories")
{
	CHECK(strcmp(mime_from_path("/var/www/index.html"), "text/html; charset=utf-8") == 0);
	CHECK(strcmp(mime_from_path("sub/dir/style.css"), "text/css") == 0);
	CHECK(strcmp(mime_from_path("./app.js"), "application/javascript") == 0);
}
