#include <gio/gio.h>
#include <string.h>

#include "passwords.h"

/* Entry paths (relative, without ".gpg"), rebuilt on each passwords_match. */
static GPtrArray* entries = NULL;

static char* store_dir(void)
{
    const char* env = g_getenv("PASSWORD_STORE_DIR");
    if (env != NULL && env[0] != '\0')
        return g_strdup(env);
    return g_build_filename(g_get_home_dir(), ".password-store", NULL);
}

/* Recursively collect "*.gpg" entries under `dir`, storing each as a path
 * relative to `root` with the ".gpg" suffix stripped. */
static void collect(const char* root, const char* dir, GPtrArray* out)
{
    GDir* d = g_dir_open(dir, 0, NULL);
    if (d == NULL)
        return;

    const char* name;
    while ((name = g_dir_read_name(d)) != NULL) {
        if (name[0] == '.')
            continue; /* skip .git, .gpg-id, etc. */
        char* path = g_build_filename(dir, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            collect(root, path, out);
        } else if (g_str_has_suffix(name, ".gpg")) {
            const char* rel = path + strlen(root);
            while (*rel == G_DIR_SEPARATOR)
                rel++;
            g_ptr_array_add(out, g_strndup(rel, strlen(rel) - 4)); /* drop ".gpg" */
        }
        g_free(path);
    }
    g_dir_close(d);
}

/* True if `token` appears in `s` bounded by non-alphanumeric chars (or the
 * string ends), so "github" matches "sites/github" and "github.com" but not
 * "mygithub-clone.com" — keeping host matches on real name boundaries. */
static gboolean contains_token(const char* s, const char* token)
{
    size_t tl = strlen(token);
    for (const char* p = strstr(s, token); p != NULL; p = strstr(p + 1, token)) {
        gboolean lok = (p == s) || !g_ascii_isalnum((guchar)p[-1]);
        gboolean rok = !g_ascii_isalnum((guchar)p[tl]);
        if (lok && rok)
            return TRUE;
    }
    return FALSE;
}

/* True if every character of `q` appears in `s`, in order (case-insensitive). */
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

guint passwords_match(const char* host, const char* query, const char** out, guint max)
{
    if (entries != NULL)
        g_ptr_array_free(entries, TRUE);
    entries = g_ptr_array_new_with_free_func(g_free);
    char* root = store_dir();
    collect(root, root, entries);
    g_free(root);
    if (entries->len == 0)
        return 0;

    /* Break the host into matchable pieces. `cands` holds the host and each of
     * its parent domains down to the registrable pair (app.github.com ->
     * app.github.com, github.com) so a `github.com` entry matches on any of its
     * subdomains; we never reduce to a bare TLD, so unrelated ".com" sites don't
     * collide. `core` is the registrable label ("github") for entries saved
     * under just the site name. Matching stays on domain boundaries — no
     * edit-distance/"similar domain" fuzz, which would be a phishing foothold. */
    char* h = NULL;
    char* core = NULL;
    GPtrArray* cands = g_ptr_array_new_with_free_func(g_free);
    if (host != NULL && host[0] != '\0') {
        h = g_ascii_strdown(host, -1);
        if (g_str_has_prefix(h, "www.")) {
            char* t = g_strdup(h + 4);
            g_free(h);
            h = t;
        }
        for (const char* p = h; *p != '\0';) {
            guint labels = 1;
            for (const char* q = p; *q != '\0'; q++)
                if (*q == '.')
                    labels++;
            g_ptr_array_add(cands, g_strdup(p));
            if (labels <= 2)
                break;
            p = strchr(p, '.') + 1; /* drop the leftmost label */
        }
        const char* reg = cands->pdata[cands->len - 1]; /* registrable domain */
        char* lastdot = strrchr(reg, '.');
        if (lastdot != NULL) {
            const char* start = reg;
            for (const char* q = reg; q < lastdot; q++)
                if (*q == '.')
                    start = q + 1;
            core = g_strndup(start, lastdot - start);
        } else {
            core = g_strdup(reg);
        }
    }

    gboolean have_query = query != NULL && query[0] != '\0';
    /* Pass kinds: 0 = exact host, 1 = host/parent-domain, 2 = registrable core
     * label, 3 = fuzzy query. When the user is typing, their query leads;
     * otherwise the most-specific host match leads. */
    int order[4];
    guint npass;
    if (have_query) {
        order[0] = 3;
        order[1] = 0;
        order[2] = 1;
        order[3] = 2;
        npass = 4;
    } else {
        order[0] = 0;
        order[1] = 1;
        order[2] = 2;
        npass = 3;
    }

    gboolean* used = g_new0(gboolean, entries->len);
    guint n = 0;
    for (guint pi = 0; pi < npass && n < max; pi++) {
        int pass = order[pi];
        for (guint i = 0; i < entries->len && n < max; i++) {
            if (used[i])
                continue;
            const char* e = entries->pdata[i];
            gboolean hit = FALSE;
            if (pass == 3) {
                hit = fuzzy_match(query, e);
            } else {
                char* el = g_ascii_strdown(e, -1);
                if (pass == 0) {
                    hit = h != NULL && contains_token(el, h);
                } else if (pass == 1) {
                    for (guint c = 0; c < cands->len && !hit; c++)
                        hit = contains_token(el, cands->pdata[c]);
                } else { /* registrable core label; skip 2-char labels ("co") to stay specific */
                    hit = core != NULL && strlen(core) >= 3 && contains_token(el, core);
                }
                g_free(el);
            }
            if (hit) {
                out[n++] = e;
                used[i] = TRUE;
            }
        }
    }

    g_free(used);
    g_free(h);
    g_free(core);
    g_ptr_array_free(cands, TRUE);
    return n;
}

