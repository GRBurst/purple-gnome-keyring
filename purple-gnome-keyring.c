#ifndef VERSION
#define VERSION "1.2.0"
#endif

#include <dbus/dbus-glib.h> //Needed if implement dbus start fallback
#include <glib.h>

#ifndef G_GNUC_NULL_TERMINATED
#if __GNUC__ >= 4
#define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#else
#define G_GNUC_NULL_TERMINATED
#endif
#endif

#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#include <libsecret/secret.h>
#include <string.h>

#include "account.h"
#include "connection.h"
#include "core.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "request.h"
#include "signals.h"
#include "util.h"
#include "version.h"

// Plug status
typedef enum { ENABLED = 0,
    LOADED = 1,
    UNLOADED = 2,
    INITIALIZING = 3,
    DELETING = 4,
    STORING = 5,
    LOADING = 6 } status_type;

/* Preferences */
#define PLUGIN_ID "core-grburst-purple_gnome_keyring"
#define KEYRING_PLUG_STATUS_PREF "/plugins/core/purple_gnome_keyring/plug_status"
#define KEYRING_PLUG_STATUS_DEFAULT UNLOADED
#define KEYRING_CUSTOM_NAME_PREF "/plugins/core/purple_gnome_keyring/custom_keyring"
#define KEYRING_CUSTOM_NAME_DEFAULT FALSE
#define KEYRING_NAME_PREF "/plugins/core/purple_gnome_keyring/custom_keyring/keyring_name"
#define KEYRING_NAME_DEFAULT ""
#define KEYRING_AUTO_SAVE_PREF "/plugins/core/purple_gnome_keyring/auto_save"
#define KEYRING_AUTO_SAVE_DEFAULT TRUE
#define KEYRING_AUTO_LOCK_PREF "/plugins/core/purple_gnome_keyring/auto_lock"
#define KEYRING_AUTO_LOCK_DEFAULT FALSE

// Plugin handles
const SecretSchema* get_purple_schema(void) G_GNUC_CONST;
#define PURPLE_SCHEMA get_purple_schema()
//#define SECRET_SERVICE(inst)    (G_TYPE_CHECK_INSTANCE_CAST ((inst), SECRET_TYPE_SERVICE,   SecretService))
//#define SECRET_ITEM(inst)       (G_TYPE_CHECK_INSTANCE_CAST ((inst), SECRET_TYPE_ITEM,      SecretItem))

// Vars
PurplePlugin* gnome_keyring_plugin = NULL;
SecretCollection* plugin_collection = NULL;

// Prototypes
static void store_account_password(gpointer data, gpointer user_data);
static void load_account_password(gpointer data, gpointer user_data);
static void delete_account_password(gpointer data, gpointer user_data);

/**************************************************
 **************************************************
 **************** Schema related ******************
 **************************************************
 **************************************************/
