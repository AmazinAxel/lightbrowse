#include <gdk/gdk.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <webkit/webkit.h>

#include "config.h"
#include "plugins/plugins.h"

static const char* CSS =
    ".tabbar { background: shade(@theme_bg_color, 1.2); border-right: 0.25rem solid #5e81ac; padding: 4px; }"
    ".tab { padding: 6px; margin: 2px 0; min-height: 0; min-width: 0; border: 2px solid transparent; border-radius: 4px; outline: none; box-shadow: none; }"
    ".tab:hover, .tab:focus, .tab:active, .tab:checked { outline: none; box-shadow: none; }"
    ".tab.active { border-color: #5e81ac; }"
    ".dim { background: alpha(black, 0.3); }"
    ".modal { background: @theme_bg_color; color: @theme_fg_color; border: 2px solid #5e81ac; border-radius: 12px; padding: 6px; }"
    ".modal entry { min-width: 280px; }"
    ".modal label { margin: 3px 0; padding: 6px; }"
    ".modal .selected { background: #81A1C1; color: #ECEFF4; outline: 2px #5E81AC; border-radius: 4px; }"
    ".findbar { background: @theme_bg_color; color: @theme_fg_color; border: 1px solid #5e81ac; border-radius: 8px; padding: 4px; margin-bottom: 12px; }";

/* Global widgets */
static GtkWindow* window;
static GtkOverlay* overlay;
static GtkNotebook* notebook;
static GtkBox* tabbar; /* vertical favicon strip */
static gboolean tabbar_visible = TRUE;
static int num_tabs = 0;

/* Modal (search / bookmark) */
typedef enum { MODAL_NONE, MODAL_SEARCH, MODAL_BOOKMARK } ModalMode;
static ModalMode modal_mode = MODAL_NONE;
static gboolean modal_new_tab = FALSE; /* search: open in a new tab vs current */
static gboolean modal_blocked = FALSE; /* tab limit reached: don't open on submit */
static GtkWidget* dim;
static GtkWidget* modal_box;
static GtkLabel* modal_info;
static GtkEntry* modal_entry1; /* search text / bookmark name */
static GtkEntry* modal_entry2; /* bookmark url (hidden in search mode) */
static GtkBox* modal_results;
static const char* fuzzy_urls[FUZZY_RESULTS];
static guint fuzzy_count = 0;
static int fuzzy_sel = -1;

/* Find bar */
static GtkWidget* findbar;
static GtkEntry* find_entry;
static GtkLabel* find_label;
static guint find_total = 0;
static guint find_current = 0;

/* Closed-tab ring (full WebKit session state, so the webview is freed) */
static WebKitWebViewSessionState* closed_tabs[CLOSED_TAB_HISTORY];
static int closed_count = 0;

/* Forward declarations */
static void notebook_create_new_tab(const char* uri);
static WebKitWebView* current_view(void);
static void session_save(void);

/* ---------------------------------------------------------------- URI load */
static void load_uri(WebKitWebView* view, const char* uri)
{
    if (uri[0] == '\0')
        return;

    bool direct = g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://") || g_str_has_prefix(uri, "file://") || g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:");
    if (direct) {
        webkit_web_view_load_uri(view, uri);
        return;
    }

    if (strstr(uri, ".com") || strstr(uri, ".org")) {
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

    char* search = g_strdup_printf(SEARCH, uri);
    webkit_web_view_load_uri(view, search);
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
        if (ADBLOCK_ENABLED) {
            webkit_web_context_set_web_process_extensions_directory(context, ADBLOCK_EXTENSIONS_DIR);
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            webkit_web_context_set_web_process_extensions_initialization_user_data(
                context, g_variant_builder_end(&builder));
        }
    }
    return context;
}

