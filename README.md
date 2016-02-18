# purple-gnome-keyring

Plugin for your purple based instand messenger like Pidgin or Finch.
This plugin seemlessly integrates the Gnome Keyring with your instant messanger. Therefore your password are stored securely - since Pidgin itself stores passwords in plaintext.

## Usage
- Clone repo
- Call `make purple-gnome-keyring.so`
- Call `make install` (Asuming you have installed pidgin locally)

## Progress
### Features
- Store passwords in an abitrary keyring
- Load passwords when plugin loaded
- Move or delete all passwords to / from Gnome Keyring at once
    - Actions are available in menu: `Tools->Gnome Keyring Plugin`
- Signal handling when account is created / deleted
    - If enabled in preferences, passwords of new accounts are automatically stored in the Gnome Keyring

### TODO
- Create Keyring if given keyringname does not exists
- Signal handling when account password was changed

### Testing
- Tested with Pidgin without any issues
- Tested with Finch without any issues

## Known issues
### Prevent issues
- Make sure that Gnome Keyring is running
- Make sure thad DBUS in running
