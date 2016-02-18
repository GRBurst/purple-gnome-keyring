# purple-gnome-keyring

Purple plugin for seemless integration of the Gnome Keyring as password storage.

## Usage
- Clone repo
- Call `make purple-gnome-keyring.so`
- Call `make install`\*
\* Asuming you have installed pidgin locally

## Progress
### Features
- Store passwords in an abitrary keyring
- Load passwords when plugin loaded
- Move or delete all passwords to / from Gnome Keyring at once

### TODO
- Signal handling when account is created / deleted or password has changed
- Create Keyring if given keyringname does not exists


