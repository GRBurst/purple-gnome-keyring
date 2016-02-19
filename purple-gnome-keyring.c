#ifndef VERSION
#define VERSION "0.8.2"
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

#include <notify.h>
#include <plugin.h>
#include <version.h>

#include <account.h>
#include <signal.h>
#include <core.h>
#include <debug.h>

#include <libsecret/secret.h>
#include <string.h>


/* Preferences */
#define PLUGIN_ID "core-grburst-purple_gnome_keyring"
#define KEYRING_NAME_PREF "/plugins/core/purple_gnome_keyring/keyring_name"
#define KEYRING_NAME_DEFAULT SECRET_COLLECTION_DEFAULT
#define KEYRING_AUTO_SAVE_PREF "/plugins/core/purple_gnome_keyring/auto_save"
#define KEYRING_AUTO_SAVE_DEFAULT TRUE

// Plugin handles
const SecretSchema* get_purple_schema (void) G_GNUC_CONST;
#define PURPLE_SCHEMA  get_purple_schema()
#define SECRET_SERVICE(inst) (G_TYPE_CHECK_INSTANCE_CAST ((inst), SECRET_TYPE_SERVICE, SecretService))


PurplePlugin* gnome_keyring_plugin = NULL;

// Struct for schema
const SecretSchema* get_purple_schema (void)
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

// Unified Error messages
static void print_error_message(char* prim_msg, char* sec_msg)
{
    purple_notify_error(gnome_keyring_plugin,
            "Gnome Keyring Plugin Error",
            prim_msg,
            sec_msg);
}

static void print_protocol_error_message(const char* protocol_name, char* prim_msg, char* sec_msg)
{

    char msg[255] = "Error in ";
    strcat(msg, protocol_name);
    strcat(msg, " account: ");
    strcat(msg, prim_msg);

    purple_notify_error(gnome_keyring_plugin,
            "Gnome Keyring Plugin Error",
            msg,
            sec_msg);
}


/*
 * Save password(s) section.
 * Including callbacks and actions
 */

// Error check
static void on_item_created(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError* error       = NULL;
    SecretItem* item    = secret_item_create_finish(result, &error);

    if (error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name((PurpleAccount*) user_data), "Error saving passwort to keyring", error->message);
        g_error_free (error);
    }
    else
    {
        purple_account_set_remember_password((PurpleAccount*) user_data, FALSE);
        g_object_unref(item);
    }
}

// Finally store password in the keyring
static void store_password(SecretCollection* collection, PurpleAccount *account)
{
    const char* id  = purple_account_get_protocol_id(account);
    const char* un  = purple_account_get_username(account);

    GHashTable* attributes  = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attributes, "protocol" , (gpointer*) id);
    g_hash_table_insert(attributes, "username" , (gpointer*) un);

    secret_item_create(collection,
            PURPLE_SCHEMA,
            attributes,
            "Pidgin account password",
            secret_value_new(purple_account_get_password(account), -1, "text/plain"),
            SECRET_ITEM_CREATE_REPLACE,
            NULL,
            on_item_created,
            account
            );

    g_hash_table_destroy(attributes);
    g_object_unref(collection);

}

static void on_alias_collection_load(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error                   = NULL;
    SecretCollection* collection    = secret_collection_for_alias_finish(result, &error);

    if(error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name((PurpleAccount*) user_data), "Could not load default keyring", error->message);
        g_error_free (error);
    }
    else
    {
        PurpleAccount *account = (PurpleAccount*) user_data;
        store_password(collection, account);
    }

}


// Collection callback
static void on_service_collections_load(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error           = NULL;
    SecretService* service  = SECRET_SERVICE(source);
    gboolean success        = secret_service_load_collections_finish(service, result, &error);

    if (error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name((PurpleAccount*) user_data), "Could not load given keyring", error->message);
        g_error_free (error);
    }
    else if(success)
    {
        GList* collections = secret_service_get_collections(service);

        if (!collections)
        {
            print_protocol_error_message(purple_account_get_protocol_name((PurpleAccount*) user_data), "Could not load keyrings", "Check if DBus in running");
            return;
        }

        const gchar* kname              = purple_prefs_get_string(KEYRING_NAME_PREF);
        SecretCollection* collection    = NULL;

        for(GList* li = collections; li != NULL; li = li->next)
        {
            gchar* label = secret_collection_get_label(li->data);
            if(strcmp(label, kname) == 0)
            {
                collection = li->data;
                break;
            }
        }

        PurpleAccount *account = (PurpleAccount*) user_data;
        store_password(collection, account);

    }

    g_object_unref(service);

}

