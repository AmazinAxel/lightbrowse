#include <glib.h>
#include <string.h>

#include "shortcuts.h"

/* Inspired by https://duckduckgo.com/bangs
 *
 * If `uri` starts with one of the shortcut prefixes, return a newly-allocated
 * expanded URI (caller frees with g_free). Otherwise return NULL. */
char* shortcut_expand(const char* uri)
{
    static const struct {
        const char* prefix;
        const char* expansion;
    } shortcuts[] = {
        { "nx ", "https://search.nixos.org/packages?channel=unstable&query=" },
        { "gh ", "https://github.com/search?q=" },
        { "g ", "https://google.com/search?q=" },
        { "yt ", "https://youtube.com/search?q=" },
    };

    for (size_t i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        if (g_str_has_prefix(uri, shortcuts[i].prefix))
            return g_strconcat(shortcuts[i].expansion,
                uri + strlen(shortcuts[i].prefix), NULL);
    }

    return NULL;
}
