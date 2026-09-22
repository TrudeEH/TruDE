# TruDE

**Trude’s Desktop Environment** is a Debian-first desktop built around Hyprland. It brings together a Wayland session, a matching dark theme, keyboard-driven controls, and terminal user interfaces for common desktop tasks.

## Screenshots

<table>
  <tr>
    <td align="center"><strong>Empty desktop</strong><br><img src="assets/screenshots/empty-desktop.webp" alt="TruDE desktop with no open windows" width="100%"></td>
    <td align="center"><strong>Application launcher</strong><br><img src="assets/screenshots/launcher.webp" alt="TruDE application launcher open over the desktop" width="100%"></td>
  </tr>
  <tr>
    <td align="center"><strong>Terminal tools</strong><br><img src="assets/screenshots/tui-scripts.webp" alt="TruDE terminal interfaces for maintenance, power profiles, power actions, and sensors" width="100%"></td>
    <td align="center"><strong>Settings</strong><br><img src="assets/screenshots/settings.webp" alt="TruDE settings menu" width="100%"></td>
  </tr>
  <tr>
    <td align="center"><strong>Bar when idle</strong><br><img src="assets/screenshots/bar-idle.webp" alt="TruDE Waybar in its idle state" width="100%"></td>
    <td align="center"><strong>Bar while streaming</strong><br><img src="assets/screenshots/bar-streaming.webp" alt="TruDE Waybar while streaming" width="100%"></td>
  </tr>
</table>

<p align="center"><strong>Playing a game</strong><br><img src="assets/screenshots/gameplay.webp" alt="A game running alongside TruDE desktop tools" width="100%"></p>

## What’s included

- Hyprland desktop session with Waybar, lock screen, idle handling, notifications, and wallpaper.
- Application launcher and keyboard shortcuts for windows, workspaces, screenshots, audio, and brightness.
- Terminal interfaces for settings, package management, system maintenance, networking, Bluetooth, power, notifications, file browsing, sensors, and system monitoring.
- Matching GTK, Qt, terminal, editor, and system-monitor colors using a dark palette with warm orange accents.
- LightDM with Slick Greeter, PipeWire audio, Flatpak with Flathub, and supporting desktop services.

## Installation

The installer supports **Debian 13 (Trixie) and newer Debian releases**. Clone this repository, enter its directory, then run:

```sh
./install.sh
```

Run the installer as your regular desktop user; it uses `sudo` for system-level setup. It configures Debian package sources, installs the desktop packages, enables selected services, and links the dotfiles into your home directory. Review `install.sh` before running if you want to see all system changes. When it finishes, log out and back in to start the TruDE session.

## Configuration

- `configs/hypr/hyprland.lua` contains the Hyprland desktop settings and keybindings.
- `configs/waybar/` contains the bar layout and styling.
- `scripts/tui/` contains the terminal interfaces.
- `assets/wallpapers/default.jpg` is the default desktop and lock-screen wallpaper.

The installer links these files into their expected locations. Edit the files in this repository to keep changes versioned with the rest of TruDE.

## Repository layout

| Path | Contents |
| --- | --- |
| `assets/` | Default wallpaper and README screenshots |
| `configs/` | Hyprland, Waybar, GTK, Qt, terminal, and application settings |
| `scripts/hypr/` | Hyprland helpers, including the launcher and screenshot command |
| `scripts/tui/` | Terminal user interfaces for desktop tasks |
| `scripts/waybar/` | Waybar status and menu helpers |
| `install.sh` | Debian setup and dotfile installer |
