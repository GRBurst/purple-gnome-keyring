#ifndef VERSION
#define VERSION "0.9.1"
#endif

#include <glib.h>
#include <dbus/dbus-glib.h> //Needed if implement dbus start fallback

#ifndef G_GNUC_NULL_TERMINATED
# if __GNUC__ >= 4
#  define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
# else
#  define G_GNUC_NULL_TERMINATED
# endif
#endif

#ifndef PURPLE_PLUGINS
# define PURPLE_PLUGINS
#endif

#include <libsecret/secret.h>
#include <string.h>

#include "account.h"
#include "accountopt.h"
#include "connection.h"
#include "core.h"
#include "debug.h"
#include "notify.h"
#include "plugin.h"
#include "request.h"
#include "signals.h"
#include "util.h"
#include "version.h"

/* Preferences */
#define PLUGIN_ID "core-grburst-purple_gnome_keyring"
#define KEYRING_CUSTOM_NAME_PREF "/plugins/core/purple_gnome_keyring/use_custom_keyring"
#define KEYRING_CUSTOM_NAME_DEFAULT FALSE
#define KEYRING_NAME_PREF "/plugins/core/purple_gnome_keyring/keyring_name"
#define KEYRING_NAME_DEFAULT SECRET_COLLECTION_DEFAULT
#define KEYRING_AUTO_SAVE_PREF "/plugins/core/purple_gnome_keyring/auto_save"
#define KEYRING_AUTO_SAVE_DEFAULT TRUE
#define KEYRING_PLUG_STATE_PREF "/plugins/core/purple_gnome_keyring/plug_state"
#define KEYRING_PLUG_STATE_DEFAULT FALSE

// Plugin handles
const SecretSchema* get_purple_schema (void) G_GNUC_CONST;
#define PURPLE_SCHEMA   get_purple_schema()
#define SECRET_SERVICE(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), SECRET_TYPE_SERVICE, SecretService))
#define SECRET_ITEM(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), SECRET_TYPE_ITEM, SecretItem))


PurplePlugin* gnome_keyring_plugin  = NULL;
SecretCollection* plugin_collection = NULL;

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
            /* {  "program", SECRET_SCHEMA_ATTRIBUTE_STRING }, */
            {  "protocol", SECRET_SCHEMA_ATTRIBUTE_STRING },
            {  "username", SECRET_SCHEMA_ATTRIBUTE_STRING },
            {  "NULL", 0 },
        }
    };
    return &schema;
}

static GHashTable* get_attributes(PurpleAccount* account)
{
    const gchar* id  = purple_account_get_protocol_id(account);
    const gchar* un  = purple_account_get_username(account);

    GHashTable* attributes  = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attributes, "protocol" , (gpointer*) id);
    g_hash_table_insert(attributes, "username" , (gpointer*) un);

    return attributes;
}
/**************************************************
 **************************************************
 ******************** MESSAGES ********************
 **************************************************
 **************************************************/
//PurpleNotifyMsgType { PURPLE_NOTIFY_MSG_ERROR = 0, PURPLE_NOTIFY_MSG_WARNING, PURPLE_NOTIFY_MSG_INFO }
static void dialog(PurpleNotifyMsgType type, const gchar* prim, const gchar* sec)
{
    purple_notify_message(
            gnome_keyring_plugin,
            type,
            "Gnome Keyring Plugin",
            prim,
            sec,
            NULL,
            NULL
            );
}

// Unified Error messages
static void print_protocol_error_message(const gchar* protocol_name, gchar* prim_msg, GError* error)
{
    GString *msg = g_string_new(NULL);
    g_string_append_printf(msg, "Error in %s account: %s", protocol_name, prim_msg);

    dialog(PURPLE_NOTIFY_MSG_ERROR,
            msg->str,
            error->message);
    g_error_free(error);
    g_string_free(msg, TRUE);
}

// Unified inof messages
static void print_protocol_info_message(const gchar* protocol_name, gchar* prim_msg)
{
    GString* msg = g_string_new(NULL);
    g_string_append_printf(msg, "%s account: %s", protocol_name, prim_msg);

    dialog(PURPLE_NOTIFY_MSG_INFO,
            msg->str,
            NULL);

    g_string_free(msg, TRUE);
}