const SecretSchema* get_purple_schema(void)
{
    static const SecretSchema schema = {
        "pidgin password scheme", SECRET_SCHEMA_NONE,
        {
            { "protocol", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "username", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { "NULL", 0 },
        }
    };
    return &schema;
}

static GHashTable* get_attributes(PurpleAccount* account)
{

    return secret_attributes_build(PURPLE_SCHEMA,
        "protocol", (gpointer*)purple_account_get_username(account),
        "username", (gpointer*)purple_account_get_username(account),
        NULL);
}

/**************************************************
 **************************************************
 ******************** MESSAGES ********************
 **************************************************
 **************************************************/
//PurpleNotifyMsgType { PURPLE_NOTIFY_MSG_ERROR = 0, PURPLE_NOTIFY_MSG_WARNING, PURPLE_NOTIFY_MSG_INFO }
// Show error dialog
static void dialog(PurpleNotifyMsgType type, const gchar* prim, const gchar* sec)
{
    purple_notify_message(
        gnome_keyring_plugin,
        type,
        "Gnome Keyring Plugin",
        prim,
        sec,
        NULL,
        NULL);
}

// Unified Error messages
static void print_protocol_error_message(const gchar* protocol_name, gchar* prim_msg, GError* error)
{
    GString* msg = g_string_new(NULL);
    g_string_append_printf(msg, "Error in %s account: %s", protocol_name, prim_msg);

    dialog(PURPLE_NOTIFY_MSG_ERROR,
        msg->str,
        error->message);
    g_error_free(error);
    g_string_free(msg, TRUE);
}

// Determine next action
/* static void nextAction(PurpleAccount* account, int status) */
/* { */
/*     switch(status) */
/*     { */
/*         case LOADING: */
/*             load_account_password(account, NULL); */
/*             break; */
/*         case STORING: */
/*             store_account_password(account, NULL); */
/*             break; */
/*         case DELETING: */
/*             delete_account_password(account, NULL); */
/*             break; */
/*     } */
/* } */

/**************************************************
 **************************************************
 *********** Collection initalization *************
 **************************************************
 **************************************************/
static void on_init_item_loaded(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)user_data;

    GError* error = NULL;
    GList* items = secret_collection_search_finish(plugin_collection, result, &error);

    if (error != NULL) {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Could not read init password", error);
    } else if (items == NULL) {
        purple_debug_info(PLUGIN_ID, "%s: Init password is empty - no password saved\n", account->protocol_id);
    } else {
        SecretItem* item = items->data;
        SecretValue* value = secret_item_get_secret(item);

        purple_debug_info(PLUGIN_ID, "Setting init password for %s with username %s\n", account->protocol_id, account->username);
        purple_account_set_password(account, secret_value_get_text(value));

        secret_value_unref(value);
        g_object_unref(item);
        g_list_free(items);
    }

    purple_request_close_with_handle(account);
    purple_account_set_enabled(account, purple_core_get_ui(), TRUE);
}

// This will do the trick
static void init_account(gpointer data, gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)data;

    if (!purple_account_get_remember_password(account)) {

        /* unlock_collection(plugin_collection); */
        purple_debug_info(PLUGIN_ID, "Loading init password %s with username %s\n", account->protocol_id, account->username);
        purple_account_set_enabled(account, purple_core_get_ui(), FALSE);

        secret_collection_search(plugin_collection,
            PURPLE_SCHEMA,
            get_attributes(account),
            SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS,
            NULL,
            on_init_item_loaded,
            data);

        /* if(purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF)) lock_collection(); */
    }
}

// Deferred enabling of accounts
static void init_accounts()
{
    /* GList* accounts     = purple_accounts_get_all(); */
    purple_debug_info(PLUGIN_ID, "Init accounts\n");

    GList* accounts = purple_accounts_get_all_active();
    g_list_foreach(accounts, init_account, NULL);
    g_list_free(accounts);
}

// Callback to lock collection
static void on_collection_locked(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    GError* error = NULL;
    GList* locked_collections = NULL;

    secret_service_lock_finish(SECRET_SERVICE(source),
        result,
        &locked_collections,
        &error);

    if (error != NULL) {
        dialog(PURPLE_NOTIFY_MSG_ERROR, "Could not lock Gnome Keyring.", error->message);
        g_error_free(error);
    } else if (locked_collections != NULL) {
        purple_debug_info(PLUGIN_ID, "Successfully locked collection\n");
        plugin_collection = locked_collections->data;
        g_list_free(locked_collections);
    }
}

// lock collection
static gboolean lock_collection()
{
    gboolean was_unlocked = FALSE;
    purple_debug_info(PLUGIN_ID, "Locking collection\n");

    if ((plugin_collection != NULL) && (!secret_collection_get_locked(plugin_collection))) {
        was_unlocked = TRUE;

        GList* unlocked_collections = NULL;
        unlocked_collections = g_list_append(unlocked_collections, plugin_collection);

        secret_service_lock(
            secret_collection_get_service(plugin_collection),
            unlocked_collections,
            NULL,
            on_collection_locked,
            NULL);

        g_list_free(unlocked_collections);

    } else {
        purple_debug_info(PLUGIN_ID, "Collection already locked\n");
        /* nextAction(status); */
    }

    return was_unlocked;
}

