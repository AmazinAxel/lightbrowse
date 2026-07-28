/* Reverse image search from the clipboard.
 *
 * Google Lens takes an image as a multipart POST and answers with a redirect to
 * the results page -- there is no GET URL we could just navigate to, and WebKit
 * can't issue a POST for a top-level load anyway. So the upload is done here with
 * libsoup: redirects are switched off so the 302's Location *is* the answer, and
 * the response body is thrown away. The caller then loads that URL normally. */

#include "imagesearch.h"

#include <libsoup/soup.h>

#define LENS_UPLOAD_URI "https://lens.google.com/upload"
/* Lens serves a "your browser is unsupported" page to non-browser agents. */
#define LENS_USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15"
/* Lens rejects oversized uploads; a screenshot-sized PNG is well under this. */
#define MAX_UPLOAD_BYTES (16 * 1024 * 1024)

typedef struct {
    ImageSearchDone done;
    gpointer user_data;
} Request;

/* Shared across uploads so the TLS session to Lens is reused; created lazily
 * because most sessions never paste an image. */
static SoupSession* upload_session(void)
{
    static SoupSession* session = NULL;
    if (session == NULL)
        session = soup_session_new_with_options("user-agent", LENS_USER_AGENT, NULL);
    return session;
}

static void request_finish(Request* req, const char* uri, const char* error)
{
    req->done(uri, error, req->user_data);
    g_free(req);
}

static void on_upload_response(GObject* source, GAsyncResult* res, gpointer data)
{
    Request* req = data;
    GError* err = NULL;
    GBytes* body = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);
    if (body != NULL)
        g_bytes_unref(body); /* the redirect's body is a stub page; only Location matters */
    if (err != NULL) {
        char* msg = g_strdup(err->message);
        g_error_free(err);
        request_finish(req, NULL, msg);
        g_free(msg);
        return;
    }

    SoupMessage* msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
    const char* location = soup_message_headers_get_one(soup_message_get_response_headers(msg), "Location");
    if (location == NULL) {
        request_finish(req, NULL, "Lens did not return a results page");
        return;
    }
    /* Location is absolute in practice, but resolve it against the request URI
     * so a relative one still works. */
    GUri* target = g_uri_parse_relative(soup_message_get_uri(msg), location, SOUP_HTTP_URI_FLAGS, NULL);
    if (target == NULL) {
        request_finish(req, NULL, "Lens returned an unreadable results URL");
        return;
    }
    char* uri = g_uri_to_string(target);
    g_uri_unref(target);
    request_finish(req, uri, NULL);
    g_free(uri);
}

static void upload_png(GBytes* png, Request* req)
{
    SoupMultipart* form = soup_multipart_new(SOUP_FORM_MIME_TYPE_MULTIPART);
    soup_multipart_append_form_file(form, "encoded_image", "image.png", "image/png", png);

    SoupMessage* msg = soup_message_new("POST", LENS_UPLOAD_URI);
    GBytes* body = NULL;
    /* Writes the multipart Content-Type (with its boundary) into the request
     * headers, so the body is attached without a content type of its own. */
    soup_multipart_to_message(form, soup_message_get_request_headers(msg), &body);
    soup_multipart_free(form);
    soup_message_set_request_body_from_bytes(msg, NULL, body);
    g_bytes_unref(body);
    soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);

    soup_session_send_and_read_async(upload_session(), msg, G_PRIORITY_DEFAULT, NULL, on_upload_response, req);
    g_object_unref(msg);
}

static void on_clipboard_texture(GObject* source, GAsyncResult* res, gpointer data)
{
    Request* req = data;
    GError* err = NULL;
    GdkTexture* texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), res, &err);
    if (texture == NULL) {
        char* m = g_strdup(err != NULL ? err->message : "clipboard held no image");
        g_clear_error(&err);
        request_finish(req, NULL, m);
        g_free(m);
        return;
    }

    GBytes* png = gdk_texture_save_to_png_bytes(texture);
    g_object_unref(texture);
    if (png == NULL) {
        request_finish(req, NULL, "could not encode the pasted image");
        return;
    }
    if (g_bytes_get_size(png) > MAX_UPLOAD_BYTES) {
        g_bytes_unref(png);
        request_finish(req, NULL, "pasted image is too large to search");
        return;
    }
    upload_png(png, req);
    g_bytes_unref(png);
}

gboolean imagesearch_clipboard_has_image(GdkClipboard* clipboard)
{
    GdkContentFormats* formats = gdk_clipboard_get_formats(clipboard);
    if (formats == NULL)
        return FALSE;
    /* Text wins when both are offered: that paste has something to type, and
     * silently searching it as an image would be a surprise. */
    if (gdk_content_formats_contain_gtype(formats, G_TYPE_STRING)
        || gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8"))
        return FALSE;
    return gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE);
}

void imagesearch_from_clipboard(GdkClipboard* clipboard, ImageSearchDone done, gpointer user_data)
{
    Request* req = g_new0(Request, 1);
    req->done = done;
    req->user_data = user_data;
    gdk_clipboard_read_texture_async(clipboard, NULL, on_clipboard_texture, req);
}