// Service callback
static void on_got_service(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error = NULL;
    SecretService* service = secret_service_get_finish(result, &error);

    if (error != NULL)
    {
        print_protocol_error_message(purple_account_get_protocol_name((PurpleAccount*) user_data), "Could not connect any service", error->message);
        g_error_free (error);
    }
    else
    {
        if(strcmp(purple_prefs_get_string(KEYRING_NAME_PREF), KEYRING_NAME_DEFAULT))
            secret_service_load_collections(service, NULL, on_service_collections_load, user_data);
        else
            secret_collection_for_alias(service, KEYRING_NAME_DEFAULT, SECRET_COLLECTION_NONE, NULL, on_alias_collection_load, user_data);
    }
}

// Begin storing pipeline
static void service_store_account_password(gpointer data, gpointer user_data)
{

    PurpleAccount *account = (PurpleAccount*) data;

    if(purple_account_get_password(account))
        secret_service_get(SECRET_SERVICE_NONE, NULL, on_got_service, data);
    else
        print_protocol_error_message(purple_account_get_protocol_name(account), "Password is empty - no password saved", NULL);

}

// Store all passwords action
static void save_all_passwords(PurplePluginAction *action)
{
    gpointer user_data  = NULL;
    GList *accounts     = purple_accounts_get_all();
    g_list_foreach(accounts, service_store_account_password, user_data);
    purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished saving of passwords to keyring", NULL);
}





/*
 * Load password(s) section.
 * Including callbacks
 *
 */

// Finish callback
static void on_password_lookup (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error   = NULL;
    gchar *password = secret_password_lookup_finish (result, &error);


    if (error != NULL)
    {
        print_protocol_error_message((char*)(user_data), "Could not read password", error->message);
        g_error_free(error);
    }
    else if (password == NULL)
    {
        print_protocol_error_message((char*)(user_data), "Password is empty or no password given", NULL);
    }
    else
    {
        purple_account_set_password(user_data, password);
        secret_password_free (password);
    }

}

static void get_account_password_sync(gpointer data, gpointer user_data)
{
    PurpleAccount *account = (PurpleAccount*)data;

    if(!purple_account_get_remember_password(account))
    {
        // Make in synchronously to prevent asks for password dialogs
        GError *error   = NULL;
        gchar *password = secret_password_lookup_sync (PURPLE_SCHEMA, NULL, &error,
                "protocol" , purple_account_get_protocol_id(account),
                "username" , purple_account_get_username(account),
                NULL);

        if (error != NULL)
        {
            print_protocol_error_message(purple_account_get_protocol_name(account), "Could not read password", error->message);
            g_error_free(error);
        }
        else if (password == NULL)
        {
            print_protocol_error_message(purple_account_get_protocol_name(account), "Password is empty or no password given", NULL);
        }
        else
        {
            purple_account_set_password(account, password);
            secret_password_free (password);
        }

    }
}





/*
 * Delete password(s) section.
 * Including callbacks
 *
 */

// Deleted callback
static void on_password_deleted(GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error = NULL;

    secret_password_clear_finish(result, &error);
    if(error != NULL)
    {
        print_error_message("", error->message);
        print_protocol_error_message((char*)(user_data), "Could not delete password to store it in messanger", error->message);
        g_error_free (error);
    }
}