// Callback to unlock collection
static void on_collection_unlocked(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    GError* error = NULL;
    GList* unlocked_collections = NULL;

    secret_service_unlock_finish(SECRET_SERVICE(source),
        result,
        &unlocked_collections,
        &error);

    if (error != NULL) {
        dialog(PURPLE_NOTIFY_MSG_ERROR, "Could not unlock Gnome Keyring.", error->message);
        g_error_free(error);
    } else if (unlocked_collections != NULL) {
        purple_debug_info(PLUGIN_ID, "Successfully unlocked collection\n");

        plugin_collection = unlocked_collections->data;
        if (GPOINTER_TO_INT(user_data) == INITIALIZING)
            init_accounts();

        g_list_free(unlocked_collections);
    }
}

// Unlock collection
static gboolean unlock_collection(SecretCollection* collection, gpointer status)
{
    gboolean was_locked = FALSE;
    purple_debug_info(PLUGIN_ID, "Unlocking collection (if necessary)\n");

    if ((collection != NULL) && (secret_collection_get_locked(collection))) {
        was_locked = TRUE;

        GList* locked_collections = NULL;
        locked_collections = g_list_append(locked_collections, collection);

        purple_debug_info(PLUGIN_ID, "Unlocking collection\n");
        secret_service_unlock(secret_collection_get_service(collection),
            locked_collections,
            NULL,
            on_collection_unlocked,
            status);

        g_list_free(locked_collections);

    } else if (collection != NULL) {
        plugin_collection = collection;
        if (GPOINTER_TO_INT(status) == INITIALIZING)
            init_accounts();
        purple_debug_info(PLUGIN_ID, "Collection already unlocked\n");
    }

    g_object_unref(collection);

    return was_locked;
}

// Load alias collection callback
static void on_got_alias_collection(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    GError* error = NULL;
    SecretCollection* collection = secret_collection_for_alias_finish(result, &error);

    if (error != NULL) {
        dialog(PURPLE_NOTIFY_MSG_ERROR, "Could not load collection.", error->message);
        g_error_free(error);
    } else if (collection != NULL) {
        purple_debug_info(PLUGIN_ID, "Successfully loaded default collection\n");
        unlock_collection(collection, GINT_TO_POINTER(INITIALIZING));
    } else
        purple_debug_info(PLUGIN_ID, "No collection received - load collections first\n");
}

// Determine and load correct keyring
static void init_collection(SecretService* service)
{

    purple_debug_info(PLUGIN_ID, "Initializing secret collection\n");

    // Check if user defined a different collection name (not the alias default)
    if (purple_prefs_get_bool(KEYRING_CUSTOM_NAME_PREF)) {

        const gchar* collection_name = purple_prefs_get_string(KEYRING_NAME_PREF);
        GList* collections = secret_service_get_collections(service); // already loads collection items

        if (!collections) {
            purple_debug_info(PLUGIN_ID, "Could not load keyrings. Check if DBus in running!\n");
        } else {

            for (GList* li = collections; li != NULL; li = li->next) {
                gchar* label = secret_collection_get_label(li->data);
                if (strcmp(label, collection_name) == 0) {
                    unlock_collection(li->data, GINT_TO_POINTER(INITIALIZING));
                    purple_debug_info(PLUGIN_ID, "Determine collection by name: %s\n", label);
                    break;
                }
            }

            g_list_free(collections);
        }

    } else {
        purple_debug_info(PLUGIN_ID, "Loading default (alias) collection\n");
        secret_collection_for_alias(service,
            SECRET_COLLECTION_DEFAULT,
            SECRET_COLLECTION_LOAD_ITEMS,
            NULL,
            on_got_alias_collection,
            NULL);
    }
}

