# Ctrl+F for the whole screen: type what to find, then click it. The find bar
# (wl-find.qml, $WL_FIND_QML, kept loaded) comes up at once, dims the focused
# output once it is captured, and outlines each match as it is typed;
# wl-find-ocr (ocr/) reads the screen meanwhile. On Enter a lone match is
# clicked at once; several get wl-kbptr's hints, where the compositor has the
# virtual pointer they click with (not COSMIC: there the bar waits for a query
# with one match). Ctrl+Enter makes that a right click, and Ctrl+Y copies the
# text of the match instead, through wl-copy, with the same hints to pick one
# of several (they need no pointer to click with, so COSMIC has them too).
# wl-find-helper finds the output and clicks, the same way on any compositor.
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
    let dir = mktemp -d -p $env.XDG_RUNTIME_DIR wl-find.XXXXXX
    let lines = $"($dir)/lines"
    let result = $"($dir)/result"
    # The find bar reads the lines from this pipe, a few at a time. The first
    # line, empty, is sent once the screen is captured, and only then does the
    # bar dim the screen, so the dim stays out of the picture. A failed
    # capture closes the pipe unwritten, and the bar closes.
    ^mkfifo $lines
    # The bar first: the reading takes every core there is.
    let bar = open-bar $output.name $output.pointer $lines $result $pattern $copy
    if $bar == null {
        rm -r $dir
        print -e "wl-find: the find bar did not start"
        exit 1
    }
    # grim's picture goes straight to wl-find-ocr, which loads its models
    # while the screen is captured, and what that reads straight down the pipe.
    # The pointer is hidden for the picture, or it may lie across the very
    # word sought.
    job spawn {
        try { ^wl-find-helper hide-cursor $output.name grim -o $output.name -t ppm - | ^wl-find-ocr --scale $output.scale out> $lines }
    }
    # The bar leaves its answer in $result when it closes; a bar that crashed
    # leaves none.
    while (not ($result | path exists)) and ($"/proc/($bar)" | path exists) {
        sleep 10ms
    }
    # Stop whatever still reads: the bar may have left before it was done.
    job list | each {|j| job kill $j.id } | ignore
    # Logical pixels, relative to the output.
    let answer = if ($result | path exists) { open --raw $result | from json } else { { boxes: [], right: false, copy: false } }
    rm -r $dir
    let boxes = $answer.boxes
    let button = if $answer.right { "right" } else { "left" }

    match ($boxes | length) {
        0 => { }
        1 if $answer.copy => { copy-text $boxes.0.text }
        1 => {
            let b = $boxes.0
            ^wl-find-helper click $output.name ($b.x + $b.w / 2) ($b.y + $b.h / 2) $button
        }
        _ if $answer.copy => {
            let picked = pick-box $output.name $boxes
            if $picked != null { copy-text $picked.text }
        }
        _ => {
            hint-lines $boxes
            | wl-kbptr --output $output.name -o modes=floating,click -o mode_floating.source=stdin -o $"mode_click.button=($button)"
        }
    }
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
# nearest box to it is the one, whatever the rounding.
def pick-box [name: string, boxes: list] {
    let chosen = (
        hint-lines $boxes
        | ^wl-kbptr --only-print --output $name -o modes=floating -o mode_floating.source=stdin
        | complete | get stdout | lines | first 1
        | parse --regex '^(?<w>\d+)x(?<h>\d+)\+(?<x>-?\d+)\+(?<y>-?\d+) '
    )
    if ($chosen | is-empty) { return null }
    let c = $chosen.0
    let cx = ($c.x | into int) + ($c.w | into int) / 2
    let cy = ($c.y | into int) + ($c.h | into int) / 2
    $boxes | sort-by {|b| ($b.x + $b.w / 2 - $cx) ** 2 + ($b.y + $b.h / 2 - $cy) ** 2 } | first
}

# Opens the find bar on an output, with or without hints for several
# matches. A non-empty `query` is typed in, and submitted once the screen is
# read; `copy` has Enter copy the match rather than click it. The bar's pid, or
# null if no bar answers. The bar stays loaded between finds: wl-find-bar.service's where there is one, or else the one the
# first find starts here. One not running yet is started, and waited for while
# it loads. It takes finds on a socket named for its directory (wl-find.qml), so
# a service of another build (`nix run` beside an installed wl-find) starts fine
# and never answers: past ANSWER_WAIT attempts this build starts a bar of its
# own. --no-duplicate leaves one of this build that is still loading alone.
def open-bar [screen: string, hints: bool, lines: string, result: string, query: string, copy: bool] {
    let socket = $"($env.XDG_RUNTIME_DIR)/wl-find-($env.WL_FIND_QML | path dirname | path basename).sock"
    # The query as JSON, the one field that may hold a tab or a newline.
    let line = [$screen $hints $lines $result ($query | to json --raw) $copy] | str join "\t"
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
