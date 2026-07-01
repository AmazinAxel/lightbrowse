/*
 * Lightbrowse ad blocking — WebKit native content blockers.
 *
 * Replaces the old web-process extension (a per-request GRegex matcher). Here we
 * hand WebKit content-blocker JSON to a WebKitUserContentFilterStore, which
 * compiles it into a serialized bytecode matcher cached on disk, then attach the
 * compiled filters to each tab's user content manager. WebKit does the matching
 * (network blocking + cosmetic element hiding) natively.
 *
 * License: GPL-3.0
 */

#include "content_filters.h"

#include <glib/gstdio.h>
#include <string.h>

/* Compiled, ready-to-use filters (owned refs, kept for the process lifetime). */
static GPtrArray* g_filters = NULL;
/* Live views to keep in sync as filters finish compiling. Not reffed — each is
 * weak-tracked so it drops out of the array when the tab closes. */
static GPtrArray* g_views = NULL;
static WebKitUserContentFilterStore* g_store = NULL;

typedef struct {
    char* id;   /* store identifier, e.g. "adblock-<version>-combined-part1" */
    char* path; /* absolute path to the source JSON */
} LoadCtx;

static void load_ctx_free(LoadCtx* ctx)
{
    if (!ctx)
        return;
    g_free(ctx->id);
    g_free(ctx->path);
    g_free(ctx);
}

static void view_add_filter(WebKitWebView* view, WebKitUserContentFilter* filter)
{
    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_add_filter(ucm, filter);
}

/* A tracked view was destroyed: drop it so we never touch freed memory. */
static void on_view_destroyed(gpointer user_data G_GNUC_UNUSED, GObject* where)
{
    if (g_views)
        g_ptr_array_remove_fast(g_views, where);
}

/* A filter finished compiling (or loaded from cache): keep it and apply it to
 * every view we already know about. Takes ownership of the passed reference. */
static void register_ready(WebKitUserContentFilter* filter)
{
    g_ptr_array_add(g_filters, filter);
    for (guint i = 0; i < g_views->len; i++)
        view_add_filter(g_views->pdata[i], filter);
}

static void on_saved(GObject* source, GAsyncResult* result, gpointer user_data)
{
    LoadCtx* ctx = user_data;
    GError* error = NULL;
    WebKitUserContentFilter* filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), result, &error);
    if (filter)
        register_ready(filter);
    else
        g_warning("[adblock] failed to compile %s: %s", ctx->id,
            error ? error->message : "unknown error");
    g_clear_error(&error);
    load_ctx_free(ctx);
}

static void on_loaded(GObject* source, GAsyncResult* result, gpointer user_data)
{
    LoadCtx* ctx = user_data;
    WebKitUserContentFilterStore* store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
    GError* error = NULL;

    WebKitUserContentFilter* filter = webkit_user_content_filter_store_load_finish(
        store, result, &error);
    if (filter) {
        /* Cache hit: WebKit already had this identifier compiled. */
        register_ready(filter);
        g_clear_error(&error);
        load_ctx_free(ctx);
        return;
    }
    g_clear_error(&error); /* not cached yet — compile from the JSON source */

    char* json = NULL;
    gsize len = 0;
    if (!g_file_get_contents(ctx->path, &json, &len, &error)) {
        g_warning("[adblock] cannot read %s: %s", ctx->path,
            error ? error->message : "unknown error");
        g_clear_error(&error);
        load_ctx_free(ctx);
        return;
    }

    GBytes* bytes = g_bytes_new_take(json, len);
    /* ctx lives on until on_saved frees it. */
    webkit_user_content_filter_store_save(store, ctx->id, bytes, NULL, on_saved, ctx);
    g_bytes_unref(bytes);
}

static char* read_marker(const char* path)
{
    char* contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        g_strchomp(contents);
        return contents;
    }
    return NULL;
}

/* Delete the (flat) files WebKit wrote into the store dir. Called when the shipped
 * filter version changes so stale compiled blobs don't pile up. */
static void wipe_store_files(const char* store_dir)
{
    GDir* dir = g_dir_open(store_dir, 0, NULL);
    if (!dir)
        return;
    const char* name;
    while ((name = g_dir_read_name(dir))) {
        char* p = g_build_filename(store_dir, name, NULL);
        if (!g_file_test(p, G_FILE_TEST_IS_DIR))
            g_unlink(p);
        g_free(p);
    }
    g_dir_close(dir);
}

void adblock_content_init(const char* filters_dir, const char* store_dir)
{
    if (g_store)
        return; /* once */

    g_filters = g_ptr_array_new();
    g_views = g_ptr_array_new();

    /* Version marker shipped alongside the JSON. Baked into the store identifiers
     * so a new list release recompiles exactly once, then reloads from cache. */
    char* version_path = g_build_filename(filters_dir, "version", NULL);
    char* version = read_marker(version_path);
    g_free(version_path);
    if (!version)
        version = g_strdup("0");

    g_mkdir_with_parents(store_dir, 0700);

    /* If the shipped version changed, clear old compiled blobs (disk hygiene) and
     * update the store's marker. Identifiers already encode the version, so this is
     * not required for correctness — only to avoid unbounded growth. */
    char* store_marker = g_build_filename(store_dir, "version", NULL);
    char* prev = read_marker(store_marker);
    if (g_strcmp0(prev, version) != 0) {
        wipe_store_files(store_dir);
        g_file_set_contents(store_marker, version, -1, NULL);
    }
    g_free(prev);
    g_free(store_marker);

    g_store = webkit_user_content_filter_store_new(store_dir);

    GDir* dir = g_dir_open(filters_dir, 0, NULL);
    if (!dir) {
        g_warning("[adblock] filter dir not found: %s (ad blocking disabled)", filters_dir);
        g_free(version);
        return;
    }
    const char* name;
    guint count = 0;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, "combined-part") || !g_str_has_suffix(name, ".json"))
            continue;
        LoadCtx* ctx = g_new0(LoadCtx, 1);
        ctx->path = g_build_filename(filters_dir, name, NULL);
        char* stem = g_strndup(name, strlen(name) - strlen(".json"));
        ctx->id = g_strdup_printf("adblock-%s-%s", version, stem);
        g_free(stem);
        /* Try the cache first; on_loaded falls back to compiling the source. */
        webkit_user_content_filter_store_load(g_store, ctx->id, NULL, on_loaded, ctx);
        count++;
    }
    g_dir_close(dir);

    if (count == 0)
        g_warning("[adblock] no combined-part*.json in %s (ad blocking disabled)", filters_dir);

    g_free(version);
}

void adblock_apply_to_view(WebKitWebView* view)
{
    if (!g_views)
        return; /* init not called (or ad blocking unavailable) */

    g_ptr_array_add(g_views, view);
    g_object_weak_ref(G_OBJECT(view), on_view_destroyed, NULL);

    for (guint i = 0; i < g_filters->len; i++)
        view_add_filter(view, g_filters->pdata[i]);
}
