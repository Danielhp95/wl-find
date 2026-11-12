# wl-find

Ctrl+F for the whole screen, on Wayland. Press a key and a find bar comes up.
As you type a [pattern](#patterns), each match on the focused output is
outlined. Press Enter to act on them:

- a single match is clicked;
- several get [wl-kbptr](https://github.com/moverest/wl-kbptr) hints, so you
  pick one by its letters.

Or pick one in the bar: Tab outlines the next match brighter than the rest,
Shift+Tab the one before, and Enter then clicks that one alone.

Ctrl+Enter does the same with a right click, Ctrl+Y copies the text of the
match to the clipboard instead of clicking it (with hints, the one you pick),
and Ctrl+O opens it with `xdg-open`: a URL in your browser, a file in the app
for its type. A path from your home, as `~/notes.md`, opens the file it names.

Ctrl+R reads the screen again, and keeps what you typed: for when it changed
after the bar came up, as a page that was still loading.

Esc, a click elsewhere, or Enter on an empty bar closes it. Ctrl+H shows or
hides a cheat sheet of the patterns.

It works on Hyprland, sway, niri and COSMIC, and on other compositors with
the same Wayland protocols (see [Requirements](#requirements)).

## Patterns

What you type is a Neovim regular expression under `\v` (very magic, see
`:help /\v`), so `(foo|bar)+` needs no backslashes and a literal `(` is `\(`.
All of `:help pattern` that makes sense on one line of text works: lazy repeats
(`{-}`), `<` and `>`, lookaround (`@=`, `@!`, `@<=`, `@<!`, `@>`), `\zs` and `\ze`,
`%[]`, `&`, back references, and `\m`, `\M` and `\V` to change the magic level.
Start with `\V` to search for text literally. What needs a buffer or a cursor
(`%V`, `%#`, `%'m`, `%23l`, `%23c`, `%23v`) and `~` are refused.

Case works as with `'ignorecase'` and `'smartcase'`: `save` also finds `Save`,
but `Save` does not find `save`. `\c` and `\C` override that.

A match can span words on a line, which are searched joined by single spaces.
It cannot span lines. `bad pattern` in the bar means the text is not a pattern
yet, as `(foo` is on the way to `(foo)`, or that it backtracks too much to finish.

The bar has its own matcher, `vim-regex.js`, as QML's RegExp has no lookbehind.
`test/vim-regex.js` checks it against Neovim, pattern by pattern:

```sh
node test/vim-regex.js   # needs nvim on PATH
```

## Start with a pattern

`wl-find --query PATTERN` (`-q`) opens the bar with PATTERN already typed, as a
[pattern](#patterns) like any other, and presses Enter for you as soon as the
screen is read: one match is clicked, several get hints. If there is no match,
the bar stays open and says so. Typing in the bar before that takes the Enter
back, and the pattern is yours to edit.

`wl-find --type TYPE` (`-t`) does the same with a preset, as kitty's
`kitten hints --type` has. It is like `--query`, so use one or the other.

Add `--copy` (`-y`) to have Ctrl+Y pressed instead of Enter: the match, or the
one you pick of several by its hint, is copied rather than clicked, as kitty's
hints do with `--program @`.

| Type   | Matches                                                                     |
| ------ | --------------------------------------------------------------------------- |
| `url`  | `http`, `https`, `ftp`, `ssh`, `git`, `file`... URLs, in any case           |
| `path` | anything with a `/` in it, or a name with an extension: `src/main.rs`, `~/.zshrc`, `Cargo.lock` |

Quotes and brackets around a match are not part of it, nor is the punctuation
that ends a sentence: `(see src/main.rs).` is `src/main.rs`. A `)` ends a URL
only if it has no `(` to close, so `en.wikipedia.org/wiki/Foo_(bar)` is whole.
A path with a space in it is two matches, and `path` also takes what only
looks like one, such as `and/or`.

The presets are plain patterns, at the top of `wl-find.nu`, if you want to
adapt one: they start with `\v\c` (very magic, ignore case).

```lua
-- Hyprland, hyprland.lua
hl.bind("SUPER + U", hl.dsp.exec_cmd("wl-find --type url"))
hl.bind("SUPER + P", hl.dsp.exec_cmd("wl-find --type path"))
```

```lua
-- ...to copy, not to click
hl.bind("SUPER + SHIFT + U", hl.dsp.exec_cmd("wl-find --type url --copy"))
```

```sh
wl-find -q 'TODO|FIXME'   # your own, to click
wl-find -q 'TODO|FIXME' -y   # ...or to copy
```

`node test/presets.js` checks the presets against Neovim, line by line (it
needs nvim too).

`test/ocr-bench.js` measures how well the screen is read: it renders three
made-up screens in headless Chromium, at scale 1 and 2, has `wl-find read`
read them, and counts the words and URLs read right:

```sh
nix shell .#default nixpkgs#chromium -c node test/ocr-bench.js
```

`test/ocr.js` is the test of that reading: a small screen, kept as
`test/ocr-fixture.ppm.gz`, whose every line must come out as written, each
word where the browser drew it. `nix flake check` runs it, and the tests of
the reader's own parts (`ocr/test.cc`) as it builds the reader.

```sh
nix shell .#default.ocr -c node test/ocr.js
```

## Every output

`wl-find --all-outputs` (`-a`) searches every output at once, with the bar on
the focused one. All are captured at once, and read one after another, the
focused one first, so its matches come as soon as with one output.

```lua
-- Hyprland, hyprland.lua
hl.bind("SUPER + SHIFT + slash", hl.dsp.exec_cmd("wl-find --all-outputs"))
```

Tab goes through the matches an output at a time. Hints show on one output
only, so when the matches are on several, Enter asks you to pick one with Tab,
or to type more until they are on one.

## How it works

The text comes from OCR. When you press the key, grim captures the output (each
of them, with [`--all-outputs`](#every-output)) and
`wl-find-ocr` reads it with two small neural models, run by ONNX Runtime: one
finds the lines of text on the screen, the other reads each line off the
screen's own pixels. Every core reads at once. A line is cut into pieces of a
few words, each read with a little of its neighbours and sewn back where the
two readings agree, so that a long line does not keep the rest waiting; on a
processor with fast and slow cores, the long pieces go to the fast ones. The
bar shows matches from the lines already read. A 3840x2400 screen full of
text is read in about 0.4 s on mains power, and in under a second on battery
with the processor held back (a 24-core laptop).

A recogniser sees every line at one height, so it is unsure of what only size
or a detail tells apart. wl-find settles those from the pixels of each
letter:

- a letter that is its own capital at another size (c, o, s, u, v, w, x, z):
  as tall as the line's capitals it is the capital, and as tall as its small
  letters the small one;
- in the monospaced fonts of terminals and editors, a capital O and a zero: a
  programming font puts a dot or a slash in its zero, so `Nix0S` is `NixOS`;
- there too, a backtick, which is read as an apostrophe or not at all, a
  `|`, which is read as an I, and underscores side by side, which are read
  as one: `__init__` is as many as the cells its strokes lie under;
- a mark read where there is no ink, which is a space, and letters a whole
  cell apart with no space read between them.

To see what wl-find reads off a screen, take a picture of it and read that:

```sh
grim -t ppm shot.ppm
wl-find read shot.ppm --scale 2   # your output's scale
```

It prints the text a line of the screen to a line, as the bar searches it, or
with `--json` the words and their boxes, as the bar gets them.

The bar is a [Quickshell](https://quickshell.org) config that stays loaded
between finds. It is opened through a socket, so it is up about 0.1 s after the
key press.

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

wl-kbptr needs the virtual pointer too, so COSMIC gets no hints to click with.
If you press Enter while there are several matches, the bar stays open and asks
you to pick one with Tab, or to type more until one is left. Copying (Ctrl+Y, `--copy`) uses wl-kbptr's
`--only-print`, which moves no pointer, so it does get hints.

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

The bar, the dim and the cheat sheet are layer surfaces, with namespaces
`wl-find-bar`, `wl-find` and `wl-find-cheatsheet`. If your compositor animates layers, they fade in and out. On
Hyprland, this rule shows and hides them at once:

```lua
hl.layer_rule({ name = "wl-find-no-anim", match = { namespace = "^wl-find" }, no_anim = true })
```

## Requirements

wl-find uses no compositor's IPC, only these Wayland protocols:

- layer-shell, for the bar and the dim;
- xdg-output, to find each output's place and size;
- wlr-screencopy or ext-image-copy-capture, for grim;
- viewporter, to hide the pointer while grim captures: a compositor that draws
  the pointer in software draws it into captures too;
- wlr-virtual-pointer, to click and for wl-kbptr's hints. Without it, the
  RemoteDesktop portal clicks instead, through libei;
- wlr-data-control or ext-data-control, for wl-copy, to copy with Ctrl+Y.

GNOME has no layer-shell, so wl-find cannot run there.

The package ships the two models (9 MiB), ONNX Runtime's library alone, and a
Quickshell built with just the features the bar uses, so its closure is about
940 MiB. Most of that is Qt.

Small text on a scale-1 output is read less reliably than on a HiDPI one.
Some characters look too much alike to be told apart everywhere: `l` and `I`
in many interface fonts, and `1` and `l` in some monospaced ones, so `api/v1`
may come out `api/vl`.

Everything else comes with the package: Quickshell, grim, wl-kbptr,
wl-clipboard, `wl-find-ocr`, which reads the screen, and `wl-find-helper`, a
small C program that finds the outputs and clicks. Ctrl+O is the exception:
it runs the `xdg-open` of your session, which every desktop has. The one
xdg-utils ships would add some 200 MiB to the closure.

## Languages

The models are PaddleOCR's (Apache 2.0), as
[RapidOCR](https://github.com/RapidAI/RapidOCR) publishes them for ONNX
Runtime: PP-OCRv6's smallest detector, and PP-OCRv5's recogniser for English,
which also reads the accented letters of western Europe. Other scripts need
another recogniser, which is a package argument. PP-OCRv6's own reads some
fifty languages, Chinese and Japanese among them; on English it does as well
on the benchmark above, and takes a tenth longer:

```nix
programs.wl-find.package = inputs.wl-find.packages.${system}.default.override {
  recogniser = pkgs.fetchurl {
    url = "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6/rec/PP-OCRv6_rec_small.onnx";
    hash = "sha256-bzJyRrUDiPPBdq4wS9lXZ+ptwMmukhU++MviELPBSIQ=";
  };
};
```

Any recogniser RapidOCR publishes as ONNX will do: it carries the list of its
characters with it. `detector` is the other model, the same way.