// Get service callback
static void on_got_service(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    GError* error = NULL;
    SecretService* service = secret_service_get_finish(result, &error);

    if (error != NULL) {
        dialog(PURPLE_NOTIFY_MSG_ERROR, "Could not connect to the Gnome Keyring.", error->message);
        g_error_free(error);
    } else if (service == NULL) {
        purple_debug_info(PLUGIN_ID, "No service detected\n");
    } else {
        purple_debug_info(PLUGIN_ID, "Successfully initialized secret service\n");
        init_collection(service);
        g_object_unref(service);
    }
}

// Opened service callback
/* static void on_service_opened(GObject* source, */
/*         GAsyncResult* result, */
/*         gpointer user_data) */
/* { */

/*     GError* error           = NULL; */
/*     SecretService* service  = secret_service_open_finish(result, &error); */

/*     if(error != NULL) */
/*     { */
/*         dialog( PURPLE_NOTIFY_MSG_ERROR, "Could not connect to the Gnome Keyring.", error->message); */
/*         g_error_free(error); */
/*     } */
/*     else if (service == NULL) */
/*     { */
/*         purple_debug_info(PLUGIN_ID, "No service detected\n" ); */
/*     } */
/*     else */
/*     { */
/*         purple_debug_info(PLUGIN_ID, "Successfully initialized secret service\n" ); */
/*         init_collection(service); */
/*         g_object_unref(service); */
/*     } */

/* } */

// Init collection
static void init_secret_service()
{
    purple_debug_info(PLUGIN_ID, "Initializing secret service\n");
    secret_service_get(SECRET_SERVICE_OPEN_SESSION | SECRET_SERVICE_LOAD_COLLECTIONS, NULL, on_got_service, NULL);
    /* secret_service_open(SECRET_TYPE_SERVICE, */
    /*         NULL, */
    /*         SECRET_SERVICE_OPEN_SESSION | SECRET_SERVICE_LOAD_COLLECTIONS, */
    /*         NULL, /1* GCancellable, *1/ */
    /*         on_service_opened, */
    /*         NULL); */
}

/* End of collection functions */

/**************************************************
 **************************************************
 ************ Store password pipline **************
 **************************************************
 **************************************************/

// Error check
static void on_item_created(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)user_data;
    GError* error = NULL;
    SecretItem* item = secret_item_create_finish(result, &error);

    purple_debug_info(PLUGIN_ID, "Finished storing password\n");

    if (error != NULL) {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Error saving passwort to keyring", error);
    } else {
        if (account->password != NULL) {

            // This will lead to crashes for some protocols, e.g. opensteamworks. Maybe time it or just use the signed on freeing...
            /* purple_account_set_password(account, "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Cras eu semper eros. Donec non gravida mi. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Phasellus malesuada nisl eget est elementum, in ullamcorper nullam."); */

            /* g_free(account->password); */
            /* account->password = NULL; */

            purple_debug_info(PLUGIN_ID, "Cleared password for %s with username %s\n", account->protocol_id, account->username);
        }

        purple_debug_info(PLUGIN_ID, "%s password successfully saved for %s\n", account->protocol_id, account->username);
        purple_account_set_remember_password(account, FALSE);
        g_object_unref(item);
    }

    /* if(purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF)) lock_collection(); */
}

// Store password in the keyring
static void store_account_password(gpointer data, gpointer user_data)
{
    /* unlock_collection(plugin_collection, NULL); */
    PurpleAccount* account = (PurpleAccount*)data;

    GString* label = g_string_new(NULL);
    g_string_append_printf(label, "Purple %s password for user: %s", purple_account_get_protocol_name(account), account->username);

    purple_debug_info(PLUGIN_ID, "Storing %s password with username %s\n", account->protocol_id, account->username);
    secret_item_create(plugin_collection,
        PURPLE_SCHEMA,
        get_attributes(account),
        label->str,
        secret_value_new(purple_account_get_password(account), -1, "text/plain"),
        SECRET_ITEM_CREATE_REPLACE,
        NULL,
        on_item_created,
        data);

    g_string_free(label, FALSE);
}

