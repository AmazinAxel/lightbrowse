#pragma once
#include <glib.h>

/* Password autofill backed by the `pass` store (passwordstore.org).
 *
 * The store location is $PASSWORD_STORE_DIR, or ~/.password-store by default.
 * These functions only read the store and shell out to `pass show`; the picker
 * UI and the form injection live in lightbrowse.c. */

/* Enumerate the store and fill up to `max` matching entry paths (e.g.
 * "websites/github.com") into `entries`, ranked for the current page.
 *
 * With an empty `query` (picker just opened), entries whose path contains
 * `host` — or its registrable label — come first. Once the user types, `query`
 * is matched as a case-insensitive subsequence and takes priority. `host` may
 * be NULL. Returned pointers are owned by the plugin and valid until the next
 * call. Returns how many matched. */
guint passwords_match(const char* host, const char* query, const char** entries, guint max);

/* Result of decrypting an entry. `username` and/or `password` may be NULL. Both
 * strings are owned by the plugin and only valid for the duration of the call —
 * copy anything you need to keep. */
typedef void (*PassCB)(const char* username, const char* password, gpointer user_data);

/* Run `pass show <entry>` asynchronously (gpg-agent may pop a pinentry, so this
 * never blocks the UI) and deliver the parsed username/password to `cb`. The
 * password is line 1; the username is the first `login:`/`user:`/`username:`/
 * `email:` field, else the entry's trailing path component when that isn't a
 * bare domain. */
void passwords_show_async(const char* entry, PassCB cb, gpointer user_data);