/**************************************************
 **************************************************
 *********** Collection initalization *************
 **************************************************
 **************************************************/

static SecretCollection* get_collection(SecretService* service)
{

    SecretCollection* collection    = NULL;
    const gchar* collection_name    = purple_prefs_get_string(KEYRING_NAME_PREF);

    // Check if user defined a different collection name (not the alias default)
    if(strcmp(collection_name, KEYRING_NAME_DEFAULT) != 0)
    {

        GList* collections = secret_service_get_collections(service);

        if (!collections)
        {
            purple_debug_info(PLUGIN_ID, "Could not load keyrings. Check if DBus in running!");
            return NULL;
        }

        for(GList* li = collections; li != NULL; li = li->next)
        {
            gchar* label = secret_collection_get_label(li->data);
            if(strcmp(label, collection_name) == 0)
            {
                collection = li->data;
                break;
            }
        }

        g_list_free(collections);
    }
    else
    {
        GError* error   = NULL;
        collection      = secret_collection_for_alias_sync(service,
                KEYRING_NAME_DEFAULT,
                SECRET_COLLECTION_LOAD_ITEMS,
                NULL,
                &error);

        if(error != NULL)
        {
            dialog( PURPLE_NOTIFY_MSG_ERROR, "Could not load collection.", error->message);
            g_error_free(error);
        }
    }

    return collection;
}



// Init collection
static void init_collection()
{
    GError* error = NULL;
    SecretService* service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION | SECRET_SERVICE_LOAD_COLLECTIONS, NULL, &error);

    if(error != NULL)
    {
        dialog( PURPLE_NOTIFY_MSG_ERROR, "Could not connect to the Gnome Keyring.", error->message);
        g_error_free(error);
    }
    else
    {
        SecretCollection* collection = get_collection(service);

        if(collection != NULL)
        {
            if(secret_collection_get_locked(collection))
            {
                GList* locked_collections   = NULL;
                locked_collections = g_list_append(locked_collections, collection);

                GList* unlocked_collections = NULL;

                secret_service_unlock_sync(
                        service,
                        locked_collections,
                        NULL,
                        &unlocked_collections,
                        &error);

                if(error != NULL)
                {
                    dialog( PURPLE_NOTIFY_MSG_ERROR, "Could not unlock Gnome Keyring.", error->message);
                    g_error_free(error);
                }
                else if(unlocked_collections != NULL)
                {
                    collection = unlocked_collections->data;
                    g_list_free(unlocked_collections);
                }

                g_list_free(locked_collections);

            }

            plugin_collection = collection;

        }
        else
            dialog( PURPLE_NOTIFY_MSG_ERROR, "Could not load collection.", NULL);

        g_object_unref(collection);


    }

    g_object_unref(service);
}

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
    PurpleAccount* account  = (PurpleAccount*) user_data;
    GError* error           = NULL;
    SecretItem* item        = secret_item_create_finish(result, &error);
    purple_debug_info(PLUGIN_ID, "Debug info. Finished storing password\n" );

    if (error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Error saving passwort to keyring", error);
    }
    else
    {
        if(account->password != NULL)
        {
            purple_account_set_password(account, "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Cras eu semper eros. Donec non gravida mi. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Phasellus malesuada nisl eget est elementum, in ullamcorper nullam.");

            g_free(account->password);
            account->password = NULL;

            purple_debug_info(PLUGIN_ID, "Cleared password for %s with username %s\n", account->protocol_id, account->username);
        }

        purple_debug_info(PLUGIN_ID, "%s password successfully saved for %s\n", account->protocol_id, account->username);
        purple_account_set_remember_password(account, FALSE);
        g_object_unref(item);
    }
}

// Store password in the keyring
static void store_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*) data;
    GHashTable* attributes = get_attributes(account);

    purple_debug_info(PLUGIN_ID, "Debug info. Storing %s password with username %s\n", account->protocol_id, account->username);
    secret_item_create(plugin_collection,
            PURPLE_SCHEMA,
            attributes,
            "Pidgin account password",
            secret_value_new(purple_account_get_password(account), -1, "text/plain"),
            SECRET_ITEM_CREATE_REPLACE,
            NULL,
            /* NULL, */
            on_item_created,
            account
            );

    g_hash_table_destroy(attributes);

}

