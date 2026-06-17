#pragma once

/* Expand a DuckDuckGo-style shortcut prefix in `uri`.
 * Returns a newly-allocated expanded URI (free with g_free), or NULL if `uri`
 * does not begin with a known shortcut. */
char* shortcut_expand(const char* uri);
