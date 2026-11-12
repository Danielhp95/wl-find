# Ctrl+F for the whole screen: type what to find, then click it. The find bar
# (wl-find.qml, $WL_FIND_QML, kept loaded) comes up at once, dims the focused
# output once it is captured, and outlines each match as it is typed;
# wl-find-ocr (ocr/) reads the screen meanwhile. On Enter a lone match is
# clicked at once; several get wl-kbptr's hints, where the compositor has the
# virtual pointer they click with (not COSMIC: there the bar waits for a query
# with one match). Tab picks one of several in the bar, and Enter acts on that
# one alone. Ctrl+Enter makes that a right click, Ctrl+Y copies the text of the
# match instead, through wl-copy, and Ctrl+O opens it with xdg-open, with the
# same hints to pick one of several (they need no pointer to click with, so
# COSMIC has them too). Ctrl+R has the screen read again, the query kept.
# --all-outputs searches every output, the bar on the focused one.
# wl-find-helper finds the outputs and clicks, the same way on any compositor.
# The query is a Neovim regular expression under \v (vim-regex.js). --query
# opens the bar with one already typed, and --type with a preset, as kitty's
# `kitten hints --type` has: all the URLs, or all the file paths, on screen.
# The bar then presses Enter for you once the screen is read; --copy makes
# that Ctrl+Y. `wl-find read IMAGE` prints the text it reads off a screenshot.

# The presets of --type, as bar queries. Each is a single line, so that
# test/presets.js can read it from here, and holds the same as kitty's hint of
# its name: a match is a run of non-space characters, with the punctuation
# around it left out ("(see src/main.rs)." is src/main.rs).
#
# url: a scheme and "://", then characters up to a space or a quote; it ends on
# a character that is not punctuation, or on a ")" that closes a "(" inside it,
# as in en.wikipedia.org/wiki/Foo_(bar).
const HINT_URL = r#'\v\c<%(https?|ftps?|sftp|ssh|git|file|gemini|gopher|irc|ircs|kitty)://%([^[:space:]<>"'`()]|\([^[:space:]<>"'`()]*\))*%([^[:space:]<>"'`().,;:!?\]}]|\([^[:space:]<>"'`()]*\))'#
# path: from the start of a word, or after a quote, a bracket or "=", either
# something with a "/" in it (src/main.rs, ~/.zshrc, /etc/), or a name with an
# extension (Cargo.lock). It does not end on punctuation, nor on a "/".
const HINT_PATH = r#'\v\c%(^|[[:space:]"'`(<{=[])@<=%([^[:space:]"'`<>(){}|,;*=[\]]*/[^[:space:]"'`<>(){}|,;*=[\]]*[^[:space:]"'`<>(){}|,;*=[\]./:!?]|[^[:space:]"'`<>(){}|,;*=[\]]*\.[[:alpha:]][[:alnum:]]*)'#

# open-bar's attempts, 20 ms each, for wl-find-bar.service to answer.
const ANSWER_WAIT = 25