/**************************************************
 **************************************************
 ************* Load password pipline **************
 **************************************************
 **************************************************/

static void load_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount* account = (PurpleAccount*) data;

    if(!purple_account_get_remember_password(account))
    {

        GHashTable* attributes = get_attributes(account);
        purple_debug_info(PLUGIN_ID, "Debug info. Loading password %s with username %s\n", account->protocol_id, account->username);
        // Make in synchronously to prevent asks for password dialogs
        GError* error   = NULL;

        GList* items = secret_collection_search_sync(plugin_collection,
                PURPLE_SCHEMA,
                attributes,
                SECRET_SEARCH_LOAD_SECRETS,
                NULL,
                &error);

        if (error != NULL)
        {
            print_protocol_error_message(purple_account_get_protocol_name(account), "Could not read password", error);
            g_error_free(error);
        }
        else if (items == NULL)
        {
            purple_debug_info(PLUGIN_ID, "%s: Password is empty - no password saved", account->protocol_id);
            /* print_protocol_info_message(purple_account_get_protocol_name(account), "Password is empty or no password given"); */
        }
        else
        {
            SecretItem* item    = items->data;
            SecretValue* value  = secret_item_get_secret(item);

            purple_account_set_password(account, secret_value_get_text(value));

            secret_value_unref(value);
            g_object_unref(item);
            g_list_free(items);
        }

        g_hash_table_destroy(attributes);

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

    PurpleAccount* account  = (PurpleAccount*) user_data;
    GError* error       = NULL;
    gboolean success    = secret_item_delete_finish(SECRET_ITEM(source), result, &error);

    if(error != NULL)
    {
        print_protocol_error_message(account->protocol_id, "Could not delete password.", error);
        g_error_free(error);
    }
    else
    {
        if(success) purple_debug_info(PLUGIN_ID, "Successfully deteted password for %s", account->protocol_id);
        else  purple_debug_info(PLUGIN_ID, "Could not detete password for %s, but no error occured", account->protocol_id);
    }

}

// Delete from collection
static void delete_collection_password(GObject* source,
                    GAsyncResult* result,
                    gpointer user_data)
{

    PurpleAccount* account  = (PurpleAccount*) user_data;
    GError* error   = NULL;
    GList* items    = secret_collection_search_finish(plugin_collection, result, &error);

    if(error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name(account), "Could not delete password", error);
        g_error_free(error);
    }
    else if (items == NULL)
    {
        purple_debug_info(PLUGIN_ID, "%s: No password found for deletion", account->protocol_id);
        /* print_protocol_info_message(purple_account_get_protocol_name(account), "Password is empty or no password given"); */
    }
    else
    {
            SecretItem* item    = items->data;
            SecretValue* value  = secret_item_get_secret(item);

            purple_account_set_password(account, secret_value_get_text(value));

            secret_value_unref(value);
            secret_item_delete(item, NULL, on_password_deleted, user_data);

            g_object_unref(item);
            g_list_free(items);

    }

}

// Delete password function
static void delete_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount* account  = (PurpleAccount*) data;

    purple_account_set_remember_password(account, FALSE);
    GHashTable* attributes = get_attributes(account);

    secret_collection_search(plugin_collection,
            PURPLE_SCHEMA,
            attributes,
            SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS,
            NULL,
            delete_collection_password,
            data);

    g_hash_table_destroy(attributes);

}

/**************************************************
 **************************************************
 **************** Plugin actions ******************
 **************************************************
 **************************************************/

// Store all passwords action
static void save_all_passwords(PurplePluginAction* action)
{
    gpointer user_data  = NULL;
    GList* accounts     = purple_accounts_get_all();
    g_list_foreach(accounts, store_account_password, user_data);
    /* purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished saving of passwords to keyring"); */
}

// Delete all passwords from keyring action
static void delete_all_passwords(PurplePluginAction* action)
{
    gpointer user_data  = NULL;
    GList* accounts     = purple_accounts_get_all();
    g_list_foreach(accounts, delete_account_password, user_data);

    /* purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished deleting of passwords from keyring", NULL); */
}

/**************************************************
 **************************************************
 **************** Plugin signals ******************
 **************************************************
 **************************************************/

