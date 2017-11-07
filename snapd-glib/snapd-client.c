/*
 * Copyright (C) 2016 Canonical Ltd.
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2 or version 3 of the License.
 * See http://www.gnu.org/copyleft/lgpl.html the full text of the license.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "snapd-client.h"
#include "snapd-alias.h"
#include "snapd-app.h"
#include "snapd-assertion.h"
#include "snapd-channel.h"
#include "snapd-error.h"
#include "snapd-json.h"
#include "snapd-login.h"
#include "snapd-plug.h"
#include "snapd-screenshot.h"
#include "snapd-slot.h"
#include "snapd-task.h"

/**
 * SECTION:snapd-client
 * @short_description: Client connection to snapd
 * @include: snapd-glib/snapd-glib.h
 *
 * A #SnapdClient is the means of talking to snapd.
 *
 * To communicate with snapd create a client with snapd_client_new() then
 * send requests.
 *
 * Some requests require authorization which can be set with
 * snapd_client_set_auth_data().
 */

/**
 * SnapdClient:
 *
 * #SnapdClient contains connection state with snapd.
 *
 * Since: 1.0
 */

/**
 * SnapdClientClass:
 *
 * Class structure for #SnapdClient.
 */

/**
 * SECTION:snapd-version
 * @short_description: Library version information
 * @include: snapd-glib/snapd-glib.h
 *
 * Programs can check if snapd-glib feature is enabled by checking for the
 * existance of a define called SNAPD_GLIB_VERSION_<version>, i.e.
 *
 * |[<!-- language="C" -->
 * #ifdef SNAPD_GLIB_VERSION_1_14
 * confinement = snapd_system_information_get_confinement (info);
 * #endif
 * ]|
 */

typedef struct
{
    /* Socket path to connect to */
    gchar *socket_path;

    /* Socket to communicate with snapd */
    GSocket *snapd_socket;

    /* User agent to send to snapd */
    gchar *user_agent;

    /* Authentication data to send with requests to snapd */
    SnapdAuthData *auth_data;

    /* Outstanding requests */
    GMutex requests_mutex;
    GList *requests;

    /* Whether to send the X-Allow-Interaction request header */
    gboolean allow_interaction;

    /* Data received from snapd */
    GMutex buffer_mutex;
    GByteArray *buffer;
    gsize n_read;
} SnapdClientPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (SnapdClient, snapd_client, G_TYPE_OBJECT)

/* snapd API documentation is at https://github.com/snapcore/snapd/wiki/REST-API */

/* Default socket to connect to */
#define SNAPD_SOCKET "/run/snapd.socket"

/* Number of bytes to read at a time */
#define READ_SIZE 1024

/* Number of milliseconds to poll for status in asynchronous operations */
#define ASYNC_POLL_TIME 100

G_DECLARE_DERIVABLE_TYPE (SnapdRequest, snapd_request, SNAPD, REQUEST, GObject)

struct _SnapdRequestClass
{
    GObjectClass parent_class;

    SoupMessage *(*generate_request)(SnapdRequest *request);
    void (*parse_response)(SnapdRequest *request);
};

typedef struct
{
    GMainContext *context;

    SnapdClient *client;

    SoupMessage *message;

    GSource *read_source;

    GCancellable *cancellable;
    gulong cancelled_id;

    gboolean responded;
    GAsyncReadyCallback ready_callback;
    gpointer ready_callback_data;

    GError *error;
} SnapdRequestPrivate;

static void snapd_request_async_result_init (GAsyncResultIface *iface);

G_DEFINE_TYPE_WITH_CODE (SnapdRequest, snapd_request, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_RESULT, snapd_request_async_result_init)
                         G_ADD_PRIVATE (SnapdRequest))

struct _SnapdRequestGetChange
{
    SnapdRequest parent_instance;
    gchar *change_id;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetChange, snapd_request_get_change, SNAPD, REQUEST_GET_CHANGE, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetChange, snapd_request_get_change, snapd_request_get_type ())

struct _SnapdRequestPostChange
{
    SnapdRequest parent_instance;
    gchar *change_id;
    gchar *action;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostChange, snapd_request_post_change, SNAPD, REQUEST_POST_CHANGE, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostChange, snapd_request_post_change, snapd_request_get_type ())

struct _SnapdRequestGetSystemInfo
{
    SnapdRequest parent_instance;
    SnapdSystemInformation *system_information;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetSystemInfo, snapd_request_get_system_info, SNAPD, REQUEST_GET_SYSTEM_INFO, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetSystemInfo, snapd_request_get_system_info, snapd_request_get_type ())

struct _SnapdRequestPostLogin
{
    SnapdRequest parent_instance;
    gchar *username;
    gchar *password;
    gchar *otp;
    SnapdAuthData *auth_data;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostLogin, snapd_request_post_login, SNAPD, REQUEST_POST_LOGIN, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostLogin, snapd_request_post_login, snapd_request_get_type ())

struct _SnapdRequestGetSnaps
{
    SnapdRequest parent_instance;
    GPtrArray *snaps;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetSnaps, snapd_request_get_snaps, SNAPD, REQUEST_GET_SNAPS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetSnaps, snapd_request_get_snaps, snapd_request_get_type ())

struct _SnapdRequestGetSnap
{
    SnapdRequest parent_instance;
    gchar *name;
    SnapdSnap *snap;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetSnap, snapd_request_get_snap, SNAPD, REQUEST_GET_SNAP, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetSnap, snapd_request_get_snap, snapd_request_get_type ())

struct _SnapdRequestGetIcon
{
    SnapdRequest parent_instance;
    gchar *name;
    SnapdIcon *icon;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetIcon, snapd_request_get_icon, SNAPD, REQUEST_GET_ICON, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetIcon, snapd_request_get_icon, snapd_request_get_type ())

struct _SnapdRequestGetApps
{
    SnapdRequest parent_instance;
    SnapdGetAppsFlags flags;
    GPtrArray *apps;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetApps, snapd_request_get_apps, SNAPD, REQUEST_GET_APPS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetApps, snapd_request_get_apps, snapd_request_get_type ())

struct _SnapdRequestGetSections
{
    SnapdRequest parent_instance;
    gchar **sections;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetSections, snapd_request_get_sections, SNAPD, REQUEST_GET_SECTIONS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetSections, snapd_request_get_sections, snapd_request_get_type ())

struct _SnapdRequestGetFind
{
    SnapdRequest parent_instance;
    SnapdFindFlags flags;
    gchar *query;
    gchar *section;
    gchar *suggested_currency;
    GPtrArray *snaps;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetFind, snapd_request_get_find, SNAPD, REQUEST_GET_FIND, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetFind, snapd_request_get_find, snapd_request_get_type ())

struct _SnapdRequestGetBuyReady
{
    SnapdRequest parent_instance;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetBuyReady, snapd_request_get_buy_ready, SNAPD, REQUEST_GET_BUY_READY, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetBuyReady, snapd_request_get_buy_ready, snapd_request_get_type ())

struct _SnapdRequestPostBuy
{
    SnapdRequest parent_instance;
    gchar *id;
    gdouble amount;
    gchar *currency;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostBuy, snapd_request_post_buy, SNAPD, REQUEST_POST_BUY, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostBuy, snapd_request_post_buy, snapd_request_get_type ())

struct _SnapdRequestAsync
{
    SnapdRequest parent_instance;
    gchar *change_id;
    GSource *poll_source;
    SnapdChange *change;
    gboolean sent_cancel;
    SnapdProgressCallback progress_callback;
    gpointer progress_callback_data;
    JsonNode *async_data;
};
G_DECLARE_FINAL_TYPE (SnapdRequestAsync, snapd_request_async, SNAPD, REQUEST_ASYNC, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestAsync, snapd_request_async, snapd_request_get_type ())

struct _SnapdRequestPostSnap
{
    SnapdRequestAsync parent_instance;
    gchar *name;
    gchar *action;
    SnapdInstallFlags flags;
    gchar *channel;
    gchar *revision;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostSnap, snapd_request_post_snap, SNAPD, REQUEST_POST_SNAP, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostSnap, snapd_request_post_snap, snapd_request_async_get_type ())

struct _SnapdRequestPostSnaps
{
    SnapdRequestAsync parent_instance;
    gchar *action;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostSnaps, snapd_request_post_snaps, SNAPD, REQUEST_POST_SNAPS, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostSnaps, snapd_request_post_snaps, snapd_request_async_get_type ())

struct _SnapdRequestPostSnapStream
{
    SnapdRequestAsync parent_instance;
    SnapdInstallFlags install_flags;
    GByteArray *snap_contents;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostSnapStream, snapd_request_post_snap_stream, SNAPD, REQUEST_POST_SNAP_STREAM, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostSnapStream, snapd_request_post_snap_stream, snapd_request_async_get_type ())

struct _SnapdRequestPostSnapTry
{
    SnapdRequestAsync parent_instance;
    gchar *path;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostSnapTry, snapd_request_post_snap_try, SNAPD, REQUEST_POST_SNAP_TRY, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostSnapTry, snapd_request_post_snap_try, snapd_request_async_get_type ())

struct _SnapdRequestGetAliases
{
    SnapdRequest parent_instance;
    GPtrArray *aliases;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetAliases, snapd_request_get_aliases, SNAPD, REQUEST_GET_ALIASES, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetAliases, snapd_request_get_aliases, snapd_request_get_type ())

struct _SnapdRequestPostAliases
{
    SnapdRequestAsync parent_instance;
    gchar *action;
    gchar *snap;
    gchar *app;
    gchar *alias;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostAliases, snapd_request_post_aliases, SNAPD, REQUEST_POST_ALIASES, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostAliases, snapd_request_post_aliases, snapd_request_async_get_type ())

struct _SnapdRequestGetInterfaces
{
    SnapdRequest parent_instance;
    GPtrArray *plugs;
    GPtrArray *slots;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetInterfaces, snapd_request_get_interfaces, SNAPD, REQUEST_GET_INTERFACES, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetInterfaces, snapd_request_get_interfaces, snapd_request_get_type ())

struct _SnapdRequestPostInterfaces
{
    SnapdRequestAsync parent_instance;
    gchar *action;
    gchar *plug_snap;
    gchar *plug_name;
    gchar *slot_snap;
    gchar *slot_name;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostInterfaces, snapd_request_post_interfaces, SNAPD, REQUEST_POST_INTERFACES, SnapdRequestAsync)
G_DEFINE_TYPE (SnapdRequestPostInterfaces, snapd_request_post_interfaces, snapd_request_async_get_type ())

struct _SnapdRequestGetAssertions
{
    SnapdRequest parent_instance;
    gchar *type;
    gchar **assertions;
};
G_DECLARE_FINAL_TYPE (SnapdRequestGetAssertions, snapd_request_get_assertions, SNAPD, REQUEST_GET_ASSERTIONS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestGetAssertions, snapd_request_get_assertions, snapd_request_get_type ())

struct _SnapdRequestPostAssertions
{
    SnapdRequest parent_instance;
    gchar **assertions;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostAssertions, snapd_request_post_assertions, SNAPD, REQUEST_POST_ASSERTIONS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostAssertions, snapd_request_post_assertions, snapd_request_get_type ())

struct _SnapdRequestPostCreateUser
{
    SnapdRequest parent_instance;
    gchar *email;
    SnapdCreateUserFlags flags;
    SnapdUserInformation *user_information;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostCreateUser, snapd_request_post_create_user, SNAPD, REQUEST_POST_CREATE_USER, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostCreateUser, snapd_request_post_create_user, snapd_request_get_type ())

struct _SnapdRequestPostCreateUsers
{
    SnapdRequest parent_instance;
    GPtrArray *users_information;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostCreateUsers, snapd_request_post_create_users, SNAPD, REQUEST_POST_CREATE_USERS, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostCreateUsers, snapd_request_post_create_users, snapd_request_get_type ())

struct _SnapdRequestPostSnapctl
{
    SnapdRequest parent_instance;
    gchar *context_id;
    gchar **args;
    gchar *stdout_output;
    gchar *stderr_output;
};
G_DECLARE_FINAL_TYPE (SnapdRequestPostSnapctl, snapd_request_post_snapctl, SNAPD, REQUEST_POST_SNAPCTL, SnapdRequest)
G_DEFINE_TYPE (SnapdRequestPostSnapctl, snapd_request_post_snapctl, snapd_request_get_type ())

static void send_request (SnapdClient *client, SnapdRequest *request);

static gboolean
respond_cb (gpointer user_data)
{
    SnapdRequest *request = user_data;
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    if (priv->ready_callback != NULL)
        priv->ready_callback (G_OBJECT (priv->client), G_ASYNC_RESULT (request), priv->ready_callback_data);

    return G_SOURCE_REMOVE;
}

static void
snapd_request_respond (SnapdRequest *request, GError *error)
{
    g_autoptr(GSource) source = NULL;
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    if (priv->responded)
        return;
    priv->responded = TRUE;
    priv->error = error;

    source = g_idle_source_new ();
    g_source_set_callback (source, respond_cb, g_object_ref (request), g_object_unref);
    g_source_attach (source, priv->context);
}

static void
snapd_request_complete_unlocked (SnapdRequest *request, GError *error)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    SnapdClientPrivate *client_priv = snapd_client_get_instance_private (priv->client);

    if (priv->read_source != NULL)
        g_source_destroy (priv->read_source);
    g_clear_pointer (&priv->read_source, g_source_unref);

    snapd_request_respond (request, error);
    client_priv->requests = g_list_remove (client_priv->requests, request);
    g_object_unref (request);
}

static void
snapd_request_complete (SnapdRequest *request, GError *error)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    SnapdClientPrivate *client_priv = snapd_client_get_instance_private (priv->client);
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&client_priv->requests_mutex);
    snapd_request_complete_unlocked (request, error);
}

static void
setup_request (SnapdRequest *request,
               GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    priv->ready_callback = callback;
    priv->ready_callback_data = user_data;
    if (cancellable != NULL)
        priv->cancellable = g_object_ref (cancellable);
}

static gboolean
async_poll_cb (gpointer data)
{
    SnapdRequestAsync *request = data;
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (SNAPD_REQUEST (request));
    SnapdRequestGetChange *change_request;

    change_request = SNAPD_REQUEST_GET_CHANGE (g_object_new (snapd_request_get_change_get_type (), NULL));
    change_request->change_id = g_strdup (request->change_id);
    setup_request (SNAPD_REQUEST (change_request), NULL, NULL, NULL);
    send_request (priv->client, SNAPD_REQUEST (change_request));

    request->poll_source = NULL;
    return G_SOURCE_REMOVE;
}

static void
schedule_poll (SnapdRequestAsync *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (SNAPD_REQUEST (request));
    if (request->poll_source != NULL)
        g_source_destroy (request->poll_source);
    request->poll_source = g_timeout_source_new (ASYNC_POLL_TIME);
    g_source_set_callback (request->poll_source, async_poll_cb, request, NULL);
    g_source_attach (request->poll_source, priv->context);
}

static void
complete_all_requests (SnapdClient *client, GError *error)
{
    SnapdClientPrivate *priv = snapd_client_get_instance_private (client);
    g_autoptr(GList) requests = NULL;
    GList *link;
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->requests_mutex);

    /* Disconnect socket - we will reconnect on demand */
    if (priv->snapd_socket != NULL)
        g_socket_close (priv->snapd_socket, NULL);
    g_clear_object (&priv->snapd_socket);

    /* Cancel synchronous requests (we'll never know the result); reschedule async ones (can reconnect to check result) */
    requests = g_list_copy (priv->requests);
    for (link = requests; link; link = link->next) {
        SnapdRequest *request = link->data;

        if (SNAPD_IS_REQUEST_ASYNC (request))
            schedule_poll (SNAPD_REQUEST_ASYNC (request));
        else
            snapd_request_complete_unlocked (request, g_error_copy (error));
    }
}

static gboolean
snapd_request_return_error (SnapdRequest *request, GError **error)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    if (priv->error != NULL) {
        g_propagate_error (error, priv->error);
        priv->error = NULL;
        return FALSE;
    }

    /* If no error provided from snapd, use a generic cancelled error */
    if (g_cancellable_set_error_if_cancelled (priv->cancellable, error))
        return FALSE;

    return TRUE;
}

static GObject *
snapd_request_get_source_object (GAsyncResult *result)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (SNAPD_REQUEST (result));
    return g_object_ref (priv->client);
}

static void
snapd_request_async_result_init (GAsyncResultIface *iface)
{
    iface->get_source_object = snapd_request_get_source_object;
}

static void
snapd_request_finalize (GObject *object)
{
    SnapdRequest *request = SNAPD_REQUEST (object);
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    g_clear_object (&priv->message);
    if (priv->read_source != NULL)
        g_source_destroy (priv->read_source);
    g_clear_pointer (&priv->read_source, g_source_unref);
    g_cancellable_disconnect (priv->cancellable, priv->cancelled_id);
    g_clear_object (&priv->cancellable);
    g_clear_pointer (&priv->error, g_error_free);
    g_clear_pointer (&priv->context, g_main_context_unref);

    G_OBJECT_CLASS (snapd_request_parent_class)->finalize (object);
}

static void
snapd_request_class_init (SnapdRequestClass *klass)
{
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   gobject_class->finalize = snapd_request_finalize;
}

static void
snapd_request_init (SnapdRequest *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    priv->context = g_main_context_ref_thread_default ();
}

static void
append_string (GByteArray *array, const gchar *value)
{
    g_byte_array_append (array, (const guint8 *) value, strlen (value));
}