# Type what to find on the screen, then click it. The pattern is a Neovim
# regular expression, under \v.
def main [
    --output: string # the output to search, by name; by default the focused one
    --all-outputs (-a) # search every output, with the bar on the focused one (or --output's)
    --query (-q): string # a pattern to find at once, as if typed and Enter pressed (edit it first to take that back)
    --type (-t): string # --query with a preset pattern: url or path (not both)
    --copy (-y) # copy the text of the match to the clipboard instead of clicking it, as Ctrl+Y does
] {
    let presets = { url: $HINT_URL, path: $HINT_PATH }
    if $query != null and $type != null {
        print -e "wl-find: use --query or --type, not both"
        exit 1
    }
    if $type != null and $type not-in $presets {
        print -e $"wl-find: no such --type: ($type); there are ($presets | columns | str join ', ')"
        exit 1
    }
    let pattern = if $type != null { $presets | get $type } else { $query | default "" }
    let output = ^wl-find-helper output ...([$output] | compact) | from json
    # The bar's output first: it is read first, and the others after it.
    let outputs = if $all_outputs {
        [$output] ++ (^wl-find-helper outputs | lines | each { from json } | where name != $output.name | sort-by x y)
    } else {
        [$output]
    }
    let dir = mktemp -d -p $env.XDG_RUNTIME_DIR wl-find.XXXXXX
    let result = $"($dir)/result"
    # The bar asks for the screen to be read again by making this file.
    let reread = $"($dir)/reread"
    let socket = bar-socket
    mut reads = make-reads $dir 0 $outputs
    # The bar first: the reading takes every core there is.
    let bar = open-bar $socket {
        kind: open
        screen: $output.name
        hints: $output.pointer
        reads: ($reads | select output lines)
        result: $result
        reread: $reread
        query: $pattern
        copy: $copy
    }
    if $bar == null {
        rm -r $dir
        print -e "wl-find: the find bar did not start"
        exit 1
    }
    start-reads $reads
    # The bar leaves its answer in $result when it closes; a bar that crashed
    # leaves none. Ctrl+R has the bar take its dim off the screen and ask
    # for it to be read again: the reading is stopped, wherever it was, and a
    # new one goes down new pipes, which the bar is told of.
    mut generation = 0
    while (not ($result | path exists)) and ($"/proc/($bar)" | path exists) {
        if ($reread | path exists) {
            rm $reread
            job list | each {|j| job kill $j.id } | ignore
            $generation += 1
            $reads = make-reads $dir $generation $outputs
            let line = { kind: read, result: $result, reads: ($reads | select output lines) } | to json --raw
            ^wl-find-helper send $socket $line | complete | ignore
            start-reads $reads
        }
        sleep 10ms
    }
    # Stop whatever still reads: the bar may have left before it was done.
    job list | each {|j| job kill $j.id } | ignore
    # Logical pixels, relative to the output each box names.
    let answer = if ($result | path exists) { open --raw $result | from json } else { { boxes: [], action: click } }
    rm -r $dir
    let boxes = $answer.boxes
    if ($boxes | is-empty) { return }

    # The bar leaves several boxes only when they are on one output.
    if $answer.action in [copy open] {
        let picked = if ($boxes | length) == 1 { $boxes.0 } else { pick-box $boxes }
        if $picked == null { return }
        if $answer.action == copy { copy-text $picked.text } else { open-text $picked.text }
    } else {
        let button = if $answer.action == right { "right" } else { "left" }
        if ($boxes | length) == 1 {
            let b = $boxes.0
            ^wl-find-helper click $b.output ($b.x + $b.w / 2) ($b.y + $b.h / 2) $button
        } else {
            hint-lines $boxes
            | wl-kbptr --output $boxes.0.output -o modes=floating,click -o mode_floating.source=stdin -o $"mode_click.button=($button)"
        }
    }
}

# A pipe for each output to send its lines down, as the bar takes them:
# { output, scale, lines }, the pipe at `lines`. The bar reads the lines from
# it a few at a time. The first line, empty, is sent once the output is
# captured, and only then does the bar dim it, so the dim stays out of the
# picture. A failed capture closes the pipe unwritten.
def make-reads [dir: string, generation: int, outputs: list] {
    $outputs | enumerate | each {|o|
        let lines = $"($dir)/lines-($generation)-($o.index)"
        ^mkfifo $lines
        { output: $o.item.name, scale: $o.item.scale, lines: $lines }
    }
}

# Reads each output down its pipe: grim's picture goes straight to
# wl-find-ocr, which loads its models while the output is captured, and what
# that reads straight down the pipe. All are captured at once, but read one
# after another, the bar's first: wl-find-ocr's threads spin on every core, so
# two read at once each take as long as both one after the other, nice or not.
# Each after the first waits (--after) for the one before it to be done, its
# picture and models in already. The pointer is hidden for each picture, or it
# may lie across the very word sought.
def start-reads [reads: list] {
    $reads | enumerate | each {|r|
        let read = $r.item
        let after = if $r.index == 0 { [] } else { [--after $"(($reads | get ($r.index - 1)).lines).done"] }
        job spawn {
            try { ^wl-find-helper hide-cursor $read.output grim -o $read.output -t ppm - | ^wl-find-ocr --scale $read.scale ...$after out> $read.lines }
            touch $"($read.lines).done"
        }
    } | ignore
}

# Prints the text wl-find reads off an image, a line of the screen to a line,
# as the find bar searches it: where to look when it does not find something.
# `grim -t ppm shot.ppm` takes one.
def "main read" [
    image: path # a PPM, as grim -t ppm writes
    --scale: number = 1 # the scale of the output it was taken on
    --json # the lines as the bar gets them: words with their boxes, in logical pixels
] {
    let read = ^wl-find-ocr --scale $scale $image | complete
    if $read.exit_code != 0 {
        print -e $"wl-find: cannot read ($image): ($read.stderr | str trim | str replace --regex '^wl-find-ocr: ' '')"
        exit 1
    }
    # A few lines of the screen to each line of output, as they were read.
    let lines = $read.stdout | lines | each { from json } | flatten | sort-by {|l| $l.0.y } {|l| $l.0.x }
    if $json { $lines | each { to json --raw } | str join "\n" } else { $lines | each { get text | str join " " } | str join "\n" }
}

