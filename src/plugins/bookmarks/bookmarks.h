#pragma once
#include <glib.h>

/* Load (and cache) bookmarks from `dir` — one file per bookmark, where the
 * filename is the bookmark name and the file contents are its URL. Not
 * recursive. Call once at startup; bookmarks_save() keeps the cache current. */
void bookmarks_load(const char* dir);

/* Write a bookmark file (name -> url) into `dir` and add it to the cache. */
gboolean bookmarks_save(const char* dir, const char* name, const char* url);

/* Fuzzy (subsequence, case-insensitive) match `query` against cached bookmark
 * names. Fills up to `max` entries into the caller-provided arrays (pointers
 * into the cache, valid until the next load). Returns how many matched. */
guint bookmarks_fuzzy(const char* query, const char** names, const char** urls, guint max);