/**************************************************
 **************************************************
 ************* Load password pipline **************
 **************************************************
 **************************************************/

static void on_item_loaded(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)user_data;

    GError* error = NULL;
    GList* items = secret_collection_search_finish(plugin_collection, result, &error);

    if (error != NULL) {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Could not read password", error);
        g_error_free(error);
    } else if (items == NULL) {
        purple_debug_info(PLUGIN_ID, "%s: Password is empty - no password saved\n", account->protocol_id);
    } else {
        SecretItem* item = items->data;
        SecretValue* value = secret_item_get_secret(item);

        purple_debug_info(PLUGIN_ID, "Setting password for %s with username %s\n", account->protocol_id, account->username);
        purple_account_set_password(account, secret_value_get_text(value));

        secret_value_unref(value);
        g_object_unref(item);
        g_list_free(items);
    }
}

// Load password of account from secret collection
static void load_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)data;

    if (!purple_account_get_remember_password(account)) {

        /* unlock_collection(plugin_collection); */
        purple_debug_info(PLUGIN_ID, "Loading password %s with username %s\n", account->protocol_id, account->username);

        secret_collection_search(plugin_collection,
            PURPLE_SCHEMA,
            get_attributes(account),
            SECRET_SEARCH_UNLOCK | SECRET_SEARCH_LOAD_SECRETS,
            NULL,
            on_item_loaded,
            data);

        /* if(purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF)) lock_collection(); */
    }
}

/**************************************************
 **************************************************
 ************ Delete password pipline *************
 **************************************************
 **************************************************/

// Deleted callback
static void on_password_deleted(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    PurpleAccount* account = (PurpleAccount*)user_data;
    GError* error = NULL;
    gboolean success = secret_item_delete_finish(SECRET_ITEM(source), result, &error);

    if (error != NULL) {
        print_protocol_error_message(account->protocol_id, "Could not delete password.", error);
    } else {
        if (success)
            purple_debug_info(PLUGIN_ID, "Successfully deteted password for %s\n", account->protocol_id);
        else
            purple_debug_info(PLUGIN_ID, "Could not detete password for %s, but no error occured\n", account->protocol_id);
    }

    /* if(purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF)) lock_collection(); */
}

// Delete from collection
static void delete_collection_password(GObject* source,
    GAsyncResult* result,
    gpointer user_data)
{

    PurpleAccount* account = (PurpleAccount*)user_data;
    GError* error = NULL;
    GList* items = secret_collection_search_finish(plugin_collection, result, &error);

    if (error != NULL) {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Could not delete password", error);
    } else if (items == NULL) {
        purple_debug_info(PLUGIN_ID, "%s: No password found for deletion\n", account->protocol_id);
        /* print_protocol_info_message(purple_account_get_protocol_name(account), "Password is empty or no password given"); */
    } else {
        SecretItem* item = items->data;

        // Not valid operation if account is deleted -> separate it
        /* SecretValue* value  = secret_item_get_secret(item); */
        /* purple_account_set_password(account, secret_value_get_text(value)); */
        /* secret_value_unref(value); */

        secret_item_delete(item, NULL, on_password_deleted, user_data);

        g_object_unref(item);
        g_list_free(items);
    }
}

// Delete password function
static void delete_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*)data;

    purple_account_set_remember_password(account, FALSE);

    /* if(purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF)) unlock_collection(plugin_collection, NULL, DELETING); */

    secret_collection_search(plugin_collection,
        PURPLE_SCHEMA,
        get_attributes(account),
        SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS,
        NULL,
        delete_collection_password,
        data);
}

/**************************************************
 **************************************************
 **************** Plugin actions ******************
 **************************************************
 **************************************************/

