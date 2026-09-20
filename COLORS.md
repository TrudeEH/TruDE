# Color Palette

These dotfiles use a libadwaita-inspired dark palette with a light-orange
accent. The canonical colors are shared by Waybar, Foot, Hyprland, Hyprlock,
Mako, LightDM, Qt, btop, and the terminal UI scripts.

## Core Palette

| Role | Color | Usage |
| --- | --- | --- |
| Window background | `#222226` | Main windows, terminal background, bar background |
| Raised surface | `#38383c` | Inputs, cards, active surface blocks |
| Primary text | `#ffffff` | Normal foreground and labels |
| Muted text | `#aaaaaa` | Disabled, secondary, and inactive content |
| Accent | `#ffbe6f` | Selection, focus, active borders, highlights |
| Strong accent | `#ffa348` | Hovered or emphasized accent states |
| Text on accent | `#222226` | Foreground placed on the orange accent |

## Surfaces and Borders

Waybar uses a stepped set of dark surfaces to separate adjacent modules.

| Role | Color | Waybar name |
| --- | --- | --- |
| Outer shadow | `#1d1d20` | `shadow` |
| Base background | `#222226` | `bg-0` |
| Raised background 1 | `#28282c` | `bg-1` |
| Raised background 2 | `#2e2e32` | `bg-2` |
| Raised background 3 | `#38383c` | `bg-3` |
| Hover background | `#46464b` | `hover-bg` |
| Subtle border | `#55555a` | Terminal UI borders |
| Visible border | `#77767b` | Waybar tooltips |
| Translucent border | `#ffffff1a` | Hyprland inactive borders, Mako borders |
| Muted hover text | `rgba(255, 255, 255, 0.75)` | Waybar hover foreground |

## Status Colors

| Role | Color | Usage |
| --- | --- | --- |
| Warning | `#f8e45c` | Updates and elevated resource usage |
| Critical | `#f66151` | Critical resource usage and screen sharing |
| Success | `#57e389` | Power-saving and positive states |

## Terminal ANSI Colors

Foot uses the core palette plus softened libadwaita-style ANSI colors.

| ANSI color | Regular | Bright |
| --- | --- | --- |
| Black | `#38383c` | `#aaaaaa` |
| Red | `#ff7b72` | `#ff8f87` |
| Green | `#8fd694` | `#a5e3a8` |
| Yellow | `#ffbe6f` | `#ffcf8a` |
| Blue | `#8cb4ff` | `#a8c7ff` |
| Magenta | `#d8a8ff` | `#e5c6ff` |
| Cyan | `#8bd5ca` | `#a9e6dc` |
| White | `#ffffff` | `#ffffff` |

The Foot selection and cursor use `#ffbe6f` with `#222226` foreground text.

## Component Mapping

- **Waybar:** Defines the complete surface, text, accent, and status palette in
  `configs/waybar/style.css`.
- **Foot:** Uses `#222226` and `#ffffff` as its base, with orange selections and
  the ANSI palette above in `configs/foot/foot.ini`.
- **Hyprland:** Uses an active border gradient from `#ffbe6f` to `#ffa348` and a
  `#ffffff1a` inactive border in `configs/hypr/hyprland.lua`.
- **Hyprlock:** Uses the base background, raised surface, primary and muted text,
  and accent outline in `configs/hypr/hyprlock.conf`.

## Format Conventions

Different applications express the same colors differently:

- CSS and GTK: `#ffbe6f`
- Foot: `ffbe6f`
- Hyprland: `rgb(ffbe6f)`
- Hyprland with alpha: `rgba(ffffff1a)`
- Mako with alpha: `#ffbe6fff`
- Qt ARGB: `#ffffbe6f`
