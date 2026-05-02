# hyprwinwrap-interactive

A fork of [hyprwm/hyprland-plugins](https://github.com/hyprwm/hyprland-plugins) that provides only the hyprwinwrap plugin with added interactivity toggle functionality.

## Features

- Render any window as a desktop background (like xwinwrap)
- Toggle interactivity with background windows via dispatchers
- Optional focus guarding when background windows are hidden
- Optional fullscreen, full-monitor, and above-layer rendering for interactive mode
- Runtime adoption, release, refresh, and status dispatchers
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

Toggle, show, hide, adopt, release, refresh, or inspect background windows:

```bash
# Toggle interactivity
hyprctl dispatch hyprwinwrap:toggle

# Explicit show/hide
hyprctl dispatch hyprwinwrap:show
hyprctl dispatch hyprwinwrap:hide

# Scan already-open windows and wrap matches
hyprctl dispatch hyprwinwrap:adopt

# Release wrapped windows back to normal
hyprctl dispatch hyprwinwrap:release

# Reapply current plugin settings
hyprctl dispatch hyprwinwrap:refresh

# Show wrapped/interactable counts
hyprctl dispatch hyprwinwrap:status
```

### Complete Config Example

```ini
plugin {
    hyprwinwrap {
        class = steam_app_3213850
        title =
        match_current_class = false
        match_initial_title = false

        pos_x = 0
        pos_y = 0
        size_x = 100
        size_y = 100

        adopt_existing = false
        start_interactive = false
        force_float = true
        pin = true
        auto_windowrules = false

        render_hidden = true
        render_interactive = true
        force_full_monitor = false
        force_fullscreen = false
        client_fullscreen = false
        interactive_above_layers = false

        focus_guard = false
        refocus_on_show = false
        refocus_on_hide = false
        notifications = false
    }
}
```

### Tips

- Set `new_optimizations = false` in hyprland.conf blur settings if you want tiled windows to render the app instead of your wallpaper when opaque.
- To intentionally cover Waybar/top layers in interactive mode, enable `force_fullscreen`, `client_fullscreen`, and `interactive_above_layers`.

See [hyprwinwrap/README.md](./hyprwinwrap/README.md) for every option, defaults, and profile examples.

## Contributing

Feel free to open issues and PRs with fixes or improvements.