static WebKitNetworkSession* get_shared_network_session(void)
{
    static WebKitNetworkSession* session = NULL;
    if (session == NULL) {
        session = webkit_network_session_new(DATA_DIR, DATA_DIR);
        WebKitCookieManager* cm = webkit_network_session_get_cookie_manager(session);
        webkit_cookie_manager_set_persistent_storage(cm, DATA_DIR "/cookies.sqlite", WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
        webkit_website_data_manager_set_favicons_enabled(
            webkit_network_session_get_website_data_manager(session), TRUE);
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

/* ----------------------------------------------------------------- tabs */
/* Switch on press (capture phase) rather than on the button's release-driven
 * "clicked", so mouse selection feels as instant as the keyboard. */
static void on_tab_pressed(GtkGestureClick* gesture, int n_press, double x, double y, WebKitWebView* view)
{
    int n = gtk_notebook_page_num(notebook, GTK_WIDGET(view));
    if (n >= 0)
        gtk_notebook_set_current_page(notebook, n);
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
}

static void on_counted_matches(WebKitFindController* fc, guint count, gpointer data);
static GtkWidget* on_create_tab(WebKitWebView* self, WebKitNavigationAction* action, gpointer data);

/* Bigger mouse-wheel steps: WebKit's default wheel scroll barely moves the page.
 * On a wheel notch, scroll the hovered element (or the page) by SCROLL_STEP_PX
 * and consume the native event. Touchpad / smooth scrolling passes through. The
 * `:hover` chain gives the element under the cursor without tracking the pointer. */
static gboolean on_view_scroll(GtkEventControllerScroll* c, double dx, double dy, gpointer data)
{
    if (SCROLL_STEP_PX == 0 || gtk_event_controller_scroll_get_unit(c) != GDK_SCROLL_UNIT_WHEEL)
        return FALSE; /* let touchpad / smooth scrolling through */

    char* js = g_strdup_printf(
        "(function(dx,dy){var h=document.querySelectorAll(':hover'),e=h[h.length-1];"
        "while(e){var s=getComputedStyle(e);"
        "if(e.scrollHeight>e.clientHeight&&/auto|scroll/.test(s.overflowY)){e.scrollBy(dx,dy);return;}"
        "e=e.parentElement;}window.scrollBy(dx,dy);})(%f,%f);",
        dx * SCROLL_STEP_PX, dy * SCROLL_STEP_PX);
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(data), js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    return TRUE; /* consume so WebKit doesn't also scroll */
}

/* Middle-click a link -> open it in a new tab (other clicks navigate normally). */
static gboolean on_decide_policy(WebKitWebView* view, WebKitPolicyDecision* decision,
    WebKitPolicyDecisionType type, gpointer data)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;
    WebKitNavigationAction* a = webkit_navigation_policy_decision_get_navigation_action(
        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    if (webkit_navigation_action_get_mouse_button(a) != 2) /* 2 = middle */
        return FALSE;
    notebook_create_new_tab(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

static WebKitWebView* append_tab(void)
{
    WebKitWebView* view = g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "settings", get_shared_settings(),
        "network-session", get_shared_network_session(),
        "web-context", get_shared_web_context(),
        NULL);
    NULLCHECK(view);

    webkit_settings_set_user_agent(get_shared_settings(),
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/605.1.15 (KHTML, like Gecko) "
        "Version/26.0 Safari/605.1.15");

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

    GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_view_scroll), view);
    gtk_widget_add_controller(GTK_WIDGET(view), scroll);

    g_signal_connect(view, "create", G_CALLBACK(on_create_tab), NULL);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_favicon_notify), NULL);
    g_signal_connect(webkit_web_view_get_find_controller(view), "counted-matches", G_CALLBACK(on_counted_matches), NULL);

    int n = gtk_notebook_append_page(notebook, GTK_WIDGET(view), NULL);
    gtk_notebook_set_current_page(notebook, n);
    num_tabs += 1;
    return view;
}

/* The MAX_NUM_TABS limit is only enforced by the ctrl+t shortcut; tabs opened
 * by a page (window.open / target=_blank) or from another app are never blocked. */
static void notebook_create_new_tab(const char* uri)
{
    WebKitWebView* view = append_tab();
    load_uri(view, uri ? uri : "about:blank"); /* about:blank spins up the web process */
}