// Store all passwords action
static void save_all_passwords(PurplePluginAction* action)
{
    gpointer user_data = NULL;
    GList* accounts = purple_accounts_get_all();
    g_list_foreach(accounts, store_account_password, user_data);
    /* purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished saving of passwords to keyring"); */
}

// Delete all passwords from keyring action
static void delete_all_passwords(PurplePluginAction* action)
{
    gpointer user_data = NULL;
    GList* accounts = purple_accounts_get_all();
    g_list_foreach(accounts, delete_account_password, user_data);

    /* purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished deleting of passwords from keyring", NULL); */
}

/**************************************************
 **************************************************
 **************** Plugin signals ******************
 **************************************************
 **************************************************/

// Account auth-failure helper
static void account_reset_password(PurpleAccount* account, const char* user_data)
{
    if (user_data != NULL) {
        purple_debug_info(PLUGIN_ID, "Resetting %s with username %s\n", account->protocol_id, account->username);
        purple_account_set_password(account, user_data);
        store_account_password(account, NULL);
    }
}

// Signal account added action
static void account_added(PurpleAccount* account, gpointer data)
{
    store_account_password(account, NULL);
    purple_debug_info(PLUGIN_ID, "Added %s with username %s\n", account->protocol_id, account->username);
}

// Signal account removed action
static void account_removed(PurpleAccount* account, gpointer data)
{
    delete_account_password(account, NULL);
    purple_debug_info(PLUGIN_ID, "Deleted %s with username %s\n", account->protocol_id, account->username);
}

// Account enabled
static void account_enabled(PurpleAccount* account, gpointer data)
{
    load_account_password(account, NULL);
    purple_debug_info(PLUGIN_ID, "Enabled %s with username %s\n", account->protocol_id, account->username);
}

// Account disabled
static void account_disabled(PurpleAccount* account, gpointer data)
{
    purple_debug_info(PLUGIN_ID, "Disabled %s with username %s\n", account->protocol_id, account->username);
}

// Account signed on
static void account_signed_on(PurpleAccount* account, gpointer data)
{
    static const char* lorem = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Cras eu semper eros. Donec non gravida mi. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Phasellus malesuada nisl eget est elementum, in ullamcorper nullam.";

    if ((account->password != NULL) && (purple_account_get_remember_password(account))) {
        store_account_password(account, data);
        purple_debug_info(PLUGIN_ID, "Signed on. Saving password for %s with username %s\n", account->protocol_id, account->username);
    } else if (account->password != NULL) {
        purple_account_set_password(account, lorem);
        g_free(account->password);
        account->password = NULL;
        purple_debug_info(PLUGIN_ID, "Signed on. Cleared password for %s with username %s\n", account->protocol_id, account->username);
    }
}

// Account auth-failure
static void account_connection_error(PurpleAccount* account, PurpleConnectionError err, const gchar* desc, gpointer data)
{
    purple_debug_info(PLUGIN_ID, "%s connection error %i with username %s\n", account->protocol_id, err, account->username);

    if (err == PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED) {
        purple_debug_info(PLUGIN_ID, "Auth error\n");
        purple_request_input(gnome_keyring_plugin,
            "Gnome Keyring",
            "Could not connect to the server due to authetication failure.",
            "Please insert the correct password. The password will be saved the choosen keyring.",
            0,
            FALSE,
            TRUE,
            NULL,
            "Save in keyring",
            G_CALLBACK(account_reset_password),
            "Cancel",
            NULL,
            account,
            NULL,
            NULL,
            account);

    } else if (err == PURPLE_CONNECTION_ERROR_NETWORK_ERROR) {
        load_account_password(account, NULL);
        purple_debug_info(PLUGIN_ID, "Enabled %s with username %s\n", account->protocol_id, account->username);
    }
}

