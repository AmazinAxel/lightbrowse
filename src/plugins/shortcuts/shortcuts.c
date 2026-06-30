#include <glib.h>
#include <string.h>

#include "shortcuts.h"

/* Inspired by https://duckduckgo.com/bangs */
static const struct {
    const char* prefix;
    const char* expansion;
} shortcuts[] = {
    { "nx ", "https://search.nixos.org/packages?channel=unstable&query=" },
    { "gh ", "https://github.com/search?q=" },
    { "g ", "https://google.com/search?q=" },
    { "yt ", "https://youtube.com/search?q=" },
    { "sk ", "https://skripthub.net/docs/?search=" },
    { "wa ", "https://www.wolframalpha.com/input?i=" },
};

char* shortcut_expand(const char* uri)
{
    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        if (g_str_has_prefix(uri, shortcuts[i].prefix)) {
            /* URL-encode the query so '+', '&', spaces, etc. survive intact --
             * Wolfram in particular needs '+' as %2B, not a space. */
            char* q = g_uri_escape_string(uri + strlen(shortcuts[i].prefix), NULL, TRUE);
            char* out = g_strconcat(shortcuts[i].expansion, q, NULL);
            g_free(q);
            return out;
        }
    }
    return NULL;
}

/* Length of the leader token (excluding its trailing space) if `uri` begins
 * with a known shortcut, e.g. 2 for "nx ...". Returns 0 otherwise. */
int shortcut_leader_len(const char* uri)
{
    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        if (g_str_has_prefix(uri, shortcuts[i].prefix))
            return (int)strlen(shortcuts[i].prefix) - 1;
    }
    return 0;
}
