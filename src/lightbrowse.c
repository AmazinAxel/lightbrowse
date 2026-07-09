#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <webkit/webkit.h>

#include "config.h"
#include "plugins/plugins.h"

/* The chrome is hardcoded to Graphite-nord-dark colours so the UI is always dark,
 * independent of the system light/dark setting. Nord palette: bg #2E3440, a lighter
 * surface #3B4252, fg #ECEFF4, accent #5E81AC. The loading canvas is pure black
 * (see apply_view_background) so a navigation flash is never a bright white. */
static const char* CSS =
    /* Root: pin the window dark so the GTK theme's light bg never shows in any gap. */
    "window { background: #2E3440; color: #ECEFF4; }"
    ".webarea, .webarea > stack { background: #2E3440; }"
    ".tabbar { background: #3B4252; border-right: 0.25rem solid #5e81ac; padding: 4px; }"
    ".tab { padding: 4px; border: 0.2rem solid #4C566A; border-radius: 4px; background: #3B4252; color: #ECEFF4; outline: none; box-shadow: none; transition: border-color .1s ease; }"
    ".tab.active { border-color: #5e81ac; }"
    ".tab image { color: #ECEFF4; }" /* recolour the symbolic globe placeholder white (real favicons are untouched) */
    ".tab.asleep { opacity: 0.5; }" /* slept tab: web process freed, reopen to reload */
    ".dim { background: alpha(black, 0.3); }"
    ".modal { background: #2E3440; color: #ECEFF4; border: 2px solid #5e81ac; border-radius: 12px; padding: 12px; }"
    ".modal entry { min-width: 280px; background: #3B4252; color: #ECEFF4; border: 1px solid #4C566A; caret-color: #ECEFF4; }"
    ".modal entry selection { background: #81A1C1; color: #2E3440; }"
    ".modal label { padding: 8px; border: 1px solid #4C566A; background-color: #3B4252; border-radius: 6px; transition: all 120ms ease; }"
    ".modal .selected { background: #81A1C1; color: #ECEFF4; border-color: #D8DEE9; transform: translateY(-2px) scale(1.01); outline: 2px solid #81A1C1; outline-offset: 1px; }"
    /* Calculator result: a plain label under the search box, not a result row. */
    ".modal .calc { padding: 4px 0 0 4px; border: none; background: none; color: #5e81ac; font-weight: bold; font-size: 1.3rem; }"

    ".findbar { background: #2E3440; color: #ECEFF4; border: 1px solid #5e81ac; border-radius: 8px; padding: 4px; margin-bottom: 12px; outline: 2px solid #5E81AC; }"
    ".findbar entry { background: #3B4252; color: #ECEFF4; border: 1px solid #4C566A; caret-color: #ECEFF4; }"
    ".findbar entry selection { background: #81A1C1; color: #2E3440; }"
    ".findbar label { color: #ECEFF4; }"

    ".statusbar label { color: #ECEFF4; font-size: 0.85em; padding: 2px 4px; background: #3B4252; border-top-right-radius: 6px; }"
    "progressbar.loadbar trough { border: none; background: transparent; border-radius: 0; padding: 0; min-height: 3px; }"
    "progressbar.loadbar progress { border: none; border-radius: 0; background: #5e81ac; }"
    "progressbar.downloadbar trough { border: none; background: transparent; border-radius: 0; padding: 0; min-height: 3px; }"
    "progressbar.downloadbar progress { border: none; border-radius: 0; background: #a3be8c; }";

/* Global widgets */
static GtkWindow* window;
static GtkOverlay* overlay;
static GtkNotebook* notebook;
static GtkBox* tabbar; /* vertical favicon strip */
static gboolean tabbar_visible = TRUE;
static int num_tabs = 0;

/* Most-recently-used tab history (alt+tab walks back through it). The front
 * (index 0) is the current tab, except during an active alt+tab walk: then the
 * displayed tab is mru[alt_walk] and the list is only reshuffled once the user
 * releases Alt, so repeated Tab presses keep stepping deeper into history. */
static GtkWidget* mru[MRU_HISTORY];
static int mru_len = 0;
static int alt_walk = -1;            /* index into mru during a walk; -1 when idle */
static gboolean alt_switch = FALSE;  /* TRUE while the walk drives the page switch itself */

/* Modal (search / bookmark / password picker) */
typedef enum { MODAL_NONE, MODAL_SEARCH, MODAL_BOOKMARK, MODAL_PASSWORD, MODAL_PERMISSION } ModalMode;
static ModalMode modal_mode = MODAL_NONE;
static gboolean modal_new_tab = FALSE; /* search: open in a new tab vs current */
static gboolean modal_blocked = FALSE; /* tab limit reached: don't open on submit */
static GtkWidget* dim;
static GtkWidget* modal_box;
static GtkLabel* modal_info;
static GtkEntry* modal_entry1; /* search text / bookmark name */
static GtkEntry* modal_entry2; /* bookmark url (hidden in search mode) */
static GtkBox* modal_results;
static GtkLabel* calc_label; /* search: live calculation result, shown under the entry */
static gboolean calc_active = FALSE; /* a valid calculation is currently displayed */
static char calc_result[64]; /* its formatted value, for the clipboard */
static const char* fuzzy_urls[FUZZY_RESULTS];
static guint fuzzy_count = 0;
static int fuzzy_sel = -1;

/* Password picker (MODAL_PASSWORD): pass_entries mirrors fuzzy_urls but holds
 * `pass` entry paths; pass_host is the current page's host we match against;
 * pass_target is the view to inject the filled credentials into. */
static const char* pass_entries[FUZZY_RESULTS];
static char pass_host[256];
static WebKitWebView* pass_target = NULL;

/* Permission prompts (camera/microphone getUserMedia + Storage Access API).
 * WebKitGTK denies every permission request unless the app allows it explicitly,
 * so without this a page (Zoom, Meet, ...) silently gets no mic/camera and its
 * whole call collapses. Each request is shown in the shared modal (Enter allows,
 * Esc denies); a grant is remembered for the browser session only (perm_allowed,
 * an in-memory set gone on quit) so repeating it within the session doesn't
 * re-prompt. Requests are keyed by host (media) or requesting|current domain
 * (storage) in "perm-key"; a NULL key is never remembered. */
static WebKitPermissionRequest* perm_current = NULL; /* request awaiting a decision, or NULL */
static GQueue perm_queue = G_QUEUE_INIT;             /* further requests, shown one at a time */
static GHashTable* perm_allowed = NULL;              /* set of perm-keys granted this session */

/* Find bar */
static GtkWidget* findbar;
static GtkEntry* find_entry;
static GtkLabel* find_label;
static guint find_total = 0;
static guint find_current = 0;

/* Bottom status / loading bar (the whole bar hides when idle) */
static GtkWidget* statusbar;       /* container: hidden unless loading or hovering */
static GtkLabel* status_label;     /* hovered or keyboard-focused link */
static GtkProgressBar* progress;   /* page load progress (hidden when idle) */
static GtkProgressBar* download_progress; /* green download progress, below the load bar */
static int active_downloads = 0;
static char* status_link = NULL;   /* hovered or keyboard-focused link URI */
static char* status_flash = NULL;  /* transient message (e.g. "Download started") */
static guint status_flash_source = 0;
static gboolean page_loading = FALSE;

/* Closed-tab ring (full WebKit session state, so the webview is freed) */
static WebKitWebViewSessionState* closed_tabs[CLOSED_TAB_HISTORY];
static int closed_count = 0;

/* System color scheme watcher (the chrome stays dark; only websites follow it). */
static GSettings* iface_settings = NULL;

/* Forward declarations */
static void notebook_create_new_tab(const char* uri);
static WebKitWebView* current_view(void);
static void do_find(const char* text);
static void session_save(void);
static void update_status(void);
static void on_download_started(WebKitNetworkSession* session, WebKitDownload* download, gpointer data);
static void mru_promote(GtkWidget* page);
static void mru_remove(GtkWidget* page);
static void tab_set_asleep(WebKitWebView* view, gboolean asleep);
static void modal_show_permission(const char* markup);
static void modal_hide(void);

/* ---------------------------------------------------------------- URI load */
/* Does `s` carry an explicit URI scheme we should navigate to as-is? Accepts a
 * valid RFC-3986 scheme ("[a-z][a-z0-9+.-]*:") in two forms: "scheme://…" (any
 * scheme, so custom app URLs like myapp:// navigate — the decide-policy handler
 * then hands non-web ones to the OS), and "scheme:rest" without a slash for the
 * schemeless-authority schemes (about:, data:, mailto:, tel:, …). A bare
 * "host:1234" would look like "scheme:digits", so an all-digit remainder is
 * treated as a host:port, not a scheme. */
static bool has_uri_scheme(const char* s)
{
    if (!g_ascii_isalpha((guchar)s[0]))
        return false;
    const char* p = s + 1;
    while (g_ascii_isalnum((guchar)*p) || *p == '+' || *p == '-' || *p == '.')
        p++;
    if (*p != ':')
        return false;
    if (p[1] == '/' && p[2] == '/')
        return true;
    if (p[1] == '\0')
        return false;
    for (const char* d = p + 1; *d != '\0'; d++)
        if (!g_ascii_isdigit((guchar)*d))
            return true; /* non-digit remainder -> a real scheme (mailto:, data:, …) */
    return false; /* all digits -> host:port, not a scheme */
}

/* Does `s` look like a bare hostname the user meant to visit (vs. a search)?
 * True when it has no whitespace and its authority is a plausible host: an
 * alphabetic TLD of 2+ chars (.com .dev .net .local .io …), a dotted-quad /
 * bracketed-IPv6 literal, or exactly "localhost" — with an optional :port. This
 * replaces the old hardcoded ".com"/".org" check with a general heuristic. */
static bool looks_like_host(const char* s)
{
    for (const char* p = s; *p != '\0'; p++)
        if (g_ascii_isspace((guchar)*p))
            return false;

    gsize alen = strcspn(s, "/?#"); /* the authority, before any path/query/fragment */
    if (alen == 0)
        return false;
    char* auth = g_strndup(s, alen);
    char* at = strrchr(auth, '@');
    char* host = at ? at + 1 : auth; /* drop any userinfo */

    bool ok = false;
    if (host[0] == '[') {
        ok = strchr(host, ']') != NULL; /* bracketed IPv6 literal */
    } else {
        char* colon = strrchr(host, ':'); /* strip an all-digit :port */
        if (colon != NULL && colon[1] != '\0') {
            bool digits = true;
            for (char* d = colon + 1; *d != '\0'; d++)
                if (!g_ascii_isdigit((guchar)*d)) {
                    digits = false;
                    break;
                }
            if (digits)
                *colon = '\0';
        }
        if (g_ascii_strcasecmp(host, "localhost") == 0) {
            ok = true;
        } else {
            char* dot = strrchr(host, '.');
            if (dot != NULL && dot != host && dot[1] != '\0') {
                int tldlen = 0;
                bool tld_alpha = true;
                for (char* d = dot + 1; *d != '\0'; d++, tldlen++)
                    if (!g_ascii_isalpha((guchar)*d)) {
                        tld_alpha = false;
                        break;
                    }
                if (tld_alpha && tldlen >= 2) {
                    ok = true;
                } else { /* not an alpha TLD -> only accept an IPv4-ish dotted number */
                    bool ipish = true;
                    for (char* d = host; *d != '\0'; d++)
                        if (!g_ascii_isdigit((guchar)*d) && *d != '.') {
                            ipish = false;
                            break;
                        }
                    ok = ipish;
                }
            }
        }
    }
    g_free(auth);
    return ok;
}