/* Converts a language in POSIX format and to be RFC2616 compliant */
static gchar *
posix_lang_to_rfc2616 (const gchar *language)
{
    /* Don't include charset variants, etc */
    if (strchr (language, '.') || strchr (language, '@'))
        return NULL;

    /* Ignore "C" locale, which g_get_language_names() always includes as a fallback. */
    if (strcmp (language, "C") == 0)
        return NULL;

    return g_strdelimit (g_ascii_strdown (language, -1), "_", '-');
}

/* Converts @quality from 0-100 to 0.0-1.0 and appends to @str */
static gchar *
add_quality_value (const gchar *str, int quality)
{
    g_return_val_if_fail (str != NULL, NULL);

    if (quality >= 0 && quality < 100) {
        /* We don't use %.02g because of "." vs "," locale issues */
        if (quality % 10)
            return g_strdup_printf ("%s;q=0.%02d", str, quality);
        else
            return g_strdup_printf ("%s;q=0.%d", str, quality / 10);
    } else
        return g_strdup (str);
}

/* Returns a RFC2616 compliant languages list from system locales */
/* Copied from libsoup */
static gchar *
get_accept_languages (void)
{
    const char * const * lang_names;
    g_autoptr(GPtrArray) langs = NULL;
    int delta;
    guint i;

    lang_names = g_get_language_names ();
    g_return_val_if_fail (lang_names != NULL, NULL);

    /* Build the array of languages */
    langs = g_ptr_array_new_with_free_func (g_free);
    for (i = 0; lang_names[i] != NULL; i++) {
        gchar *lang = posix_lang_to_rfc2616 (lang_names[i]);
        if (lang != NULL)
            g_ptr_array_add (langs, lang);
    }

    /* Add quality values */
    if (langs->len < 10)
        delta = 10;
    else if (langs->len < 20)
        delta = 5;
    else
        delta = 1;
    for (i = 0; i < langs->len; i++) {
        gchar *lang = langs->pdata[i];
        langs->pdata[i] = add_quality_value (lang, 100 - i * delta);
        g_free (lang);
    }

    /* Fallback to "en" if list is empty */
    if (langs->len == 0)
        return g_strdup ("en");

    g_ptr_array_add (langs, NULL);
    return g_strjoinv (", ", (char **)langs->pdata);
}

static void
set_json_body (SoupMessage *message, JsonBuilder *builder)
{
    g_autoptr(JsonNode) json_root = NULL;
    g_autoptr(JsonGenerator) json_generator = NULL;
    g_autofree gchar *data = NULL;
    gsize data_length;

    json_root = json_builder_get_root (builder);
    json_generator = json_generator_new ();
    json_generator_set_pretty (json_generator, TRUE);
    json_generator_set_root (json_generator, json_root);
    data = json_generator_to_data (json_generator, &data_length);

    soup_message_headers_set_content_type (message->request_headers, "application/json", NULL);
    soup_message_body_append_take (message->request_body, g_steal_pointer (&data), data_length);
    soup_message_headers_set_content_length (message->request_headers, message->request_body->length);
}

static JsonObject *
snapd_request_parse_json_response (SnapdRequest *request, GError **error)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    return _snapd_json_parse_response (priv->message, error);
}

static SoupMessage *
generate (SnapdRequest *request, const gchar *method, const gchar *path)
{
    g_autofree gchar *uri = NULL;

    uri = g_strdup_printf ("http://snapd%s", path);
    return soup_message_new (method, uri);
}

static SoupMessage *
generate_get_system_info_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/system-info");
}

static void
parse_get_system_info_response (SnapdRequest *request)
{
    SnapdRequestGetSystemInfo *r = SNAPD_REQUEST_GET_SYSTEM_INFO (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(SnapdSystemInformation) system_information = NULL;
    const gchar *confinement_string;
    SnapdSystemConfinement confinement = SNAPD_SYSTEM_CONFINEMENT_UNKNOWN;
    JsonObject *os_release, *locations;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    confinement_string = _snapd_json_get_string (result, "confinement", "");
    if (strcmp (confinement_string, "strict") == 0)
        confinement = SNAPD_SYSTEM_CONFINEMENT_STRICT;
    else if (strcmp (confinement_string, "partial") == 0)
        confinement = SNAPD_SYSTEM_CONFINEMENT_PARTIAL;
    os_release = _snapd_json_get_object (result, "os-release");
    locations  = _snapd_json_get_object (result, "locations");
    system_information = g_object_new (SNAPD_TYPE_SYSTEM_INFORMATION,
                                       "binaries-directory", locations != NULL ? _snapd_json_get_string (locations, "snap-bin-dir", NULL) : NULL,
                                       "confinement", confinement,
                                       "kernel-version", _snapd_json_get_string (result, "kernel-version", NULL),
                                       "managed", _snapd_json_get_bool (result, "managed", FALSE),
                                       "mount-directory", locations != NULL ? _snapd_json_get_string (locations, "snap-mount-dir", NULL) : NULL,
                                       "on-classic", _snapd_json_get_bool (result, "on-classic", FALSE),
                                       "os-id", os_release != NULL ? _snapd_json_get_string (os_release, "id", NULL) : NULL,
                                       "os-version", os_release != NULL ? _snapd_json_get_string (os_release, "version-id", NULL) : NULL,
                                       "series", _snapd_json_get_string (result, "series", NULL),
                                       "store", _snapd_json_get_string (result, "store", NULL),
                                       "version", _snapd_json_get_string (result, "version", NULL),
                                       NULL);
    r->system_information = g_steal_pointer (&system_information);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_icon_request (SnapdRequest *request)
{
    SnapdRequestGetIcon *r = SNAPD_REQUEST_GET_ICON (request);
    g_autofree gchar *escaped = NULL, *path = NULL;

    escaped = soup_uri_encode (r->name, NULL);
    path = g_strdup_printf ("/v2/icons/%s/icon", escaped);

    return generate (request, "GET", path);
}

static void
parse_get_icon_response (SnapdRequest *request)
{
    SnapdRequestGetIcon *r = SNAPD_REQUEST_GET_ICON (request);
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    const gchar *content_type;
    g_autoptr(SoupBuffer) buffer = NULL;
    g_autoptr(GBytes) data = NULL;
    g_autoptr(SnapdIcon) icon = NULL;

    content_type = soup_message_headers_get_content_type (priv->message->response_headers, NULL);
    if (g_strcmp0 (content_type, "application/json") == 0) {
        g_autoptr(JsonObject) response = NULL;
        g_autoptr(JsonObject) result = NULL;
        GError *error = NULL;

        response = snapd_request_parse_json_response (request, &error);
        if (response == NULL) {
            snapd_request_complete (request, error);
            return;
        }
        result = _snapd_json_get_sync_result_o (response, &error);
        if (result == NULL) {
            snapd_request_complete (request, error);
            return;
        }

        error = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Unknown response");
        snapd_request_complete (request, error);
        return;
    }

    if (priv->message->status_code != SOUP_STATUS_OK) {
        GError *error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_READ_FAILED,
                                     "Got response %u retrieving icon", priv->message->status_code);
        snapd_request_complete (request, error);
    }

    buffer = soup_message_body_flatten (priv->message->response_body);
    data = soup_buffer_get_as_bytes (buffer);
    icon = g_object_new (SNAPD_TYPE_ICON,
                         "mime-type", content_type,
                         "data", data,
                         NULL);

    r->icon = g_steal_pointer (&icon);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_snaps_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/snaps");
}

static void
parse_get_snaps_response (SnapdRequest *request)
{
    SnapdRequestGetSnaps *r = SNAPD_REQUEST_GET_SNAPS (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonArray) result = NULL;
    GPtrArray *snaps;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_a (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snaps = _snapd_json_parse_snap_array (result, &error);
    if (snaps == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->snaps = g_steal_pointer (&snaps);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_snap_request (SnapdRequest *request)
{
    SnapdRequestGetSnap *r = SNAPD_REQUEST_GET_SNAP (request);
    g_autofree gchar *escaped = NULL, *path = NULL;

    escaped = soup_uri_encode (r->name, NULL);
    path = g_strdup_printf ("/v2/snaps/%s", escaped);

    return generate (request, "GET", path);
}

static void
parse_get_snap_response (SnapdRequest *request)
{
    SnapdRequestGetSnap *r = SNAPD_REQUEST_GET_SNAP (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(SnapdSnap) snap = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snap = _snapd_json_parse_snap (result, &error);
    if (snap == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->snap = g_steal_pointer (&snap);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_apps_request (SnapdRequest *request)
{
    SnapdRequestGetApps *r = SNAPD_REQUEST_GET_APPS (request);
    if ((r->flags & SNAPD_GET_APPS_FLAGS_SELECT_SERVICES) != 0)
        return generate (request, "GET", "/v2/apps?select=service");
    else
        return generate (request, "GET", "/v2/apps");
}

static void
parse_get_apps_response (SnapdRequest *request)
{
    SnapdRequestGetApps *r = SNAPD_REQUEST_GET_APPS (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonArray) result = NULL;
    GPtrArray *apps;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_a (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    apps = _snapd_json_parse_app_array (result, &error);
    if (apps == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->apps = g_steal_pointer (&apps);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_aliases_request (SnapdRequest *request)
{
    SnapdRequestPostAliases *r = SNAPD_REQUEST_POST_ALIASES (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/aliases");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "action");
    json_builder_add_string_value (builder, r->action);
    if (r->snap != NULL) {
        json_builder_set_member_name (builder, "snap");
        json_builder_add_string_value (builder, r->snap);
    }
    if (r->app != NULL) {
        json_builder_set_member_name (builder, "app");
        json_builder_add_string_value (builder, r->app);
    }
    if (r->alias != NULL) {
        json_builder_set_member_name (builder, "alias");
        json_builder_add_string_value (builder, r->alias);
    }
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static SoupMessage *
generate_get_interfaces_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/interfaces");
}

static SoupMessage *
generate_post_interfaces_request (SnapdRequest *request)
{
    SnapdRequestPostInterfaces *r = SNAPD_REQUEST_POST_INTERFACES (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/interfaces");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "action");
    json_builder_add_string_value (builder, r->action);
    json_builder_set_member_name (builder, "plugs");
    json_builder_begin_array (builder);
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "snap");
    json_builder_add_string_value (builder, r->plug_snap);
    json_builder_set_member_name (builder, "plug");
    json_builder_add_string_value (builder, r->plug_name);
    json_builder_end_object (builder);
    json_builder_end_array (builder);
    json_builder_set_member_name (builder, "slots");
    json_builder_begin_array (builder);
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "snap");
    json_builder_add_string_value (builder, r->slot_snap);
    json_builder_set_member_name (builder, "slot");
    json_builder_add_string_value (builder, r->slot_name);
    json_builder_end_object (builder);
    json_builder_end_array (builder);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static SoupMessage *
generate_get_assertions_request (SnapdRequest *request)
{
    SnapdRequestGetAssertions *r = SNAPD_REQUEST_GET_ASSERTIONS (request);
    g_autofree gchar *escaped = NULL, *path = NULL;

    escaped = soup_uri_encode (r->type, NULL);
    path = g_strdup_printf ("/v2/assertions/%s", escaped);

    return generate (request, "GET", path);
}

static GPtrArray *
get_connections (JsonObject *object, const gchar *name, GError **error)
{
    g_autoptr(JsonArray) array = NULL;
    GPtrArray *connections;
    guint i;

    connections = g_ptr_array_new_with_free_func (g_object_unref);
    array = _snapd_json_get_array (object, "connections");
    for (i = 0; i < json_array_get_length (array); i++) {
        JsonNode *node = json_array_get_element (array, i);
        JsonObject *object;
        SnapdConnection *connection;

        if (json_node_get_value_type (node) != JSON_TYPE_OBJECT) {
            g_set_error_literal (error,
                                 SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected connection type");
            return NULL;
        }

        object = json_node_get_object (node);
        connection = g_object_new (SNAPD_TYPE_CONNECTION,
                                   "name", _snapd_json_get_string (object, name, NULL),
                                   "snap", _snapd_json_get_string (object, "snap", NULL),
                                   NULL);
        g_ptr_array_add (connections, connection);
    }

    return connections;
}

static GVariant *node_to_variant (JsonNode *node);

static GVariant *
object_to_variant (JsonObject *object)
{
    JsonObjectIter iter;
    GType container_type = G_TYPE_INVALID;
    const gchar *name;
    JsonNode *node;
    GVariantBuilder builder;

    /* If has a consistent type, make an array of that type */
    json_object_iter_init (&iter, object);
    while (json_object_iter_next (&iter, &name, &node)) {
        GType type;
        type = json_node_get_value_type (node);
        if (container_type == G_TYPE_INVALID || type == container_type)
            container_type = type;
        else {
            container_type = G_TYPE_INVALID;
            break;
        }
    }

    switch (container_type)
    {
    case G_TYPE_BOOLEAN:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sb}"));
        json_object_iter_init (&iter, object);
        while (json_object_iter_next (&iter, &name, &node))
            g_variant_builder_add (&builder, "{sb}", name, json_node_get_boolean (node));
        return g_variant_builder_end (&builder);
    case G_TYPE_INT64:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sx}"));
        json_object_iter_init (&iter, object);
        while (json_object_iter_next (&iter, &name, &node))
            g_variant_builder_add (&builder, "{sx}", name, json_node_get_int (node));
        return g_variant_builder_end (&builder);
    case G_TYPE_DOUBLE:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sd}"));
        json_object_iter_init (&iter, object);
        while (json_object_iter_next (&iter, &name, &node))
            g_variant_builder_add (&builder, "{sd}", name, json_node_get_double (node));
        return g_variant_builder_end (&builder);
    case G_TYPE_STRING:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));
        json_object_iter_init (&iter, object);
        while (json_object_iter_next (&iter, &name, &node))
            g_variant_builder_add (&builder, "{ss}", name, json_node_get_string (node));
        return g_variant_builder_end (&builder);
    default:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
        json_object_iter_init (&iter, object);
        while (json_object_iter_next (&iter, &name, &node))
            g_variant_builder_add (&builder, "{sv}", name, node_to_variant (node));
        return g_variant_builder_end (&builder);
    }
}

static GVariant *
array_to_variant (JsonArray *array)
{
    guint i, length;
    GType container_type = G_TYPE_INVALID;
    GVariantBuilder builder;

    /* If has a consistent type, make an array of that type */
    length = json_array_get_length (array);
    for (i = 0; i < length; i++) {
        GType type;
        type = json_node_get_value_type (json_array_get_element (array, i));
        if (container_type == G_TYPE_INVALID || type == container_type)
            container_type = type;
        else {
            container_type = G_TYPE_INVALID;
            break;
        }
    }

    switch (container_type)
    {
    case G_TYPE_BOOLEAN:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("ab"));
        for (i = 0; i < length; i++)
            g_variant_builder_add (&builder, "b", json_array_get_boolean_element (array, i));
        return g_variant_builder_end (&builder);
    case G_TYPE_INT64:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("ax"));
        for (i = 0; i < length; i++)
            g_variant_builder_add (&builder, "x", json_array_get_int_element (array, i));
        return g_variant_builder_end (&builder);
    case G_TYPE_DOUBLE:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("ad"));
        for (i = 0; i < length; i++)
            g_variant_builder_add (&builder, "d", json_array_get_double_element (array, i));
        return g_variant_builder_end (&builder);
    case G_TYPE_STRING:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        for (i = 0; i < length; i++)
            g_variant_builder_add (&builder, "s", json_array_get_string_element (array, i));
        return g_variant_builder_end (&builder);
    default:
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("av"));
        for (i = 0; i < length; i++)
            g_variant_builder_add (&builder, "v", node_to_variant (json_array_get_element (array, i)));
        return g_variant_builder_end (&builder);
    }
}

static GVariant *
node_to_variant (JsonNode *node)
{
    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_OBJECT:
        return object_to_variant (json_node_get_object (node));
    case JSON_NODE_ARRAY:
        return array_to_variant (json_node_get_array (node));
    case JSON_NODE_VALUE:
        switch (json_node_get_value_type (node))
        {
        case G_TYPE_BOOLEAN:
            return g_variant_new_boolean (json_node_get_boolean (node));
        case G_TYPE_INT64:
            return g_variant_new_int64 (json_node_get_int (node));
        case G_TYPE_DOUBLE:
            return g_variant_new_double (json_node_get_double (node));
        case G_TYPE_STRING:
            return g_variant_new_string (json_node_get_string (node));
        default:
            /* Should never occur - as the above are all the valid types */
            return g_variant_new ("mv", NULL);
        }
    default:
        return g_variant_new ("mv", NULL);
    }
}

static GHashTable *
get_attributes (JsonObject *object, const gchar *name, GError **error)
{
    JsonObject *attrs;
    JsonObjectIter iter;
    GHashTable *attributes;
    const gchar *attribute_name;
    JsonNode *node;

    attributes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_variant_unref);
    attrs = _snapd_json_get_object (object, "attrs");
    if (attrs == NULL)
        return attributes;

    json_object_iter_init (&iter, attrs);
    while (json_object_iter_next (&iter, &attribute_name, &node))
        g_hash_table_insert (attributes, g_strdup (attribute_name), node_to_variant (node));

    return attributes;
}

static void
parse_get_assertions_response (SnapdRequest *request)
{
    SnapdRequestGetAssertions *r = SNAPD_REQUEST_GET_ASSERTIONS (request);
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    const gchar *content_type;
    g_autoptr(GPtrArray) assertions = NULL;
    g_autoptr(SoupBuffer) buffer = NULL;
    gsize offset = 0;

    content_type = soup_message_headers_get_content_type (priv->message->response_headers, NULL);
    if (g_strcmp0 (content_type, "application/json") == 0) {
        g_autoptr(JsonObject) response = NULL;
        g_autoptr(JsonObject) result = NULL;
        GError *error = NULL;

        response = snapd_request_parse_json_response (request, &error);
        if (response == NULL) {
            snapd_request_complete (request, error);
            return;
        }
        result = _snapd_json_get_sync_result_o (response, &error);
        if (result == NULL) {
            snapd_request_complete (request, error);
            return;
        }

        error = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Unknown response");
        snapd_request_complete (request, error);
        return;
    }

    if (priv->message->status_code != SOUP_STATUS_OK) {
        GError *error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_READ_FAILED,
                                     "Got response %u retrieving assertions", priv->message->status_code);
        snapd_request_complete (request, error);
        return;
    }

    if (g_strcmp0 (content_type, "application/x.ubuntu.assertion") != 0) {
        GError *error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_READ_FAILED,
                                     "Got unknown content type '%s' retrieving assertions", content_type);
        snapd_request_complete (request, error);
        return;
    }

    assertions = g_ptr_array_new ();
    buffer = soup_message_body_flatten (priv->message->response_body);
    while (offset < buffer->length) {
        gsize assertion_start, assertion_end, body_length = 0;
        g_autofree gchar *body_length_header = NULL;
        SnapdAssertion *assertion;

        /* Headers terminated by double newline */
        assertion_start = offset;
        while (offset < buffer->length && !g_str_has_prefix (buffer->data + offset, "\n\n"))
            offset++;
        offset += 2;

        /* Make a temporary assertion object to decode body length header */
        assertion = snapd_assertion_new (g_strndup (buffer->data + assertion_start, offset - assertion_start));
        body_length_header = snapd_assertion_get_header (assertion, "body-length");
        g_object_unref (assertion);

        /* Skip over body */
        body_length = body_length_header != NULL ? strtoul (body_length_header, NULL, 10) : 0;
        if (body_length > 0)
            offset += body_length + 2;

        /* Find end of signature */
        while (offset < buffer->length && !g_str_has_prefix (buffer->data + offset, "\n\n"))
            offset++;
        assertion_end = offset;
        offset += 2;

        g_ptr_array_add (assertions, g_strndup (buffer->data + assertion_start, assertion_end - assertion_start));
    }
    g_ptr_array_add (assertions, NULL);

    r->assertions = g_steal_pointer (&assertions->pdata);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_assertions_request (SnapdRequest *request)
{
    SnapdRequestPostAssertions *r = SNAPD_REQUEST_POST_ASSERTIONS (request);
    SoupMessage *message;
    int i;

    message = generate (request, "POST", "/v2/assertions");

    soup_message_headers_set_content_type (message->request_headers, "application/x.ubuntu.assertion", NULL); //FIXME
    for (i = 0; r->assertions[i]; i++) {
        if (i != 0)
            soup_message_body_append (message->request_body, SOUP_MEMORY_TEMPORARY, "\n\n", 2);
        soup_message_body_append (message->request_body, SOUP_MEMORY_TEMPORARY, r->assertions[i], strlen (r->assertions[i]));
    }
    soup_message_headers_set_content_length (message->request_headers, message->request_body->length);

    return message;
}