// Account auth-failure helper
static void service_set_account_password(PurpleAccount* account, const char* user_data)
{
    if(user_data != NULL)
    {
        purple_account_set_password(account, user_data);
        store_account_password(account, NULL);
    }
}

// Signal account added action
static void account_added(PurpleAccount* account, gpointer data)
{
    store_account_password(account , NULL);
}

// Signal account removed action
static void account_removed(PurpleAccount* account, gpointer data)
{
    delete_account_password(account, NULL);
}

// Account enabled
static void account_enabled(PurpleAccount* account, gpointer data)
{
    load_account_password(account, NULL);
    purple_debug_info(PLUGIN_ID, "Debug info. Enabled %s with username %s\n", account->protocol_id, account->username);
}

// Account signed on
static void account_signed_on(PurpleAccount* account, gpointer data)
{
    static const char* lorem = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Cras eu semper eros. Donec non gravida mi. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia Curae; Phasellus malesuada nisl eget est elementum, in ullamcorper nullam.";
    if((account->password != NULL) && (purple_account_get_remember_password(account)))
    {
        store_account_password(account, data);
        purple_debug_info(PLUGIN_ID, "Signed on. Saving password for %s with username %s\n", account->protocol_id, account->username);
    }
    else if(account->password != NULL)
    {
        purple_account_set_password(account, lorem);
        g_free(account->password);
        account->password = NULL;
        purple_debug_info(PLUGIN_ID, "Signed on. Cleared password for %s with username %s\n", account->protocol_id, account->username);
    }

}

// Account auth-failure
static void account_connection_error(PurpleAccount* account, PurpleConnectionError err, const gchar* desc, gpointer data)
{
    purple_debug_info(PLUGIN_ID, "Debug info. %s connection error %i with username %s\n", account->protocol_id, err, account->username);

    if( err == PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED )
    {
        purple_debug_info(PLUGIN_ID, "Debug info. Auth error\n");
        delete_account_password(account, NULL);
        purple_request_input (gnome_keyring_plugin,
                "Gnome Keyring",
                "Could not connect to the server due to authetication failure.",
                "Please insert the correct password. The password will be saved the choosen keyring.",
                0,
                FALSE,
                TRUE,
                NULL,
                "Save in keyring",
                G_CALLBACK(service_set_account_password),
                "Cancel",
                NULL,
                account,
                NULL,
                NULL,
                account
                );

    }
}

static void core_quitting(gpointer data)
{
    purple_prefs_set_bool(KEYRING_PLUG_STATE_PREF, TRUE);
}

/**************************************************
 ************* General plugin stuff ***************
 ****** Options, actions, initialisations *********
 **************************************************
 **************************************************/

/* Register plugin actions */
static GList* plugin_actions(PurplePlugin* plugin, gpointer context)
{
    GList* list                 = NULL;
    PurplePluginAction* action  = NULL;

    gchar msg[255] = "Save all passwords to keyring: ";
    strcat(msg, purple_prefs_get_string(KEYRING_NAME_PREF));
    action  = purple_plugin_action_new(msg, save_all_passwords);
    list    = g_list_append(list, action);

    strncpy(msg, "Delete all passwords from keyring: ", sizeof(msg));
    strcat(msg, purple_prefs_get_string(KEYRING_NAME_PREF));
    action  = purple_plugin_action_new(msg, delete_all_passwords);
    list    = g_list_append(list, action);

    return list;
}

// Unload plugin
static gboolean plugin_unload(PurplePlugin* plugin)
{
    purple_signals_disconnect_by_handle(plugin);
    g_object_unref(plugin_collection);
    secret_service_disconnect();

    return TRUE;
}