static void load_uri(WebKitWebView* view, const char* uri)
{
    if (uri[0] == '\0')
        return;

    if (has_uri_scheme(uri)) {
        webkit_web_view_load_uri(view, uri);
        return;
    }

    if (looks_like_host(uri)) {
        char* tmp = g_strconcat("https://", uri, NULL);
        webkit_web_view_load_uri(view, tmp);
        g_free(tmp);
        return;
    }

    char* expanded = shortcut_expand(uri);
    if (expanded != NULL) {
        webkit_web_view_load_uri(view, expanded);
        g_free(expanded);
        return;
    }

    /* Encode the query so special characters ('+', '&', '~', spaces, ...) reach
     * the search engine intact instead of being mangled into URL syntax. */
    char* q = g_uri_escape_string(uri, NULL, TRUE);
    char* search = g_strdup_printf(SEARCH, q);
    webkit_web_view_load_uri(view, search);
    g_free(q);
    g_free(search);
}

/* ------------------------------------------------------------ shared state */
static WebKitWebContext* get_shared_web_context(void)
{
    static WebKitWebContext* context = NULL;
    if (context == NULL) {
        context = webkit_web_context_new();
        /* Most aggressive caching: "improve document load speed substantially by
         * caching a very large number of resources and previously viewed content." */
        webkit_web_context_set_cache_model(context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
        /* Ad blocking is no longer a web-process extension: it runs as native
         * WebKit content filters attached per view (see adblock_apply_to_view). */
    }
    return context;
}

static WebKitNetworkSession* get_shared_network_session(void)
{
    static WebKitNetworkSession* session = NULL;
    if (session == NULL) {
        /* Bound WebKit's memory before the first network/web process spawns (the
         * setter is global and must run before any session). WebKit reclaims a
         * tab's process on close fine; this caps the *inherent* per-process cost
         * so a few heavy pages can't exhaust a small-RAM machine. As a process
         * climbs toward the per-process limit, caches are released early; a single
         * runaway page is killed (sad-tab) before the whole system OOMs. Typical
         * tabs (100-400MB) stay well under these thresholds, so normal browsing
         * and repeat-visit speed are unaffected. */
        WebKitMemoryPressureSettings* mp = webkit_memory_pressure_settings_new();
        webkit_memory_pressure_settings_set_memory_limit(mp, 2048);          /* MB per process */
        /* Set thresholds top-down (kill > strict > conservative): each setter asserts
         * its value sits below the *current* next-higher threshold, and the defaults
         * (conservative 0.33, strict 0.5) would reject conservative=0.5 if set first. */
        webkit_memory_pressure_settings_set_kill_threshold(mp, 0.95);        /* ~1.95GB: kill a runaway process */
        webkit_memory_pressure_settings_set_strict_threshold(mp, 0.65);      /* >1.3GB: release aggressively */
        webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.5); /* >1.0GB: start releasing caches */
        webkit_network_session_set_memory_pressure_settings(mp);
        webkit_memory_pressure_settings_free(mp);

        session = webkit_network_session_new(DATA_DIR, DATA_DIR);
        /* Disable ITP: its storage partitioning blocks the third-party
         * challenges.cloudflare.com iframe from accessing storage until a user
         * interaction is recorded for the domain, which made Cloudflare Turnstile
         * fail on first appearance and only pass on the second attempt. */
        webkit_network_session_set_itp_enabled(session, FALSE);
        WebKitCookieManager* cm = webkit_network_session_get_cookie_manager(session);
        webkit_cookie_manager_set_persistent_storage(cm, DATA_DIR "/cookies.sqlite", WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
        webkit_website_data_manager_set_favicons_enabled(
            webkit_network_session_get_website_data_manager(session), TRUE);
        g_signal_connect(session, "download-started", G_CALLBACK(on_download_started), NULL);
    }
    return session;
}

static WebKitSettings* get_shared_settings(void)
{
    static WebKitSettings* settings = NULL;
    if (settings == NULL)
        settings = webkit_settings_new_with_settings(WEBKIT_DEFAULT_SETTINGS, NULL);
    return settings;
}

static WebKitWebView* current_view(void)
{
    GtkWidget* page = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));
    return page ? WEBKIT_WEB_VIEW(page) : NULL;
}

/* ----------------------------------- status bar (link hover + load progress) */
static void update_status(void)
{
    const char* text = status_flash ? status_flash : status_link; /* flash wins over hover */
    gtk_label_set_text(status_label, text ? text : "");
    gtk_widget_set_visible(GTK_WIDGET(status_label), text != NULL);
    gtk_widget_set_visible(GTK_WIDGET(progress), page_loading);
    gtk_widget_set_visible(statusbar, text != NULL || page_loading || active_downloads > 0);
}

static gboolean status_flash_clear(gpointer data)
{
    g_clear_pointer(&status_flash, g_free);
    status_flash_source = 0;
    update_status();
    return G_SOURCE_REMOVE;
}

/* Show a transient message in the status bar for 2 seconds. */
static void status_flash_message(const char* msg)
{
    g_clear_pointer(&status_flash, g_free);
    status_flash = g_strdup(msg);
    if (status_flash_source != 0)
        g_source_remove(status_flash_source);
    status_flash_source = g_timeout_add_seconds(2, status_flash_clear, NULL);
    update_status();
}

/* ----------------------------------------------------------------- downloads */
/* Pick the destination path under DOWNLOADS_DIR, uniquifying on collision so an
 * existing "file.zip" becomes "file (1).zip", "file (2).zip", ... rather than
 * being clobbered. An already-numbered "file (1).zip" renumbers to "file (2).zip"
 * instead of growing into "file (1) (1).zip". */
static gboolean on_download_destination(WebKitDownload* download, gchar* suggested_filename, gpointer data)
{
    const char* base = (suggested_filename != NULL && suggested_filename[0] != '\0')
        ? suggested_filename : "download";

    const char* ext = strrchr(base, '.'); /* keep the extension when numbering */
    int stem_len = ext ? (int)(ext - base) : (int)strlen(base);

    /* Strip a trailing " (N)" from the stem so we renumber rather than append. */
    if (stem_len >= 4 && base[stem_len - 1] == ')') {
        int j = stem_len - 2;
        while (j > 0 && g_ascii_isdigit(base[j]))
            j -= 1;
        if (j >= 1 && j <= stem_len - 3 && base[j] == '(' && base[j - 1] == ' ')
            stem_len = j - 1;
    }

    char* dest = g_build_filename(DOWNLOADS_DIR, base, NULL);
    for (int i = 1; g_file_test(dest, G_FILE_TEST_EXISTS); i++) {
        g_free(dest);
        char* name = g_strdup_printf("%.*s (%d)%s", stem_len, base, i, ext ? ext : "");
        dest = g_build_filename(DOWNLOADS_DIR, name, NULL);
        g_free(name);
    }

    webkit_download_set_destination(download, dest);
    g_free(dest);
    return TRUE;
}

static void on_download_progress(GObject* obj, GParamSpec* pspec, gpointer data)
{
    gtk_progress_bar_set_fraction(download_progress,
        webkit_download_get_estimated_progress(WEBKIT_DOWNLOAD(obj)));
}

/* "finished" fires on success, cancel, and after "failed" alike, so it's the one
 * place to drop the active count and hide the bar once nothing is downloading. */
static void on_download_finished(WebKitDownload* download, gpointer data)
{
    if (active_downloads > 0)
        active_downloads -= 1;
    if (active_downloads == 0)
        gtk_widget_set_visible(GTK_WIDGET(download_progress), FALSE);
    update_status();
}

/* A download opened in a fresh tab (target=_blank / window.open) leaves that tab
 * blank, since the response converts to a download and never commits a page. Such
 * a tab has no back/forward history, so close it; tabs with real content stay. */
static gboolean close_blank_download_tab(gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(data);
    int n = gtk_notebook_page_num(notebook, GTK_WIDGET(view));
    if (n >= 0 && num_tabs > 1
        && webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(view)) == NULL) {
        mru_remove(GTK_WIDGET(view));
        GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
        if (btn != NULL)
            gtk_box_remove(tabbar, btn);
        gtk_notebook_remove_page(notebook, n);
        num_tabs -= 1;
    }
    g_object_unref(view);
    return G_SOURCE_REMOVE;
}

static void on_download_started(WebKitNetworkSession* session, WebKitDownload* download, gpointer data)
{
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_download_destination), NULL);
    g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress), NULL);
    g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), NULL);

    active_downloads += 1;
    gtk_progress_bar_set_fraction(download_progress, 0.0);
    gtk_widget_set_visible(GTK_WIDGET(download_progress), TRUE);
    status_flash_message("Download started");

    WebKitWebView* view = webkit_download_get_web_view(download);
    if (view != NULL)
        g_idle_add(close_blank_download_tab, g_object_ref(view));
}

/* NULL clears it; ignored for background tabs so they can't hijack the status. */
static void status_set_link(WebKitWebView* view, const char* uri)
{
    if (view != current_view())
        return;
    g_clear_pointer(&status_link, g_free);
    if (uri != NULL && uri[0] != '\0')
        status_link = g_strdup(uri);
    update_status();
}

/* Restyle the page's text selection to a dark-blue wash with white text. Applies
 * to any selection, including the current match WebKit selects while finding --
 * which otherwise shows the faint, low-contrast default selection colour. */
static const char* SELECTION_CSS =
    "::selection { background-color: #5E81AC !important; color: #FFFFFF !important; }";

/* Report the focused link (keyboard tabbing) the way hover reports it. */
static const char* LINK_FOCUS_JS =
    "document.addEventListener('focusin',function(e){"
    "var a=e.target.closest?e.target.closest('a[href]'):null;"
    "window.webkit.messageHandlers.linkFocus.postMessage(a?a.href:'');},true);"
    "document.addEventListener('focusout',function(){"
    "window.webkit.messageHandlers.linkFocus.postMessage('');},true);";

