# wl-find

Ctrl+F for the whole screen, on Wayland. Press a key and a find bar comes up.
As you type, each match on the focused output is outlined. Press Enter to act
on them:

- a single match is clicked;
- several get [wl-kbptr](https://github.com/moverest/wl-kbptr) hints, so you
  pick one by its letters.

Esc, a click elsewhere, or Enter on an empty bar closes it.

It works on Hyprland, sway, niri and COSMIC, and on other compositors with
the same Wayland protocols (see [Requirements](#requirements)).

## How it works

The text comes from OCR. When you press the key, grim captures the output
and tesseract reads it in twelve strips at once. Before tesseract, ImageMagick
makes every region dark text on a light ground. The bar shows matches from the
strips already read, so the first ones appear about half a second after the
key press.

The bar is a [Quickshell](https://quickshell.org) config that stays loaded
between finds. It is opened through a socket, so it is up about 70 ms after
the key press on an output it was last used on, and about 0.25 s on another.

## Try it

```sh
nix run github:Danielhp95/wl-find
```

The first run starts the bar, which takes about half a second, and the bar
then stays loaded. `--output NAME` searches that output instead of the
focused one.

## Install

With Home Manager, the module installs wl-find and runs the bar as a user
service (`wl-find-bar.service`), so it is loaded before the first find:

```nix
# flake.nix
inputs.wl-find.url = "github:Danielhp95/wl-find";

# a Home Manager module
imports = [ inputs.wl-find.homeManagerModules.default ];
programs.wl-find.enable = true;
```

Otherwise, install the package (`packages.<system>.default`) any way you like.
Without the service, wl-find starts the bar on first use.

Then bind a key to `wl-find`:

```lua
-- Hyprland, hyprland.lua
hl.bind("SUPER + slash", hl.dsp.exec_cmd("wl-find"))
```

```ini
# Hyprland, hyprland.conf
bind = SUPER, slash, exec, wl-find

# sway
bindsym $mod+slash exec wl-find
```

```kdl
// niri
binds {
    Mod+Slash { spawn "wl-find"; }
}
```

On COSMIC, add a custom shortcut that runs `wl-find` in Settings → Keyboard →
Keyboard shortcuts.

## COSMIC

COSMIC has no wlroots virtual pointer, so wl-find clicks through the
RemoteDesktop portal instead. The first click asks you to allow remote
control. Choose "Always allow" and later clicks go through without asking.
The portal's restore token is kept in `~/.local/state/wl-find`.

wl-kbptr needs the virtual pointer too, so COSMIC gets no hints. If you press
Enter while there are several matches, the bar stays open and asks you to type
more until one is left.

## Restyle

The bar's colours and font are package arguments:

```nix
programs.wl-find.package = inputs.wl-find.packages.${system}.default.override {
  font = "JetBrains Mono";
  colors = {
    accent = "#7aa2f7"; # the "/" and the outlines
    bg = "#1a1b26";
  };
};
```

The colour slots are `dim`, `bg`, `border`, `accent`, `accentDim` (the text
selection), `fg`, `fgDim` and `error`. Each takes `#RRGGBB`, or `#AARRGGBB`
with the alpha first, as QML reads it. `dim` defaults to wl-kbptr's own shade,
so the screen does not change shade when the hints take over. If you set
wl-kbptr's `unselectable_bg_color`, set `dim` to match it.

## Animations

The bar and the dim are layer surfaces, with namespaces `wl-find-bar` and
`wl-find`. If your compositor animates layers, they fade in and out. On
Hyprland, this rule shows and hides them at once:

```lua
hl.layer_rule({ name = "wl-find-no-anim", match = { namespace = "^wl-find" }, no_anim = true })
```

## Requirements

wl-find uses no compositor's IPC, only these Wayland protocols:

- layer-shell, for the bar and the dim;
- xdg-output, to find each output's place and size;
- wlr-screencopy or ext-image-copy-capture, for grim;
- wlr-virtual-pointer, to click and for wl-kbptr's hints. Without it, the
  RemoteDesktop portal clicks instead, through libei.

GNOME has no layer-shell, so wl-find cannot run there.

Text is read with tesseract's `eng` model. Small text on a scale-1 output is
read less reliably than on a HiDPI one.

Everything else comes with the package: Quickshell, grim, ImageMagick,
tesseract, wl-kbptr, and `wl-find-helper`, a small C program that finds the
focused output and clicks.
