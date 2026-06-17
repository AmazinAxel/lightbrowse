#pragma once

/* Expand a DuckDuckGo-style shortcut prefix in `uri`.
 * Returns a newly-allocated expanded URI (free with g_free), or NULL if `uri`
 * does not begin with a known shortcut. */
char* shortcut_expand(const char* uri);

/* Length of the leader token (no trailing space) if `uri` starts with a known
 * shortcut, e.g. 2 for "nx ...". 0 otherwise. Used to highlight the leader. */
int shortcut_leader_len(const char* uri);