static void on_load_progress(GObject* obj, GParamSpec* pspec, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(obj);
    if (view != current_view())
        return;
    gtk_progress_bar_set_fraction(progress, webkit_web_view_get_estimated_load_progress(view));
}

static void view_set_dark_chrome(WebKitWebView* view, gboolean dark);

static void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    if (event == WEBKIT_LOAD_STARTED) {
        g_object_set_data(G_OBJECT(view), "load-error", NULL); /* assume this load is fine */
        view_set_dark_chrome(view, TRUE);                      /* dark pre-paint loading flash */
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        /* Keep dark chrome only where the page has no background of its own: the
         * (transparent) error page, and about: pages like the blank new tab. A real
         * site has committed its document now, so hand it the plain white UA canvas. */
        const char* uri = webkit_web_view_get_uri(view);
        gboolean dark = g_object_get_data(G_OBJECT(view), "load-error") != NULL
            || uri == NULL || g_str_has_prefix(uri, "about:");
        view_set_dark_chrome(view, dark);
    }

    if (view != current_view())
        return;
    if (event == WEBKIT_LOAD_STARTED) {
        page_loading = TRUE;
        g_object_set_data(G_OBJECT(view), "dead", NULL); /* a load means it's alive again */
        tab_set_asleep(view, FALSE);                     /* and no longer dimmed */
        gtk_progress_bar_set_fraction(progress, 0.0);
    } else if (event == WEBKIT_LOAD_FINISHED) {
        page_loading = FALSE;
    }
    update_status();
}

/* A failed load (DNS failure, TLS error, ...) makes WebKit swap in its built-in
 * error page, which is transparent and relies on the UA white canvas + black text.
 * Flag the view so the COMMITTED of that error page keeps the dark chrome (legible
 * white-on-dark); FALSE lets WebKit render its default error page. */
static gboolean on_load_failed(WebKitWebView* view, WebKitLoadEvent event,
    char* failing_uri, GError* error, gpointer data)
{
    g_object_set_data(G_OBJECT(view), "load-error", GINT_TO_POINTER(1));
    return FALSE;
}

static void on_mouse_target(WebKitWebView* view, WebKitHitTestResult* hit, guint modifiers, gpointer data)
{
    status_set_link(view, webkit_hit_test_result_context_is_link(hit)
            ? webkit_hit_test_result_get_link_uri(hit)
            : NULL);
}

static void on_link_focus_message(WebKitUserContentManager* ucm, JSCValue* value, gpointer data)
{
    char* uri = jsc_value_to_string(value);
    status_set_link(WEBKIT_WEB_VIEW(data), uri);
    g_free(uri);
}

/* ----------------------------------------------------------------- tabs */
/* When a web process is killed (e.g. the memory kill-threshold) the view stays
 * alive but blank, so the tab stays in the bar. We remember its URL and mark it
 * "dead"; the page is reloaded lazily only when the user navigates back to that
 * tab — so a runaway background page can't immediately OOM again. */
/* Dim a tab's button to 50% (kept favicon and all) while it's slept/dead. */
static void tab_set_asleep(WebKitWebView* view, gboolean asleep)
{
    GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
    if (btn == NULL)
        return;
    if (asleep)
        gtk_widget_add_css_class(btn, "asleep");
    else
        gtk_widget_remove_css_class(btn, "asleep");
}

/* Record when a tab was last the active one, so the sweep sleeps the least-recently-used one first. */
static void tab_touch(WebKitWebView* view)
{
    g_object_set_data(G_OBJECT(view), "last-active",
        GSIZE_TO_POINTER((gsize)(g_get_monotonic_time() / G_USEC_PER_SEC)));
}

static void revive_if_dead(WebKitWebView* view)
{
    if (view == NULL || g_object_get_data(G_OBJECT(view), "dead") == NULL)
        return;
    g_object_set_data(G_OBJECT(view), "dead", NULL);
    tab_set_asleep(view, FALSE);
    const char* uri = g_object_get_data(G_OBJECT(view), "reload-uri");
    if (uri != NULL && uri[0] != '\0')
        webkit_web_view_load_uri(view, uri);
    else
        webkit_web_view_reload(view);
}

/* Both a memory-kill and a proactive sleep land here: keep the tab but free its
 * web process, remember the URL, mark it dead, and dim it. revive_if_dead (driven
 * by on_tab_pressed / on_switch_page) reloads it only when the user goes back. */
static void on_web_process_terminated(WebKitWebView* view,
    WebKitWebProcessTerminationReason reason, gpointer data)
{
    const char* uri = webkit_web_view_get_uri(view);
    g_object_set_data_full(G_OBJECT(view), "reload-uri", g_strdup(uri ? uri : ""), g_free);
    g_object_set_data(G_OBJECT(view), "dead", GINT_TO_POINTER(1));
    tab_set_asleep(view, TRUE);
    /* Sleeping must never leave the tab you're looking at blank/dimmed: if WebKit's
     * memory-pressure killer (or a crash) takes the foreground process, reload it
     * right away instead of waiting for the user to navigate back. */
    if (view == current_view())
        revive_if_dead(view);
}

/* A tab is worth sleeping only if it actually holds a live page we can reload:
 * not already slept/crashed, not playing audio (don't cut off music/video), and
 * showing a real URL (a blank/about tab has nothing to free). */
static gboolean tab_sleepable(WebKitWebView* view)
{
    if (g_object_get_data(G_OBJECT(view), "dead") != NULL)
        return FALSE;
    if (webkit_web_view_is_playing_audio(view))
        return FALSE;
    const char* uri = webkit_web_view_get_uri(view);
    return uri != NULL && uri[0] != '\0' && !g_str_has_prefix(uri, "about:");
}

/* Sleep a background tab: terminating its web process hands the RAM back to the OS
 * (the bulk of a heavy page's cost). The web-process-terminated handler above
 * remembers the URL and dims the tab; it reloads when the user reselects it. */
static void sleep_tab(WebKitWebView* view)
{
    if (tab_sleepable(view))
        webkit_web_view_terminate_web_process(view);
}

/* Current memory-pressure reading: the PSI "some avg10" percentage — the share of
 * the last 10s in which at least one task stalled waiting on memory. ~0 on a
 * healthy machine (even one sitting at 100% RAM with free swap), climbing only
 * under genuine thrash. Returns -1 if PSI isn't available (pre-4.20 kernel or
 * CONFIG_PSI off), so the caller falls back to the absolute OOM floor. */
static double system_mem_pressure(void)
{
    char* contents = NULL;
    if (!g_file_get_contents("/proc/pressure/memory", &contents, NULL, NULL))
        return -1;
    double pressure = -1;
    char* p = strstr(contents, "some avg10=");
    if (p != NULL)
        pressure = g_ascii_strtod(p + strlen("some avg10="), NULL);
    g_free(contents);
    return pressure;
}

/* Free RAM *plus* free swap in MiB (everywhere the kernel can still put a page),
 * or -1 if /proc/meminfo can't be read. This is the true "about to OOM" gauge:
 * unlike MemAvailable alone it doesn't false-alarm while swap has headroom. */
static long system_oom_headroom_mb(void)
{
    char* contents = NULL;
    if (!g_file_get_contents("/proc/meminfo", &contents, NULL, NULL))
        return -1;
    long mb = 0;
    gboolean got = FALSE;
    const char* fields[] = { "MemAvailable:", "SwapFree:" };
    for (int i = 0; i < 2; i++) {
        char* p = strstr(contents, fields[i]);
        if (p != NULL) {
            mb += strtol(p + strlen(fields[i]), NULL, 10) / 1024; /* fields are in kB */
            got = TRUE;
        }
    }
    g_free(contents);
    return got ? mb : -1;
}

/* The crash defense: every few seconds, gauge real memory pressure. While the
 * machine is comfortable (low PSI, swap headroom) leave every tab loaded — a
 * swap-backed laptop can sit near 100% RAM without trouble, so we don't reap
 * tabs just because free RAM is low. Under genuine pressure, sleep the
 * least-recently-used background tab (one per sweep — self-correcting as the OS
 * reclaims memory), skipping any tab seen too recently so tab-switching stays
 * snappy. If the machine is thrashing or has run clean out of RAM *and* swap,
 * dump every background tab at once so it can't OOM out from under us. */
static gboolean sleep_sweep(gpointer data)
{
    if (notebook == NULL)
        return G_SOURCE_CONTINUE;

    double pressure = system_mem_pressure();
    long headroom = system_oom_headroom_mb();
    gboolean oom_floor = headroom >= 0 && headroom < TAB_SLEEP_OOM_FLOOR_MB;

    if (!oom_floor && (pressure < 0 || pressure < TAB_SLEEP_PRESSURE))
        return G_SOURCE_CONTINUE; /* comfortable (or PSI unavailable and swap fine): leave tabs be */
    gboolean critical = oom_floor || pressure >= TAB_SLEEP_PRESSURE_CRITICAL;

    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    GtkWidget* cur = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));
    GtkWidget* lru = NULL;
    gint64 lru_time = G_MAXINT64;
    int n = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n; i++) {
        GtkWidget* page = gtk_notebook_get_nth_page(notebook, i);
        if (page == cur || !tab_sleepable(WEBKIT_WEB_VIEW(page)))
            continue; /* never sleep the tab you're looking at */
        if (critical) {
            sleep_tab(WEBKIT_WEB_VIEW(page)); /* OOM imminent: dump them all */
            continue;
        }
        gint64 last = (gint64)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(page), "last-active"));
        if (now - last < TAB_SLEEP_MIN_AGE_SECONDS)
            continue; /* just left this tab — keep it warm so switching back is instant */
        if (last < lru_time) {
            lru_time = last;
            lru = page;
        }
    }
    if (!critical && lru != NULL)
        sleep_tab(WEBKIT_WEB_VIEW(lru));
    return G_SOURCE_CONTINUE;
}

/* Switch on press (capture phase) rather than on the button's release-driven
 * "clicked", so mouse selection feels as instant as the keyboard. */