// Delete password function
static void delete_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount *account  = (PurpleAccount*) data;
    GError *error           = NULL;
    gchar *password         = secret_password_lookup_sync (PURPLE_SCHEMA, NULL, &error,
            "protocol" , purple_account_get_protocol_id(account),
            "username" , purple_account_get_username(account),
            NULL);

    const char* protocol_name = purple_account_get_protocol_name(account);

    if(error != NULL)
    {
        print_protocol_error_message(protocol_name, "Could not read password to store it in messanger", error->message);
        g_error_free(error);
    }
    else if(password == NULL)
    {
        print_protocol_error_message(protocol_name, "Password is empty and cannot be stored in messanger", NULL);
    }
    else
    {
        purple_account_set_password(account, password);
        secret_password_free (password);
        secret_password_clear (PURPLE_SCHEMA, NULL, on_password_deleted, (gpointer*) protocol_name,
                "protocol" , purple_account_get_protocol_id(account),
                "username" , purple_account_get_username(account),
                NULL);
    }

}

// Delete all passwords from keyring action
static void delete_all_passwords(PurplePluginAction *action)
{
    gpointer user_data  = NULL;
    GList *accounts     = purple_accounts_get_all();
    g_list_foreach(accounts, delete_account_password, user_data);

    purple_notify_info(gnome_keyring_plugin, "Gnome Keyring Info", "Finished deleting of passwords from keyring", NULL);
}




// Signal account added action
static void account_added(gpointer data, gpointer user_data)
{
    service_store_account_password((PurpleAccount*) data, NULL);
}

// Signal account removed action
static void account_removed(gpointer data, gpointer user_data)
{
    delete_account_password((PurpleAccount*) data, NULL);
}

// Signal account password changed action
static void account_changed(gpointer data, gpointer user_data)
{
}



/*
 * General plugin handling
 */

/* Register plugin actions */
static GList *plugin_actions(PurplePlugin *plugin, gpointer context)
{
    GList *list                 = NULL;
    PurplePluginAction *action  = NULL;

    char msg[255] = "Save all passwords to keyring: ";
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
static gboolean plugin_unload(PurplePlugin *plugin)
{
    /* secret_service_disconnect(); */
    return TRUE;
}

// Load plugin
static gboolean plugin_load(PurplePlugin *plugin)
{
    gnome_keyring_plugin = plugin;

    GList *accounts = NULL;
    accounts = purple_accounts_get_all_active();
    g_list_foreach(accounts, get_account_password_sync, NULL);

    void *accounts_handle = purple_accounts_get_handle();

    /* Accounts subsystem signals */
    if(purple_prefs_get_bool(KEYRING_AUTO_SAVE_PREF))
    {
        purple_signal_connect(accounts_handle, "account-added",     plugin, PURPLE_CALLBACK(account_added),     NULL);
        purple_signal_connect(accounts_handle, "account-removed",   plugin, PURPLE_CALLBACK(account_removed),   NULL);
        purple_signal_connect(accounts_handle, "account-set-info",  plugin, PURPLE_CALLBACK(account_changed),   NULL);
    }

    return TRUE;
}

// Plugin preference window
static PurplePluginPrefFrame* get_plugin_pref_frame(PurplePlugin *plugin)
{
    PurplePluginPrefFrame *frame;
    PurplePluginPref *ppref;

    g_return_val_if_fail(plugin != NULL, FALSE);

    frame = purple_plugin_pref_frame_new();

    ppref = purple_plugin_pref_new_with_label("Gnome Keyring settings");
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_NAME_PREF,"Gnome Keyring name: " );
    purple_plugin_pref_frame_add(frame, ppref);

    ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_AUTO_SAVE_PREF, "Save new passwords to Gnome Keyring" );
    purple_plugin_pref_frame_add(frame, ppref);


    return frame;
}

// Preference info
static PurplePluginUiInfo prefs_info = {
    get_plugin_pref_frame,
    0,      /* page_num (reserved) */
    NULL,   /* frame (reserved) */

    /* padding */
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
    NULL,   /* UI requirement */
    0,
    NULL,   /* GList of plugin dependencies */
    PURPLE_PRIORITY_HIGHEST,
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
    purple_prefs_add_string(KEYRING_NAME_PREF, KEYRING_NAME_DEFAULT);
    purple_prefs_add_bool(KEYRING_AUTO_SAVE_PREF, KEYRING_AUTO_SAVE_DEFAULT);
}

PURPLE_INIT_PLUGIN(gnome_keyring, init_plugin, info)
