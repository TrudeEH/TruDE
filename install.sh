#! /bin/bash

wallpaper="dracula_arch.png"

# Update
sudo pacman -Sy

# Xorg
sudo pacman -S xorg-server

# DWM dependencies
sudo pacman -S base-devel libx11 libxft libxinerama freetype2 fontconfig ttf-ubuntu-font-family ttf-font-awesome

# Slock dependencies
sudo pacman -S xorg-xrandr

# Wallpaper dependencies
sudo pacman -S feh

# -------------------------------------------
# Compile

compile () {
  cd $1
  make
  sudo make clean install
  rm -f $1 *.o */*.o config.h stest
  cd ..
}

compile "dwm"
compile "slstatus"
compile "st"
compile "dmenu"
compile "slock"

# Autostart + Wallpaper
mkdir -p ~/.local/share/dwm/
cp -f dwm/autostart.sh ~/.local/share/dwm/
cp -f wallpapers/$wallpaper ~/.local/share/dwm/
echo "feh --bg-fill ~/.local/share/dwm/$wallpaper" >> ~/.local/share/dwm/autostart.sh
feh --bg-fill ~/.local/share/dwm/$wallpaper

# GTK theme

sudo pacman -S lxappearance

sudo mkdir -p /usr/share/icons
sudo cp -rf themes/Dracula /usr/share/icons

sudo mkdir -p /usr/share/themes
sudo cp -rf themes/dracula-gtk /usr/share/themes

echo ---------------------------------------
echo - Lauch LXappearance to change themes -
echo ---------------------------------------

# LXsession
sudo pacman -S lxsession

# Network Manager
sudo pacman -S wpa_supplicant wireless_tools networkmanager network-manager-applet
sudo systemctl enable NetworkManager.service
sudo systemctl disable dhcpcd.service
sudo systemctl enable wpa_supplicant.service
sudo systemctl start NetworkManager.service

# Volume Icon
sudo pacman -S volumeicon

# Picom
sudo pacman -S picom
mkdir -p ~/.config/picom
cp -f picom.conf ~/.config/picom

# Dunst
sudo pacman -S dunst libnotify
mkdir -p ~/.config/dunst
cp -f dunstrc ~/.config/dunst