static void on_tab_pressed(GtkGestureClick* gesture, int n_press, double x, double y, WebKitWebView* view)
{
    int n = gtk_notebook_page_num(notebook, GTK_WIDGET(view));
    if (n >= 0)
        gtk_notebook_set_current_page(notebook, n);
    revive_if_dead(view); /* clicking a killed tab reloads it (same page if current) */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void update_favicon(WebKitWebView* view)
{
    GtkImage* img = g_object_get_data(G_OBJECT(view), "icon");
    GdkTexture* fav = webkit_web_view_get_favicon(view);
    if (fav != NULL)
        gtk_image_set_from_paintable(img, GDK_PAINTABLE(fav));
    else
        gtk_image_set_from_icon_name(img, "folder-earth-symbolic");
}

static void on_favicon_notify(GObject* view, GParamSpec* pspec, gpointer data)
{
    update_favicon(WEBKIT_WEB_VIEW(view));
}

static void on_switch_page(GtkNotebook* nb, GtkWidget* page, guint n, gpointer data)
{
    for (GtkWidget* c = gtk_widget_get_first_child(GTK_WIDGET(tabbar)); c != NULL; c = gtk_widget_get_next_sibling(c))
        gtk_widget_remove_css_class(c, "active");
    GtkWidget* btn = g_object_get_data(G_OBJECT(page), "button");
    if (btn != NULL)
        gtk_widget_add_css_class(btn, "active");

    /* Track most-recently-used order so alt+tab can walk back through it. While a
     * walk is driving the switch itself, leave the order untouched: it's committed
     * only when the user releases Alt (see handle_signal_keyrelease). */
    if (!alt_switch) {
        mru_promote(page);
        alt_walk = -1; /* a genuine switch ends any in-progress walk */
    }

    /* Resync the status bar to the now-current tab. */
    WebKitWebView* view = WEBKIT_WEB_VIEW(page);
    tab_touch(view); /* mark active now so the sleep sweep leaves it alone */
    g_clear_pointer(&status_link, g_free); /* no hover/focus on the new tab yet */
    page_loading = webkit_web_view_is_loading(view);
    if (page_loading)
        gtk_progress_bar_set_fraction(progress, webkit_web_view_get_estimated_load_progress(view));
    update_status();
    revive_if_dead(view); /* reload a killed tab when the user switches to it */

    /* Re-run the find against the now-current tab so its matches/highlight
     * reflect the page the user is actually looking at. */
    if (gtk_widget_get_visible(findbar))
        do_find(gtk_editable_get_text(GTK_EDITABLE(find_entry)));
}

static void on_counted_matches(WebKitFindController* fc, guint count, gpointer data);
static GtkWidget* on_create_tab(WebKitWebView* self, WebKitNavigationAction* action, gpointer data);

/* Middle-click a link -> open it in a new tab (other clicks navigate normally). */
static gboolean on_decide_policy(WebKitWebView* view, WebKitPolicyDecision* decision,
    WebKitPolicyDecisionType type, gpointer data)
{
    /* A response WebKit can't render (zip, exe, ...) should download, not show a
     * blank page. WebKit doesn't do this on its own, so force it here. */
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision* rpd = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        /* application/pdf isn't reported as a "supported" MIME, but WebKit's bundled
         * PDF.js viewer (PDFJSViewer, on by default) renders it — so let it through
         * and the PDF opens in the tab instead of downloading. PDF.js binds Ctrl+S
         * itself to save the file (through the normal download pipeline). */
        WebKitURIResponse* resp = webkit_response_policy_decision_get_response(rpd);
        const char* mime = resp ? webkit_uri_response_get_mime_type(resp) : NULL;
        if (mime != NULL && g_ascii_strcasecmp(mime, "application/pdf") == 0)
            return FALSE;
        if (!webkit_response_policy_decision_is_mime_type_supported(rpd)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
        return FALSE;
    }

    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;
    WebKitNavigationAction* a = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));

    /* A navigation to a scheme WebKit can't load itself (an app callback like
     * myapp://, or mailto:/tel:) would otherwise error. Hand it to the OS's
     * registered handler instead and drop the in-page navigation. */
    const char* nav_uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
    const char* scheme = nav_uri ? g_uri_peek_scheme(nav_uri) : NULL;
    static const char* web_schemes[] = { "http", "https", "file", "about", "data", "blob", "ws", "wss" };
    if (scheme != NULL) {
        bool web = false;
        for (size_t i = 0; i < G_N_ELEMENTS(web_schemes); i++)
            if (g_ascii_strcasecmp(scheme, web_schemes[i]) == 0) {
                web = true;
                break;
            }
        if (!web) {
            g_app_info_launch_default_for_uri(nav_uri, NULL, NULL);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }

    if (webkit_navigation_action_get_mouse_button(a) != 2) /* 2 = middle */
        return FALSE;
    notebook_create_new_tab(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

/* WebKit's built-in error page sets no colours of its own — it relied on the UA
 * default white canvas + black text, so on our dark canvas its text is invisible.
 * This user-level (low-priority) stylesheet gives a Nord-white default text colour:
 * it only takes effect where the page itself sets no colour (the error page, plain
 * documents), and is overridden by any real site that styles its own text. */
static const char* DEFAULT_TEXT_CSS = "html { color: #ECEFF4; }";

/* Dark chrome = Nord polar-night canvas (#2E3440) + the white-text default above.
 * It keeps the pre-paint loading flash, about: pages and WebKit's transparent error
 * page legible on dark. We deliberately DON'T keep it on a real site: many sites set
 * no background and lean on the UA default white canvas + black text, where the dark
 * canvas would show through and the forced white text would wash their text out. So
 * those pages get plain white chrome instead (toggled per-load in on_load_changed).
 *
 * Selection styling is always on; the white-text default only in dark mode. We
 * remove-all + re-add rather than toggle a single sheet to stay off the newer
 * remove-single-sheet API; our user scripts (link focus) live separately and are
 * left untouched by remove_all_style_sheets. */
static void view_set_dark_chrome(WebKitWebView* view, gboolean dark)
{
    /* Background stays dark for every load, even after a real page commits: a real
     * site paints its own opaque background over this canvas, so the dark only ever
     * shows in the pre-paint gap (and in overscroll). Flipping to white at COMMITTED
     * used to expose that gap as a white flash before the page's first frame. The
     * `dark` flag now only gates the white-text helper, which the transparent
     * about:/error pages need but a real site (black-on-white default) must not get. */
    GdkRGBA dark_c = { 0.180, 0.204, 0.251, 1.0 }; /* Nord polar night #2E3440 */
    webkit_web_view_set_background_color(view, &dark_c);

    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    WebKitUserStyleSheet* sel = webkit_user_style_sheet_new(SELECTION_CSS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_AUTHOR, NULL, NULL);
    webkit_user_content_manager_add_style_sheet(ucm, sel);
    webkit_user_style_sheet_unref(sel);
    if (dark) {
        WebKitUserStyleSheet* text = webkit_user_style_sheet_new(DEFAULT_TEXT_CSS,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
        webkit_user_content_manager_add_style_sheet(ucm, text);
        webkit_user_style_sheet_unref(text);
    }
}

/* ------------------------------------------------ camera/mic permissions */
/* The host of the view's current page (for the prompt text + session memory), or
 * NULL for a page without one (about:, file:, ...). Caller frees. */
static char* view_host(WebKitWebView* view)
{
    const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
    if (uri == NULL)
        return NULL;
    GUri* u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    if (u == NULL)
        return NULL;
    const char* host = g_uri_get_host(u);
    char* out = (host != NULL && host[0] != '\0') ? g_strdup(host) : NULL;
    g_uri_unref(u);
    return out;
}

/* Pop and display the next queued permission request in the shared modal, or hide
 * the modal once the queue is drained. */
static void perm_show_next(void)
{
    if (perm_current != NULL)
        return; /* one decision at a time */
    perm_current = g_queue_pop_head(&perm_queue);
    if (perm_current == NULL) {
        modal_hide();
        return;
    }
    const char* msg = g_object_get_data(G_OBJECT(perm_current), "perm-msg");
    modal_show_permission(msg ? msg : "This site wants access");
}

/* Resolve the shown request (Enter=allow, Esc=deny), remember an allow for its key
 * this session, then show any request that queued up behind it. */
static void perm_decide(gboolean allow)
{
    if (perm_current == NULL)
        return;
    if (allow) {
        webkit_permission_request_allow(perm_current);
        const char* key = g_object_get_data(G_OBJECT(perm_current), "perm-key");
        if (key != NULL && key[0] != '\0')
            g_hash_table_add(perm_allowed, g_strdup(key));
    } else {
        webkit_permission_request_deny(perm_current);
    }
    g_clear_object(&perm_current);
    perm_show_next(); /* re-shows the modal for the next request, or hides it */
}

/* Build the prompt for a request, or return FALSE to leave it at WebKit's default.
 * On TRUE, *key is the session-memory key (caller owns; NULL = never remember) and
 * *msg is the Pango markup to show (caller owns). */
static gboolean perm_describe(WebKitWebView* view, WebKitPermissionRequest* request,
    char** key, char** msg)
{
    const char* hint = "\n<span size='small' foreground='#81A1C1'>"
                       "Enter to allow · Esc to deny</span>";
    if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
        WebKitUserMediaPermissionRequest* um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
        gboolean audio = webkit_user_media_permission_is_for_audio_device(um);
        gboolean video = webkit_user_media_permission_is_for_video_device(um);
        const char* kind = (audio && video) ? "microphone &amp; camera"
            : video                         ? "camera"
                                            : "microphone";
        char* host = view_host(view);
        char* who = g_markup_escape_text(host != NULL ? host : "This site", -1);
        *key = host; /* takes host (NULL for about:/file: — those aren't remembered) */
        *msg = g_strdup_printf("<b>%s</b> wants %s access%s", who, kind, hint);
        g_free(who);
        return TRUE;
    }
    if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request)) {
        WebKitWebsiteDataAccessPermissionRequest* wr
            = WEBKIT_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request);
        const char* req = webkit_website_data_access_permission_request_get_requesting_domain(wr);
        const char* cur = webkit_website_data_access_permission_request_get_current_domain(wr);
        char* ereq = g_markup_escape_text(req ? req : "A site", -1);
        char* ecur = g_markup_escape_text(cur ? cur : "this site", -1);
        *key = g_strdup_printf("storage\n%s\n%s", req ? req : "", cur ? cur : "");
        *msg = g_strdup_printf("<b>%s</b> wants to use its cookies on <b>%s</b>%s", ereq, ecur, hint);
        g_free(ereq);
        g_free(ecur);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_permission_request(WebKitWebView* view,
    WebKitPermissionRequest* request, gpointer data G_GNUC_UNUSED)
{
    /* Device enumeration (the mic/camera picker Zoom shows) is benign and needed
     * for the media UI once capture is in play; allow it silently. */
    if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(request)) {
        webkit_permission_request_allow(request);
        return TRUE;
    }

    char* key = NULL;
    char* msg = NULL;
    /* Anything we don't describe keeps WebKit's safe default (denied). */
    if (!perm_describe(view, request, &key, &msg))
        return FALSE;

    if (perm_allowed == NULL)
        perm_allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* Already granted this session: allow without re-prompting. */
    if (key != NULL && g_hash_table_contains(perm_allowed, key)) {
        webkit_permission_request_allow(request);
        g_free(key);
        g_free(msg);
        return TRUE;
    }

    g_object_set_data_full(G_OBJECT(request), "perm-key", key, g_free); /* may be NULL */
    g_object_set_data_full(G_OBJECT(request), "perm-msg", msg, g_free);

    g_queue_push_tail(&perm_queue, g_object_ref(request));
    perm_show_next();
    return TRUE; /* handled asynchronously: we hold a ref until perm_decide */
}