// Core quitting
static void core_quitting(gpointer data)
{
    printf("quitting purple gnome keyring");
    purple_debug_info(PLUGIN_ID, "Quitting");
    purple_prefs_set_int(KEYRING_PLUG_STATUS_PREF, ENABLED);
}

/**************************************************
 ************* General plugin stuff ***************
 ****** Options, actions, initialisations *********
 **************************************************
 **************************************************/

/* Register plugin actions */
static GList* plugin_actions(PurplePlugin* plugin, gpointer context)
{
    GList* list = NULL;
    PurplePluginAction* action = NULL;

    const gchar* name = (purple_prefs_get_bool(KEYRING_CUSTOM_NAME_PREF) ? purple_prefs_get_string(KEYRING_NAME_PREF) : "(default)");

    GString* action_label = g_string_new(NULL);
    g_string_append_printf(action_label, "Save all passwords to keyring: %s", name);
    action = purple_plugin_action_new(action_label->str, save_all_passwords);
    list = g_list_append(list, action);

    g_string_assign(action_label, "Delete all passwords from keyring: ");
    g_string_append(action_label, name);
    action = purple_plugin_action_new(action_label->str, delete_all_passwords);
    list = g_list_append(list, action);

    return list;
}

// preference callback
/* static void custom_name_changed(const char *name, PurplePrefType type, gconstpointer val, gpointer data) */
/* { */
/*     purple_debug_info(PLUGIN_ID, "custom name pref changed: name = %s, type = %i\n", name, type); */
/* } */

// Plugin preference window
static PurplePluginPrefFrame* get_plugin_pref_frame(PurplePlugin* plugin)
{
    g_return_val_if_fail(plugin != NULL, FALSE);

    PurplePluginPrefFrame* frame;
    PurplePluginPref* ppref;

    frame = purple_plugin_pref_frame_new();

    ppref = purple_plugin_pref_new_with_label("Gnome Keyring settings");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(KEYRING_CUSTOM_NAME_PREF, "Use custom keyring (not default).\nYou must check this box to use the <Gnome Keyring name> option below");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(KEYRING_NAME_PREF, "Gnome Keyring name: ");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(KEYRING_AUTO_SAVE_PREF, "Save new passwords to Gnome Keyring");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label(KEYRING_AUTO_LOCK_PREF, "Lock keyring when closing messenger");
    purple_plugin_pref_frame_add(frame, ppref);

    return frame;
}

// Load plugin
static gboolean plugin_load(PurplePlugin* plugin)
{
    /* purple_debug_info(PLUGIN_ID, "Loading plugin"); */
    gnome_keyring_plugin = plugin;

    // Handles
    void* core_handle = purple_get_core();
    void* accounts_handle = purple_accounts_get_handle();

    // Core signals
    purple_signal_connect(core_handle, "quitting", plugin, PURPLE_CALLBACK(core_quitting), NULL);

    /* Accounts subsystem signals */
    purple_signal_connect(accounts_handle, "account-enabled", plugin, PURPLE_CALLBACK(account_enabled), NULL);
    purple_signal_connect(accounts_handle, "account-disabled", plugin, PURPLE_CALLBACK(account_disabled), NULL);
    purple_signal_connect(accounts_handle, "account-signed-on", plugin, PURPLE_CALLBACK(account_signed_on), NULL);
    purple_signal_connect(accounts_handle, "account-connection-error", plugin, PURPLE_CALLBACK(account_connection_error), NULL);

    if (purple_prefs_get_bool(KEYRING_AUTO_SAVE_PREF)) {
        purple_signal_connect(accounts_handle, "account-added", plugin, PURPLE_CALLBACK(account_added), NULL);
        purple_signal_connect(accounts_handle, "account-removed", plugin, PURPLE_CALLBACK(account_removed), NULL);
    }

    // Load collection when plugin is activated
    init_secret_service();

    if (purple_prefs_get_int(KEYRING_PLUG_STATUS_PREF) == UNLOADED) {
        purple_request_action(plugin,
            "Gnome Keyring",
            "Do you want to move your passwords to the keyring?",
            "You can do this later by choosing the appropriate menu option in Tools->Gnome Keyring Plugin\n\n(Info) This dialog appears because: \n1.) This is the first time you are running this plugin\n2.) You reenabled this plugin",
            0,
            NULL,
            NULL,
            NULL,
            NULL,
            2,
            "Yes",
            save_all_passwords,
            "No",
            NULL);
    }

    purple_prefs_set_int(KEYRING_PLUG_STATUS_PREF, LOADED);

    // Pref callbacks
    /* purple_prefs_connect_callback(plugin, KEYRING_CUSTOM_NAME_PREF, (PurplePrefCallback)custom_name_changed, NULL); */
    /* purple_prefs_trigger_callback(KEYRING_CUSTOM_NAME_PREF); */

    return TRUE;
}

