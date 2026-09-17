# Hyprland + Quickshell

For Debian 13 (trixie). Run from a terminal as your desktop user:

```sh
./install.sh
```

The script uses sudo for APT, so run it in an interactive terminal where you
can enter your password.
It works from any working directory. Configs are symlinked into `~/.config`;
keep this repository in place and customize the files here. A normal non-root
invocation also honors `XDG_CONFIG_HOME`.

The installer manages one `trixie-backports` source file and leaves it in
place on later runs. It installs Hyprland, Quickshell, their portal, and a
small set of desktop programs: Hyprland GUI utilities, Foot, Fuzzel, Nautilus,
GNOME Software with Flatpak support, Text Editor, Calculator, and Disks,
NetworkManager, PipeWire/WirePlumber, brightness and media-key utilities,
screenshot selection and clipboard tools, the Qt Wayland plugin, and a Polkit agent.
APT resolves required and recommended dependencies. Hyprland, Quickshell, and
their portal use backports; the remaining desktop packages use normal Trixie
priorities.
Adwaita's GTK and Qt support is installed, and the desktop color-scheme is set
to dark while leaving the native GTK theme and palette intact.
It also enables Debian's `i386` architecture and installs Debian's
`steam-libs:i386` runtime dependency metapackage, so Steam's 32-bit libraries
are available with versions matched to the backported graphics stack. Steam
itself is not installed by this setup.
The installer also adds the per-user Flathub remote when it is missing.

Existing files at managed symlink destinations are backed up to dated
`.backup-*` files. Repeated runs preserve the correct symlinks and edits here.
Other configuration files are left alone.

After installation, log out and select **Hyprland** in your existing login
screen, or log into a local text console and run `start-hyprland` as your user.
No display manager or automatic login is installed. **After upgrading from
the original setup, log out and back in:** a Quickshell process started using
X11 cannot switch to Wayland through a QML reload.

## Configuration

`hypr/hyprland.lua` starts from the
[upstream 0.55.2 default](https://github.com/hyprwm/Hyprland/blob/v0.55.2/example/hyprland.lua).
It keeps the default appearance and layout, uses Foot/Nautilus/Fuzzel, and starts
Quickshell explicitly on Wayland plus the Polkit agent. The keyboard layout
is `us`; change `kb_layout` if needed.

| Shortcut | Action |
| --- | --- |
| Super+Enter | Foot terminal |
| Tap Super (left or right) | Toggle the application launcher |
| Super+R | Alternative launcher shortcut |
| Super+H | Show searchable Hyprland shortcuts |
| Super+E | Nautilus file manager |
| Super+C | Close window |
| Super+M | Exit Hyprland |
| Super+1…0 | Switch workspace |
| Super+Shift+1…0 | Move window to workspace |
| Print Screen | Select an area and save a screenshot |
| Shift+Print Screen | Save a full-screen screenshot |

The monitor section matches this machine:

- `DP-1`: AOC, 1920×1080 at 300 Hz, scale 1, at `0x0`.
- `HDMI-A-1`: LG, 1920×1080 at approximately 75 Hz, scale 1, rotated
  clockwise (`transform = 1`), positioned at `-1080x0` to the left with top
  edges aligned. Its logical dimensions after rotation are 1080×1920.
- Other monitors retain the upstream preferred-mode/automatic-position fallback.

`quickshell/shell.qml` loads separate `Bar.qml`, `LauncherButton.qml`, and
`Shortcuts.qml`, `NetworkButton.qml`, and `NetworkPanel.qml` components.
`Theme.qml` contains the shared Adwaita dark
palette. The bar is a 34-pixel Hyprland layer-shell panel on every monitor. It has
Apps and Network buttons, clickable workspaces 1–10 with an active indicator,
a system tray, and a clock. Left-click tray icons to activate, right-click
for menus, middle-click for secondary actions, or scroll for app-specific
controls. The tray fills as applications register icons; it doesn't start
network/Bluetooth applets itself.

The Wayland panel reserves its height so tiled and maximized windows do not
sit underneath it. Fullscreen windows may cover the bar. Quickshell normally
reloads QML edits live; `hyprctl configerrors` reports Hyprland config errors.
Screenshots are saved as timestamped PNG files in `~/Pictures/Screenshots` and
copied to the Wayland clipboard.
Fuzzel uses a matching dark Adwaita-style configuration in
`fuzzel/fuzzel.ini`.
The Network button opens a focused-monitor panel backed by Quickshell's
NetworkManager integration. It shows connectivity, interface details, Wi-Fi
networks, signal strength, saved-network state, and controls for scanning,
connecting, disconnecting, and forgetting networks.
Review upstream changes when upgrading Hyprland because this repository
preserves your configuration rather than replacing it with package defaults.

References: [Debian Backports](https://backports.debian.org/Instructions/),
[Quickshell guide](https://quickshell.org/docs/guide/introduction/),
[Hyprland monitor configuration](https://wiki.hypr.land/Configuring/Basics/Monitors/).
