# hyprwinwrap

Clone of xwinwrap for Hyprland with optional interactivity, fullscreen, focus, and layer-order controls.

## Configuration

Minimal configuration:

```ini
plugin {
    hyprwinwrap {
        # class is an exact match against initialClass by default
        class = kitty-bg
    }
}

bind = SUPER, B, hyprwinwrap:toggle
```

Full configuration example with every option:

```ini
plugin {
    hyprwinwrap {
        # Matching
        class = steam_app_3213850
        title =
        match_current_class = false
        match_initial_title = false

        # Geometry, as percentages of the monitor
        pos_x = 0
        pos_y = 0
        size_x = 100
        size_y = 100

        # Window ownership and adoption
        adopt_existing = false
        start_interactive = false
        force_float = true
        pin = true
        auto_windowrules = false

        # Rendering
        render_hidden = true
        render_interactive = true
        force_full_monitor = false
        force_fullscreen = false
        client_fullscreen = false
        interactive_above_layers = false

        # Focus and notifications
        focus_guard = false
        refocus_on_show = false
        refocus_on_hide = false
        notifications = false
    }
}
```

## Options

| Option | Default | Description |
|--------|---------|-------------|
| `class` | `kitty-bg` | Exact class match. Uses `initialClass` unless `match_current_class` is enabled. |
| `title` | empty | Exact current title match. |
| `match_current_class` | `false` | Also match the current `class`, useful if an app changes class after launch. |
| `match_initial_title` | `false` | Also match `initialTitle`, useful if the title changes after launch. |
| `pos_x` / `pos_y` | `0` / `0` | Top-left position as a monitor percentage. |
| `size_x` / `size_y` | `100` / `100` | Window size as a monitor percentage. |
| `adopt_existing` | `false` | Automatically wrap already-open matching windows on plugin init or config reload. |
| `start_interactive` | `false` | New wrapped windows start interactable instead of hidden. |
| `force_float` | `true` | Float matching windows before positioning them. |
| `pin` | `true` | Pin wrapped windows across workspaces. |
| `auto_windowrules` | `false` | Inject float and full-size window rules on config reload for matching class/title. |
| `render_hidden` | `true` | Render hidden wrapped windows as the background. |
| `render_interactive` | `true` | Render interactable wrapped windows through the plugin render path. |
| `force_full_monitor` | `false` | Force wrapped windows to the full monitor rectangle, ignoring reserved areas. |
| `force_fullscreen` | `false` | Set compositor fullscreen while a wrapped window is interactable. |
| `client_fullscreen` | `false` | Tell the client it is fullscreen. Useful for games that size incorrectly. |
| `interactive_above_layers` | `false` | Render interactable wrapped windows after layer surfaces, covering Waybar/top layers. |
| `focus_guard` | `false` | Prevent hidden/non-interactable wrapped windows from retaining keyboard focus. |
| `refocus_on_show` | `false` | Focus the first wrapped window when showing/toggling to interactive. |
| `refocus_on_hide` | `false` | Ask Hyprland to refocus when hiding/toggling away from interactive. |
| `notifications` | `false` | Show notifications for non-status dispatchers. |

## Profiles

Wallpaper-style defaults:

```ini
plugin {
    hyprwinwrap {
        class = kitty-bg
        render_hidden = true
        force_float = true
        pin = true
    }
}
```

Fullscreen game background that can become fully interactive above Waybar:

```ini
plugin {
    hyprwinwrap {
        class = steam_app_3213850
        adopt_existing = true
        force_full_monitor = true
        force_fullscreen = true
        client_fullscreen = true
        interactive_above_layers = true
        focus_guard = true
        refocus_on_show = true
        refocus_on_hide = true
    }
}
```

Partial coverage:

```ini
plugin {
    hyprwinwrap {
        class = kitty-bg
        pos_x = 25
        pos_y = 30
        size_x = 40
        size_y = 70
    }
}
```

## Dispatchers

| Dispatcher | Description |
|------------|-------------|
| `hyprwinwrap:toggle` | Toggle interactivity for all wrapped windows. |
| `hyprwinwrap:show` | Make all wrapped windows interactable. |
| `hyprwinwrap:hide` | Make all wrapped windows non-interactable and render them as background. |
| `hyprwinwrap:adopt` | Scan current windows and wrap matches immediately. |
| `hyprwinwrap:release` | Release all wrapped windows back to normal windows. |
| `hyprwinwrap:refresh` | Reapply current geometry/fullscreen/focus settings to wrapped windows. |
| `hyprwinwrap:status` | Show a status notification with wrapped/interactable counts. |
| `hyprwinwrap_toggle` | Legacy alias for `hyprwinwrap:toggle`. |