// Unload plugin
static gboolean plugin_unload(PurplePlugin* plugin)
{
    printf("unloading purple gnome keyring\n");
    purple_debug_info(PLUGIN_ID, "Unloading plugin\n");

    if (purple_prefs_get_bool(KEYRING_AUTO_LOCK_PREF))
        lock_collection();
    g_object_unref(plugin_collection);
    secret_service_disconnect();

    if (purple_prefs_get_int(KEYRING_PLUG_STATUS_PREF) == LOADED)
        purple_prefs_set_int(KEYRING_PLUG_STATUS_PREF, UNLOADED);

    purple_signals_disconnect_by_handle(plugin);
    /* purple_prefs_disconnect_by_handle(plugin); */

    return TRUE;
}

// Preference info
static PurplePluginUiInfo prefs_info = {
    get_plugin_pref_frame,
    0,    /* page_num (reserved) */
    NULL, /* frame (reserved) */
    NULL,
    NULL,
    NULL,
    NULL
};

// Plugin general info
static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,
    NULL, /* UI requirement */
    0,    /* flags */
    NULL, /* GList of plugin dependencies */
    PURPLE_PRIORITY_LOWEST,
    /* PURPLE_PRIORITY_HIGHEST, */
    PLUGIN_ID,
    "Gnome Keyring Plugin",
    VERSION,
    "Use gnome keyring to securely store passwords.",
    "This plugin will use the gnome keyring to store and load passwords for your accounts.",
    "GRBurst",
    "https://github.com/GRBurst/purple-gnome-keyring",
    plugin_load,
    plugin_unload,
    NULL, /* plugin destory */
    NULL, /* UI-specific struct || PidginPluginUiInfo  */
    NULL, /* PurplePluginLoaderInfo || PurplePluginProtocolInfo  */
    &prefs_info,
    plugin_actions,
    NULL, /* reserved */
    NULL, /* reserved */
    NULL, /* reserved */
    NULL  /* reserved */
};

// Init function
static void init_plugin(PurplePlugin* plugin)
{
    purple_prefs_add_none("/plugins/core/purple_gnome_keyring");
    purple_prefs_add_bool(KEYRING_CUSTOM_NAME_PREF, KEYRING_CUSTOM_NAME_DEFAULT);
    purple_prefs_add_string(KEYRING_NAME_PREF, KEYRING_NAME_DEFAULT);

    purple_prefs_add_bool(KEYRING_AUTO_SAVE_PREF, KEYRING_AUTO_SAVE_DEFAULT);
    purple_prefs_add_bool(KEYRING_AUTO_LOCK_PREF, KEYRING_AUTO_LOCK_DEFAULT);

    purple_prefs_add_int(KEYRING_PLUG_STATUS_PREF, KEYRING_PLUG_STATUS_DEFAULT);

    purple_prefs_remove("/plugins/core/purple_gnome_keyring/keyring_name");
    purple_prefs_remove("/plugins/core/purple_gnome_keyring/plug_state");
}

PURPLE_INIT_PLUGIN(gnome_keyring, init_plugin, info)