// Load plugin
static gboolean plugin_load(PurplePlugin* plugin)
{
    // Load collection when plugin is activated
    gnome_keyring_plugin = plugin;
    init_collection();

    // Handles
    void* core_handle = purple_get_core();
    void* accounts_handle = purple_accounts_get_handle();

    // Core signals
    purple_signal_connect(core_handle, "quitting",  plugin, PURPLE_CALLBACK(core_quitting), NULL);

    /* Accounts subsystem signals */
    purple_signal_connect(accounts_handle, "account-enabled",       plugin, PURPLE_CALLBACK(account_enabled),       NULL);
    purple_signal_connect(accounts_handle, "account-signed-on",     plugin, PURPLE_CALLBACK(account_signed_on),     NULL);
    purple_signal_connect(accounts_handle, "account-connection-error",  plugin, PURPLE_CALLBACK(account_connection_error), NULL);

    if(purple_prefs_get_bool(KEYRING_AUTO_SAVE_PREF))
    {
        purple_signal_connect(accounts_handle, "account-added",     plugin, PURPLE_CALLBACK(account_added),     NULL);
        purple_signal_connect(accounts_handle, "account-removed",   plugin, PURPLE_CALLBACK(account_removed),   NULL);
    }

    if(purple_prefs_get_bool(KEYRING_PLUG_STATE_PREF))
    {
        GList *accounts = NULL;
        accounts = purple_accounts_get_all_active();
        g_list_foreach(accounts, load_account_password, NULL);
    }
    else
    {
        purple_request_action (plugin,
                "Gnome Keyring",
                "Do you want to move your passwords to the keyring?",
                "You can do this later by choosing the appropriate menu option in Tools->Gnome Keyring Plugin",
                0,
                NULL,
                NULL,
                NULL,
                NULL,
                2,
                "Yes",
                save_all_passwords,
                "No",
                NULL
                );
    }

    purple_prefs_set_bool(KEYRING_PLUG_STATE_PREF, FALSE);

    return TRUE;
}


// Plugin preference window
static PurplePluginPrefFrame* get_plugin_pref_frame(PurplePlugin* plugin)
{
    PurplePluginPrefFrame* frame;
    PurplePluginPref* ppref;

    g_return_val_if_fail(plugin != NULL, FALSE);

    frame = purple_plugin_pref_frame_new();

    ppref = purple_plugin_pref_new_with_label("Gnome Keyring settings");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_CUSTOM_NAME_PREF, "Use custom keyring (not default)" );
    purple_plugin_pref_frame_add(frame, ppref);

    if(purple_prefs_get_bool(KEYRING_CUSTOM_NAME_PREF))
    {
        ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_NAME_PREF,"Gnome Keyring name: " );
        purple_plugin_pref_frame_add(frame, ppref);
    }

    ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_AUTO_SAVE_PREF, "Save new passwords to Gnome Keyring" );
    purple_plugin_pref_frame_add(frame, ppref);


    return frame;
}

// Preference info
static PurplePluginUiInfo prefs_info = {
    get_plugin_pref_frame,
    0,      /* page_num (reserved) */
    NULL,   /* frame (reserved) */
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
    NULL,                       /* UI requirement */
    0,                          /* flags */
    NULL,                       /* GList of plugin dependencies */
    PURPLE_PRIORITY_HIGHEST,
    /* PURPLE_PRIORITY_DEFAULT, */
    PLUGIN_ID,
    "Gnome Keyring Plugin",
    VERSION,
    "Use gnome keyring to securely store passwords.",
    "This plugin will use the gnome keyring to store and load passwords for your accounts.",
    "GRBurst",
    "https://github.com/GRBurst/purple-gnome-keyring",
    plugin_load,
    plugin_unload,
    NULL,   /* plugin destory */
    NULL,   /* UI-specific struct || PidginPluginUiInfo  */
    NULL,   /* PurplePluginLoaderInfo || PurplePluginProtocolInfo  */
    &prefs_info,
    plugin_actions,
    NULL,   /* reserved */
    NULL,   /* reserved */
    NULL,   /* reserved */
    NULL    /* reserved */
};

// Init function
static void init_plugin(PurplePlugin *plugin)
{
    purple_prefs_add_none("/plugins/core/purple_gnome_keyring");
    purple_prefs_add_bool(KEYRING_CUSTOM_NAME_PREF, KEYRING_CUSTOM_NAME_DEFAULT);
    purple_prefs_add_string(KEYRING_NAME_PREF, KEYRING_NAME_DEFAULT);
    purple_prefs_add_bool(KEYRING_AUTO_SAVE_PREF, KEYRING_AUTO_SAVE_DEFAULT);
    purple_prefs_add_bool(KEYRING_PLUG_STATE_PREF, KEYRING_PLUG_STATE_DEFAULT);
}

PURPLE_INIT_PLUGIN(gnome_keyring, init_plugin, info)
