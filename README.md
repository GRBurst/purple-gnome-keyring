# purple-gnome-keyring

Plugin for your purple based instant messenger like Pidgin or Finch.
This plugin seamlessly integrates the Gnome Keyring with your instant messenger. Therefore your passwords are stored securely - since Pidgin itself stores passwords in plaintext.

## Usage
### Installation
- Clone repo
- Call `make`
- Call `make install` (Currently installs plugin locally)

On Arch Linux you can install the package `purple-gnome-keyring` from the AUR.

### Integration
- Go to the plugin page in the menu: `Tools->Plugins` or with `Ctrl+U`
- Enable plugin `Gnome Keyring Plugin`
- Optional: Configure plugin, e.g. to use with a separate keyring
- To move all currently active passwords to the keyring, hit
    - `Save all passwords to keyring` in menu: `Tools->Gnome Keyring Plugin`

## Progress
### Features
- Store passwords in an arbitrary keyring
- Load passwords from the same keyring
- Automatically unlock keyring
    - Prompt for a password if necessary
- Move or delete all passwords to / from Gnome Keyring at once
    - Actions are available in menu: `Tools->Gnome Keyring Plugin`
- Automatically save passwords to keyring if an account is created / deleted
    - If enabled in preferences, passwords of new accounts are automatically stored in the Gnome Keyring
- Workaround to update password if password was changed
- Automatically lock keyring if messenger gets closed (must be enabled in settings)

### TODO
- Create keyring if given keyringname does not exist

## Supported Software
This plugin has been tested with Pidgin and Finch.

## Troubleshooting
- If you added a new keyring (e.g. with Seahorse), this keyring is not recognized. You must restart the keyring.

If you encounter any problems, please create an issue on GitHub.


### Preventing issues
- Make sure that Gnome Keyring is running
- Make sure that DBUS in running
