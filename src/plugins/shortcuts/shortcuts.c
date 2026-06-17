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
};

char* shortcut_expand(const char* uri)
{
    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        if (g_str_has_prefix(uri, shortcuts[i].prefix))
            return g_strconcat(shortcuts[i].expansion,
                uri + strlen(shortcuts[i].prefix), NULL);
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