/* When `related` is non-NULL the new view is a popup (window.open / target=_blank):
 * constructing it with "related-view" keeps it in the opener's web process and
 * session and, crucially, preserves window.opener. Microsoft Rewards card
 * activities (and many OAuth popups) report completion back through that opener
 * channel, so a detached popup silently fails to credit. A NULL related view is a
 * normal top-level tab using the shared session/context. */
static WebKitWebView* append_tab(WebKitWebView* related)
{
    WebKitWebView* view = related
        ? g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "related-view", related, /* shares process + session, keeps window.opener */
            "settings", get_shared_settings(),
            NULL)
        : g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "settings", get_shared_settings(),
            "network-session", get_shared_network_session(),
            "web-context", get_shared_web_context(),
            NULL);
    NULLCHECK(view);
    view_set_dark_chrome(view, TRUE); /* dark until a real page commits (on_load_changed) */

    webkit_settings_set_user_agent(get_shared_settings(), "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15");

    GtkWidget* img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), TAB_ICON_SIZE);
    gtk_image_set_from_icon_name(GTK_IMAGE(img), "folder-earth-symbolic");
    GtkWidget* btn = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(btn), img);
    gtk_widget_add_css_class(btn, "tab");
    gtk_widget_set_focusable(btn, FALSE); /* no focus ring on tab buttons */
    g_object_set_data(G_OBJECT(view), "icon", img);
    g_object_set_data(G_OBJECT(view), "button", btn);
    GtkGesture* click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), view);
    gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(tabbar, btn);

    g_signal_connect(view, "create", G_CALLBACK(on_create_tab), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_favicon_notify), NULL);
    g_signal_connect(webkit_web_view_get_find_controller(view), "counted-matches", G_CALLBACK(on_counted_matches), NULL);

    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_load_progress), NULL);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_mouse_target), NULL);

    WebKitUserContentManager* ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_register_script_message_handler(ucm, "linkFocus", NULL);
    g_signal_connect(ucm, "script-message-received::linkFocus", G_CALLBACK(on_link_focus_message), view);
    WebKitUserScript* script = webkit_user_script_new(LINK_FOCUS_JS,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    /* Attach native ad-block content filters to this tab (adds any that are already
     * compiled, and back-fills the rest as compilation finishes). */
    adblock_apply_to_view(view);

    /* The selection + (dark-only) default-text stylesheets are installed by
     * view_set_dark_chrome (called above), which re-runs them on every load. */

    int n = gtk_notebook_append_page(notebook, GTK_WIDGET(view), NULL);
    gtk_notebook_set_current_page(notebook, n);
    tab_touch(view); /* seed last-active so the sleep sweep doesn't fire immediately */
    num_tabs += 1;
    return view;
}

/* The MAX_NUM_TABS limit is only enforced by the ctrl+t shortcut; tabs opened
 * by a page (window.open / target=_blank) or from another app are never blocked. */
static void notebook_create_new_tab(const char* uri)
{
    WebKitWebView* view = append_tab(NULL);
    load_uri(view, uri ? uri : "about:blank"); /* about:blank spins up the web process */
}

static GtkWidget* on_create_tab(WebKitWebView* self,
    WebKitNavigationAction* action G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    /* Return a real related view so WebKit drives the popup itself, preserving
     * window.opener. The old code stopped the opener and reloaded the URL in a
     * detached tab, which broke Rewards/OAuth completion (see append_tab). */
    return GTK_WIDGET(append_tab(self));
}

/* The page's slot in the MRU list, or -1 if it isn't tracked. */
static int mru_index(GtkWidget* page)
{
    for (int i = 0; i < mru_len; i++)
        if (mru[i] == page)
            return i;
    return -1;
}

/* Move (or insert) a page at the front of the MRU list, capped at MRU_HISTORY
 * (the oldest entry falls off the back once it's full). */
static void mru_promote(GtkWidget* page)
{
    int at = mru_index(page);
    if (at < 0)
        at = (mru_len < MRU_HISTORY) ? mru_len++ : MRU_HISTORY - 1; /* drop the oldest */
    for (int i = at; i > 0; i--)
        mru[i] = mru[i - 1];
    mru[0] = page;
}

/* Drop a page from the MRU list (its tab was closed) so it can't linger as an
 * alt+tab target. Reopening it later (ctrl+shift+t) re-promotes it to the front. */
static void mru_remove(GtkWidget* page)
{
    int at = mru_index(page);
    if (at < 0)
        return;
    for (int i = at; i < mru_len - 1; i++)
        mru[i] = mru[i + 1];
    mru[--mru_len] = NULL;
    alt_walk = -1; /* the list shifted under any walk in progress; restart it */
}

static void push_closed(WebKitWebViewSessionState* st)
{
    if (closed_count == CLOSED_TAB_HISTORY) {
        webkit_web_view_session_state_unref(closed_tabs[0]);
        memmove(closed_tabs, closed_tabs + 1, sizeof(closed_tabs[0]) * (CLOSED_TAB_HISTORY - 1));
        closed_count -= 1;
    }
    closed_tabs[closed_count++] = st;
}

static void close_current_tab(void)
{
    WebKitWebView* view = current_view();
    if (view == NULL)
        return;
    push_closed(webkit_web_view_get_session_state(view));

    mru_remove(GTK_WIDGET(view));
    GtkWidget* btn = g_object_get_data(G_OBJECT(view), "button");
    if (btn != NULL)
        gtk_box_remove(tabbar, btn);
    gtk_notebook_remove_page(notebook, gtk_notebook_get_current_page(notebook));
    num_tabs -= 1;
    if (num_tabs <= 0) { /* no homepage to fall back to: quit */
        session_save();  /* 0 tabs left -> clears the saved session */
        gtk_window_destroy(window);
    }
}

static void reopen_closed_tab(void)
{
    if (closed_count == 0)
        return;
    WebKitWebViewSessionState* st = closed_tabs[--closed_count];
    WebKitWebView* view = append_tab(NULL);
    webkit_web_view_restore_session_state(view, st);
    WebKitBackForwardList* bf = webkit_web_view_get_back_forward_list(view);
    WebKitBackForwardListItem* item = webkit_back_forward_list_get_current_item(bf);
    if (item != NULL)
        webkit_web_view_go_to_back_forward_list_item(view, item);
    else
        load_uri(view, "about:blank");
    webkit_web_view_session_state_unref(st);
}

/* ------------------------------------------------------ session restore */
/* Persist every open tab's URL to SESSION_FILE so the next cold start can bring
 * them back. Called on window close and on SIGTERM (logout / poweroff). With no
 * real tabs open it removes the file, so closing everything starts fresh. */
static void session_save(void)
{
    if (notebook == NULL)
        return;
    GString* s = g_string_new(NULL);
    int n = gtk_notebook_get_n_pages(notebook);
    for (int i = 0; i < n; i++) {
        WebKitWebView* v = WEBKIT_WEB_VIEW(gtk_notebook_get_nth_page(notebook, i));
        const char* uri = webkit_web_view_get_uri(v);
        if (uri == NULL || uri[0] == '\0' || g_str_has_prefix(uri, "about:"))
            continue;
        g_string_append(s, uri);
        g_string_append_c(s, '\n');
    }
    if (s->len > 0)
        g_file_set_contents(SESSION_FILE, s->str, s->len, NULL);
    else
        g_unlink(SESSION_FILE);
    g_string_free(s, TRUE);
}

/* Reload the saved tabs into the freshly-built window (reusing its blank tab for
 * the first URL). Returns TRUE if at least one tab was restored. */
static gboolean session_restore(void)
{
    char* contents = NULL;
    if (!g_file_get_contents(SESSION_FILE, &contents, NULL, NULL))
        return FALSE;
    char** lines = g_strsplit(contents, "\n", -1);
    g_free(contents);

    gboolean any = FALSE;
    for (char** l = lines; *l != NULL; l++) {
        if ((*l)[0] == '\0')
            continue;
        notebook_create_new_tab(*l);
        any = TRUE;
    }
    g_strfreev(lines);
    return any;
}

/* ----------------------------------------------------------------- modal */
/* modal_results holds modal_entry1, then the persistent calc_label, then the
 * bookmark rows -- so the box's spacing only grows when rows exist. Clear the
 * bookmark rows only, keeping the entry and calc_label in place. */
static void clear_results(void)
{
    GtkWidget* c;
    while ((c = gtk_widget_get_last_child(GTK_WIDGET(modal_results))) != GTK_WIDGET(calc_label))
        gtk_box_remove(modal_results, c);
}

/* Hide the live calculation result and forget its value. */
static void calc_clear(void)
{
    calc_active = FALSE;
    calc_result[0] = '\0';
    gtk_widget_set_visible(GTK_WIDGET(calc_label), FALSE);
}

static void modal_hide(void)
{
    modal_mode = MODAL_NONE;
    gtk_widget_set_visible(dim, FALSE);
    gtk_widget_set_visible(modal_box, FALSE);
    gtk_entry_set_attributes(modal_entry1, NULL);
    clear_results();
    calc_clear();
    fuzzy_count = 0;
    fuzzy_sel = -1;
}

static void modal_show(ModalMode mode, gboolean open_new_tab)
{
    modal_mode = mode;
    modal_new_tab = open_new_tab;
    modal_blocked = FALSE;
    fuzzy_count = 0;
    fuzzy_sel = -1;
    clear_results();
    calc_clear();
    gtk_entry_set_attributes(modal_entry1, NULL);
    gtk_widget_set_visible(GTK_WIDGET(modal_entry1), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(modal_results), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(modal_info), FALSE); /* only shown for "Tab limit reached" */
    gtk_widget_set_halign(GTK_WIDGET(modal_info), GTK_ALIGN_START); /* undo permission-prompt centering */

    if (mode == MODAL_SEARCH || mode == MODAL_PASSWORD) {
        gtk_widget_set_visible(GTK_WIDGET(modal_entry2), FALSE);
        gtk_editable_set_text(GTK_EDITABLE(modal_entry1), "");
    } else { /* MODAL_BOOKMARK */
        gtk_widget_set_visible(GTK_WIDGET(modal_entry2), TRUE);
        WebKitWebView* v = current_view();
        const char* title = v ? webkit_web_view_get_title(v) : NULL;
        const char* uri = v ? webkit_web_view_get_uri(v) : NULL;
        gtk_editable_set_text(GTK_EDITABLE(modal_entry1), title ? title : "");
        gtk_editable_set_text(GTK_EDITABLE(modal_entry2), uri ? uri : "");
    }

    gtk_widget_set_visible(dim, TRUE);
    gtk_widget_set_visible(modal_box, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(modal_entry1));
    gtk_editable_select_region(GTK_EDITABLE(modal_entry1), 0, -1);
}