static GtkWidget* on_create_tab(WebKitWebView* self, WebKitNavigationAction* action, gpointer data)
{
    WebKitURIRequest* req = webkit_navigation_action_get_request(action);
    webkit_web_view_stop_loading(self);
    notebook_create_new_tab(webkit_uri_request_get_uri(req));
    return ABORT_REQUEST_ON_CURRENT_TAB;
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
    WebKitWebView* view = append_tab();
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
static void clear_box(GtkBox* box)
{
    GtkWidget* c;
    while ((c = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, c);
}

static void modal_hide(void)
{
    modal_mode = MODAL_NONE;
    gtk_widget_set_visible(dim, FALSE);
    gtk_widget_set_visible(modal_box, FALSE);
    gtk_entry_set_attributes(modal_entry1, NULL);
    clear_box(modal_results);
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
    clear_box(modal_results);
    gtk_entry_set_attributes(modal_entry1, NULL);
    gtk_widget_set_visible(GTK_WIDGET(modal_entry1), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(modal_info), FALSE); /* only shown for "Tab limit reached" */

    if (mode == MODAL_SEARCH) {
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

static void modal_restyle_results(void)
{
    int i = 0;
    for (GtkWidget* c = gtk_widget_get_first_child(GTK_WIDGET(modal_results)); c != NULL; c = gtk_widget_get_next_sibling(c), i++) {
        if (i == fuzzy_sel)
            gtk_widget_add_css_class(c, "selected");
        else
            gtk_widget_remove_css_class(c, "selected");
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

static void on_search_changed(GtkEditable* editable, gpointer data)
{
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
    clear_box(modal_results);
    fuzzy_count = 0;
    fuzzy_sel = -1;
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

static void on_modal_activate(GtkEntry* entry, gpointer data)
{
    if (modal_blocked) {
        modal_hide();
        return;
    }
    if (modal_mode == MODAL_SEARCH) {
        const char* uri;
        if (fuzzy_sel >= 0)
            uri = fuzzy_urls[fuzzy_sel];
        else
            uri = gtk_editable_get_text(GTK_EDITABLE(modal_entry1));
        if (modal_new_tab || current_view() == NULL) /* no warm tab yet -> make one */
            notebook_create_new_tab(uri);
        else
            load_uri(current_view(), uri);
    } else { /* MODAL_BOOKMARK */
        const char* name = gtk_editable_get_text(GTK_EDITABLE(modal_entry1));
        const char* url = gtk_editable_get_text(GTK_EDITABLE(modal_entry2));
        if (name[0] != '\0' && url[0] != '\0')
            bookmarks_save(BOOKMARKS_DIR, name, url);
    }
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
    WebKitWebView* v = current_view();
    if (v == NULL)
        return;
    webkit_find_controller_search_next(webkit_web_view_get_find_controller(v));
    if (find_total > 0)
        find_current = (find_current % find_total) + 1;
    update_find_label();
}

static void find_prev(void)
{
    WebKitWebView* v = current_view();
    if (v == NULL)
        return;
    webkit_find_controller_search_previous(webkit_web_view_get_find_controller(v));
    if (find_total > 0)
        find_current = (find_current + find_total - 2) % find_total + 1;
    update_find_label();
}

static void on_find_activate(GtkEntry* entry, gpointer data)
{
    find_next(); /* Enter = next; Shift+Enter (handled in keypress) = previous */
}

static void find_show(void)
{
    gtk_widget_set_visible(findbar, TRUE);
    gtk_widget_grab_focus(GTK_WIDGET(find_entry));
    gtk_editable_select_region(GTK_EDITABLE(find_entry), 0, -1);
    do_find(gtk_editable_get_text(GTK_EDITABLE(find_entry)));
}

static void find_hide(void)
{
    WebKitWebView* v = current_view();
    if (v != NULL)
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(v));
    gtk_widget_set_visible(findbar, FALSE);
}

/* ------------------------------------------------------------- shortcuts */
static void handle_shortcut(func id)
{
    static double zoom = ZOOM_START_LEVEL;
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
            if (view) webkit_web_view_set_zoom_level(view, (zoom = ZOOM_START_LEVEL));
            break;
        case next_tab: {
            int n = gtk_notebook_get_n_pages(notebook);
            int k = gtk_notebook_get_current_page(notebook);
            gtk_notebook_set_current_page(notebook, (k + 1) % n);
            break;
        }
        case prev_tab: {
            int n = gtk_notebook_get_n_pages(notebook);
            int k = gtk_notebook_get_current_page(notebook);
            gtk_notebook_set_current_page(notebook, (n + k - 1) % n);
            break;
        }
        case new_tab:
            modal_show(MODAL_SEARCH, TRUE);
            if (MAX_NUM_TABS != 0 && num_tabs >= MAX_NUM_TABS) {
                gtk_label_set_text(modal_info, "Tab limit reached");
                gtk_widget_set_visible(GTK_WIDGET(modal_info), TRUE);
                gtk_widget_set_visible(GTK_WIDGET(modal_entry1), FALSE); /* message only */
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
        case reading_mode:
            /* Apply the reader transform (refresh the page to undo it). */
            if (READABILITY_ENABLED && view) {
                char* js = read_readability_js();
                if (js != NULL) {
                    webkit_web_view_evaluate_javascript(view, js, -1, NULL, "lightbrowse-readability", NULL, NULL, NULL);
                    g_free(js);
                }
            }
            break;
    }
}

/* ----------------------------------------------------------------- keys */
static gboolean handle_signal_keypress(GtkEventControllerKey* self, guint keyval,
    guint keycode, GdkModifierType state, gpointer user_data)
{
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
        if (modal_mode == MODAL_SEARCH) {
            if (keyval == GDK_KEY_Down) {
                modal_move_sel(1);
                return TRUE;
            }
            if (keyval == GDK_KEY_Up) {
                modal_move_sel(-1);
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

/* ---------------------------------------------------- system dark/light */
/* Follow the desktop's GSettings `color-scheme` live: set
 * gtk-application-prefer-dark-theme, which is all WebKit (and GTK) need to flip
 * dark/light. Same approach as the AGS sideview. */
static GSettings* iface_settings = NULL;

static void apply_system_theme(void)
{
    if (iface_settings == NULL)
        return;
    char* scheme = g_settings_get_string(iface_settings, "color-scheme");
    g_object_set(gtk_settings_get_default(),
        "gtk-application-prefer-dark-theme", g_strcmp0(scheme, "prefer-dark") == 0, NULL);
    g_free(scheme);
}

static void on_interface_changed(GSettings* s, const char* key, gpointer data)
{
    apply_system_theme();
}

static void setup_color_scheme(void)
{
    GSettingsSchemaSource* src = g_settings_schema_source_get_default();
    if (src == NULL)
        return;
    GSettingsSchema* schema = g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE);
    if (schema == NULL)
        return; /* schema not installed: leave GTK's own defaults in place */
    gboolean ok = g_settings_schema_has_key(schema, "gtk-theme") && g_settings_schema_has_key(schema, "color-scheme");
    g_settings_schema_unref(schema);
    if (!ok)
        return;

    iface_settings = g_settings_new("org.gnome.desktop.interface");
    apply_system_theme();
    g_signal_connect(iface_settings, "changed::gtk-theme", G_CALLBACK(on_interface_changed), NULL);
    g_signal_connect(iface_settings, "changed::color-scheme", G_CALLBACK(on_interface_changed), NULL);
}

/* ------------------------------------------------------------------ build */
static void build_modal(void)
{
    dim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(dim, "dim");
    gtk_widget_set_can_target(dim, FALSE);
    gtk_widget_set_visible(dim, FALSE);
    gtk_overlay_add_overlay(overlay, dim);

    modal_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(modal_box, "modal");
    gtk_widget_set_halign(modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(modal_box, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(modal_box, FALSE);

    modal_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(modal_info), GTK_ALIGN_START);
    modal_entry1 = GTK_ENTRY(gtk_entry_new());
    modal_entry2 = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_visible(GTK_WIDGET(modal_entry2), FALSE);
    modal_results = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_info));
    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_entry1));
    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_entry2));
    gtk_box_append(GTK_BOX(modal_box), GTK_WIDGET(modal_results));

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
    g_object_set(gtk_settings_get_default(), GTK_SETTINGS_CONFIG_H, NULL);

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
    gtk_widget_set_hexpand(GTK_WIDGET(notebook), TRUE);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);

    tabbar = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_widget_add_css_class(GTK_WIDGET(tabbar), "tabbar");

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(hbox), GTK_WIDGET(tabbar));
    gtk_box_append(GTK_BOX(hbox), GTK_WIDGET(notebook));

    overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_overlay_set_child(overlay, hbox);
    gtk_window_set_child(window, GTK_WIDGET(overlay));

    build_modal();
    build_findbar();

    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(handle_signal_keypress), NULL);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);

    setup_color_scheme();
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