static void
parse_post_assertions_response (SnapdRequest *request)
{
    g_autoptr(JsonObject) response = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snapd_request_complete (request, NULL);
}

static void
parse_get_interfaces_response (SnapdRequest *request)
{
    SnapdRequestGetInterfaces *r = SNAPD_REQUEST_GET_INTERFACES (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(GPtrArray) plug_array = NULL;
    g_autoptr(GPtrArray) slot_array = NULL;
    g_autoptr(JsonArray) plugs = NULL;
    g_autoptr(JsonArray) slots = NULL;
    guint i;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    plugs = _snapd_json_get_array (result, "plugs");
    plug_array = g_ptr_array_new_with_free_func (g_object_unref);
    for (i = 0; i < json_array_get_length (plugs); i++) {
        JsonNode *node = json_array_get_element (plugs, i);
        JsonObject *object;
        g_autoptr(GPtrArray) connections = NULL;
        g_autoptr(GHashTable) attributes = NULL;
        g_autoptr(SnapdPlug) plug = NULL;

        if (json_node_get_value_type (node) != JSON_TYPE_OBJECT) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected plug type");
            snapd_request_complete (request, error);
            return;
        }
        object = json_node_get_object (node);

        connections = get_connections (object, "slot", &error);
        if (connections == NULL) {
            snapd_request_complete (request, error);
            return;
        }
        attributes = get_attributes (object, "slot", &error);

        plug = g_object_new (SNAPD_TYPE_PLUG,
                             "name", _snapd_json_get_string (object, "plug", NULL),
                             "snap", _snapd_json_get_string (object, "snap", NULL),
                             "interface", _snapd_json_get_string (object, "interface", NULL),
                             "label", _snapd_json_get_string (object, "label", NULL),
                             "connections", connections,
                             "attributes", attributes,
                             // FIXME: apps
                             NULL);
        g_ptr_array_add (plug_array, g_steal_pointer (&plug));
    }
    slots = _snapd_json_get_array (result, "slots");
    slot_array = g_ptr_array_new_with_free_func (g_object_unref);
    for (i = 0; i < json_array_get_length (slots); i++) {
        JsonNode *node = json_array_get_element (slots, i);
        JsonObject *object;
        g_autoptr(GPtrArray) connections = NULL;
        g_autoptr(GHashTable) attributes = NULL;
        g_autoptr(SnapdSlot) slot = NULL;

        if (json_node_get_value_type (node) != JSON_TYPE_OBJECT) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected slot type");
            snapd_request_complete (request, error);
            return;
        }
        object = json_node_get_object (node);

        connections = get_connections (object, "plug", &error);
        if (connections == NULL) {
            snapd_request_complete (request, error);
            return;
        }
        attributes = get_attributes (object, "plug", &error);

        slot = g_object_new (SNAPD_TYPE_SLOT,
                             "name", _snapd_json_get_string (object, "slot", NULL),
                             "snap", _snapd_json_get_string (object, "snap", NULL),
                             "interface", _snapd_json_get_string (object, "interface", NULL),
                             "label", _snapd_json_get_string (object, "label", NULL),
                             "connections", connections,
                             "attributes", attributes,
                             // FIXME: apps
                             NULL);
        g_ptr_array_add (slot_array, g_steal_pointer (&slot));
    }

    r->plugs = g_steal_pointer (&plug_array);
    r->slots = g_steal_pointer (&slot_array);
    snapd_request_complete (request, NULL);
}

static gboolean
times_equal (GDateTime *time1, GDateTime *time2)
{
    if (time1 == NULL || time2 == NULL)
        return time1 == time2;
    return g_date_time_equal (time1, time2);
}

static gboolean
tasks_equal (SnapdTask *task1, SnapdTask *task2)
{
    return g_strcmp0 (snapd_task_get_id (task1), snapd_task_get_id (task2)) == 0 &&
           g_strcmp0 (snapd_task_get_kind (task1), snapd_task_get_kind (task2)) == 0 &&
           g_strcmp0 (snapd_task_get_summary (task1), snapd_task_get_summary (task2)) == 0 &&
           g_strcmp0 (snapd_task_get_status (task1), snapd_task_get_status (task2)) == 0 &&
           g_strcmp0 (snapd_task_get_progress_label (task1), snapd_task_get_progress_label (task2)) == 0 &&
           snapd_task_get_progress_done (task1) == snapd_task_get_progress_done (task2) &&
           snapd_task_get_progress_total (task1) == snapd_task_get_progress_total (task2) &&
           times_equal (snapd_task_get_spawn_time (task1), snapd_task_get_spawn_time (task2)) &&
           times_equal (snapd_task_get_spawn_time (task1), snapd_task_get_spawn_time (task2));
}

static gboolean
changes_equal (SnapdChange *change1, SnapdChange *change2)
{
    GPtrArray *tasks1, *tasks2;

    if (change1 == NULL || change2 == NULL)
        return change1 == change2;

    tasks1 = snapd_change_get_tasks (change1);
    tasks2 = snapd_change_get_tasks (change2);
    if (tasks1 == NULL || tasks2 == NULL) {
        if (tasks1 != tasks2)
            return FALSE;
    }
    else {
        int i;

        if (tasks1->len != tasks2->len)
            return FALSE;
        for (i = 0; i < tasks1->len; i++) {
            SnapdTask *t1 = tasks1->pdata[i], *t2 = tasks2->pdata[i];
            if (!tasks_equal (t1, t2))
                return FALSE;
        }
    }

    return g_strcmp0 (snapd_change_get_id (change1), snapd_change_get_id (change2)) == 0 &&
           g_strcmp0 (snapd_change_get_kind (change1), snapd_change_get_kind (change2)) == 0 &&
           g_strcmp0 (snapd_change_get_summary (change1), snapd_change_get_summary (change2)) == 0 &&
           g_strcmp0 (snapd_change_get_status (change1), snapd_change_get_status (change2)) == 0 &&
           !!snapd_change_get_ready (change1) == !!snapd_change_get_ready (change2) &&
           times_equal (snapd_change_get_spawn_time (change1), snapd_change_get_spawn_time (change2)) &&
           times_equal (snapd_change_get_spawn_time (change1), snapd_change_get_spawn_time (change2));

    return TRUE;
}

static void
send_cancel (SnapdRequestAsync *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (SNAPD_REQUEST (request));
    g_autofree gchar *path = NULL;
    SnapdRequestPostChange *change_request;

    if (request->sent_cancel)
        return;
    request->sent_cancel = TRUE;

    change_request = SNAPD_REQUEST_POST_CHANGE (g_object_new (snapd_request_post_change_get_type (), NULL));
    change_request->change_id = g_strdup (request->change_id);
    change_request->action = g_strdup ("abort");
    setup_request (SNAPD_REQUEST (change_request), NULL, NULL, NULL);

    send_request (priv->client, SNAPD_REQUEST (change_request));
}

static void
parse_async_response (SnapdRequest *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    SnapdRequestAsync *r = SNAPD_REQUEST_ASYNC (request);
    g_autoptr(JsonObject) response = NULL;
    gchar *change_id = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    change_id = _snapd_json_get_async_result (response, &error);
    if (change_id == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->change_id = g_strdup (change_id);

    /* Immediately cancel if requested */
    if (g_cancellable_is_cancelled (priv->cancellable)) {
        send_cancel (r);
        return;
    }

    /* Poll for updates */
    schedule_poll (r);
}

static SnapdRequestAsync *
find_change_request (SnapdClient *client, const gchar *change_id)
{
    SnapdClientPrivate *priv = snapd_client_get_instance_private (client);
    GList *link;
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->requests_mutex);

    for (link = priv->requests; link; link = link->next) {
        SnapdRequest *request = link->data;

        if (SNAPD_IS_REQUEST_ASYNC (request) &&
            strcmp (SNAPD_REQUEST_ASYNC (request)->change_id, change_id) == 0)
            return SNAPD_REQUEST_ASYNC (request);
    }

    return NULL;
}

static void
parse_change_response (SnapdRequest *request, const gchar *change_id)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    SnapdRequestAsync *parent;
    SnapdRequestPrivate *parent_priv;
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    gboolean ready;
    GError *error = NULL;

    parent = find_change_request (priv->client, change_id);
    parent_priv = snapd_request_get_instance_private (SNAPD_REQUEST (parent));

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (SNAPD_REQUEST (parent), error);
        snapd_request_complete (request, NULL);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (SNAPD_REQUEST (parent), error);
        snapd_request_complete (request, NULL);
        return;
    }

    if (g_strcmp0 (change_id, _snapd_json_get_string (result, "id", NULL)) != 0) {
        error = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Unexpected change ID returned");
        snapd_request_complete (SNAPD_REQUEST (parent), error);
        snapd_request_complete (request, NULL);
        return;
    }

    /* Update caller with progress */
    if (parent->progress_callback != NULL) {
        g_autoptr(JsonArray) array = NULL;
        guint i;
        g_autoptr(GPtrArray) tasks = NULL;
        g_autoptr(SnapdChange) change = NULL;
        g_autoptr(GDateTime) main_spawn_time = NULL;
        g_autoptr(GDateTime) main_ready_time = NULL;

        array = _snapd_json_get_array (result, "tasks");
        tasks = g_ptr_array_new_with_free_func (g_object_unref);
        for (i = 0; i < json_array_get_length (array); i++) {
            JsonNode *node = json_array_get_element (array, i);
            JsonObject *object, *progress;
            g_autoptr(GDateTime) spawn_time = NULL;
            g_autoptr(GDateTime) ready_time = NULL;
            g_autoptr(SnapdTask) t = NULL;

            if (json_node_get_value_type (node) != JSON_TYPE_OBJECT) {
                error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_READ_FAILED,
                                     "Unexpected task type");
                snapd_request_complete (SNAPD_REQUEST (parent), error);
                snapd_request_complete (request, NULL);
                return;
            }
            object = json_node_get_object (node);
            progress = _snapd_json_get_object (object, "progress");
            spawn_time = _snapd_json_get_date_time (object, "spawn-time");
            ready_time = _snapd_json_get_date_time (object, "ready-time");

            t = g_object_new (SNAPD_TYPE_TASK,
                              "id", _snapd_json_get_string (object, "id", NULL),
                              "kind", _snapd_json_get_string (object, "kind", NULL),
                              "summary", _snapd_json_get_string (object, "summary", NULL),
                              "status", _snapd_json_get_string (object, "status", NULL),
                              "progress-label", progress != NULL ? _snapd_json_get_string (progress, "label", NULL) : NULL,
                              "progress-done", progress != NULL ? _snapd_json_get_int (progress, "done", 0) : 0,
                              "progress-total", progress != NULL ? _snapd_json_get_int (progress, "total", 0) : 0,
                              "spawn-time", spawn_time,
                              "ready-time", ready_time,
                              NULL);
            g_ptr_array_add (tasks, g_steal_pointer (&t));
        }

        main_spawn_time = _snapd_json_get_date_time (result, "spawn-time");
        main_ready_time = _snapd_json_get_date_time (result, "ready-time");
        change = g_object_new (SNAPD_TYPE_CHANGE,
                               "id", _snapd_json_get_string (result, "id", NULL),
                               "kind", _snapd_json_get_string (result, "kind", NULL),
                               "summary", _snapd_json_get_string (result, "summary", NULL),
                               "status", _snapd_json_get_string (result, "status", NULL),
                               "tasks", tasks,
                               "ready", _snapd_json_get_bool (result, "ready", FALSE),
                               "spawn-time", main_spawn_time,
                               "ready-time", main_ready_time,
                               NULL);

        if (!changes_equal (parent->change, change)) {
            g_clear_object (&parent->change);
            parent->change = g_steal_pointer (&change);
            // NOTE: tasks is passed for ABI compatibility - this field is
            // deprecated and can be accessed with snapd_change_get_tasks ()
            parent->progress_callback (parent_priv->client, parent->change, tasks, parent->progress_callback_data);
        }
    }

    ready = _snapd_json_get_bool (result, "ready", FALSE);
    if (ready) {
        GError *error = NULL;

        if (json_object_has_member (result, "data"))
            parent->async_data = json_node_ref (json_object_get_member (result, "data"));
        if (!g_cancellable_set_error_if_cancelled (parent_priv->cancellable, &error) &&
            json_object_has_member (result, "err"))
            error = g_error_new_literal (SNAPD_ERROR,
                                         SNAPD_ERROR_FAILED,
                                         _snapd_json_get_string (result, "err", "Unknown error"));
        snapd_request_complete (SNAPD_REQUEST (parent), error);
        snapd_request_complete (request, NULL);
        return;
    }

    /* Poll for updates */
    schedule_poll (parent);

    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_change_request (SnapdRequest *request)
{
    SnapdRequestGetChange *r = SNAPD_REQUEST_GET_CHANGE (request);
    g_autofree gchar *path = NULL;

    path = g_strdup_printf ("/v2/changes/%s", r->change_id);
    return generate (request, "GET", path);
}

static void
parse_get_change_response (SnapdRequest *request)
{
    SnapdRequestGetChange *r = SNAPD_REQUEST_GET_CHANGE (request);
    parse_change_response (request, r->change_id);
}

static SoupMessage *
generate_post_change_request (SnapdRequest *request)
{
    SnapdRequestPostChange *r = SNAPD_REQUEST_POST_CHANGE (request);
    g_autofree gchar *path = NULL;
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    path = g_strdup_printf ("/v2/changes/%s", r->change_id);
    message = generate (request, "POST", path);

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "action");
    json_builder_add_string_value (builder, r->action);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_change_response (SnapdRequest *request)
{
    SnapdRequestPostChange *r = SNAPD_REQUEST_POST_CHANGE (request);
    parse_change_response (request, r->change_id);
}