/* Show a confirm-only prompt (no entry, no results) in the shared modal for a
 * permission request: `markup` is the Pango text, Enter allows / Esc denies
 * (handled in handle_signal_keypress). Reuses the search/bookmark dim + box. */
static void modal_show_permission(const char* markup)
{
    modal_mode = MODAL_PERMISSION;
    modal_blocked = FALSE;
    fuzzy_count = 0;
    fuzzy_sel = -1;
    clear_results();
    calc_clear();
    gtk_entry_set_attributes(modal_entry1, NULL);
    gtk_widget_set_visible(GTK_WIDGET(modal_entry1), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(modal_entry2), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(modal_results), FALSE);

    gtk_label_set_markup(modal_info, markup);
    gtk_label_set_justify(modal_info, GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(GTK_WIDGET(modal_info), GTK_ALIGN_CENTER);
    gtk_widget_set_visible(GTK_WIDGET(modal_info), TRUE);

    gtk_widget_set_visible(dim, TRUE);
    gtk_widget_set_visible(modal_box, TRUE);
}

static void modal_restyle_results(void)
{
    int i = 0;
    for (GtkWidget* c = gtk_widget_get_next_sibling(GTK_WIDGET(modal_entry1)); c != NULL; c = gtk_widget_get_next_sibling(c)) {
        if (c == GTK_WIDGET(calc_label))
            continue; /* the calc result is a label, not a selectable row */
        if (i == fuzzy_sel)
            gtk_widget_add_css_class(c, "selected");
        else
            gtk_widget_remove_css_class(c, "selected");
        i++;
    }
}

static void modal_move_sel(int dir)
{
    if (fuzzy_count == 0)
        return;
    fuzzy_sel += dir;
    if (fuzzy_sel < 0)
        fuzzy_sel = 0;
    if (fuzzy_sel >= (int)fuzzy_count)
        fuzzy_sel = fuzzy_count - 1;
    modal_restyle_results();
}

/* Speculative DNS: as soon as the typed text looks like a host, resolve it so
 * the lookup is already done (or in flight) by the time Enter fires. WebKit dedups
 * repeats, so calling it on each keystroke is cheap. */
static void prefetch_host(const char* text)
{
    const char* p = strstr(text, "://");
    p = p ? p + 3 : text;
    if (strchr(p, ' ') != NULL || strchr(p, '.') == NULL)
        return; /* not a bare host (a search query or a shortcut) */
    const char* end = strchr(p, '/');
    gsize len = end ? (gsize)(end - p) : strlen(p);
    if (len == 0 || len > 253)
        return;
    char* host = g_strndup(p, len);
    webkit_network_session_prefetch_dns(get_shared_network_session(), host);
    g_free(host);
}

/* Rebuild the password picker's result rows for the current host + typed filter. */
static void password_refresh_rows(const char* query)
{
    clear_results();
    fuzzy_sel = -1;
    fuzzy_count = passwords_match(pass_host, query, pass_entries, FUZZY_RESULTS);
    for (guint i = 0; i < fuzzy_count; i++) {
        GtkWidget* row = gtk_label_new(pass_entries[i]);
        gtk_label_set_xalign(GTK_LABEL(row), 0.0);
        gtk_box_append(modal_results, row);
    }
}

static void on_search_changed(GtkEditable* editable, gpointer data)
{
    if (modal_mode == MODAL_PASSWORD) {
        password_refresh_rows(gtk_editable_get_text(editable));
        return;
    }
    if (modal_mode != MODAL_SEARCH)
        return;
    const char* text = gtk_editable_get_text(editable);

    /* Highlight a leader (nx, g, ...) so it's clear the search engine changes. */
    PangoAttrList* attrs = pango_attr_list_new();
    int ll = shortcut_leader_len(text);
    if (ll > 0) {
        PangoAttribute* bg = pango_attr_background_new(0x5e * 257, 0x81 * 257, 0xac * 257);
        bg->start_index = 0;
        bg->end_index = ll;
        pango_attr_list_insert(attrs, bg);
        PangoAttribute* fg = pango_attr_foreground_new(0xffff, 0xffff, 0xffff);
        fg->start_index = 0;
        fg->end_index = ll;
        pango_attr_list_insert(attrs, fg);
    }
    gtk_entry_set_attributes(modal_entry1, attrs);
    pango_attr_list_unref(attrs);

    /* Fuzzy-match bookmarks once 2+ chars are typed and there's no leader. */
    clear_results();
    fuzzy_count = 0;
    fuzzy_sel = -1;

    /* Live calculator: show the result under the box when the text is a valid
     * expression (a cheap synchronous parse -- no blocking). Skip when a
     * shortcut leader is active, since that's a search, not a sum. */
    calc_clear();
    if (ll == 0 && calc_eval(text, calc_result, sizeof calc_result)) {
        char* shown = g_strconcat("= ", calc_result, NULL);
        gtk_label_set_text(calc_label, shown);
        g_free(shown);
        gtk_widget_set_visible(GTK_WIDGET(calc_label), TRUE);
        calc_active = TRUE;
    }

    if (ll == 0)
        prefetch_host(text); /* warm DNS for a directly-typed host */
    if (ll == 0 && strlen(text) >= 2) {
        const char* names[FUZZY_RESULTS];
        fuzzy_count = bookmarks_fuzzy(text, names, fuzzy_urls, FUZZY_RESULTS);
        for (guint i = 0; i < fuzzy_count; i++) {
            GtkWidget* row = gtk_label_new(names[i]);
            gtk_label_set_xalign(GTK_LABEL(row), 0.0);
            gtk_box_append(modal_results, row);
            prefetch_host(fuzzy_urls[i]); /* warm DNS for matching bookmarks */
        }
    }
}

static void modal_open_search_uri(const char* uri)
{
    if (modal_new_tab || current_view() == NULL) /* no warm tab yet -> make one */
        notebook_create_new_tab(uri);
    else
        load_uri(current_view(), uri);
    modal_hide();
}

/* JS injected into the page to fill a login form. `username`/`password` arrive
 * as function arguments (never interpolated into the source) so the secret stays
 * out of the code string and escaping can't break. Values are written through the
 * native value setter + input/change events so framework-controlled inputs (React,
 * Vue, ...) pick them up. The form is NOT submitted -- the user presses Enter. */
static const char* PASSWORD_FILL_JS =
    "const vis = el => el && el.offsetParent !== null && !el.disabled && !el.readOnly;\n"
    "const pw = Array.from(document.querySelectorAll('input[type=password]')).find(vis)\n"
    "        || document.querySelector('input[type=password]');\n"
    "if (!pw) return false;\n"
    "const setVal = (el, val) => {\n"
    "  const d = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(el), 'value')\n"
    "         || Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');\n"
    "  (d && d.set ? d.set.bind(el) : (v => { el.value = v; }))(val);\n"
    "  el.dispatchEvent(new Event('input', { bubbles: true }));\n"
    "  el.dispatchEvent(new Event('change', { bubbles: true }));\n"
    "};\n"
    "if (username) {\n"
    "  let userEl = null;\n"
    "  const all = Array.from(document.querySelectorAll('input'));\n"
    "  for (let i = all.indexOf(pw) - 1; i >= 0; i--) {\n"
    "    const t = (all[i].type || '').toLowerCase();\n"
    "    if (t === 'text' || t === 'email' || t === 'tel'\n"
    "        || (all[i].autocomplete || '').includes('username')) { userEl = all[i]; break; }\n"
    "  }\n"
    "  if (!userEl) userEl = document.querySelector(\n"
    "    'input[autocomplete~=username], input[type=email], input[name*=user i], input[id*=user i]');\n"
    "  if (userEl) setVal(userEl, username);\n"
    "}\n"
    "setVal(pw, password);\n"
    "pw.focus();\n"
    "return true;\n";

/* passwords_show_async callback: got the decrypted credentials, now fill the form. */
static void on_password_ready(const char* username, const char* password, gpointer data)
{
    WebKitWebView* view = data;
    if (view == NULL || !WEBKIT_IS_WEB_VIEW(view) || password == NULL)
        return;

    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "username", g_variant_new_string(username ? username : ""));
    g_variant_builder_add(&b, "{sv}", "password", g_variant_new_string(password));
    GVariant* args = g_variant_ref_sink(g_variant_builder_end(&b));

    webkit_web_view_call_async_javascript_function(view, PASSWORD_FILL_JS, -1,
        args, NULL, NULL, NULL, NULL, NULL);
    g_variant_unref(args);
}

static void on_modal_activate(GtkEntry* entry, gpointer data)
{
    if (modal_blocked) {
        modal_hide();
        return;
    }
    if (modal_mode == MODAL_PASSWORD) {
        /* Enter fills the highlighted entry, or the top match if none is picked. */
        int sel = fuzzy_sel >= 0 ? fuzzy_sel : (fuzzy_count > 0 ? 0 : -1);
        if (sel >= 0) {
            WebKitWebView* target = pass_target;
            char* sel_entry = g_strdup(pass_entries[sel]);
            modal_hide();
            passwords_show_async(sel_entry, on_password_ready, target);
            g_free(sel_entry);
        } else {
            modal_hide();
        }
        return;
    }
    if (modal_mode == MODAL_SEARCH) {
        if (fuzzy_sel >= 0) {
            modal_open_search_uri(fuzzy_urls[fuzzy_sel]);
            return;
        }
        /* No bookmark picked but a calculation is showing: copy the value. */
        if (calc_active) {
            gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(modal_entry1)), calc_result);
            modal_hide();
            return;
        }
        modal_open_search_uri(gtk_editable_get_text(GTK_EDITABLE(modal_entry1)));
        return;
    }
    /* MODAL_BOOKMARK */
    const char* name = gtk_editable_get_text(GTK_EDITABLE(modal_entry1));
    const char* url = gtk_editable_get_text(GTK_EDITABLE(modal_entry2));
    if (name[0] != '\0' && url[0] != '\0')
        bookmarks_save(BOOKMARKS_DIR, name, url);
    modal_hide();
}

/* ------------------------------------------------------------------- find */
static void update_find_label(void)
{
    char* s = g_strdup_printf("%u of %u", find_total ? find_current : 0, find_total);
    gtk_label_set_text(find_label, s);
    g_free(s);
}

static void on_counted_matches(WebKitFindController* fc, guint count, gpointer data)
{
    find_total = count;
    update_find_label();
}

/* (Re)issue a fresh search on the live DOM. WebKit's search() both re-highlights
 * all matches and advances the selection to the next match in document order, so
 * calling it on every step keeps results up to date (content revealed since the
 * last search is picked up) without glitching the order -- it's one step, not two.
 * count_matches refreshes the "N of M" total against the same live DOM. */
