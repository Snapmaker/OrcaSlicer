#! /bin/bash

sudo apt update
sudo apt install build-essential flatpak flatpak-builder gnome-software-plugin-flatpak -y
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
# Must match `runtime-version` in scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml.
flatpak install flathub org.gnome.Platform//49 org.gnome.Sdk//49


##
# in Snapmaker Orca folder, run following command to build
# # First time build
# flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user --force-clean build-dir scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml

# # Subsequent builds (only rebuilding Snapmaker Orca)
# flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user build-dir scripts/flatpak/io.github.Snapmaker.Snapmaker_Orca.yml --build-only=Snapmaker_Orca