static SoupMessage *
generate_post_login_request (SnapdRequest *request)
{
    SnapdRequestPostLogin *r = SNAPD_REQUEST_POST_LOGIN (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/login");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "username");
    json_builder_add_string_value (builder, r->username);
    json_builder_set_member_name (builder, "password");
    json_builder_add_string_value (builder, r->password);
    if (r->otp != NULL) {
        json_builder_set_member_name (builder, "otp");
        json_builder_add_string_value (builder, r->otp);
    }
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_login_response (SnapdRequest *request)
{
    SnapdRequestPostLogin *r = SNAPD_REQUEST_POST_LOGIN (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(JsonArray) discharges = NULL;
    g_autoptr(GPtrArray) discharge_array = NULL;
    guint i;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    discharges = _snapd_json_get_array (result, "discharges");
    discharge_array = g_ptr_array_new ();
    for (i = 0; i < json_array_get_length (discharges); i++) {
        JsonNode *node = json_array_get_element (discharges, i);

        if (json_node_get_value_type (node) != G_TYPE_STRING) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected discharge type");
            snapd_request_complete (request, error);
            return;
        }

        g_ptr_array_add (discharge_array, (gpointer) json_node_get_string (node));
    }
    g_ptr_array_add (discharge_array, NULL);
    r->auth_data = snapd_auth_data_new (_snapd_json_get_string (result, "macaroon", NULL), (gchar **) discharge_array->pdata);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_find_request (SnapdRequest *request)
{
    SnapdRequestGetFind *r = SNAPD_REQUEST_GET_FIND (request);
    g_autoptr(GPtrArray) query_attributes = NULL;
    g_autoptr(GString) path = NULL;

    query_attributes = g_ptr_array_new_with_free_func (g_free);
    if (r->query != NULL) {
        g_autofree gchar *escaped = soup_uri_encode (r->query, NULL);
        if ((r->flags & SNAPD_FIND_FLAGS_MATCH_NAME) != 0)
            g_ptr_array_add (query_attributes, g_strdup_printf ("name=%s", escaped));
        else
            g_ptr_array_add (query_attributes, g_strdup_printf ("q=%s", escaped));
    }

    if ((r->flags & SNAPD_FIND_FLAGS_SELECT_PRIVATE) != 0)
        g_ptr_array_add (query_attributes, g_strdup_printf ("select=private"));
    else if ((r->flags & SNAPD_FIND_FLAGS_SELECT_REFRESH) != 0)
        g_ptr_array_add (query_attributes, g_strdup_printf ("select=refresh"));

    if (r->section != NULL) {
        g_autofree gchar *escaped = soup_uri_encode (r->section, NULL);
        g_ptr_array_add (query_attributes, g_strdup_printf ("section=%s", escaped));
    }

    path = g_string_new ("/v2/find");
    if (query_attributes->len > 0) {
        guint i;

        g_string_append_c (path, '?');
        for (i = 0; i < query_attributes->len; i++) {
            if (i != 0)
                g_string_append_c (path, '&');
            g_string_append (path, (gchar *) query_attributes->pdata[i]);
        }
    }

    return generate (request, "GET", path->str);
}

static void
parse_get_find_response (SnapdRequest *request)
{
    SnapdRequestGetFind *r = SNAPD_REQUEST_GET_FIND (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonArray) result = NULL;
    g_autoptr(GPtrArray) snaps = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_a (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snaps = _snapd_json_parse_snap_array (result, &error);
    if (snaps == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->suggested_currency = g_strdup (_snapd_json_get_string (response, "suggested-currency", NULL));

    r->snaps = g_steal_pointer (&snaps);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_snap_request (SnapdRequest *request)
{
    SnapdRequestPostSnap *r = SNAPD_REQUEST_POST_SNAP (request);
    g_autofree gchar *escaped = NULL, *path = NULL;
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    escaped = soup_uri_encode (r->name, NULL);
    path = g_strdup_printf ("/v2/snaps/%s", escaped);
    message = generate (request, "POST", path);

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "action");
    json_builder_add_string_value (builder, r->action);
    if (r->channel != NULL) {
        json_builder_set_member_name (builder, "channel");
        json_builder_add_string_value (builder, r->channel);
    }
    if (r->revision != NULL) {
        json_builder_set_member_name (builder, "revision");
        json_builder_add_string_value (builder, r->revision);
    }
    if ((r->flags & SNAPD_INSTALL_FLAGS_CLASSIC) != 0) {
        json_builder_set_member_name (builder, "classic");
        json_builder_add_boolean_value (builder, TRUE);
    }
    if ((r->flags & SNAPD_INSTALL_FLAGS_DANGEROUS) != 0) {
        json_builder_set_member_name (builder, "dangerous");
        json_builder_add_boolean_value (builder, TRUE);
    }
    if ((r->flags & SNAPD_INSTALL_FLAGS_DEVMODE) != 0) {
        json_builder_set_member_name (builder, "devmode");
        json_builder_add_boolean_value (builder, TRUE);
    }
    if ((r->flags & SNAPD_INSTALL_FLAGS_JAILMODE) != 0) {
        json_builder_set_member_name (builder, "jailmode");
        json_builder_add_boolean_value (builder, TRUE);
    }
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static SoupMessage *
generate_post_snaps_request (SnapdRequest *request)
{
    SnapdRequestPostSnaps *r = SNAPD_REQUEST_POST_SNAPS (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/snaps");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "action");
    json_builder_add_string_value (builder, r->action);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
append_multipart_value (SoupMultipart *multipart, const gchar *name, const gchar *value)
{
    g_autoptr(SoupMessageHeaders) headers = NULL;
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(SoupBuffer) buffer = NULL;

    headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_MULTIPART);
    params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert (params, g_strdup ("name"), g_strdup (name));
    soup_message_headers_set_content_disposition (headers, "form-data", params);
    buffer = soup_buffer_new_take ((guchar *) g_strdup (value), strlen (value));
    soup_multipart_append_part (multipart, headers, buffer);
}

static SoupMessage *
generate_post_snap_stream_request (SnapdRequest *request)
{
    SnapdRequestPostSnapStream *r = SNAPD_REQUEST_POST_SNAP_STREAM (request);
    SoupMessage *message;
    g_autoptr(GHashTable) params = NULL;
    g_autoptr(SoupBuffer) buffer = NULL;
    g_autoptr(SoupMultipart) multipart = NULL;

    message = generate (request, "POST", "/v2/snaps");

    multipart = soup_multipart_new ("multipart/form-data");
    if ((r->install_flags & SNAPD_INSTALL_FLAGS_CLASSIC) != 0)
        append_multipart_value (multipart, "classic", "true");
    if ((r->install_flags & SNAPD_INSTALL_FLAGS_DANGEROUS) != 0)
        append_multipart_value (multipart, "dangerous", "true");
    if ((r->install_flags & SNAPD_INSTALL_FLAGS_DEVMODE) != 0)
        append_multipart_value (multipart, "devmode", "true");
    if ((r->install_flags & SNAPD_INSTALL_FLAGS_JAILMODE) != 0)
        append_multipart_value (multipart, "jailmode", "true");

    params = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert (params, g_strdup ("name"), g_strdup ("snap"));
    g_hash_table_insert (params, g_strdup ("filename"), g_strdup ("x"));
    soup_message_headers_set_content_disposition (message->request_headers, "form-data", params);
    soup_message_headers_set_content_type (message->request_headers, "application/vnd.snap", NULL);
    buffer = soup_buffer_new (SOUP_MEMORY_TEMPORARY, r->snap_contents->data, r->snap_contents->len);
    soup_multipart_append_part (multipart, message->request_headers, buffer);
    soup_multipart_to_message (multipart, message->request_headers, message->request_body);
    soup_message_headers_set_content_length (message->request_headers, message->request_body->length);

    return message;
}

static SoupMessage *
generate_post_snap_try_request (SnapdRequest *request)
{
    SnapdRequestPostSnapTry *r = SNAPD_REQUEST_POST_SNAP_TRY (request);
    SoupMessage *message;
    g_autoptr(SoupMultipart) multipart = NULL;

    message = generate (request, "POST", "/v2/snaps");

    multipart = soup_multipart_new ("multipart/form-data");
    append_multipart_value (multipart, "action", "try");
    append_multipart_value (multipart, "snap-path", r->path);
    soup_multipart_to_message (multipart, message->request_headers, message->request_body);
    soup_message_headers_set_content_length (message->request_headers, message->request_body->length);

    return message;
}

static SoupMessage *
generate_get_buy_ready_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/buy/ready");
}

static void
parse_get_buy_ready_response (SnapdRequest *request)
{
    g_autoptr(JsonObject) response = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_buy_request (SnapdRequest *request)
{
    SnapdRequestPostBuy *r = SNAPD_REQUEST_POST_BUY (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/buy");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "snap-id");
    json_builder_add_string_value (builder, r->id);
    json_builder_set_member_name (builder, "price");
    json_builder_add_double_value (builder, r->amount);
    json_builder_set_member_name (builder, "currency");
    json_builder_add_string_value (builder, r->currency);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_buy_response (SnapdRequest *request)
{
    g_autoptr(JsonObject) response = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_create_user_request (SnapdRequest *request)
{
    SnapdRequestPostCreateUser *r = SNAPD_REQUEST_POST_CREATE_USER (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/create-user");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "email");
    json_builder_add_string_value (builder, r->email);
    if ((r->flags & SNAPD_CREATE_USER_FLAGS_SUDO) != 0) {
        json_builder_set_member_name (builder, "sudoer");
        json_builder_add_boolean_value (builder, TRUE);
    }
    if ((r->flags & SNAPD_CREATE_USER_FLAGS_KNOWN) != 0) {
        json_builder_set_member_name (builder, "known");
        json_builder_add_boolean_value (builder, TRUE);
    }
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_create_user_response (SnapdRequest *request)
{
    SnapdRequestPostCreateUser *r = SNAPD_REQUEST_POST_CREATE_USER (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(SnapdUserInformation) user_information = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    user_information = _snapd_json_parse_user_information (result, &error);
    if (user_information == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->user_information = g_steal_pointer (&user_information);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_create_users_request (SnapdRequest *request)
{
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;

    message = generate (request, "POST", "/v2/create-user");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "known");
    json_builder_add_boolean_value (builder, TRUE);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_create_users_response (SnapdRequest *request)
{
    SnapdRequestPostCreateUsers *r = SNAPD_REQUEST_POST_CREATE_USERS (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonArray) result = NULL;
    g_autoptr(GPtrArray) users_information = NULL;
    guint i;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_a (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    users_information = g_ptr_array_new_with_free_func (g_object_unref);
    for (i = 0; i < json_array_get_length (result); i++) {
        JsonNode *node = json_array_get_element (result, i);
        SnapdUserInformation *user_information;

        if (json_node_get_value_type (node) != JSON_TYPE_OBJECT) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected user information type");
            snapd_request_complete (request, error);
            return;
        }

        user_information = _snapd_json_parse_user_information (json_node_get_object (node), &error);
        if (user_information == NULL)
        {
            snapd_request_complete (request, error);
            return;
        }
        g_ptr_array_add (users_information, user_information);
    }

    r->users_information = g_steal_pointer (&users_information);
    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_sections_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/sections");
}

static void
parse_get_sections_response (SnapdRequest *request)
{
    SnapdRequestGetSections *r = SNAPD_REQUEST_GET_SECTIONS (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonArray) result = NULL;
    g_autoptr(GPtrArray) sections = NULL;
    guint i;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_a (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    sections = g_ptr_array_new ();
    for (i = 0; i < json_array_get_length (result); i++) {
        JsonNode *node = json_array_get_element (result, i);
        if (json_node_get_value_type (node) != G_TYPE_STRING) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected snap name type");
            snapd_request_complete (request, error);
            return;
        }

        g_ptr_array_add (sections, g_strdup (json_node_get_string (node)));
    }
    g_ptr_array_add (sections, NULL);

    r->sections = g_steal_pointer (&sections->pdata);

    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_get_aliases_request (SnapdRequest *request)
{
    return generate (request, "GET", "/v2/aliases");
}

static void
parse_get_aliases_response (SnapdRequest *request)
{
    SnapdRequestGetAliases *r = SNAPD_REQUEST_GET_ALIASES (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    g_autoptr(GPtrArray) aliases = NULL;
    JsonObjectIter snap_iter;
    const gchar *snap;
    JsonNode *snap_node;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    aliases = g_ptr_array_new_with_free_func (g_object_unref);
    json_object_iter_init (&snap_iter, result);
    while (json_object_iter_next (&snap_iter, &snap, &snap_node)) {
        JsonObjectIter alias_iter;
        const gchar *name;
        JsonNode *alias_node;

        if (json_node_get_value_type (snap_node) != JSON_TYPE_OBJECT) {
            error = g_error_new (SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected alias type");
            snapd_request_complete (request, error);
        }

        json_object_iter_init (&alias_iter, json_node_get_object (snap_node));
        while (json_object_iter_next (&alias_iter, &name, &alias_node)) {
            JsonObject *o;
            SnapdAliasStatus status = SNAPD_ALIAS_STATUS_UNKNOWN;
            const gchar *status_string;
            g_autoptr(SnapdAlias) alias = NULL;

            if (json_node_get_value_type (alias_node) != JSON_TYPE_OBJECT) {
                error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_READ_FAILED,
                                     "Unexpected alias type");
                snapd_request_complete (request, error);
            }

            o = json_node_get_object (alias_node);
            status_string = _snapd_json_get_string (o, "status", NULL);
            if (strcmp (status_string, "disabled") == 0)
                status = SNAPD_ALIAS_STATUS_DISABLED;
            else if (strcmp (status_string, "auto") == 0)
                status = SNAPD_ALIAS_STATUS_AUTO;
            else if (strcmp (status_string, "manual") == 0)
                status = SNAPD_ALIAS_STATUS_MANUAL;
            else
                status = SNAPD_ALIAS_STATUS_UNKNOWN;

            alias = g_object_new (SNAPD_TYPE_ALIAS,
                                  "snap", snap,
                                  "app-auto", _snapd_json_get_string (o, "auto", NULL),
                                  "app-manual", _snapd_json_get_string (o, "manual", NULL),
                                  "command", _snapd_json_get_string (o, "command", NULL),
                                  "name", name,
                                  "status", status,
                                  NULL);
            g_ptr_array_add (aliases, g_steal_pointer (&alias));
        }
    }

    r->aliases = g_steal_pointer (&aliases);

    snapd_request_complete (request, NULL);
}

static SoupMessage *
generate_post_snapctl_request (SnapdRequest *request)
{
    SnapdRequestPostSnapctl *r = SNAPD_REQUEST_POST_SNAPCTL (request);
    SoupMessage *message;
    g_autoptr(JsonBuilder) builder = NULL;
    int i;

    message = generate (request, "POST", "/v2/snapctl");

    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "context-id");
    json_builder_add_string_value (builder, r->context_id);
    json_builder_set_member_name (builder, "args");
    json_builder_begin_array (builder);
    for (i = 0; r->args[i] != NULL; i++)
        json_builder_add_string_value (builder, r->args[i]);
    json_builder_end_array (builder);
    json_builder_end_object (builder);
    set_json_body (message, builder);

    return message;
}

static void
parse_post_snapctl_response (SnapdRequest *request)
{
    SnapdRequestPostSnapctl *r = SNAPD_REQUEST_POST_SNAPCTL (request);
    g_autoptr(JsonObject) response = NULL;
    g_autoptr(JsonObject) result = NULL;
    GError *error = NULL;

    response = snapd_request_parse_json_response (request, &error);
    if (response == NULL) {
        snapd_request_complete (request, error);
        return;
    }
    result = _snapd_json_get_sync_result_o (response, &error);
    if (result == NULL) {
        snapd_request_complete (request, error);
        return;
    }

    r->stdout_output = g_strdup (_snapd_json_get_string (result, "stdout", NULL));
    r->stderr_output = g_strdup (_snapd_json_get_string (result, "stderr", NULL));

    snapd_request_complete (request, NULL);
}

static SnapdRequest *
get_first_request (SnapdClient *client)
{
    SnapdClientPrivate *priv = snapd_client_get_instance_private (client);
    GList *link;
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&priv->requests_mutex);

    for (link = priv->requests; link; link = link->next) {
        SnapdRequest *request = link->data;

        /* Return first non-async request or async request without change id */
        if (SNAPD_IS_REQUEST_ASYNC (request)) {
            if (SNAPD_REQUEST_ASYNC (request)->change_id == NULL)
                return request;
        }
        else
            return request;
    }

    return NULL;
}

static void
parse_response (SnapdRequest *request)
{
    GError *error = NULL;

    if (SNAPD_REQUEST_GET_CLASS (request)->parse_response == NULL) {
        error = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_FAILED,
                             "Unknown request");
        snapd_request_complete (request, error);
        return;
    }

    SNAPD_REQUEST_GET_CLASS (request)->parse_response (request);
}

/* Check if we have all HTTP chunks */
static gboolean
have_chunked_body (const gchar *body, gsize body_length)
{
    while (TRUE) {
        const gchar *chunk_start;
        gsize chunk_header_length, chunk_length;

        /* Read chunk header, stopping on zero length chunk */
        chunk_start = g_strstr_len (body, body_length, "\r\n");
        if (chunk_start == NULL)
            return FALSE;
        chunk_header_length = chunk_start - body + 2;
        chunk_length = strtoul (body, NULL, 16);
        if (chunk_length == 0)
            return TRUE;

        /* Check enough space for chunk body */
        if (chunk_header_length + chunk_length + strlen ("\r\n") > body_length)
            return FALSE;
        // FIXME: Validate that \r\n is on the end of a chunk?
        body += chunk_header_length + chunk_length;
        body_length -= chunk_header_length + chunk_length;
    }
}

/* If more than one HTTP chunk, re-order buffer to contain one chunk.
 * Assumes body is a valid chunked data block (as checked with have_chunked_body()) */
static void
compress_chunks (gchar *body, gsize body_length, gchar **combined_start, gsize *combined_length, gsize *total_length)
{
    gchar *chunk_start;

    /* Use first chunk as output */
    *combined_length = strtoul (body, NULL, 16);
    *combined_start = strstr (body, "\r\n") + 2;

    /* Copy any remaining chunks beside the first one */
    chunk_start = *combined_start + *combined_length + 2;
    while (TRUE) {
        gsize chunk_length;

        chunk_length = strtoul (chunk_start, NULL, 16);
        chunk_start = strstr (chunk_start, "\r\n") + 2;
        if (chunk_length == 0)
            break;

        /* Move this chunk on the end of the last one */
        memmove (*combined_start + *combined_length, chunk_start, chunk_length);
        *combined_length += chunk_length;

        chunk_start += chunk_length + 2;
    }

    *total_length = chunk_start - body;
}

static gboolean
read_cb (GSocket *socket, GIOCondition condition, SnapdRequest *r)
{
    SnapdRequestPrivate *r_priv = snapd_request_get_instance_private (r);
    SnapdClientPrivate *client_priv = snapd_client_get_instance_private (r_priv->client);
    gssize n_read;
    gchar *body;
    gsize header_length;
    g_autoptr(SoupMessageHeaders) headers = NULL;
    gchar *combined_start;
    gsize content_length, combined_length;
    g_autoptr(GError) error = NULL;
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&client_priv->buffer_mutex);

    if (client_priv->n_read + READ_SIZE > client_priv->buffer->len)
        g_byte_array_set_size (client_priv->buffer, client_priv->n_read + READ_SIZE);
    n_read = g_socket_receive (socket,
                               (gchar *) (client_priv->buffer->data + client_priv->n_read),
                               READ_SIZE,
                               NULL,
                               &error);

    if (n_read == 0) {
        g_autoptr(GError) e = NULL;

        e = g_error_new (SNAPD_ERROR,
                         SNAPD_ERROR_READ_FAILED,
                         "snapd connection closed");
        complete_all_requests (r_priv->client, e);

        r_priv->read_source = NULL;
        return G_SOURCE_REMOVE;
    }

    if (n_read < 0) {
        g_autoptr(GError) e = NULL;

        if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            return TRUE;

        e = g_error_new (SNAPD_ERROR,
                         SNAPD_ERROR_READ_FAILED,
                         "Failed to read from snapd: %s",
                         error->message);
        complete_all_requests (r_priv->client, e);

        r_priv->read_source = NULL;
        return G_SOURCE_REMOVE;
    }

    client_priv->n_read += n_read;

    while (TRUE) {
        SnapdRequest *request;
        SnapdRequestPrivate *priv;
        g_autoptr(GError) e = NULL;

        /* Look for header divider */
        body = g_strstr_len ((gchar *) client_priv->buffer->data, client_priv->n_read, "\r\n\r\n");
        if (body == NULL)
            return G_SOURCE_CONTINUE;
        body += 4;
        header_length = body - (gchar *) client_priv->buffer->data;

        /* Match this response to the next uncompleted request */
        request = get_first_request (r_priv->client);
        if (request == NULL) {
            g_warning ("Ignoring unexpected response");
            return G_SOURCE_REMOVE;
        }
        priv = snapd_request_get_instance_private (request);

        /* Parse headers */
        if (!soup_headers_parse_response ((gchar *) client_priv->buffer->data, header_length, priv->message->response_headers,
                                          NULL, &priv->message->status_code, &priv->message->reason_phrase)) {
            e = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Failed to parse headers from snapd");
            complete_all_requests (priv->client, e);
            priv->read_source = NULL;
            return G_SOURCE_REMOVE;
        }

        /* Read content and process content */
        switch (soup_message_headers_get_encoding (priv->message->response_headers)) {
        case SOUP_ENCODING_EOF:
            if (!g_socket_is_closed (client_priv->snapd_socket))
                return G_SOURCE_CONTINUE;

            content_length = client_priv->n_read - header_length;
            soup_message_body_append (priv->message->response_body, SOUP_MEMORY_COPY, body, content_length);
            parse_response (request);
            break;

        case SOUP_ENCODING_CHUNKED:
            // FIXME: Find a way to abort on error
            if (!have_chunked_body (body, client_priv->n_read - header_length))
                return G_SOURCE_CONTINUE;

            compress_chunks (body, client_priv->n_read - header_length, &combined_start, &combined_length, &content_length);
            soup_message_body_append (priv->message->response_body, SOUP_MEMORY_COPY, combined_start, combined_length);
            parse_response (request);
            break;

        case SOUP_ENCODING_CONTENT_LENGTH:
            content_length = soup_message_headers_get_content_length (priv->message->response_headers);
            if (client_priv->n_read < header_length + content_length)
                return G_SOURCE_CONTINUE;

            soup_message_body_append (priv->message->response_body, SOUP_MEMORY_COPY, body, content_length);
            parse_response (request);
            break;

        default:
            e = g_error_new (SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Unable to determine header encoding");
            complete_all_requests (priv->client, e);
            priv->read_source = NULL;
            return G_SOURCE_REMOVE;
        }

        /* Move remaining data to the start of the buffer */
        g_byte_array_remove_range (client_priv->buffer, 0, header_length + content_length);
        client_priv->n_read -= header_length + content_length;
    }
}

static void
request_cancelled_cb (GCancellable *cancellable, SnapdRequest *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);

    /* Asynchronous requests require asking snapd to stop them */
    if (SNAPD_IS_REQUEST_ASYNC (request)) {
        SnapdRequestAsync *r = SNAPD_REQUEST_ASYNC (request);

        /* Cancel if we have got a response from snapd */
        if (r->change_id != NULL)
            send_cancel (r);
    }
    else {
        GError *error = NULL;
        g_cancellable_set_error_if_cancelled (priv->cancellable, &error);
        snapd_request_respond (request, error);
    }
}

static void
send_request (SnapdClient *client, SnapdRequest *request)
{
    SnapdRequestPrivate *priv = snapd_request_get_instance_private (request);
    SnapdClientPrivate *client_priv = snapd_client_get_instance_private (client);
    g_autofree gchar *accept_languages = NULL;
    g_autoptr(GByteArray) request_data = NULL;
    SoupURI *uri;
    SoupMessageHeadersIter iter;
    const char *name, *value;
    g_autoptr(SoupBuffer) buffer = NULL;
    gssize n_written;
    g_autoptr(GError) local_error = NULL;

    // NOTE: Would love to use libsoup but it doesn't support unix sockets
    // https://bugzilla.gnome.org/show_bug.cgi?id=727563

    priv->client = client;

    {
        g_autoptr(GMutexLocker) locker = g_mutex_locker_new (&client_priv->requests_mutex);
        client_priv->requests = g_list_append (client_priv->requests, request);
    }

    if (priv->cancellable != NULL)
        priv->cancelled_id = g_cancellable_connect (priv->cancellable, G_CALLBACK (request_cancelled_cb), request, NULL);

    priv->message = SNAPD_REQUEST_GET_CLASS (request)->generate_request (request);
    soup_message_headers_append (priv->message->request_headers, "Host", "");
    soup_message_headers_append (priv->message->request_headers, "Connection", "keep-alive");
    if (client_priv->user_agent != NULL)
        soup_message_headers_append (priv->message->request_headers, "User-Agent", client_priv->user_agent);
    if (client_priv->allow_interaction)
        soup_message_headers_append (priv->message->request_headers, "X-Allow-Interaction", "true");

    accept_languages = get_accept_languages ();
    soup_message_headers_append (priv->message->request_headers, "Accept-Language", accept_languages);

    if (client_priv->auth_data != NULL) {
        g_autoptr(GString) authorization = NULL;
        gchar **discharges;
        gsize i;

        authorization = g_string_new ("");
        g_string_append_printf (authorization, "Macaroon root=\"%s\"", snapd_auth_data_get_macaroon (client_priv->auth_data));
        discharges = snapd_auth_data_get_discharges (client_priv->auth_data);
        if (discharges != NULL)
            for (i = 0; discharges[i] != NULL; i++)
                g_string_append_printf (authorization, ",discharge=\"%s\"", discharges[i]);
        soup_message_headers_append (priv->message->request_headers, "Authorization", authorization->str);
    }

    request_data = g_byte_array_new ();
    append_string (request_data, priv->message->method);
    append_string (request_data, " ");
    uri = soup_message_get_uri (priv->message);
    append_string (request_data, uri->path);
    if (uri->query != NULL) {
        append_string (request_data, "?");
        append_string (request_data, uri->query);
    }
    append_string (request_data, " HTTP/1.1\r\n");
    soup_message_headers_iter_init (&iter, priv->message->request_headers);
    while (soup_message_headers_iter_next (&iter, &name, &value)) {
        append_string (request_data, name);
        append_string (request_data, ": ");
        append_string (request_data, value);
        append_string (request_data, "\r\n");
    }
    append_string (request_data, "\r\n");

    buffer = soup_message_body_flatten (priv->message->request_body);
    g_byte_array_append (request_data, (const guint8 *) buffer->data, buffer->length);

    if (client_priv->snapd_socket == NULL) {
        g_autoptr(GSocketAddress) address = NULL;
        g_autoptr(GError) error_local = NULL;

        client_priv->snapd_socket = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                                  G_SOCKET_TYPE_STREAM,
                                                  G_SOCKET_PROTOCOL_DEFAULT,
                                                  &error_local);
        if (client_priv->snapd_socket == NULL) {
            GError *error = g_error_new (SNAPD_ERROR,
                                         SNAPD_ERROR_CONNECTION_FAILED,
                                         "Unable to create snapd socket: %s",
                                         error_local->message);
            snapd_request_complete (request, error);
            return;
        }
        g_socket_set_blocking (client_priv->snapd_socket, FALSE);
        address = g_unix_socket_address_new (client_priv->socket_path);
        if (!g_socket_connect (client_priv->snapd_socket, address, priv->cancellable, &error_local)) {
            g_clear_object (&client_priv->snapd_socket);
            GError *error = g_error_new (SNAPD_ERROR,
                                         SNAPD_ERROR_CONNECTION_FAILED,
                                         "Unable to connect snapd socket: %s",
                                         error_local->message);
            snapd_request_complete (request, error);
            return;
        }
    }

    priv->read_source = g_socket_create_source (client_priv->snapd_socket, G_IO_IN, NULL);
    g_source_set_name (priv->read_source, "snapd-glib-read-source");
    g_source_set_callback (priv->read_source, (GSourceFunc) read_cb, request, NULL);
    g_source_attach (priv->read_source, priv->context);

    /* send HTTP request */
    // FIXME: Check for short writes
    n_written = g_socket_send (client_priv->snapd_socket, (const gchar *) request_data->data, request_data->len, priv->cancellable, &local_error);
    if (n_written < 0) {
        GError *error = g_error_new (SNAPD_ERROR,
                                     SNAPD_ERROR_WRITE_FAILED,
                                     "Failed to write to snapd: %s",
                                     local_error->message);
        snapd_request_complete (request, error);
    }
}

/**
 * snapd_client_connect_async:
 * @client: a #SnapdClient
 * @cancellable: (allow-none): a #GCancellable or %NULL
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * This method is no longer required and does nothing, snapd-glib now connects on demand.
 *
 * Since: 1.3
 * Deprecated: 1.24
 */
void
snapd_client_connect_async (SnapdClient *client,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error_local = NULL;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    task = g_task_new (client, cancellable, callback, user_data);
    g_task_return_boolean (task, TRUE);
}

/**
 * snapd_client_connect_finish:
 * @client: a #SnapdClient
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_connect_async().
 * See snapd_client_connect_sync() for more information.
 *
 * Returns: %TRUE if successfully connected to snapd.
 *
 * Since: 1.3
 */
gboolean
snapd_client_connect_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (g_task_is_valid (result, client), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * snapd_client_set_socket_path:
 * @client: a #SnapdClient
 * @socket_path: (allow-none): a socket path or %NULL to reset to the default.
 *
 * Set the Unix socket path to connect to snapd with.
 * Defaults to the system socket.
 *
 * Since: 1.24
 */
void
snapd_client_set_socket_path (SnapdClient *client, const gchar *socket_path)
{
    SnapdClientPrivate *priv;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    priv = snapd_client_get_instance_private (client);
    g_free (priv->socket_path);
    if (priv->socket_path != NULL)
        priv->socket_path = g_strdup (socket_path);
    else
        priv->socket_path = g_strdup (SNAPD_SOCKET);
}

/**
 * snapd_client_get_socket_path:
 * @client: a #SnapdClient
 *
 * Get the unix socket path to connect to snapd with.
 *
 * Returns: socket path.
 *
 * Since: 1.24
 */
const gchar *
snapd_client_get_socket_path (SnapdClient *client)
{
    SnapdClientPrivate *priv;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);

    priv = snapd_client_get_instance_private (client);
    return priv->socket_path;
}

/**
 * snapd_client_set_user_agent:
 * @client: a #SnapdClient
 * @user_agent: (allow-none): a user agent or %NULL.
 *
 * Set the HTTP user-agent that is sent with each request to snapd.
 * Defaults to "snapd-glib/VERSION".
 *
 * Since: 1.16
 */
void
snapd_client_set_user_agent (SnapdClient *client, const gchar *user_agent)
{
    SnapdClientPrivate *priv;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    priv = snapd_client_get_instance_private (client);
    g_free (priv->user_agent);
    priv->user_agent = g_strdup (user_agent);
}

/**
 * snapd_client_get_user_agent:
 * @client: a #SnapdClient
 *
 * Get the HTTP user-agent that is sent with each request to snapd.
 *
 * Returns: user agent or %NULL if none set.
 *
 * Since: 1.16
 */
const gchar *
snapd_client_get_user_agent (SnapdClient *client)
{
    SnapdClientPrivate *priv;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);

    priv = snapd_client_get_instance_private (client);
    return priv->user_agent;
}

/**
 * snapd_client_set_allow_interaction:
 * @client: a #SnapdClient
 * @allow_interaction: whether to allow interaction.
 *
 * Set whether snapd operations are allowed to interact with the user.
 * This affects operations that use polkit authorisation.
 * Defaults to TRUE.
 *
 * Since: 1.19
 */
void
snapd_client_set_allow_interaction (SnapdClient *client, gboolean allow_interaction)
{
    SnapdClientPrivate *priv;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    priv = snapd_client_get_instance_private (client);
    priv->allow_interaction = allow_interaction;
}

/**
 * snapd_client_get_allow_interaction:
 * @client: a #SnapdClient
 *
 * Get whether snapd operations are allowed to interact with the user.
 *
 * Returns: %TRUE if interaction is allowed.
 *
 * Since: 1.19
 */
gboolean
snapd_client_get_allow_interaction (SnapdClient *client)
{
    SnapdClientPrivate *priv;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);

    priv = snapd_client_get_instance_private (client);
    return priv->allow_interaction;
}

/**
 * snapd_client_login_async:
 * @client: a #SnapdClient.
 * @username: usename to log in with.
 * @password: password to log in with.
 * @otp: (allow-none): response to one-time password challenge.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get authorization to install/remove snaps.
 * See snapd_client_login_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_login_async (SnapdClient *client,
                          const gchar *username, const gchar *password, const gchar *otp,
                          GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostLogin *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_POST_LOGIN (g_object_new (snapd_request_post_login_get_type (), NULL));
    request->username = g_strdup (username);
    request->password = g_strdup (password);
    request->otp = g_strdup (otp);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_login_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_login_async().
 * See snapd_client_login_sync() for more information.
 *
 * Returns: (transfer full): a #SnapdAuthData or %NULL on error.
 *
 * Since: 1.0
 */
SnapdAuthData *
snapd_client_login_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestPostLogin *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_LOGIN (result), NULL);

    request = SNAPD_REQUEST_POST_LOGIN (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->auth_data);
}

/**
 * snapd_client_set_auth_data:
 * @client: a #SnapdClient.
 * @auth_data: (allow-none): a #SnapdAuthData or %NULL.
 *
 * Set the authorization data to use for requests. Authorization data can be
 * obtained by:
 *
 * - Logging into snapd using snapd_login_sync() or snapd_client_login_sync()
 *   (requires root access)
 *
 * - Using an existing authorization with snapd_auth_data_new().
 *
 * Since: 1.0
 */
void
snapd_client_set_auth_data (SnapdClient *client, SnapdAuthData *auth_data)
{
    SnapdClientPrivate *priv;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    priv = snapd_client_get_instance_private (client);
    g_clear_object (&priv->auth_data);
    if (auth_data != NULL)
        priv->auth_data = g_object_ref (auth_data);
}

/**
 * snapd_client_get_auth_data:
 * @client: a #SnapdClient.
 *
 * Get the authorization data that is used for requests.
 *
 * Returns: (transfer none) (allow-none): a #SnapdAuthData or %NULL.
 *
 * Since: 1.0
 */
SnapdAuthData *
snapd_client_get_auth_data (SnapdClient *client)
{
    SnapdClientPrivate *priv;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);

    priv = snapd_client_get_instance_private (client);

    return priv->auth_data;
}

/**
 * snapd_client_get_system_information_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Request system information asynchronously from snapd.
 * See snapd_client_get_system_information_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_get_system_information_async (SnapdClient *client,
                                           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_system_info_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_system_information_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_system_information_async().
 * See snapd_client_get_system_information_sync() for more information.
 *
 * Returns: (transfer full): a #SnapdSystemInformation or %NULL on error.
 *
 * Since: 1.0
 */
SnapdSystemInformation *
snapd_client_get_system_information_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetSystemInfo *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_SYSTEM_INFO (result), NULL);

    request = SNAPD_REQUEST_GET_SYSTEM_INFO (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->system_information);
}

/**
 * snapd_client_list_one_async:
 * @client: a #SnapdClient.
 * @name: name of snap to get.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get information of a single installed snap.
 * See snapd_client_list_one_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_list_one_async (SnapdClient *client,
                             const gchar *name,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_GET_SNAP (g_object_new (snapd_request_get_snap_get_type (), NULL));
    request->name = g_strdup (name);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_list_one_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_list_one_async().
 * See snapd_client_list_one_sync() for more information.
 *
 * Returns: (transfer full): a #SnapdSnap or %NULL on error.
 *
 * Since: 1.0
 */
SnapdSnap *
snapd_client_list_one_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetSnap *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_SNAP (result), NULL);

    request = SNAPD_REQUEST_GET_SNAP (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->snap);
}

/**
 * snapd_client_get_apps_async:
 * @client: a #SnapdClient.
 * @flags: a set of #SnapdGetAppsFlags to control what results are returned.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get information on installed apps.
 * See snapd_client_get_apps_sync() for more information.
 *
 * Since: 1.25
 */
void
snapd_client_get_apps_async (SnapdClient *client,
                             SnapdGetAppsFlags flags,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetApps *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_GET_APPS (g_object_new (snapd_request_get_apps_get_type (), NULL));
    request->flags = flags;
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_apps_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_apps_async().
 * See snapd_client_get_apps_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdApp): an array of #SnapdApp or %NULL on error.
 *
 * Since: 1.25
 */
GPtrArray *
snapd_client_get_apps_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetApps *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_APPS (result), NULL);

    request = SNAPD_REQUEST_GET_APPS (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->apps);
}

/**
 * snapd_client_get_icon_async:
 * @client: a #SnapdClient.
 * @name: name of snap to get icon for.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get the icon for an installed snap.
 * See snapd_client_get_icon_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_get_icon_async (SnapdClient *client,
                             const gchar *name,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetIcon *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_GET_ICON (g_object_new (snapd_request_get_icon_get_type (), NULL));
    request->name = g_strdup (name);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_icon_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_icon_async().
 * See snapd_client_get_icon_sync() for more information.
 *
 * Returns: (transfer full): a #SnapdIcon or %NULL on error.
 *
 * Since: 1.0
 */
SnapdIcon *
snapd_client_get_icon_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetIcon *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_ICON (result), NULL);

    request = SNAPD_REQUEST_GET_ICON (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->icon);
}