static void find_step(WebKitFindOptions dir)
{
    WebKitWebView* v = current_view();
    if (v == NULL)
        return;
    const char* text = gtk_editable_get_text(GTK_EDITABLE(find_entry));
    WebKitFindController* fc = webkit_web_view_get_find_controller(v);
    WebKitFindOptions opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND | dir;
    if (text[0] == '\0') {
        webkit_find_controller_search_finish(fc);
        find_total = 0;
        find_current = 0;
    } else {
        webkit_find_controller_count_matches(fc, text, opts, G_MAXUINT);
        webkit_find_controller_search(fc, text, opts, G_MAXUINT);
        if (find_total > 0)
            find_current = (dir & WEBKIT_FIND_OPTIONS_BACKWARDS)
                ? (find_current + find_total - 2) % find_total + 1
                : (find_current % find_total) + 1;
        else
            find_current = 1;
    }
    update_find_label();
}

static void do_find(const char* text)
{
    WebKitWebView* v = current_view();
    if (v == NULL)
        return;
    WebKitFindController* fc = webkit_web_view_get_find_controller(v);
    WebKitFindOptions opts = WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND;
    if (text[0] == '\0') {
        webkit_find_controller_search_finish(fc);
        find_total = 0;
        find_current = 0;
    } else {
        webkit_find_controller_count_matches(fc, text, opts, G_MAXUINT);
        webkit_find_controller_search(fc, text, opts, G_MAXUINT);
        find_current = 1;
    }
    update_find_label();
}

static void on_find_changed(GtkEditable* editable, gpointer data)
{
    do_find(gtk_editable_get_text(editable));
}

static void find_next(void)
{
    find_step(WEBKIT_FIND_OPTIONS_NONE);
}

static void find_prev(void)
{
    find_step(WEBKIT_FIND_OPTIONS_BACKWARDS);
}

static void on_find_activate(GtkEntry* entry, gpointer data)
{
    find_next(); /* Enter = next; Shift+Enter (handled in keypress) = previous */
}

static void find_show(void)
{
    /* If the bar is already open, its matches are still highlighted from the
     * previous search -- Ctrl+F must leave them (and the "N of M" position)
     * exactly as they are and only re-focus the box. When reopening a closed
     * bar there are no highlights left (find_hide cleared them), so re-run the
     * search to restore them. */
    gboolean was_visible = gtk_widget_get_visible(findbar);
    gtk_widget_set_visible(findbar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(find_entry));
    gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1);
    if (!was_visible)
        do_find(gtk_editable_get_text(GTK_EDITABLE(find_entry)));
}

static void find_hide(void)
{
    WebKitWebView* v = current_view();
    if (v != NULL)
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
    gtk_widget_set_visible(findbar, FALSE);
}

/* Fetch the page's serialized HTML, then show it as plain text in a new tab. */
static void on_source_ready(GObject* obj, GAsyncResult* res, gpointer data)
{
    WebKitWebView* view = WEBKIT_WEB_VIEW(obj);
    GError* err = NULL;
    JSCValue* val = webkit_web_view_evaluate_javascript_finish(view, res, &err);
    if (val == NULL) {
        if (err != NULL) g_error_free(err);
        return;
    }
    char* html = jsc_value_to_string(val);
    const char* base = webkit_web_view_get_uri(view);

    WebKitWebView* src = append_tab(NULL);
    GBytes* bytes = g_bytes_new(html, strlen(html));
    webkit_web_view_load_bytes(src, bytes, "text/plain", "utf-8", base);

    g_bytes_unref(bytes);
    g_free(html);
    g_object_unref(val);
}

/* ------------------------------------------------------------- shortcuts */
static void handle_shortcut(func id)
{
    static double zoom = 1.0;
    WebKitWebView* view = current_view();

    switch (id) {
        case goback:
            if (view) webkit_web_view_go_back(view);
            break;
        case goforward:
            if (view) webkit_web_view_go_forward(view);
            break;
        case refresh:
            if (view) webkit_web_view_reload(view);
            break;
        case refresh_force:
            if (view) webkit_web_view_reload_bypass_cache(view);
            break;
        case zoomin:
            if (view) webkit_web_view_set_zoom_level(view, (zoom += ZOOM_STEPSIZE));
            break;
        case zoomout:
            if (view) webkit_web_view_set_zoom_level(view, (zoom -= ZOOM_STEPSIZE));
            break;
        case zoom_reset:
            if (view) webkit_web_view_set_zoom_level(view, (zoom = 1.0));
            break;
        case tab_up: { /* up the list; do nothing if already at the top */
            int k = gtk_notebook_get_current_page(notebook);
            if (k > 0)
                gtk_notebook_set_current_page(notebook, k - 1);
            break;
        }
        case tab_down: { /* down the list; do nothing if already at the bottom */
            int n = gtk_notebook_get_n_pages(notebook);
            int k = gtk_notebook_get_current_page(notebook);
            if (k < n - 1)
                gtk_notebook_set_current_page(notebook, k + 1);
            break;
        }
        case last_tab: { /* alt+tab: walk back through the most-recently-used tabs */
            if (mru_len < 2)
                break;
            if (alt_walk < 0)
                alt_walk = 0; /* start the walk at the current (front) tab */
            /* Step to the next entry, wrapping, skipping any whose page has gone. */
            int k = -1;
            for (int tries = 0; tries < mru_len; tries++) {
                alt_walk = (alt_walk + 1) % mru_len;
                k = gtk_notebook_page_num(notebook, mru[alt_walk]);
                if (k >= 0)
                    break;
            }
            if (k >= 0) {
                GtkWidget* target = mru[alt_walk];
                alt_switch = TRUE; /* don't let this switch reshuffle the MRU order */
                gtk_notebook_set_current_page(notebook, k);
                alt_switch = FALSE;
                revive_if_dead(WEBKIT_WEB_VIEW(target)); /* wake it if it was slept */
            }
            break;
        }
        case new_tab:
            modal_show(MODAL_SEARCH, TRUE);
            if (MAX_NUM_TABS != 0 && num_tabs >= MAX_NUM_TABS) {
                gtk_label_set_text(modal_info, "Tab limit reached");
                gtk_widget_set_visible(GTK_WIDGET(modal_info), TRUE);
                gtk_widget_set_visible(GTK_WIDGET(modal_entry1), FALSE); /* message only */
                gtk_widget_set_visible(GTK_WIDGET(modal_results), FALSE); /* drop the empty box's spacing below the label */
                modal_blocked = TRUE;
            }
            break;
        case close_tab:
            close_current_tab();
            break;
        case reopen_tab:
            reopen_closed_tab();
            break;
        case show_finder:
            /* Ctrl+F only opens/focuses the bar and highlights matches; it never
             * moves through results -- only Enter advances to the next match. */
            find_show();
            break;
        case find_reset:
            do_find(gtk_editable_get_text(GTK_EDITABLE(find_entry)));
            break;
        case bookmark_add:
            modal_show(MODAL_BOOKMARK, FALSE);
            break;
        case edit_uri: {
            /* Open the search modal pre-filled with the current URL; Enter
             * replaces the current tab (modal_new_tab = FALSE). */
            modal_show(MODAL_SEARCH, FALSE);
            const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
            if (uri != NULL) {
                gtk_editable_set_text(GTK_EDITABLE(modal_entry1), uri);
                gtk_editable_select_region(GTK_EDITABLE(modal_entry1), 0, -1);
            }
            break;
        }
        case toggle_tabs:
            tabbar_visible = !tabbar_visible;
            gtk_widget_set_visible(GTK_WIDGET(tabbar), tabbar_visible);
            break;
        case view_source:
            if (view)
                webkit_web_view_evaluate_javascript(view,
                    "document.documentElement.outerHTML", -1, NULL, NULL, NULL,
                    on_source_ready, NULL);
            break;
        case print_page:
            if (view) {
                WebKitPrintOperation* print = webkit_print_operation_new(view);
                webkit_print_operation_run_dialog(print, window);
                g_object_unref(print);
            }
            break;
        case reading_mode:
            /* Apply the reader transform (refresh the page to undo it). */
            if (view) {
                char* js = read_readability_js();
                if (js != NULL) {
                    webkit_web_view_evaluate_javascript(view, js, -1, NULL, "lightbrowse-readability", NULL, NULL, NULL);
                    g_free(js);
                }
            }
            break;
        case translate_page: {
            /* Replace the current tab with Google Translate's whole-page proxy
             * of the current URL (auto source -> English). */
            const char* uri = view ? webkit_web_view_get_uri(view) : NULL;
            if (uri != NULL && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
                char* enc = g_uri_escape_string(uri, NULL, TRUE);
                char* translated = g_strdup_printf(
                    "https://translate.google.com/translate?sl=auto&tl=en&u=%s", enc);
                webkit_web_view_load_uri(view, translated);
                g_free(enc);
                g_free(translated);
            }
            break;
        }
        case fill_password: {
            /* Open the password picker for the current page's host. */
            if (view == NULL)
                break;
            pass_host[0] = '\0';
            const char* uri = webkit_web_view_get_uri(view);
            if (uri != NULL) {
                GUri* u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
                if (u != NULL) {
                    const char* host = g_uri_get_host(u);
                    if (host != NULL)
                        g_strlcpy(pass_host, host, sizeof pass_host);
                    g_uri_unref(u);
                }
            }
            pass_target = view;
            modal_show(MODAL_PASSWORD, FALSE);
            password_refresh_rows("");
            break;
        }
    }
}