typedef struct {
    PassCB cb;
    gpointer data;
    char* entry;
} ShowCtx;

static void on_show_done(GObject* src, GAsyncResult* res, gpointer data)
{
    ShowCtx* ctx = data;
    GSubprocess* proc = G_SUBPROCESS(src);
    char* out = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(proc, res, &out, NULL, NULL);

    char* user = NULL;
    char* pass = NULL;
    if (ok && out != NULL) {
        char** lines = g_strsplit(out, "\n", -1);
        if (lines[0] != NULL)
            pass = g_strdup(g_strstrip(lines[0]));
        for (int i = 1; lines[i] != NULL && user == NULL; i++) {
            char* colon = strchr(lines[i], ':');
            if (colon == NULL)
                continue;
            char* key = g_ascii_strdown(g_strndup(lines[i], colon - lines[i]), -1);
            char* k = g_strstrip(key);
            if (strcmp(k, "login") == 0 || strcmp(k, "user") == 0
                || strcmp(k, "username") == 0 || strcmp(k, "email") == 0)
                user = g_strdup(g_strstrip(colon + 1));
            g_free(key);
        }
        g_strfreev(lines);
    }

    /* Fallback: use the entry's trailing path component as the username, unless
     * it looks like a bare domain (contains a dot). */
    if (user == NULL && ctx->entry != NULL) {
        const char* slash = strrchr(ctx->entry, '/');
        const char* leaf = slash ? slash + 1 : ctx->entry;
        if (strchr(leaf, '.') == NULL && leaf[0] != '\0')
            user = g_strdup(leaf);
    }

    ctx->cb(user, pass, ctx->data);

    if (out != NULL) {
        memset(out, 0, strlen(out)); /* don't leave the cleartext lying in freed heap */
        g_free(out);
    }
    if (pass != NULL) {
        memset(pass, 0, strlen(pass));
        g_free(pass);
    }
    g_free(user);
    g_free(ctx->entry);
    g_free(ctx);
    g_object_unref(proc);
}

void passwords_show_async(const char* entry, PassCB cb, gpointer user_data)
{
    ShowCtx* ctx = g_new0(ShowCtx, 1);
    ctx->cb = cb;
    ctx->data = user_data;
    ctx->entry = g_strdup(entry);

    GSubprocess* proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, NULL,
        "pass", "show", entry, NULL);
    if (proc == NULL) {
        cb(NULL, NULL, user_data);
        g_free(ctx->entry);
        g_free(ctx);
        return;
    }
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_show_done, ctx);
}