/**
 * snapd_client_list_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get information on all installed snaps.
 * See snapd_client_list_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_list_async (SnapdClient *client,
                         GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_snaps_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_list_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_list_async().
 * See snapd_client_list_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdSnap): an array of #SnapdSnap or %NULL on error.
 *
 * Since: 1.0
 */
GPtrArray *
snapd_client_list_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetSnaps *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_SNAPS (result), NULL);

    request = SNAPD_REQUEST_GET_SNAPS (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->snaps);
}

/**
 * snapd_client_get_assertions_async:
 * @client: a #SnapdClient.
 * @type: assertion type to get.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get assertions.
 * See snapd_client_get_assertions_sync() for more information.
 *
 * Since: 1.8
 */
void
snapd_client_get_assertions_async (SnapdClient *client,
                                   const gchar *type,
                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetAssertions *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (type != NULL);

    request = SNAPD_REQUEST_GET_ASSERTIONS (g_object_new (snapd_request_get_assertions_get_type (), NULL));
    request->type = g_strdup (type);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_assertions_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_assertions_async().
 * See snapd_client_get_assertions_sync() for more information.
 *
 * Returns: (transfer full) (array zero-terminated=1): an array of assertions or %NULL on error.
 *
 * Since: 1.8
 */
gchar **
snapd_client_get_assertions_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetAssertions *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_ASSERTIONS (result), NULL);

    request = SNAPD_REQUEST_GET_ASSERTIONS (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->assertions);
}