/* ----------------------------------------------------------------- keys */
static gboolean handle_signal_keypress(GtkEventControllerKey* self, guint keyval,
    guint keycode, GdkModifierType state, gpointer user_data)
{
    /* A pending permission prompt (mic/camera or storage access) owns the keyboard
     * until it's answered: Enter allows, Esc denies, and every other key is swallowed
     * so nothing acts on the page (or closes the tab) out from under the request. */
    if (perm_current != NULL) {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
            perm_decide(TRUE);
        else if (keyval == GDK_KEY_Escape)
            perm_decide(FALSE);
        return TRUE;
    }

    if (keyval == GDK_KEY_Escape) {
        if (modal_mode != MODAL_NONE) {
            modal_hide();
            return TRUE;
        }
        if (gtk_widget_get_visible(findbar)) {
            find_hide();
            return TRUE;
        }
        return FALSE;
    }

    /* Shift+Enter in the find bar steps backwards through matches. */
    if (gtk_widget_get_visible(findbar) && (state & SFT) && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        find_prev();
        return TRUE;
    }

    if (modal_mode != MODAL_NONE) {
        if (modal_mode == MODAL_SEARCH || modal_mode == MODAL_PASSWORD) {
            if (keyval == GDK_KEY_Down) {
                modal_move_sel(1);
                return TRUE;
            }
            if (keyval == GDK_KEY_Up) {
                modal_move_sel(-1);
                return TRUE;
            }
        }
        if (modal_mode == MODAL_SEARCH) {
            /* Shift+Enter sends a live calculation to Wolfram|Alpha (as if the
             * user typed "wa <expr>"); otherwise it jumps straight to the top
             * fuzzy bookmark match, skipping the typed-text search. */
            if ((state & SFT) && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
                if (calc_active) {
                    char* tmp = g_strconcat("wa ", gtk_editable_get_text(GTK_EDITABLE(modal_entry1)), NULL);
                    char* url = shortcut_expand(tmp);
                    g_free(tmp);
                    if (url != NULL) {
                        modal_open_search_uri(url);
                        g_free(url);
                    }
                } else if (fuzzy_count > 0) {
                    modal_open_search_uri(fuzzy_urls[0]);
                }
                return TRUE;
            }
        } else if (keyval == GDK_KEY_Tab) {
            GtkWidget* focus = gtk_window_get_focus(window);
            gtk_widget_grab_focus(focus == GTK_WIDGET(modal_entry1)
                    ? GTK_WIDGET(modal_entry2)
                    : GTK_WIDGET(modal_entry1));
            return TRUE;
        }
        return FALSE; /* let the entries handle typing; skip global shortcuts */
    }

    for (size_t i = 0; i < sizeof(shortcut) / sizeof(shortcut[0]); i++) {
        if ((state & shortcut[i].mod || shortcut[i].mod == 0x0) && keyval == shortcut[i].key) {
            handle_shortcut(shortcut[i].id);
            return TRUE;
        }
    }
    return FALSE;
}

/* Releasing Alt commits the alt+tab walk: the tab the user landed on becomes the
 * new most-recent. Deferring the reshuffle to here is what lets repeated Tab
 * presses step deeper into history rather than just toggling the top two. */
static void handle_signal_keyrelease(GtkEventControllerKey* self G_GNUC_UNUSED,
    guint keyval, guint keycode G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED,
    gpointer user_data G_GNUC_UNUSED)
{
    if ((keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R)
        && alt_walk > 0 && alt_walk < mru_len) {
        mru_promote(mru[alt_walk]);
        alt_walk = -1;
    }
}

/* --------------------------------------------------------------- theme */
/* The chrome is always dark: every visible chrome widget hardcodes Nord colours
 * in CSS, so the UI never follows the system light/dark. The GTK theme itself is
 * deliberately left to follow the system (gtk-application-prefer-dark-theme),
 * because WebKit maps that to each page's prefers-color-scheme — forcing the GTK
 * theme dark would drag every website dark too. So: chrome dark via CSS, websites
 * dark/light via the (untouched) system theme. */

static void apply_color_scheme(void)
{
    /* The sole purpose of this is to let websites inherit the system light/dark;
     * the chrome is unaffected (its colours are hardcoded in CSS). */
    char* scheme = g_settings_get_string(iface_settings, "color-scheme");
    gboolean dark = g_strcmp0(scheme, "prefer-dark") == 0;
    g_free(scheme);

    g_object_set(gtk_settings_get_default(),
        "gtk-application-prefer-dark-theme", dark, NULL);
}

static void setup_theme(void)
{
    GSettingsSchemaSource* src = g_settings_schema_source_get_default();
    if (src == NULL)
        return;
    GSettingsSchema* schema = g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (schema == NULL)
        return; /* this check ONLY needed for development!!!! nix develop */
    g_settings_schema_unref(schema);

    iface_settings = g_settings_new("org.gnome.desktop.interface");
    apply_color_scheme();
    g_signal_connect(iface_settings, "changed::color-scheme", G_CALLBACK(apply_color_scheme), NULL);
}

/* ------------------------------------------------------------------ build */
static void build_modal(void)
{
    dim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(dim, "dim");
    gtk_widget_set_can_target(dim, FALSE);
    gtk_widget_set_visible(dim, FALSE);
    gtk_overlay_add_overlay(overlay, dim);

    modal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); /* gap between name + url entries */
    gtk_widget_add_css_class(modal_box, "modal");
    gtk_widget_set_halign(modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(modal_box, FALSE);

    modal_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(modal_info), GTK_ALIGN_START);
    modal_entry1 = GTK_ENTRY(gtk_entry_new());
    modal_entry2 = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_visible(GTK_WIDGET(modal_entry2), FALSE);
    /* Entry + bookmark rows share one box (spacing 4) so the gap shows only
     * between present children — no dangling space under the entry when empty. */
    modal_results = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
    gtk_box_append(modal_results, GTK_WIDGET(modal_entry1));
    /* Persistent calc-result label, pinned right under the entry and above any
     * bookmark rows. Hidden until the typed text is a valid expression. */
    calc_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(calc_label), "calc");
    gtk_label_set_xalign(calc_label, 0.0);
    gtk_widget_set_visible(GTK_WIDGET(calc_label), FALSE);
    gtk_box_append(modal_results, GTK_WIDGET(calc_label));

    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_info));
    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_results));
    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_entry2));

    g_signal_connect(modal_entry1, "changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(modal_entry1, "activate", G_CALLBACK(on_modal_activate), NULL);
    g_signal_connect(modal_entry2, "activate", G_CALLBACK(on_modal_activate), NULL);
    gtk_overlay_add_overlay(overlay, modal_box);
}

static void build_findbar(void)
{
    findbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(findbar, "findbar");
    gtk_widget_set_halign(findbar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(findbar, GTK_ALIGN_END);
    gtk_widget_set_visible(findbar, FALSE);

    find_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(find_entry, "Find");
    find_label = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(findbar), GTK_WIDGET(find_entry));
    gtk_box_append(GTK_BOX(findbar), GTK_WIDGET(find_label));

    g_signal_connect(find_entry, "changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_activate), NULL);
    gtk_overlay_add_overlay(overlay, findbar);
}

/* ------------------------------------------------------------ application */
static GtkApplication* app;

/* Window manager / sway close (the X): save the open tabs, then allow it. */
static gboolean on_close_request(GtkWindow* w, gpointer data)
{
    session_save();
    return FALSE;
}

/* SIGTERM/SIGINT (logout, poweroff, Ctrl-C): save the session and quit cleanly
 * so tabs survive a shutdown. */
static gboolean on_term_signal(gpointer data)
{
    session_save();
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

/* Build the window, UI and one blank tab. Idempotent and runs one-time global
 * init on the first call; later calls (a second `open`/`activate` on the
 * already-running instance) are no-ops. */
static void ensure_window(void)
{
    if (window != NULL)
        return;

    g_mkdir_with_parents(DATA_DIR, 0700);
    bookmarks_load(BOOKMARKS_DIR);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", true, NULL);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    /* Above PRIORITY_USER (800) so our custom-class rules beat the theme's
     * ~/.config/gtk-4.0/gtk.css (loaded at USER), e.g. the tab button radius. */
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

    window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_default_size(window, 1100, 700); /* initial size only; never a resize limit */
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);

    notebook = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_show_tabs(notebook, FALSE);
    gtk_notebook_set_show_border(notebook, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(notebook), "webarea");
    gtk_widget_set_hexpand(GTK_WIDGET(notebook), TRUE);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);

    tabbar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_add_css_class(GTK_WIDGET(tabbar), "tabbar");
    gtk_box_set_spacing(tabbar, 4);

    /* Status label (hugs its text) over a thin progress bar, floated at the bottom
     * of the webview so only the label's own background shows, not a full-width bar. */
    status_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(status_label, 0.0);
    gtk_label_set_ellipsize(status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(GTK_WIDGET(status_label), GTK_ALIGN_START);

    progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(progress), "loadbar");

    download_progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(download_progress), "downloadbar");
    gtk_widget_set_visible(GTK_WIDGET(download_progress), FALSE);

    statusbar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(statusbar, "statusbar");
    gtk_widget_set_valign(statusbar, GTK_ALIGN_END);
    gtk_widget_set_can_target(statusbar, FALSE); /* clicks fall through to the page */
    gtk_widget_set_visible(statusbar, FALSE);
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(status_label));
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(progress));
    gtk_box_append(GTK_BOX(statusbar), GTK_WIDGET(download_progress));

    GtkWidget* content = gtk_overlay_new();
    gtk_widget_set_hexpand(content, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(content), GTK_WIDGET(notebook));
    gtk_overlay_add_overlay(GTK_OVERLAY(content), statusbar);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(hbox), GTK_WIDGET(tabbar));
    gtk_box_append(GTK_BOX(hbox), content);

    overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_overlay_set_child(overlay, hbox);
    gtk_window_set_child(window, GTK_WIDGET(overlay));

    build_modal();
    build_findbar();

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(handle_signal_keypress), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(handle_signal_keyrelease), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);

    setup_theme();
#if ADBLOCK_ENABLED
    /* Start compiling the ad-block content filters now so they're ready by (or
     * shortly after) the first navigation. Idempotent — safe on re-activation. */
    adblock_content_init(ADBLOCK_FILTERS_DIR, ADBLOCK_STORE_DIR);
#endif
    /* Sleep idle tabs only under real memory pressure, not when RAM is merely full (see sleep_sweep). */
    g_timeout_add_seconds(TAB_SLEEP_SWEEP_SECONDS, sleep_sweep, NULL);
    /* No tab is created here: the caller paints the window/modal first, then warms
     * the web process on idle, so the search box appears without waiting on WebKit. */
}

/* Spin up the web process once the window/modal has been shown. */
static gboolean warm_blank_tab(gpointer data)
{
    if (notebook != NULL && gtk_notebook_get_n_pages(notebook) == 0)
        notebook_create_new_tab(NULL);
    return G_SOURCE_REMOVE;
}

/* Launched with no URL (or a second plain invocation): show the search modal. */
static void on_activate(GApplication* application, gpointer data)
{
    gboolean fresh = (window == NULL);
    ensure_window();
    if (fresh && !session_restore()) { /* restore last tabs, else a blank search */
        modal_show(MODAL_SEARCH, FALSE);
        g_idle_add(warm_blank_tab, NULL); /* paint the modal first, warm WebKit after */
    }
    gtk_window_present(window);
}

/* Launched/activated with URLs (default-browser link handling). No modal. */
static void on_open(GApplication* application, GFile** files, gint n_files, const char* hint, gpointer data)
{
    ensure_window();
    for (gint i = 0; i < n_files; i++) {
        char* uri = g_file_get_uri(files[i]);
        notebook_create_new_tab(uri);
        g_free(uri);
    }
    gtk_window_present(window);
}

int main(int argc, char** argv)
{
    app = gtk_application_new("com.amazinaxel.lightbrowse", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    g_unix_signal_add(SIGTERM, on_term_signal, NULL); /* save tabs on shutdown */
    g_unix_signal_add(SIGINT, on_term_signal, NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
