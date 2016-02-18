#ifndef PURPLE_PLUGINS
#define PURPLE_PLUGINS
#endif

#ifndef VERSION
#define VERSION "0.1.0"
#endif

#include <glib.h>
#include <dbus/dbus-glib.h>

#ifndef G_GNUC_NULL_TERMINATED
# if __GNUC__ >= 4
#  define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
# else
#  define G_GNUC_NULL_TERMINATED
# endif
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
#define PLUGIN_ID "core-grburst-gnome_keyring"
#define KEYRING_NAME_PREF "/plugins/core/GnomeKeyring/keyring_name"
#define KEYRING_NAME_DEFAULT SECRET_COLLECTION_DEFAULT
#define KEYRING_AUTO_SAVE_PREF "/plugins/core/GnomeKeyring/auto_save"
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
static void print_error_message(char* location, gchar* error)
{
    char sec[255] = "Error in ";
    char msg[255] = "Error message: ";

    purple_notify_error(gnome_keyring_plugin, "Gnome Keyring Plugin Error",
        strcat(sec, location),
        strcat(msg, error));
        /* location, */
        /* error); */
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
        print_error_message("on item created", error->message);
        g_error_free (error);
    }
    else
        g_object_unref(item);
}

// Finally store password in the keyring
static void store_password(SecretCollection* collection, PurpleAccount *account)
{
    gpointer id             = purple_account_get_protocol_id(account);
    gpointer un             = purple_account_get_username(account);

    GHashTable* attributes  = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(attributes, "protocol" , id);
    g_hash_table_insert(attributes, "username" , un);

    secret_item_create(collection,
            PURPLE_SCHEMA,
            attributes,
            "Pidgin account password",
            secret_value_new(purple_account_get_password(account), -1, "text/plain"),
            SECRET_ITEM_CREATE_REPLACE,
            NULL,
            on_item_created,
            NULL
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
        print_error_message("on load alias collections", error->message);
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
        print_error_message("on collections load", error->message);
        g_error_free (error);
    }
    else if(success)
    {
        GList* collections = secret_service_get_collections(service);

        if (!collections)
        {
            print_error_message("on collections load", "collections empty");
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
        print_error_message("on got service", error->message);
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
    {
        /* if(strcmp(purple_account_get_password(account), "")) */
        char msg[255] = "Password for ";
        strcat(msg, purple_account_get_protocol_name(account));
        strcat(msg, " is empty! No password saved.");
        purple_notify_warning(gnome_keyring_plugin, "Gnome Keyring Plugin", "Password empty", msg);
    }
}

// Store all passwords action
static void save_all_passwords(PurplePluginAction *action)
{
    gpointer user_data  = NULL;
    GList *accounts     = purple_accounts_get_all();
    g_list_foreach(accounts, service_store_account_password, user_data);
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
    GError *error = NULL;
    gchar *password = secret_password_lookup_finish (result, &error);


    if (error != NULL) {
        purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "Could not read password.", NULL, NULL, NULL);
        g_error_free(error);
    } else if (password == NULL) {
        purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "No password found.", NULL, NULL, NULL);
    } else {
        purple_account_set_password(user_data, password);
        secret_password_free (password);
    }

}