/**
 * snapd_client_add_assertions_async:
 * @client: a #SnapdClient.
 * @assertions: assertions to add.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously add an assertion.
 * See snapd_client_add_assertions_sync() for more information.
 *
 * Since: 1.8
 */
void
snapd_client_add_assertions_async (SnapdClient *client,
                                   gchar **assertions,
                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostAssertions *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (assertions != NULL);

    request = SNAPD_REQUEST_POST_ASSERTIONS (g_object_new (snapd_request_post_assertions_get_type (), NULL));
    request->assertions = g_strdupv (assertions);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_add_assertions_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_add_assertions_async().
 * See snapd_client_add_assertions_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.8
 */
gboolean
snapd_client_add_assertions_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_ASSERTIONS (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_get_interfaces_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get the installed snap interfaces.
 * See snapd_client_get_interfaces_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_get_interfaces_async (SnapdClient *client,
                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_interfaces_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_interfaces_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @plugs: (out) (allow-none) (transfer container) (element-type SnapdPlug): the location to store the array of #SnapdPlug or %NULL.
 * @slots: (out) (allow-none) (transfer container) (element-type SnapdSlot): the location to store the array of #SnapdSlot or %NULL.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_interfaces_async().
 * See snapd_client_get_interfaces_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_get_interfaces_finish (SnapdClient *client, GAsyncResult *result,
                                    GPtrArray **plugs, GPtrArray **slots,
                                    GError **error)
{
    SnapdRequestGetInterfaces *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_INTERFACES (result), FALSE);

    request = SNAPD_REQUEST_GET_INTERFACES (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return FALSE;
    if (plugs)
       *plugs = request->plugs != NULL ? g_ptr_array_ref (request->plugs) : NULL;
    if (slots)
       *slots = request->slots != NULL ? g_ptr_array_ref (request->slots) : NULL;
    return TRUE;
}

static void
send_interface_request (SnapdClient *client,
                        const gchar *action,
                        const gchar *plug_snap, const gchar *plug_name,
                        const gchar *slot_snap, const gchar *slot_name,
                        SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostInterfaces *request;

    request = SNAPD_REQUEST_POST_INTERFACES (g_object_new (snapd_request_post_interfaces_get_type (), NULL));
    request->action = g_strdup (action);
    request->plug_snap = g_strdup (plug_snap);
    request->plug_name = g_strdup (plug_name);
    request->slot_snap = g_strdup (slot_snap);
    request->slot_name = g_strdup (slot_name);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_connect_interface_async:
 * @client: a #SnapdClient.
 * @plug_snap: name of snap containing plug.
 * @plug_name: name of plug to connect.
 * @slot_snap: name of snap containing socket.
 * @slot_name: name of slot to connect.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously connect two interfaces together.
 * See snapd_client_connect_interface_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_connect_interface_async (SnapdClient *client,
                                      const gchar *plug_snap, const gchar *plug_name,
                                      const gchar *slot_snap, const gchar *slot_name,
                                      SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (SNAPD_IS_CLIENT (client));

    send_interface_request (client,
                            "connect",
                            plug_snap, plug_name,
                            slot_snap, slot_name,
                            progress_callback, progress_callback_data,
                            cancellable, callback, user_data);
}

/**
 * snapd_client_connect_interface_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_connect_interface_async().
 * See snapd_client_connect_interface_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_connect_interface_finish (SnapdClient *client,
                                       GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_INTERFACES (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_disconnect_interface_async:
 * @client: a #SnapdClient.
 * @plug_snap: name of snap containing plug.
 * @plug_name: name of plug to disconnect.
 * @slot_snap: name of snap containing socket.
 * @slot_name: name of slot to disconnect.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously disconnect two interfaces.
 * See snapd_client_disconnect_interface_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_disconnect_interface_async (SnapdClient *client,
                                         const gchar *plug_snap, const gchar *plug_name,
                                         const gchar *slot_snap, const gchar *slot_name,
                                         SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                         GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (SNAPD_IS_CLIENT (client));

    send_interface_request (client,
                            "disconnect",
                            plug_snap, plug_name,
                            slot_snap, slot_name,
                            progress_callback, progress_callback_data,
                            cancellable, callback, user_data);
}

/**
 * snapd_client_disconnect_interface_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_disconnect_interface_async().
 * See snapd_client_disconnect_interface_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_disconnect_interface_finish (SnapdClient *client,
                                          GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_INTERFACES (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_find_async:
 * @client: a #SnapdClient.
 * @flags: a set of #SnapdFindFlags to control how the find is performed.
 * @query: query string to send.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously find snaps in the store.
 * See snapd_client_find_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_find_async (SnapdClient *client,
                         SnapdFindFlags flags, const gchar *query,
                         GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (query != NULL);
    snapd_client_find_section_async (client, flags, NULL, query, cancellable, callback, user_data);
}

/**
 * snapd_client_find_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @suggested_currency: (allow-none): location to store the ISO 4217 currency that is suggested to purchase with.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_find_async().
 * See snapd_client_find_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdSnap): an array of #SnapdSnap or %NULL on error.
 *
 * Since: 1.0
 */
GPtrArray *
snapd_client_find_finish (SnapdClient *client, GAsyncResult *result, gchar **suggested_currency, GError **error)
{
    return snapd_client_find_section_finish (client, result, suggested_currency, error);
}

/**
 * snapd_client_find_section_async:
 * @client: a #SnapdClient.
 * @flags: a set of #SnapdFindFlags to control how the find is performed.
 * @section: (allow-none): store section to search in or %NULL to search in all sections.
 * @query: (allow-none): query string to send or %NULL to get all snaps from the given section.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously find snaps in the store.
 * See snapd_client_find_section_sync() for more information.
 *
 * Since: 1.7
 */
void
snapd_client_find_section_async (SnapdClient *client,
                                 SnapdFindFlags flags, const gchar *section, const gchar *query,
                                 GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetFind *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (section != NULL || query != NULL);

    request = SNAPD_REQUEST_GET_FIND (g_object_new (snapd_request_get_find_get_type (), NULL));
    request->flags = flags;
    request->section = g_strdup (section);
    request->query = g_strdup (query);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_find_section_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @suggested_currency: (allow-none): location to store the ISO 4217 currency that is suggested to purchase with.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_find_async().
 * See snapd_client_find_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdSnap): an array of #SnapdSnap or %NULL on error.
 *
 * Since: 1.7
 */
GPtrArray *
snapd_client_find_section_finish (SnapdClient *client, GAsyncResult *result, gchar **suggested_currency, GError **error)
{
    SnapdRequestGetFind *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_FIND (result), NULL);

    request = SNAPD_REQUEST_GET_FIND (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;

    if (suggested_currency != NULL)
        *suggested_currency = g_steal_pointer (&request->suggested_currency);
    return g_steal_pointer (&request->snaps);
}

/**
 * snapd_client_find_refreshable_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously find snaps in store that are newer revisions than locally installed versions.
 * See snapd_client_find_refreshable_sync() for more information.
 *
 * Since: 1.8
 */
void
snapd_client_find_refreshable_async (SnapdClient *client,
                                     GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestGetFind *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_GET_FIND (g_object_new (snapd_request_get_find_get_type (), NULL));
    request->flags = SNAPD_FIND_FLAGS_SELECT_REFRESH;
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_find_refreshable_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_find_refreshable_async().
 * See snapd_client_find_refreshable_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdSnap): an array of #SnapdSnap or %NULL on error.
 *
 * Since: 1.5
 */
GPtrArray *
snapd_client_find_refreshable_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetFind *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_FIND (result), NULL);

    request = SNAPD_REQUEST_GET_FIND (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;

    return g_steal_pointer (&request->snaps);
}

/**
 * snapd_client_install_async:
 * @client: a #SnapdClient.
 * @name: name of snap to install.
 * @channel: (allow-none): channel to install from or %NULL for default.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously install a snap from the store.
 * See snapd_client_install_sync() for more information.
 *
 * Since: 1.0
 * Deprecated: 1.12: Use snapd_client_install2_async()
 */
void
snapd_client_install_async (SnapdClient *client,
                            const gchar *name, const gchar *channel,
                            SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    snapd_client_install2_async (client, SNAPD_INSTALL_FLAGS_NONE, name, channel, NULL, progress_callback, progress_callback_data, cancellable, callback, user_data);
}

/**
 * snapd_client_install_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_install_async().
 * See snapd_client_install_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 * Deprecated: 1.12: Use snapd_client_install2_finish()
 */
gboolean
snapd_client_install_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    return snapd_client_install2_finish (client, result, error);
}

/**
 * snapd_client_install2_async:
 * @client: a #SnapdClient.
 * @flags: a set of #SnapdInstallFlags to control install options.
 * @name: name of snap to install.
 * @channel: (allow-none): channel to install from or %NULL for default.
 * @revision: (allow-none): revision to install or %NULL for default.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously install a snap from the store.
 * See snapd_client_install2_sync() for more information.
 *
 * Since: 1.12
 */
void
snapd_client_install2_async (SnapdClient *client,
                             SnapdInstallFlags flags,
                             const gchar *name, const gchar *channel, const gchar *revision,
                             SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (name != NULL);

    request = SNAPD_REQUEST_POST_SNAP (g_object_new (snapd_request_post_snap_get_type (), NULL));
    request->flags = flags;
    request->name = g_strdup (name);
    request->action = g_strdup ("install");
    request->channel = g_strdup (channel);
    request->revision = g_strdup (revision);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_install2_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_install2_async().
 * See snapd_client_install2_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.12
 */
gboolean
snapd_client_install2_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

typedef struct
{
    SnapdClient *client;
    SnapdRequestPostSnapStream *request;
    GCancellable *cancellable;
    GInputStream *stream;
} InstallStreamData;

static InstallStreamData *
install_stream_data_new (SnapdClient *client, SnapdRequestPostSnapStream *request, GCancellable *cancellable, GInputStream *stream)
{
    InstallStreamData *data;

    data = g_slice_new (InstallStreamData);
    data->client = g_object_ref (client);
    data->request = g_object_ref (request);
    data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
    data->stream = g_object_ref (stream);

    return data;
}

static void
install_stream_data_free (InstallStreamData *data)
{
    g_object_unref (data->client);
    g_object_unref (data->request);
    if (data->cancellable != NULL)
        g_object_unref (data->cancellable);
    g_object_unref (data->stream);
    g_slice_free (InstallStreamData, data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(InstallStreamData, install_stream_data_free)

static void
stream_read_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(InstallStreamData) data = user_data;
    g_autoptr(GBytes) read_data = NULL;
    g_autoptr(GError) error = NULL;

    read_data = g_input_stream_read_bytes_finish (data->stream, result, &error);
    if (!snapd_request_return_error (SNAPD_REQUEST (data->request), &error))
        return;

    if (g_bytes_get_size (read_data) == 0)
        send_request (data->client, SNAPD_REQUEST (data->request));
    else {
        InstallStreamData *d;
        g_byte_array_append (data->request->snap_contents, g_bytes_get_data (read_data, NULL), g_bytes_get_size (read_data));
        d = g_steal_pointer (&data);
        g_input_stream_read_bytes_async (d->stream, 65535, G_PRIORITY_DEFAULT, d->cancellable, stream_read_cb, d);
    }
}

/**
 * snapd_client_install_stream_async:
 * @client: a #SnapdClient.
 * @flags: a set of #SnapdInstallFlags to control install options.
 * @stream: a #GInputStream containing the snap file contents to install.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously install a snap.
 * See snapd_client_install_stream_sync() for more information.
 *
 * Since: 1.9
 */
void
snapd_client_install_stream_async (SnapdClient *client,
                                   SnapdInstallFlags flags,
                                   GInputStream *stream,
                                   SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnapStream *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (G_IS_INPUT_STREAM (stream));

    request = SNAPD_REQUEST_POST_SNAP_STREAM (g_object_new (snapd_request_post_snap_stream_get_type (), NULL));
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;
    request->install_flags = flags;
    request->snap_contents = g_byte_array_new ();
    g_input_stream_read_bytes_async (stream, 65535, G_PRIORITY_DEFAULT, cancellable, stream_read_cb, install_stream_data_new (client, request, cancellable, stream));
}

/**
 * snapd_client_install_stream_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_install_stream_async().
 * See snapd_client_install_stream_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.9
 */
gboolean
snapd_client_install_stream_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP_STREAM (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_try_async:
 * @client: a #SnapdClient.
 * @path: path to snap directory to try.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously try a snap.
 * See snapd_client_try_sync() for more information.
 *
 * Since: 1.9
 */
void
snapd_client_try_async (SnapdClient *client,
                        const gchar *path,
                        SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnapTry *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (path != NULL);

    request = g_object_new (snapd_request_post_snap_try_get_type (), NULL);
    request->path = g_strdup (path);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_try_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_try_async().
 * See snapd_client_try_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.9
 */
gboolean
snapd_client_try_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP_TRY (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_refresh_async:
 * @client: a #SnapdClient.
 * @name: name of snap to refresh.
 * @channel: (allow-none): channel to refresh from or %NULL for default.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously ensure an installed snap is at the latest version.
 * See snapd_client_refresh_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_refresh_async (SnapdClient *client,
                            const gchar *name, const gchar *channel,
                            SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (name != NULL);

    request = SNAPD_REQUEST_POST_SNAP (g_object_new (snapd_request_post_snap_get_type (), NULL));
    request->name = g_strdup (name);
    request->action = g_strdup ("refresh");
    request->channel = g_strdup (channel);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_refresh_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_refresh_async().
 * See snapd_client_refresh_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_refresh_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_refresh_all_async:
 * @client: a #SnapdClient.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously ensure all snaps are updated to their latest versions.
 * See snapd_client_refresh_all_sync() for more information.
 *
 * Since: 1.5
 */
void
snapd_client_refresh_all_async (SnapdClient *client,
                                SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnaps *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = SNAPD_REQUEST_POST_SNAPS (g_object_new (snapd_request_post_snaps_get_type (), NULL));
    request->action = g_strdup ("refresh");
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_refresh_all_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_refresh_all_async().
 * See snapd_client_refresh_all_sync() for more information.
 *
 * Returns: (transfer full): a %NULL-terminated array of the snap names refreshed or %NULL on error.
 *
 * Since: 1.5
 */
gchar **
snapd_client_refresh_all_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequest *request;
    g_autoptr(GPtrArray) snap_names = NULL;
    JsonObject *o;
    g_autoptr(JsonArray) a = NULL;
    guint i;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAPS (result), FALSE);

    request = SNAPD_REQUEST (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;

    if (SNAPD_REQUEST_ASYNC (request)->async_data == NULL || json_node_get_value_type (SNAPD_REQUEST_ASYNC (request)->async_data) != JSON_TYPE_OBJECT) {
        g_set_error_literal (error,
                             SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "Unexpected result type");
        return NULL;
    }
    o = json_node_get_object (SNAPD_REQUEST_ASYNC (request)->async_data);
    if (o == NULL) {
        g_set_error_literal (error,
                             SNAPD_ERROR,
                             SNAPD_ERROR_READ_FAILED,
                             "No result returned");
        return NULL;
    }
    a = _snapd_json_get_array (o, "snap-names");
    snap_names = g_ptr_array_new ();
    for (i = 0; i < json_array_get_length (a); i++) {
        JsonNode *node = json_array_get_element (a, i);
        if (json_node_get_value_type (node) != G_TYPE_STRING) {
            g_set_error_literal (error,
                                 SNAPD_ERROR,
                                 SNAPD_ERROR_READ_FAILED,
                                 "Unexpected snap name type");
            return NULL;
        }

        g_ptr_array_add (snap_names, g_strdup (json_node_get_string (node)));
    }
    g_ptr_array_add (snap_names, NULL);

    return (gchar **) g_steal_pointer (&snap_names->pdata);
}

/**
 * snapd_client_remove_async:
 * @client: a #SnapdClient.
 * @name: name of snap to remove.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously uninstall a snap.
 * See snapd_client_remove_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_remove_async (SnapdClient *client,
                           const gchar *name,
                           SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (name != NULL);

    request = SNAPD_REQUEST_POST_SNAP (g_object_new (snapd_request_post_snap_get_type (), NULL));
    request->name = g_strdup (name);
    request->action = g_strdup ("remove");
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_remove_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_remove_async().
 * See snapd_client_remove_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_remove_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_enable_async:
 * @client: a #SnapdClient.
 * @name: name of snap to enable.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously enable an installed snap.
 * See snapd_client_enable_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_enable_async (SnapdClient *client,
                           const gchar *name,
                           SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (name != NULL);

    request = SNAPD_REQUEST_POST_SNAP (g_object_new (snapd_request_post_snap_get_type (), NULL));
    request->name = g_strdup (name);
    request->action = g_strdup ("enable");
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_enable_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_enable_async().
 * See snapd_client_enable_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_enable_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_disable_async:
 * @client: a #SnapdClient.
 * @name: name of snap to disable.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously disable an installed snap.
 * See snapd_client_disable_sync() for more information.
 *
 * Since: 1.0
 */
void
snapd_client_disable_async (SnapdClient *client,
                            const gchar *name,
                            SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnap *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (name != NULL);

    request = SNAPD_REQUEST_POST_SNAP (g_object_new (snapd_request_post_snap_get_type (), NULL));
    request->name = g_strdup (name);
    request->action = g_strdup ("disable");
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_disable_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_disable_async().
 * See snapd_client_disable_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.0
 */
gboolean
snapd_client_disable_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAP (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_check_buy_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously check if able to buy snaps.
 * See snapd_client_check_buy_sync() for more information.
 *
 * Since: 1.3
 */
void
snapd_client_check_buy_async (SnapdClient *client,
                              GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_buy_ready_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_check_buy_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_check_buy_async().
 * See snapd_client_check_buy_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.3
 */
gboolean
snapd_client_check_buy_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_BUY_READY (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_buy_async:
 * @client: a #SnapdClient.
 * @id: id of snap to buy.
 * @amount: amount of currency to spend, e.g. 0.99.
 * @currency: the currency to buy with as an ISO 4217 currency code, e.g. "NZD".
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously buy a snap from the store.
 * See snapd_client_buy_sync() for more information.
 *
 * Since: 1.3
 */
void
snapd_client_buy_async (SnapdClient *client,
                        const gchar *id, gdouble amount, const gchar *currency,
                        GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostBuy *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (id != NULL);
    g_return_if_fail (currency != NULL);

    request = SNAPD_REQUEST_POST_BUY (g_object_new (snapd_request_post_buy_get_type (), NULL));
    request->id = g_strdup (id);
    request->amount = amount;
    request->currency = g_strdup (currency);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_buy_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_buy_async().
 * See snapd_client_buy_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.3
 */
gboolean
snapd_client_buy_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_BUY (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_create_user_async:
 * @client: a #SnapdClient.
 * @email: the email of the user to create.
 * @flags: a set of #SnapdCreateUserFlags to control how the user account is created.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously create a local user account.
 * See snapd_client_create_user_sync() for more information.
 *
 * Since: 1.3
 */
void
snapd_client_create_user_async (SnapdClient *client,
                                const gchar *email, SnapdCreateUserFlags flags,
                                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostCreateUser *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (email != NULL);

    request = SNAPD_REQUEST_POST_CREATE_USER (g_object_new (snapd_request_post_create_user_get_type (), NULL));
    request->email = g_strdup (email);
    request->flags = flags;
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_create_user_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_create_user_async().
 * See snapd_client_create_user_sync() for more information.
 *
 * Returns: (transfer full): a #SnapdUserInformation or %NULL on error.
 *
 * Since: 1.3
 */
SnapdUserInformation *
snapd_client_create_user_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestPostCreateUser *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_CREATE_USER (result), NULL);

    request = SNAPD_REQUEST_POST_CREATE_USER (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->user_information);
}

/**
 * snapd_client_create_users_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously create local user accounts using the system-user assertions that are valid for this device.
 * See snapd_client_create_users_sync() for more information.
 *
 * Since: 1.3
 */
void
snapd_client_create_users_async (SnapdClient *client,
                                 GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_post_create_users_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_create_users_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_create_users_async().
 * See snapd_client_create_users_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdUserInformation): an array of #SnapdUserInformation or %NULL on error.
 *
 * Since: 1.3
 */
GPtrArray *
snapd_client_create_users_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestPostCreateUsers *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_CREATE_USERS (result), NULL);

    request = SNAPD_REQUEST_POST_CREATE_USERS (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->users_information);
}

/**
 * snapd_client_get_sections_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously create a local user account.
 * See snapd_client_get_sections_sync() for more information.
 *
 * Since: 1.7
 */
void
snapd_client_get_sections_async (SnapdClient *client,
                                 GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_sections_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_sections_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_sections_async().
 * See snapd_client_get_sections_sync() for more information.
 *
 * Returns: (transfer full) (array zero-terminated=1): an array of section names or %NULL on error.
 *
 * Since: 1.7
 */
gchar **
snapd_client_get_sections_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetSections *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_SECTIONS (result), NULL);

    request = SNAPD_REQUEST_GET_SECTIONS (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->sections);
}

/**
 * snapd_client_get_aliases_async:
 * @client: a #SnapdClient.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously get the available aliases.
 * See snapd_client_get_aliases_sync() for more information.
 *
 * Since: 1.8
 */
void
snapd_client_get_aliases_async (SnapdClient *client,
                                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequest *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    request = g_object_new (snapd_request_get_aliases_get_type (), NULL);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_get_aliases_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_get_aliases_async().
 * See snapd_client_get_aliases_sync() for more information.
 *
 * Returns: (transfer container) (element-type SnapdAlias): an array of #SnapdAlias or %NULL on error.
 *
 * Since: 1.8
 */
GPtrArray *
snapd_client_get_aliases_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    SnapdRequestGetAliases *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), NULL);
    g_return_val_if_fail (SNAPD_IS_REQUEST_GET_ALIASES (result), NULL);

    request = SNAPD_REQUEST_GET_ALIASES (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return NULL;
    return g_steal_pointer (&request->aliases);
}

static void
send_change_aliases_request (SnapdClient *client,
                             const gchar *action,
                             const gchar *snap, const gchar *app, const gchar *alias,
                             SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostAliases *request;

    request = SNAPD_REQUEST_POST_ALIASES (g_object_new (snapd_request_post_aliases_get_type (), NULL));
    request->action = g_strdup (action);
    request->snap = g_strdup (snap);
    request->app = g_strdup (app);
    request->alias = g_strdup (alias);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);
    SNAPD_REQUEST_ASYNC (request)->progress_callback = progress_callback;
    SNAPD_REQUEST_ASYNC (request)->progress_callback_data = progress_callback_data;

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_alias_async:
 * @client: a #SnapdClient.
 * @snap: the name of the snap to modify.
 * @app: an app in the snap to make the alias to.
 * @alias: the name of the alias (i.e. the command that will run this app).
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously create an alias to an app.
 * See snapd_client_alias_sync() for more information.
 *
 * Since: 1.25
 */
void
snapd_client_alias_async (SnapdClient *client,
                          const gchar *snap, const gchar *app, const gchar *alias,
                          SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                          GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (snap != NULL);
    g_return_if_fail (app != NULL);
    g_return_if_fail (alias != NULL);
    send_change_aliases_request (client, "alias", snap, app, alias, progress_callback, progress_callback_data, cancellable, callback, user_data);
}

/**
 * snapd_client_alias_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_alias_async().
 * See snapd_client_alias_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.25
 */
gboolean
snapd_client_alias_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_ALIASES (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_unalias_async:
 * @client: a #SnapdClient.
 * @snap: (allow-none): the name of the snap to modify or %NULL.
 * @alias: (allow-none): the name of the alias to remove or %NULL to remove all aliases for the given snap.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously remove an alias from an app.
 * See snapd_client_unalias_sync() for more information.
 *
 * Since: 1.25
 */
void
snapd_client_unalias_async (SnapdClient *client,
                            const gchar *snap, const gchar *alias,
                            SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (alias != NULL);
    send_change_aliases_request (client, "unalias", snap, NULL, alias, progress_callback, progress_callback_data, cancellable, callback, user_data);
}

/**
 * snapd_client_unalias_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_unalias_async().
 * See snapd_client_unalias_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.25
 */
gboolean
snapd_client_unalias_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_ALIASES (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_prefer_async:
 * @client: a #SnapdClient.
 * @snap: the name of the snap to modify.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously ???.
 * See snapd_client_prefer_sync() for more information.
 *
 * Since: 1.25
 */
void
snapd_client_prefer_async (SnapdClient *client,
                           const gchar *snap,
                           SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                           GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (snap != NULL);
    send_change_aliases_request (client, "prefer", snap, NULL, NULL, progress_callback, progress_callback_data, cancellable, callback, user_data);
}

/**
 * snapd_client_prefer_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_prefer_async().
 * See snapd_client_prefer_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.25
 */
gboolean
snapd_client_prefer_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_ALIASES (result), FALSE);

    return snapd_request_return_error (SNAPD_REQUEST (result), error);
}

/**
 * snapd_client_enable_aliases_async:
 * @client: a #SnapdClient.
 * @snap: the name of the snap to modify.
 * @aliases: the aliases to modify.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously change the state of aliases.
 * See snapd_client_enable_aliases_sync() for more information.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_alias_async()
 */
void
snapd_client_enable_aliases_async (SnapdClient *client,
                                   const gchar *snap, gchar **aliases,
                                   SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                   GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error_local = NULL;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    task = g_task_new (client, cancellable, callback, user_data);
    g_task_return_new_error (task, SNAPD_ERROR, SNAPD_ERROR_FAILED, "snapd_client_enable_aliases_async is deprecated");
}

/**
 * snapd_client_enable_aliases_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_enable_aliases_async().
 * See snapd_client_enable_aliases_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_unalias_finish()
 */
gboolean
snapd_client_enable_aliases_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (g_task_is_valid (result, client), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * snapd_client_disable_aliases_async:
 * @client: a #SnapdClient.
 * @snap: the name of the snap to modify.
 * @aliases: the aliases to modify.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously change the state of aliases.
 * See snapd_client_disable_aliases_sync() for more information.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_unalias_async()
 */
void
snapd_client_disable_aliases_async (SnapdClient *client,
                                    const gchar *snap, gchar **aliases,
                                    SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error_local = NULL;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    task = g_task_new (client, cancellable, callback, user_data);
    g_task_return_new_error (task, SNAPD_ERROR, SNAPD_ERROR_FAILED, "snapd_client_disable_aliases_async is deprecated");
}

/**
 * snapd_client_disable_aliases_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_disable_aliases_async().
 * See snapd_client_disable_aliases_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_unalias_finish()
 */
gboolean
snapd_client_disable_aliases_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (g_task_is_valid (result, client), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * snapd_client_reset_aliases_async:
 * @client: a #SnapdClient.
 * @snap: the name of the snap to modify.
 * @aliases: the aliases to modify.
 * @progress_callback: (allow-none) (scope async): function to callback with progress.
 * @progress_callback_data: (closure): user data to pass to @progress_callback.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously change the state of aliases.
 * See snapd_client_reset_aliases_sync() for more information.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_disable_aliases_async()
 */
void
snapd_client_reset_aliases_async (SnapdClient *client,
                                  const gchar *snap, gchar **aliases,
                                  SnapdProgressCallback progress_callback, gpointer progress_callback_data,
                                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error_local = NULL;

    g_return_if_fail (SNAPD_IS_CLIENT (client));

    task = g_task_new (client, cancellable, callback, user_data);
    g_task_return_new_error (task, SNAPD_ERROR, SNAPD_ERROR_FAILED, "snapd_client_reset_aliases_async is deprecated");
}

/**
 * snapd_client_reset_aliases_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_reset_aliases_async().
 * See snapd_client_reset_aliases_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.8
 * Deprecated: 1.25: Use snapd_client_disable_aliases_finish()
 */
gboolean
snapd_client_reset_aliases_finish (SnapdClient *client, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (g_task_is_valid (result, client), FALSE);

    return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * snapd_client_run_snapctl_async:
 * @client: a #SnapdClient.
 * @context_id: context for this call.
 * @args: the arguments to pass to snapctl.
 * @cancellable: (allow-none): a #GCancellable or %NULL.
 * @callback: (scope async): a #GAsyncReadyCallback to call when the request is satisfied.
 * @user_data: (closure): the data to pass to callback function.
 *
 * Asynchronously run a snapctl command.
 * See snapd_client_run_snapctl_sync() for more information.
 *
 * Since: 1.8
 */
void
snapd_client_run_snapctl_async (SnapdClient *client,
                                const gchar *context_id, gchar **args,
                                GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    SnapdRequestPostSnapctl *request;

    g_return_if_fail (SNAPD_IS_CLIENT (client));
    g_return_if_fail (context_id != NULL);
    g_return_if_fail (args != NULL);

    request = SNAPD_REQUEST_POST_SNAPCTL (g_object_new (snapd_request_post_snapctl_get_type (), NULL));
    request->context_id = g_strdup (context_id);
    request->args = g_strdupv (args);
    setup_request (SNAPD_REQUEST (request), cancellable, callback, user_data);

    send_request (client, SNAPD_REQUEST (request));
}

/**
 * snapd_client_run_snapctl_finish:
 * @client: a #SnapdClient.
 * @result: a #GAsyncResult.
 * @stdout_output: (out) (allow-none): the location to write the stdout from the command or %NULL.
 * @stderr_output: (out) (allow-none): the location to write the stderr from the command or %NULL.
 * @error: (allow-none): #GError location to store the error occurring, or %NULL to ignore.
 *
 * Complete request started with snapd_client_run_snapctl_async().
 * See snapd_client_run_snapctl_sync() for more information.
 *
 * Returns: %TRUE on success or %FALSE on error.
 *
 * Since: 1.8
 */
gboolean
snapd_client_run_snapctl_finish (SnapdClient *client, GAsyncResult *result,
                                 gchar **stdout_output, gchar **stderr_output,
                                 GError **error)
{
    SnapdRequestPostSnapctl *request;

    g_return_val_if_fail (SNAPD_IS_CLIENT (client), FALSE);
    g_return_val_if_fail (SNAPD_IS_REQUEST_POST_SNAPCTL (result), FALSE);

    request = SNAPD_REQUEST_POST_SNAPCTL (result);

    if (!snapd_request_return_error (SNAPD_REQUEST (request), error))
        return FALSE;

    if (stdout_output)
        *stdout_output = g_steal_pointer (&request->stdout_output);
    if (stderr_output)
        *stderr_output = g_steal_pointer (&request->stderr_output);

    return TRUE;
}

/**
 * snapd_client_new:
 *
 * Create a new client to talk to snapd.
 *
 * Returns: a new #SnapdClient
 *
 * Since: 1.0
 **/
SnapdClient *
snapd_client_new (void)
{
    return g_object_new (SNAPD_TYPE_CLIENT, NULL);
}

/**
 * snapd_client_new_from_socket:
 * @socket: A #GSocket that is connected to snapd.
 *
 * Create a new client to talk on an existing socket.
 *
 * Returns: a new #SnapdClient
 *
 * Since: 1.5
 **/
SnapdClient *
snapd_client_new_from_socket (GSocket *socket)
{
    SnapdClient *client;
    SnapdClientPrivate *priv;

    client = snapd_client_new ();
    priv = snapd_client_get_instance_private (SNAPD_CLIENT (client));
    priv->snapd_socket = g_object_ref (socket);
    g_socket_set_blocking (priv->snapd_socket, FALSE);

    return client;
}

static void
snapd_client_finalize (GObject *object)
{
    SnapdClientPrivate *priv = snapd_client_get_instance_private (SNAPD_CLIENT (object));

    g_mutex_clear (&priv->requests_mutex);
    g_mutex_clear (&priv->buffer_mutex);
    g_clear_pointer (&priv->socket_path, g_free);
    g_clear_pointer (&priv->user_agent, g_free);
    g_clear_object (&priv->auth_data);
    g_list_free_full (priv->requests, g_object_unref);
    priv->requests = NULL;
    if (priv->snapd_socket != NULL)
        g_socket_close (priv->snapd_socket, NULL);
    g_clear_object (&priv->snapd_socket);
    g_clear_pointer (&priv->buffer, g_byte_array_unref);

    G_OBJECT_CLASS (snapd_client_parent_class)->finalize (object);
}

static void
snapd_client_class_init (SnapdClientClass *klass)
{
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   gobject_class->finalize = snapd_client_finalize;
}

static void
snapd_client_init (SnapdClient *client)
{
    SnapdClientPrivate *priv = snapd_client_get_instance_private (client);

    priv->socket_path = g_strdup (SNAPD_SOCKET);
    priv->user_agent = g_strdup ("snapd-glib/" VERSION);
    priv->allow_interaction = TRUE;
    priv->buffer = g_byte_array_new ();
    g_mutex_init (&priv->requests_mutex);
    g_mutex_init (&priv->buffer_mutex);
}

static void
snapd_request_get_change_finalize (GObject *object)
{
    SnapdRequestGetChange *request = SNAPD_REQUEST_GET_CHANGE (object);

    g_clear_pointer (&request->change_id, g_free);

    G_OBJECT_CLASS (snapd_request_get_change_parent_class)->finalize (object);
}

static void
snapd_request_get_change_class_init (SnapdRequestGetChangeClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_change_request;
   request_class->parse_response = parse_get_change_response;
   gobject_class->finalize = snapd_request_get_change_finalize;
}

static void
snapd_request_get_change_init (SnapdRequestGetChange *request)
{
}

static void
snapd_request_post_change_finalize (GObject *object)
{
    SnapdRequestPostChange *request = SNAPD_REQUEST_POST_CHANGE (object);

    g_clear_pointer (&request->change_id, g_free);
    g_clear_pointer (&request->action, g_free);

    G_OBJECT_CLASS (snapd_request_post_change_parent_class)->finalize (object);
}

static void
snapd_request_post_change_class_init (SnapdRequestPostChangeClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_change_request;
   request_class->parse_response = parse_post_change_response;
   gobject_class->finalize = snapd_request_post_change_finalize;
}

static void
snapd_request_post_change_init (SnapdRequestPostChange *request)
{
}

static void
snapd_request_get_system_info_finalize (GObject *object)
{
    SnapdRequestGetSystemInfo *request = SNAPD_REQUEST_GET_SYSTEM_INFO (object);

    g_clear_object (&request->system_information);

    G_OBJECT_CLASS (snapd_request_get_system_info_parent_class)->finalize (object);
}

static void
snapd_request_get_system_info_class_init (SnapdRequestGetSystemInfoClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_system_info_request;
   request_class->parse_response = parse_get_system_info_response;
   gobject_class->finalize = snapd_request_get_system_info_finalize;
}

static void
snapd_request_get_system_info_init (SnapdRequestGetSystemInfo *request)
{
}

static void
snapd_request_post_login_finalize (GObject *object)
{
    SnapdRequestPostLogin *request = SNAPD_REQUEST_POST_LOGIN (object);

    g_clear_pointer (&request->username, g_free);
    g_clear_pointer (&request->password, g_free);
    g_clear_pointer (&request->otp, g_free);
    g_clear_object (&request->auth_data);

    G_OBJECT_CLASS (snapd_request_post_login_parent_class)->finalize (object);
}

static void
snapd_request_post_login_class_init (SnapdRequestPostLoginClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_login_request;
   request_class->parse_response = parse_post_login_response;
   gobject_class->finalize = snapd_request_post_login_finalize;
}

static void
snapd_request_post_login_init (SnapdRequestPostLogin *request)
{
}

static void
snapd_request_get_snaps_finalize (GObject *object)
{
    SnapdRequestGetSnaps *request = SNAPD_REQUEST_GET_SNAPS (object);

    g_clear_pointer (&request->snaps, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_get_snaps_parent_class)->finalize (object);
}

static void
snapd_request_get_snaps_class_init (SnapdRequestGetSnapsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_snaps_request;
   request_class->parse_response = parse_get_snaps_response;
   gobject_class->finalize = snapd_request_get_snaps_finalize;
}

static void
snapd_request_get_snaps_init (SnapdRequestGetSnaps *request)
{
}

static void
snapd_request_get_snap_finalize (GObject *object)
{
    SnapdRequestGetSnap *request = SNAPD_REQUEST_GET_SNAP (object);

    g_clear_pointer (&request->name, g_free);
    g_clear_object (&request->snap);

    G_OBJECT_CLASS (snapd_request_get_snap_parent_class)->finalize (object);
}

static void
snapd_request_get_snap_class_init (SnapdRequestGetSnapClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_snap_request;
   request_class->parse_response = parse_get_snap_response;
   gobject_class->finalize = snapd_request_get_snap_finalize;
}

static void
snapd_request_get_snap_init (SnapdRequestGetSnap *request)
{
}

static void
snapd_request_get_icon_finalize (GObject *object)
{
    SnapdRequestGetIcon *request = SNAPD_REQUEST_GET_ICON (object);

    g_clear_pointer (&request->name, g_free);
    g_clear_object (&request->icon);

    G_OBJECT_CLASS (snapd_request_get_icon_parent_class)->finalize (object);
}

static void
snapd_request_get_icon_class_init (SnapdRequestGetIconClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_icon_request;
   request_class->parse_response = parse_get_icon_response;
   gobject_class->finalize = snapd_request_get_icon_finalize;
}

static void
snapd_request_get_icon_init (SnapdRequestGetIcon *request)
{
}

static void
snapd_request_get_apps_finalize (GObject *object)
{
    SnapdRequestGetApps *request = SNAPD_REQUEST_GET_APPS (object);

    g_clear_pointer (&request->apps, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_get_apps_parent_class)->finalize (object);
}

static void
snapd_request_get_apps_class_init (SnapdRequestGetAppsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_apps_request;
   request_class->parse_response = parse_get_apps_response;
   gobject_class->finalize = snapd_request_get_apps_finalize;
}

static void
snapd_request_get_apps_init (SnapdRequestGetApps *request)
{
}

static void
snapd_request_get_sections_finalize (GObject *object)
{
    SnapdRequestGetSections *request = SNAPD_REQUEST_GET_SECTIONS (object);

    g_clear_pointer (&request->sections, g_strfreev);

    G_OBJECT_CLASS (snapd_request_get_sections_parent_class)->finalize (object);
}

static void
snapd_request_get_sections_class_init (SnapdRequestGetSectionsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_sections_request;
   request_class->parse_response = parse_get_sections_response;
   gobject_class->finalize = snapd_request_get_sections_finalize;
}

static void
snapd_request_get_sections_init (SnapdRequestGetSections *request)
{
}

static void
snapd_request_get_buy_ready_finalize (GObject *object)
{
    G_OBJECT_CLASS (snapd_request_get_buy_ready_parent_class)->finalize (object);
}

static void
snapd_request_get_buy_ready_class_init (SnapdRequestGetBuyReadyClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_buy_ready_request;
   request_class->parse_response = parse_get_buy_ready_response;
   gobject_class->finalize = snapd_request_get_buy_ready_finalize;
}

static void
snapd_request_get_buy_ready_init (SnapdRequestGetBuyReady *request)
{
}

static void
snapd_request_post_buy_finalize (GObject *object)
{
    SnapdRequestPostBuy *request = SNAPD_REQUEST_POST_BUY (object);

    g_clear_pointer (&request->id, g_free);
    g_clear_pointer (&request->currency, g_free);

    G_OBJECT_CLASS (snapd_request_post_buy_parent_class)->finalize (object);
}

static void
snapd_request_post_buy_class_init (SnapdRequestPostBuyClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_buy_request;
   request_class->parse_response = parse_post_buy_response;
   gobject_class->finalize = snapd_request_post_buy_finalize;
}

static void
snapd_request_post_buy_init (SnapdRequestPostBuy *request)
{
}

static void
snapd_request_get_find_finalize (GObject *object)
{
    SnapdRequestGetFind *request = SNAPD_REQUEST_GET_FIND (object);

    g_free (request->query);
    g_free (request->section);
    g_free (request->suggested_currency);
    g_clear_pointer (&request->snaps, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_get_find_parent_class)->finalize (object);
}

static void
snapd_request_get_find_class_init (SnapdRequestGetFindClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_find_request;
   request_class->parse_response = parse_get_find_response;
   gobject_class->finalize = snapd_request_get_find_finalize;
}

static void
snapd_request_get_find_init (SnapdRequestGetFind *request)
{
}

static void
snapd_request_async_finalize (GObject *object)
{
    SnapdRequestAsync *request = SNAPD_REQUEST_ASYNC (object);

    g_clear_pointer (&request->change_id, g_free);
    g_clear_pointer (&request->poll_source, g_source_destroy);
    g_clear_object (&request->change);
    g_clear_pointer (&request->async_data, json_node_unref);

    G_OBJECT_CLASS (snapd_request_async_parent_class)->finalize (object);
}

static void
snapd_request_async_class_init (SnapdRequestAsyncClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->parse_response = parse_async_response;
   gobject_class->finalize = snapd_request_async_finalize;
}

static void
snapd_request_async_init (SnapdRequestAsync *request)
{
}

static void
snapd_request_post_snap_finalize (GObject *object)
{
    SnapdRequestPostSnap *request = SNAPD_REQUEST_POST_SNAP (object);

    g_clear_pointer (&request->name, g_free);
    g_clear_pointer (&request->action, g_free);
    g_clear_pointer (&request->channel, g_free);
    g_clear_pointer (&request->revision, g_free);

    G_OBJECT_CLASS (snapd_request_post_snap_parent_class)->finalize (object);
}

static void
snapd_request_post_snap_class_init (SnapdRequestPostSnapClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_snap_request;
   gobject_class->finalize = snapd_request_post_snap_finalize;
}

static void
snapd_request_post_snap_init (SnapdRequestPostSnap *request)
{
}

static void
snapd_request_post_snaps_finalize (GObject *object)
{
    SnapdRequestPostSnaps *request = SNAPD_REQUEST_POST_SNAPS (object);

    g_clear_pointer (&request->action, g_free);

    G_OBJECT_CLASS (snapd_request_post_snaps_parent_class)->finalize (object);
}

static void
snapd_request_post_snaps_class_init (SnapdRequestPostSnapsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_snaps_request;
   gobject_class->finalize = snapd_request_post_snaps_finalize;
}

static void
snapd_request_post_snaps_init (SnapdRequestPostSnaps *request)
{
}

static void
snapd_request_post_snap_stream_finalize (GObject *object)
{
    SnapdRequestPostSnapStream *request = SNAPD_REQUEST_POST_SNAP_STREAM (object);

    g_clear_pointer (&request->snap_contents, g_byte_array_unref);

    G_OBJECT_CLASS (snapd_request_post_snap_stream_parent_class)->finalize (object);
}

static void
snapd_request_post_snap_stream_class_init (SnapdRequestPostSnapStreamClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_snap_stream_request;
   gobject_class->finalize = snapd_request_post_snap_stream_finalize;
}

static void
snapd_request_post_snap_stream_init (SnapdRequestPostSnapStream *request)
{
}

static void
snapd_request_post_snap_try_finalize (GObject *object)
{
    SnapdRequestPostSnapTry *request = SNAPD_REQUEST_POST_SNAP_TRY (object);

    g_clear_pointer (&request->path, g_free);

    G_OBJECT_CLASS (snapd_request_post_snap_try_parent_class)->finalize (object);
}

static void
snapd_request_post_snap_try_class_init (SnapdRequestPostSnapTryClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_snap_try_request;
   gobject_class->finalize = snapd_request_post_snap_try_finalize;
}

static void
snapd_request_post_snap_try_init (SnapdRequestPostSnapTry *request)
{
}

static void
snapd_request_get_aliases_finalize (GObject *object)
{
    SnapdRequestGetAliases *request = SNAPD_REQUEST_GET_ALIASES (object);

    g_clear_pointer (&request->aliases, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_get_aliases_parent_class)->finalize (object);
}

static void
snapd_request_get_aliases_class_init (SnapdRequestGetAliasesClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_aliases_request;
   request_class->parse_response = parse_get_aliases_response;
   gobject_class->finalize = snapd_request_get_aliases_finalize;
}

static void
snapd_request_get_aliases_init (SnapdRequestGetAliases *request)
{
}

static void
snapd_request_post_aliases_finalize (GObject *object)
{
    SnapdRequestPostAliases *request = SNAPD_REQUEST_POST_ALIASES (object);

    g_clear_pointer (&request->action, g_free);
    g_clear_pointer (&request->snap, g_free);
    g_clear_pointer (&request->app, g_free);
    g_clear_pointer (&request->alias, g_free);

    G_OBJECT_CLASS (snapd_request_post_aliases_parent_class)->finalize (object);
}

static void
snapd_request_post_aliases_class_init (SnapdRequestPostAliasesClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_aliases_request;
   gobject_class->finalize = snapd_request_post_aliases_finalize;
}

static void
snapd_request_post_aliases_init (SnapdRequestPostAliases *request)
{
}

static void
snapd_request_get_interfaces_finalize (GObject *object)
{
    SnapdRequestGetInterfaces *request = SNAPD_REQUEST_GET_INTERFACES (object);

    g_clear_pointer (&request->plugs, g_ptr_array_unref);
    g_clear_pointer (&request->slots, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_get_interfaces_parent_class)->finalize (object);
}

static void
snapd_request_get_interfaces_class_init (SnapdRequestGetInterfacesClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_interfaces_request;
   request_class->parse_response = parse_get_interfaces_response;
   gobject_class->finalize = snapd_request_get_interfaces_finalize;
}

static void
snapd_request_get_interfaces_init (SnapdRequestGetInterfaces *request)
{
}

static void
snapd_request_post_interfaces_finalize (GObject *object)
{
    SnapdRequestPostInterfaces *request = SNAPD_REQUEST_POST_INTERFACES (object);

    g_clear_pointer (&request->action, g_free);
    g_clear_pointer (&request->plug_snap, g_free);
    g_clear_pointer (&request->plug_name, g_free);
    g_clear_pointer (&request->slot_snap, g_free);
    g_clear_pointer (&request->slot_name, g_free);

    G_OBJECT_CLASS (snapd_request_post_interfaces_parent_class)->finalize (object);
}

static void
snapd_request_post_interfaces_class_init (SnapdRequestPostInterfacesClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_interfaces_request;
   gobject_class->finalize = snapd_request_post_interfaces_finalize;
}

static void
snapd_request_post_interfaces_init (SnapdRequestPostInterfaces *request)
{
}

static void
snapd_request_get_assertions_finalize (GObject *object)
{
    SnapdRequestGetAssertions *request = SNAPD_REQUEST_GET_ASSERTIONS (object);

    g_clear_pointer (&request->type, g_free);
    g_clear_pointer (&request->assertions, g_strfreev);

    G_OBJECT_CLASS (snapd_request_get_assertions_parent_class)->finalize (object);
}

static void
snapd_request_get_assertions_class_init (SnapdRequestGetAssertionsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_get_assertions_request;
   request_class->parse_response = parse_get_assertions_response;
   gobject_class->finalize = snapd_request_get_assertions_finalize;
}

static void
snapd_request_get_assertions_init (SnapdRequestGetAssertions *request)
{
}

static void
snapd_request_post_assertions_finalize (GObject *object)
{
    SnapdRequestPostAssertions *request = SNAPD_REQUEST_POST_ASSERTIONS (object);

    g_clear_pointer (&request->assertions, g_strfreev);

    G_OBJECT_CLASS (snapd_request_post_assertions_parent_class)->finalize (object);
}

static void
snapd_request_post_assertions_class_init (SnapdRequestPostAssertionsClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_assertions_request;
   request_class->parse_response = parse_post_assertions_response;
   gobject_class->finalize = snapd_request_post_assertions_finalize;
}

static void
snapd_request_post_assertions_init (SnapdRequestPostAssertions *request)
{
}

static void
snapd_request_post_create_user_finalize (GObject *object)
{
    SnapdRequestPostCreateUser *request = SNAPD_REQUEST_POST_CREATE_USER (object);

    g_clear_pointer (&request->email, g_free);
    g_clear_object (&request->user_information);

    G_OBJECT_CLASS (snapd_request_post_create_user_parent_class)->finalize (object);
}

static void
snapd_request_post_create_user_class_init (SnapdRequestPostCreateUserClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_create_user_request;
   request_class->parse_response = parse_post_create_user_response;
   gobject_class->finalize = snapd_request_post_create_user_finalize;
}

static void
snapd_request_post_create_user_init (SnapdRequestPostCreateUser *request)
{
}

static void
snapd_request_post_create_users_finalize (GObject *object)
{
    SnapdRequestPostCreateUsers *request = SNAPD_REQUEST_POST_CREATE_USERS (object);

    g_clear_pointer (&request->users_information, g_ptr_array_unref);

    G_OBJECT_CLASS (snapd_request_post_create_users_parent_class)->finalize (object);
}

static void
snapd_request_post_create_users_class_init (SnapdRequestPostCreateUsersClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_create_users_request;
   request_class->parse_response = parse_post_create_users_response;
   gobject_class->finalize = snapd_request_post_create_users_finalize;
}

static void
snapd_request_post_create_users_init (SnapdRequestPostCreateUsers *request)
{
}

static void
snapd_request_post_snapctl_finalize (GObject *object)
{
    SnapdRequestPostSnapctl *request = SNAPD_REQUEST_POST_SNAPCTL (object);

    g_clear_pointer (&request->context_id, g_free);
    g_clear_pointer (&request->args, g_strfreev);
    g_clear_pointer (&request->stdout_output, g_free);
    g_clear_pointer (&request->stderr_output, g_free);

    G_OBJECT_CLASS (snapd_request_post_snapctl_parent_class)->finalize (object);
}

static void
snapd_request_post_snapctl_class_init (SnapdRequestPostSnapctlClass *klass)
{
   SnapdRequestClass *request_class = SNAPD_REQUEST_CLASS (klass);
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   request_class->generate_request = generate_post_snapctl_request;
   request_class->parse_response = parse_post_snapctl_response;
   gobject_class->finalize = snapd_request_post_snapctl_finalize;
}

static void
snapd_request_post_snapctl_init (SnapdRequestPostSnapctl *request)
{
}
