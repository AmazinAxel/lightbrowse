#include <glib.h>

#include "readability.h"

/* Overridden at build time (-DLIGHTBROWSE_SHARE_DIR=...) by the Nix package
 * and the makefile; falls back to a system path otherwise. */
#ifndef LIGHTBROWSE_SHARE_DIR
#define LIGHTBROWSE_SHARE_DIR "/opt/lightbrowse"
#endif

/* Returns the readability.js contents as a newly-allocated, NUL-terminated
 * string (free with g_free), or NULL if it could not be read. */
char* read_readability_js(void)
{
    gchar* contents = NULL;
    GError* error = NULL;

    if (!g_file_get_contents(LIGHTBROWSE_SHARE_DIR "/readability.js", &contents, NULL, &error)) {
        g_warning("Failed to read readability.js: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return NULL;
    }

    return contents;
}
