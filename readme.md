## helm

A lightweight Windows desktop control daemon. Keyboard-driven virtual desktop switching, app focus/launch, and window resize, all wired through a named pipe so anything can use it.

Built to be the backend for [kanata](https://github.com/jtroo/kanata) keyboard remapping, but the pipe interface works from any process.

## Commands

Commands are sent to `\\.\pipe\helm`. The client mode of `helm.exe` then performs the corresponding action.

### App focus / launch

```
helm app:firefox
helm app:wt
helm app:msedge --global
helm app:wt --admin
```

Focuses the window if it exists on the current virtual desktop, launches it if not. `--global` searches across all  virtual desktops. Supports launcher chains (Windows Terminal, Electron apps, Steam) via a background poll thread. `--admin` launches the app as admin. `--all` is an alias/alternative to `--global`.

Special aliases: `wt` and `windowsterminal` both resolve to `WindowsTerminal.exe`.

### Virtual desktop switching

```
helm vd:1
helm vd:2
helm vd:send:3
```

`vd:<n>` switches to virtual desktop n (starting at 1). `vd:send:<n>` moves the focused window to desktop n and follows it.

Uses `IVirtualDesktopManagerInternal` - proper OS-level switching, so alt-tab, the taskbar, and snap groups all behave correctly.

### Window resize

```
helm sz:left:+5
helm sz:right:-5
helm sz:down:+10
helm sz:up:-10
```

Resizes the focused snapped window by a percentage of monitor width, moving the adjacent window simultaneously. Fills the gap Windows snap leaves - snapping is Win+Left/Right, but there's no native keyboard resize.

### Instant maximize

```
helm max
```

Instantly maximizes the window, regardless if the window is snapped or not. On Windows, the default behavior when maximizing a snapped window using Windows + Up results in the window being snapped upwards, instead of maximized.

This means it is not possible to send a window to the top left/right quarter of the screen using `helm max`.

### Instant minimize

```
helm min
```

Instantly minimize the window.


### Swap

```
helm swap              # swap focused window with adjacent horizontal neighbour
helm swap:left         # swap with left neighbour, or snap focused window to left half
helm swap:right        # swap with right neighbour, or snap focused window to right half
helm swap:up           # swap with top neighbour, or snap focused window to top half
helm swap:down         # swap with bottom neighbour, or snap focused window to bottom half
```

Swaps focused window with the adjacent snapped window in that direction. If no neighbour
exists, snaps focused window to the corresponding half of the work area instead - useful
as a single binding that both snaps and swaps.

### URI Support

```
helm uri:ms-actioncenter:controlcenter/bluetooth
```

Launches the URI.

### Paste

```
helm paste
helm paste:plain
```

Injects a paste keystroke into the focused window. `paste` sends Ctrl+V, except in Windows Terminal where it sends Ctrl+Shift+V instead (the terminal's native paste binding). `paste:plain` always sends Ctrl+Shift+V regardless of window class - useful for paste-without-formatting in apps that support it.

## Kanata integration

`kanata-bridge` connects to kanata's TCP socket and routes by prefix:

| Prefix | Destination |
|--------|-------------|
| `app:firefox` | helm pipe → focus or launch firefox |
| `vd:2` | helm pipe → switch to virtual desktop 2 |
| `sz:left:+5` | helm pipe → resize left-snapped window |

Example kanata layer (in `.kbd` config):

```lisp
(defalias
  1 (cmd helm vd:1)
  2 (cmd helm vd:2)
  3 (cmd helm vd:3)
  ;; or via kanata-bridge MessagePush:
  ;; 1 (push-msg "vd:1")
)
```

Or using kanata's TCP output with kanata-bridge listening:

```
;; kanata sends MessagePush {"message": "app:firefox"}
;; bridge strips prefix, sends L"firefox" to \\.\pipe\helm
```


## Building

Requires GCC (MinGW-w64). Run `build.ps1` from a PowerShell prompt:

```powershell
.\build.ps1
```

This produces `helm.exe` and `kanata-bridge.exe`.

**Dependencies** (all Win32, no external libs):
- `helm.exe`: `ole32`, `user32`, `shell32`, `dwmapi`, `pathcch`
- `kanata-bridge.exe`: `ws2_32`

## Architecture

`helm.exe` runs as a hidden background daemon (no console window). The first time you run `helm <command>`, the client mode detects no daemon is running, spawns one with `--server`, waits for a named event `helm-daemon-ready`, then sends the command through the pipe.

Subsequent calls connect immediately - round-trip is under a millisecond on localhost named pipes.

The daemon keeps two caches to avoid `EnumWindows` on every keystroke:
- **PID cache** - process list snapshot, TTL 300ms, binary-searched by PID
- HWND cache - 16-slot LRU of exe → window handle; the topmost match is always cached, and on hit the entry is revalidated (window exists, visible, owned by the same exe, not cloaked) and confirmed still topmost in z-order, self-evicting if stale

## Weird Windows Behaviors

There are two undocumented shortcuts I found while working on this project, Win + Alt + Left and Win + Alt + Right. They seem to be like "absolute snapping", where no matter the situation, the window will snap to the entire left/right half. The biggest difference between Win + Alt + Left/Right and Win + Left/Right is when the window is currently snapped to the top or bottom half. Normally, with Win + Left/Right, the window will become a quarter window, whereas with Win + Alt + Left/Right, it snaps to the full left/right half.

Second, Win + Alt + Down requires two presses from a maximized window to snap to the bottom, whereas Win + Alt + any other direction instantly snaps to the correct position.

## Acknowledgements

Thanks to [MScholtes/VirtualDesktop](https://github.com/MScholtes/VirtualDesktop), which provided information that allows me to programmatically interact with virtual desktops.

## Why not komorebi / GlazeWM

Both are great for full tiling layouts. If you want i3-style automatic tiling, use them. Helm is for a different workflow: you already know where your windows go, you just want fast keyboard control over focus, desktops, and snap sizing. No tiling engine, no layout rules, no advanced config, just the default Windows snapping enhanced.