static void get_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount *account=(PurpleAccount*)data;

    if(!purple_account_get_remember_password(account))
    {
        // Make in synchronously to prevent asks for password dialogs
        GError *error = NULL;
        gchar *password = secret_password_lookup_sync (PURPLE_SCHEMA, NULL, &error,
                "protocol" , purple_account_get_protocol_id(account),
                "username" , purple_account_get_username(account),
                NULL);

        if (error != NULL) {
            /* purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "Could not read password.", NULL, NULL, NULL); */
            purple_notify_error(gnome_keyring_plugin, "Password error!", "Password error!", "Could not read password!");
            g_error_free(error);
        } else if (password == NULL) {
            /* purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "No password found.", NULL, NULL, NULL); */
        } else {
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
static void on_password_deleted (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error = NULL;

    secret_password_clear_finish (result, &error);
    if (error != NULL) {
        g_error_free (error);
    } else {

    }
}

// Delete password function
static void delete_account_password(gpointer data, gpointer user_data)
{
    PurpleAccount *account=(PurpleAccount*)data;
    GError *error = NULL;
    gchar *password = secret_password_lookup_sync (PURPLE_SCHEMA, NULL, &error,
            "protocol" , purple_account_get_protocol_id(account),
            "username" , purple_account_get_username(account),
            NULL);

    if (error != NULL) {
        /* purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "Could not read password.", NULL, NULL, NULL); */
        purple_notify_error(gnome_keyring_plugin, "Password error!", "Password error!", "Could not read password!");
        g_error_free(error);
    } else if (password == NULL) {
        purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Password error!", "No password found.", NULL, NULL, NULL);
    } else {
        purple_account_set_password(account, password);
        secret_password_free (password);
        secret_password_clear (PURPLE_SCHEMA, NULL, on_password_deleted, NULL,
                "protocol" , purple_account_get_protocol_id(account),
                "username" , purple_account_get_username(account),
                NULL);
    }

}

// Delete all passwords from keyring action
static void delete_all_passwords(PurplePluginAction *action)
{
    gpointer user_data = NULL;
    GList *accounts = purple_accounts_get_all();
    g_list_foreach(accounts, delete_account_password, user_data);
    purple_notify_message(gnome_keyring_plugin, PURPLE_NOTIFY_MSG_INFO, "Gnome Keyring", "Deleted all passwords from Keyring", NULL, NULL, NULL);
}












/*
 * General plugin handling
 */

/* Register plugin actions */
static GList *plugin_actions(PurplePlugin *plugin, gpointer context)
{
    GList *list                 = NULL;
    PurplePluginAction *action  = NULL;

    action  = purple_plugin_action_new("Save all passwords to keyring", save_all_passwords);
    list    = g_list_append(list, action);
    action  = purple_plugin_action_new("Delete all passwords from keyring", delete_all_passwords);
    list    = g_list_append(list, action);

    return list;
}


#ifdef AUTOSAVE_PASSWORDS
static void account_added(gpointer data, gpointer user_data)
{
    store_password((PurpleAccount*)data);
}

static void account_removed(gpointer data, gpointer user_data)
{
    PurpleAccount *account=(PurpleAccount*)data;
    int           wallet=open_wallet(TRUE);

    if(wallet>=0)
    {
        const char *key=key_for_account(account);
        char       *password=read_password(wallet, key);

        if(password)
        {
            remove_entry(wallet, key);
            g_free(password);
        }
    }
}

static void account_changed(gpointer data, gpointer user_data)
{
    // TODO: Is there a way to detect when an account has changed?????
    PurpleAccount *account=(PurpleAccount*)data;
    printf("Account changed: %s -> %s\n", key_for_account(account), password ? password : "<>");

    //store_password((PurpleAccount*)data);
}
#endif

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
    accounts = purple_accounts_get_all();
    g_list_foreach(accounts, get_account_password, NULL);

#ifdef AUTOSAVE_PASSWORDS
    void *accounts_handle=purple_accounts_get_handle();
    purple_signal_connect(accounts_handle, "account-added", plugin,
            PURPLE_CALLBACK(account_added), NULL);
    purple_signal_connect(accounts_handle, "account-removed", plugin,
            PURPLE_CALLBACK(account_removed), NULL);
    purple_signal_connect(accounts_handle, "account-set-info", plugin,
            PURPLE_CALLBACK(account_changed), NULL);
#endif

    gint i = 256;
    gfloat f = 512.1024;
    const gchar *s = "example string";

    /* Introductory message */
    purple_debug_info(PLUGIN_ID,
            "Called plugin_load.  Beginning debug demonstration\n");

    /* Show off the debug API a bit */
    purple_debug_misc(PLUGIN_ID,
            "MISC level debug message.  i = %d, f = %f, s = %s\n", i, f, s);

    purple_debug_info(PLUGIN_ID,
            "INFO level debug message.  i = %d, f = %f, s = %s\n", i, f, s);

    purple_debug_warning(PLUGIN_ID,
            "WARNING level debug message.  i = %d, f = %f, s = %s\n", i, f, s);

    purple_debug_error(PLUGIN_ID,
            "ERROR level debug message.  i = %d, f = %f, s = %s\n", i, f, s);

    purple_debug_fatal(PLUGIN_ID,
            "FATAL level debug message. i = %d, f = %f, s = %s\n", i, f, s);

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


    ppref = purple_plugin_pref_new_with_name_and_label( KEYRING_NAME_PREF,"Gnome Keyring name:" );
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
    "<github link>",
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
    purple_prefs_add_none("/plugins/core/GnomeKeyring");
    purple_prefs_add_string(KEYRING_NAME_PREF, KEYRING_NAME_DEFAULT);
    purple_prefs_add_bool(KEYRING_AUTO_SAVE_PREF, KEYRING_AUTO_SAVE_DEFAULT);
}

PURPLE_INIT_PLUGIN(gnome_keyring, init_plugin, info)
