#include <stdbool.h>
#include <gtk/gtk.h>

#define ZOOM_STEPSIZE .1
#define MAX_NUM_TABS 16 // 0 for inf tabs
#define CLOSED_TAB_HISTORY 3 // how many closed tabs can be reopened (ctrl+shift+t)
#define MRU_HISTORY 4 // how many tabs alt+tab walks back through (most-recently-used order)
/* Tab sleeping is driven by real memory *pressure*, not free-RAM headroom: a
 * machine with plenty of swap is meant to sit near 100% RAM (the kernel uses it
 * as cache), so "low on free RAM" is a false alarm there. Instead we watch the
 * kernel's PSI stall metric (/proc/pressure/memory), which only climbs when tasks
 * actually stall waiting on memory — i.e. genuine thrash / OOM risk — and is
 * swap-aware by construction. A swap-backed laptop therefore browses at full
 * speed and tabs sleep only when the machine is truly struggling. A slept tab's
 * web process is freed (RAM reclaimed), its button dims to 50%, and reselecting
 * it reloads the page. */
#define TAB_SLEEP_PRESSURE 12.0       // %% of the last 10s spent stalled on memory (PSI "some avg10") above which we sleep the least-recently-used background tab
#define TAB_SLEEP_PRESSURE_CRITICAL 35.0 // above this the machine is thrashing: sleep every background tab at once to head off an imminent OOM
#define TAB_SLEEP_OOM_FLOOR_MB 256    // last-resort backstop: if free RAM *plus* free swap drops below this (nowhere left to put pages), dump every background tab regardless of PSI
#define TAB_SLEEP_MIN_AGE_SECONDS 45  // never sleep a tab you were looking at more recently than this, even under pressure (keeps tab-switching snappy)
#define TAB_SLEEP_SWEEP_SECONDS 8     // how often to check pressure
#define FUZZY_RESULTS 3 // max bookmark suggestions shown in the search modal
#define TAB_ICON_SIZE 24 // px, favicon size in the vertical tab strip
#define SEARCH "https://bing.com/search?q=%s&form=QBRE" // form=QBRE = "typed in the search box", which Bing ranks better
/* The UI chrome is always dark — its Nord colours are hardcoded in CSS (see the
 * CSS block in lightbrowse.c), not via a GTK theme name. The GTK theme itself is
 * left to follow the system so websites' prefers-color-scheme stays correct. */

#define DATA_DIR "/home/alec/.config/lightbrowse"
#define BOOKMARKS_DIR DATA_DIR "/bookmarks"
#define SESSION_FILE DATA_DIR "/session" // open tab URLs, restored on next launch
#define DOWNLOADS_DIR "/home/alec/Downloads" // saved downloads (assumed to exist)

/* Runtime asset directory.
 * The Nix package overrides this at build time (-DLIGHTBROWSE_SHARE_DIR=...)
 * to point into the store ($out/share/lightbrowse); the makefile points it at
 * the in-repo dev tree (out/share/lightbrowse). */
#ifndef LIGHTBROWSE_SHARE_DIR
#define LIGHTBROWSE_SHARE_DIR "/opt/lightbrowse"
#endif

/* Plugins */
#define ADBLOCK_ENABLED 1
/* WebKit content-blocker JSON (combined-part*.json + a `version` marker),
 * generated at build time from EasyList/uBO lists by the ublock-webkit-filters
 * converter. */
#define ADBLOCK_FILTERS_DIR LIGHTBROWSE_SHARE_DIR "/adblock"
/* Writable cache for WebKit's compiled (serialized) filters, keyed off the
 * shipped list version so an update recompiles once then reloads from cache. */
#define ADBLOCK_STORE_DIR DATA_DIR "/adblock-store"

#define WEBKIT_DEFAULT_SETTINGS \
    "enable-developer-extras", true, \
    "allow-modal-dialogs", true, \
    "enable-encrypted-media", true, \
    "enable-media-capabilities", true, \
    "enable-media-stream", true, \
    "enable-webrtc", true, \
    /* Resolve hostnames for in-page links (and honour <link rel=dns-prefetch>) so a
     * click's DNS lookup is already done -- complements the manual hover prefetch.
     * Off by default in WebKitGTK. */ \
    "enable-dns-prefetching", true, \
    "default-charset", "utf-8"

#define KEY(x) GDK_KEY_##x
#define SFT  1 << 0
#define CTRL 1 << 2
#define ALT  1 << 3

/* Misc helpers */
#define NULLCHECK(x)                                   \
    do {                                               \
        if (x == NULL) {                               \
            printf("\nNULL check not passed");         \
            printf("@ %s (%d): ", __FILE__, __LINE__); \
            exit(0);                                   \
        }                                              \
    } while (0)

/* keybinds */
typedef enum {
    goback,
    goforward,

    refresh,
    refresh_force,

    zoomin,
    zoomout,
    zoom_reset,

    new_tab,
    tab_up,
    tab_down,
    last_tab,
    close_tab,
    reopen_tab,

    show_finder,
    find_reset,

    bookmark_add,
    edit_uri,
    toggle_tabs,
    view_source,

    print_page,

    reading_mode,

    translate_page,

    fill_password
} func;

static struct {
    unsigned mod;
    unsigned key;
    func id;
} shortcut[] = {
    { CTRL,        KEY(h),             goback               },
    { ALT,         KEY(Left),          goback               },
    { CTRL,        KEY(j),             goforward            },
    { ALT,         KEY(Right),         goforward            },

    { CTRL,        KEY(r),             refresh              },
    { CTRL,        KEY(R),             refresh_force        },

    { CTRL,        KEY(p),             reading_mode         },
    { CTRL,        KEY(m),             edit_uri             },

    { CTRL,        KEY(equal),         zoomin               },
    { CTRL,        KEY(minus),         zoomout              },
    { CTRL,        KEY(0),             zoom_reset           },

    { ALT,         KEY(Tab),           last_tab             },
    { ALT,         KEY(Up),            tab_up               },
    { ALT,         KEY(Down),          tab_down             },
    { CTRL,        KEY(t),             new_tab              },
    { CTRL,        KEY(w),             close_tab            },
    { CTRL,        KEY(T),             reopen_tab           },

    { CTRL,        KEY(f),             show_finder          },
    { CTRL,        KEY(F),             find_reset           },

    { CTRL,        KEY(b),             bookmark_add         },
    { CTRL,        KEY(x),             toggle_tabs          },
    { CTRL,        KEY(g),             translate_page       },
    { CTRL,        KEY(e),             view_source          },
    { CTRL,        KEY(l),             fill_password        },

    { CTRL,        KEY(P),             print_page           },
};
