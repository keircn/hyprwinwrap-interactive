# hyprwinwrap-interactive

A fork of [hyprwm/hyprland-plugins](https://github.com/hyprwm/hyprland-plugins) that provides only the hyprwinwrap plugin with added interactivity toggle functionality.

## Features

- Render any window as a desktop background (like xwinwrap)
- Toggle interactivity with background windows via dispatchers
- Automatic focus handling when toggling
- Configurable size and position (percentage-based)

## Install

### Install with `hyprpm`

```bash
hyprpm update
hyprpm add https://github.com/keircn/hyprwinwrap-interactive
hyprpm enable hyprwinwrap
```

## Usage

### Dispatchers

Toggle, show, or hide interactivity with background windows:

```bash
# Toggle interactivity (recommended - bind to a key)
hyprctl dispatch hyprwinwrap:toggle

# Explicit show/hide
hyprctl dispatch hyprwinwrap:show
hyprctl dispatch hyprwinwrap:hide
```

### Tips

- Set `new_optimizations = false` in hyprland.conf blur settings if you want tiled windows to render the app instead of your wallpaper when opaque.
- If using waybar and it gets covered by the app, set `layer = top` in waybar config.

See [hyprwinwrap/README.md](./hyprwinwrap/README.md) for configuration examples.

## Contributing

Feel free to open issues and PRs with fixes or improvements.
