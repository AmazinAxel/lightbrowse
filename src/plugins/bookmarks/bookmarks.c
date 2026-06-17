#include <string.h>

#include "bookmarks.h"

typedef struct {
    char* name;
    char* url;
} Bookmark;

static GPtrArray* cache = NULL; /* of Bookmark* */

static void bookmark_free(gpointer p)
{
    Bookmark* b = p;
    g_free(b->name);
    g_free(b->url);
    g_free(b);
}

void bookmarks_load(const char* dir)
{
    if (cache != NULL)
        g_ptr_array_free(cache, TRUE);
    cache = g_ptr_array_new_with_free_func(bookmark_free);

    GDir* d = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return;

    const char* name;
    while ((name = g_dir_read_name(d)) != NULL) {
        char* path = g_build_filename(dir, name, NULL);
        char* url = NULL;
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR) && g_file_get_contents(path, &url, NULL, NULL)) {
            Bookmark* b = g_new(Bookmark, 1);
            b->name = g_strdup(name);
            b->url = g_strstrip(url); /* strips in place, returns same pointer */
            g_ptr_array_add(cache, b);
        } else {
            g_free(url);
        }
        g_free(path);
    }
    g_dir_close(d);
}

gboolean bookmarks_save(const char* dir, const char* name, const char* url)
{
    g_mkdir_with_parents(dir, 0700);

    /* Filenames can't contain '/'. */
    char* safe = g_strdelimit(g_strdup(name), "/", '-');
    char* path = g_build_filename(dir, safe, NULL);
    gboolean ok = g_file_set_contents(path, url, -1, NULL);
    g_free(path);

    if (ok && cache != NULL) {
        Bookmark* b = g_new(Bookmark, 1);
        b->name = safe;
        b->url = g_strdup(url);
        g_ptr_array_add(cache, b);
    } else {
        g_free(safe);
    }
    return ok;
}

/* True if every character of `q` appears in `s`, in order. */
static gboolean fuzzy_match(const char* q, const char* s)
{
    char* ql = g_ascii_strdown(q, -1);
    char* sl = g_ascii_strdown(s, -1);
    const char* p = sl;
    gboolean ok = TRUE;
    for (const char* c = ql; *c != '\0'; c++) {
        p = strchr(p, *c);
        if (p == NULL) {
            ok = FALSE;
            break;
        }
        p++;
    }
    g_free(ql);
    g_free(sl);
    return ok;
}

guint bookmarks_fuzzy(const char* query, const char** names, const char** urls, guint max)
{
    guint n = 0;
    if (cache == NULL)
        return 0;
    for (guint i = 0; i < cache->len && n < max; i++) {
        Bookmark* b = cache->pdata[i];
        if (fuzzy_match(query, b->name)) {
            names[n] = b->name;
            urls[n] = b->url;
            n++;
        }
    }
    return n;
}
