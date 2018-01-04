{ }:

let
  pkgs = import <nixpkgs> { };
in
  pkgs.stdenv.mkDerivation {
    name = "Pidgin-Purple-Keyring";
    buildInputs = with pkgs; [
      dbus
      dbus_glib
      glib
      gnumake
      json_glib
      libsecret
      pkgconfig
    ];
  }