# What Ctrl+O opens: the xdg-open of the session, as the one that comes with
# xdg-utils would bring ~200 MiB with it. It runs in the background, as it
# may wait for what it opens. A path from the home directory, as a shell
# prints it (~/notes.md), is opened as the file it names.
def open-text [text: string] {
    let target = if $text == "~" or ($text | str starts-with "~/") { $text | path expand } else { $text }
    ^bash -c 'xdg-open "$1" > /dev/null 2>&1 &' wl-find $target
}

# What the clipboard is given. wl-copy serves it from a process of its own that
# outlives this one, so its output goes to /dev/null, or whatever waits for
# this script would wait for it too.
def copy-text [text: string] {
    $text | ^wl-copy --type text/plain out+err> /dev/null
}

# The boxes as wl-kbptr takes its targets, WxH+X+Y, top to bottom: the lines
# come in whatever order they were read, and the hints should not change with
# it.
def hint-lines [boxes: list] {
    $boxes | sort-by y x | each {|b|
        let x = $b.x | math floor
        let y = $b.y | math floor
        $"(($b.x + $b.w | math ceil) - $x)x(($b.y + $b.h | math ceil) - $y)+($x)+($y)"
    } | str join "\n"
}

# The box that wl-kbptr's hints pick, or null if none is: --only-print leaves
# the pointer alone and prints the area chosen, "WxH+X+Y +X+Y b", the output's
# place and the button, in the output's own pixels, as the targets are. The
# nearest box to it is the one, whatever the rounding. The boxes are all on
# one output.
def pick-box [boxes: list] {
    let chosen = (
        hint-lines $boxes
        | ^wl-kbptr --only-print --output $boxes.0.output -o modes=floating -o mode_floating.source=stdin
        | complete | get stdout | lines | first 1
        | parse --regex '^(?<w>\d+)x(?<h>\d+)\+(?<x>-?\d+)\+(?<y>-?\d+) '
    )
    if ($chosen | is-empty) { return null }
    let c = $chosen.0
    let cx = ($c.x | into int) + ($c.w | into int) / 2
    let cy = ($c.y | into int) + ($c.h | into int) / 2
    $boxes | sort-by {|b| ($b.x + $b.w / 2 - $cx) ** 2 + ($b.y + $b.h / 2 - $cy) ** 2 } | first
}

# The socket the find bar of this build takes finds on, named for its
# directory (wl-find.qml).
def bar-socket [] {
    $"($env.XDG_RUNTIME_DIR)/wl-find-($env.WL_FIND_QML | path dirname | path basename).sock"
}

# Opens the find bar with `message`, a line of JSON: on output `screen`, with
# or without `hints` for several matches, reading the outputs of `reads` down
# their pipes, and leaving its answer in `result`. A non-empty `query` is
# typed in, and submitted once the screen is read; `copy` has Enter copy the
# match rather than click it. The bar's pid, or null if no bar answers. The
# bar stays loaded between finds: wl-find-bar.service's where there is one,
# or else the one the first find starts here. One not running yet is started,
# and waited for while it loads. Its socket is named for its build
# (bar-socket), so a service of another build (`nix run` beside an installed
# wl-find) starts fine and never answers: past ANSWER_WAIT attempts this
# build starts a bar of its own. --no-duplicate leaves one of this build that
# is still loading alone.
def open-bar [socket: string, message: record] {
    let line = $message | to json --raw
    mut own_at = $ANSWER_WAIT
    for attempt in 0..<150 {
        let sent = ^wl-find-helper send $socket $line | complete
        if $sent.exit_code == 0 { return ($sent.stdout | str trim | into int) }
        if $attempt == 0 and (^systemctl --user start wl-find-bar.service | complete).exit_code != 0 {
            $own_at = 0
        }
        if $attempt == $own_at {
            ^qs --path $env.WL_FIND_QML --daemonize --no-duplicate out+err> /dev/null
        }
        sleep 20ms
    }
    null
}
