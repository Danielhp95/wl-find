# Ctrl+F for the whole screen: type what to find, then click it. The find bar
# (wl-find.qml, $WL_FIND_QML, kept loaded) comes up at once, dims the focused
# output once it is captured, and outlines each match as it is typed; tesseract
# reads the screen meanwhile. On Enter a lone match is clicked at once; several
# get wl-kbptr's hints, where the compositor has the virtual pointer they click
# with (not COSMIC: there the bar waits for a query with one match). Ctrl+Enter
# makes that a right click, and Ctrl+Y copies the text of the match instead,
# through wl-copy, with the same hints to pick one of several (they need no
# pointer to click with, so COSMIC has them too).
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

# tesseract takes over two seconds on the whole 3840x2400 panel, so the capture
# is read in horizontal strips, in parallel. Strips overlap by OVERLAP logical
# pixels, so a line of text up to twice that tall is whole in one of them; each
# word belongs to the strip its middle falls in. Past 12 strips the overlaps
# cost more than they save.
const STRIPS = 12
const OVERLAP = 32
# tesseract takes the rarer shade of a whole strip for ink, so where light and
# dark grounds share a strip (a terminal beside a page, a dark header, white
# text on a button) one of them loses its text. So each strip goes in grey,
# inverted wherever it is dark on average over BLOCK logical pixels: dark text
# on a light ground throughout.
const BLOCK = 24
# tesseract misreads small text, its casing above all, so each strip is read
# enlarged to READ_AT device pixels a logical one, but by MAX_ENLARGE at most:
# 1.5 times on a scale-2 output, 1.75 on a scale-1 one. More reads worse, and
# slower. Catmull-Rom keeps the strokes sharp.
const READ_AT = 3
const MAX_ENLARGE = 1.75
# The characters tesseract takes for one another, in a monospaced font
# (fix-mono). TWINS finds the words with two different characters of one set
# side by side, the only ones that can have a glyph read twice.
const LOOKALIKES = ["0Oo" "1lI|"]
const TWINS = '0[Oo]|[Oo]0|Oo|oO|1[lI|]|l[1I|]|I[1l|]|\|[1lI]'
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
    # The find bar reads the lines from this pipe, a strip's worth at a time.
    # The first line, empty, is sent once the screen is captured, and only
    # then does the bar dim the screen, so the dim stays out of the picture. A
    # failed capture closes the pipe unwritten, and the bar closes.
    ^mkfifo $lines
    # The bar first: the OCR's three dozen processes would only slow it.
    let bar = open-bar $output.name $output.pointer $lines $result $pattern $copy
    if $bar == null {
        rm -r $dir
        print -e "wl-find: the find bar did not start"
        exit 1
    }
    job spawn {
        try {
            read-lines $output.name $output.scale $dir | each { to json --raw } | to text | save -f $lines
        } catch {
            "" | save -f $lines
        }
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
    if (open --raw $image | bytes at 0..1) != 0x[50 36] {
        print -e $"wl-find: ($image) is no PPM; grim -t ppm takes one"
        exit 1
    }
    let dir = mktemp -d -p ($env.XDG_RUNTIME_DIR? | default $nu.temp-dir) wl-find.XXXXXX
    let lines = try { read-lines "" $scale $dir --image $image | flatten } catch {|e| $e }
    # Strips still waiting on their pipes, if it failed.
    job list | each {|j| job kill $j.id } | ignore
    rm -r $dir
    if ($lines | describe) =~ "^record" {
        print -e $"wl-find: cannot read ($image): ($lines.msg)"
        exit 1
    }
    let lines = $lines | sort-by {|l| $l.0.y } {|l| $l.0.x }
    if $json { $lines | each { to json --raw } | str join "\n" } else { $lines | each { get text | str join " " } | str join "\n" }
}

# What the clipboard is given. wl-copy serves it from a process of its own that
# outlives this one, so its output goes to /dev/null, or whatever waits for
# this script would wait for it too.
def copy-text [text: string] {
    $text | ^wl-copy --type text/plain out+err> /dev/null
}

# The boxes as wl-kbptr takes its targets, WxH+X+Y, top to bottom: the strips
# come in whatever order tesseract finishes them, and the hints should not
# change with it.
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

# The lines of text on an output, in batches: an empty one as soon as grim has
# captured it, then a strip's worth as tesseract finishes each. A line is a
# list of words { text, x, y, w, h } in logical pixels. The strips go to
# tesseract through pipes in $dir. --image reads a PPM instead of the output.
def read-lines [name: string, scale: number, dir: string, --image: path] {
    # The capture first, the longest wait before the first word; it mails
    # the image back here (null if grim failed). The pointer is hidden for it,
    # or it may lie across the very word sought. `complete`, not `try`: try
    # stalls on an external's output past the pipe's 64 KiB.
    let me = job id
    job spawn {
        if $image != null {
            open --raw $image | into binary | job send --tag 1 $me
            return
        }
        let capture = ^wl-find-helper hide-cursor $name grim -o $name -t ppm - | complete
        (if $capture.exit_code == 0 { $capture.stdout | into binary }) | job send --tag 1 $me
    }
    # A tesseract for each strip, started meanwhile so its models load; each
    # mails the strip's lines back here (none if it failed), fixed up by
    # fix-line, in the pixels it read them in. One thread each: their OpenMP
    # threads would only fight the other strips' for the cores, and spin while
    # they wait.
    let pipes = 0..<$STRIPS | each {|i| $"($dir)/strip($i)" }
    ^mkfifo ...$pipes
    $env.OMP_THREAD_LIMIT = "1"
    let enlarge = [([($READ_AT / $scale) 1] | math max) $MAX_ENLARGE] | math min
    # The enlarging (READ_AT) and the dark-on-light pass (BLOCK) ahead of
    # each tesseract. magick also starts before the strip's size is known,
    # so it works the sizes out itself: %[wh] is the strip's, the other the
    # BLOCK grid over it. The image tesseract reads stays here too, for
    # fix-line to look at.
    let blocks = $BLOCK * $scale * $enlarge
    let grid = $"%[fx:round\(w/($blocks)\)]x%[fx:round\(h/($blocks)\)]!"
    for i in 0..<$STRIPS {
        let pipe = $pipes | get $i
        job spawn {
            let pgm = (
                ^cat $pipe
                | ^magick ppm:- -colorspace Gray -filter Catrom -resize $"($enlarge * 100)%"
                -set option:wh "%wx%h"
                "(" +clone -scale $grid -resize "%[wh]!" -threshold 50% -negate ")"
                -compose Difference -composite pgm:-
                | complete | get stdout
            )
            let read = if ($pgm | describe) == "binary" and ($pgm | bytes length) > 32 {
                let hocr = $pgm | ^tesseract - - -c hocr_char_boxes=1 hocr | complete | get stdout
                let pixels = pgm-pixels $pgm
                read-hocr $hocr | each {|line| fix-line $line $pixels }
            } else { [] }
            { i: $i, lines: $read } | job send --tag 2 $me
        }
    }

    let image = job recv --tag 1 --timeout 5sec
    if $image == null { error make { msg: "grim captured nothing" } }
    # grim writes "P6\n<width> <height>\n255\n", then the rows, three bytes a pixel.
    let head = $image | bytes at 0..31
    let breaks = $head | bytes index-of --all 0x[0a]
    let size = $head | bytes at ($breaks.0 + 1)..<($breaks.1) | decode utf-8 | split row " " | into int
    let width = $size.0
    let height = $size.1
    let pixels = $breaks.2 + 1
    let row = $width * 3
    let step = $height / $STRIPS | math ceil
    let overlap = $OVERLAP * $scale | math round
    let strips = 0..<$STRIPS | each {|i|
        let core = $i * $step
        {
            core: $core
            top: ([($core - $overlap) 0] | math max)
            bottom: ([($core + $step + $overlap) $height] | math min)
        }
    }
    # The strips into their pipes, all at once and off this thread: a write
    # waits for its tesseract.
    job spawn {
        0..<$STRIPS | par-each {|i|
            let s = $strips | get $i
            ($"P6\n($width) ($s.bottom - $s.top)\n255\n" | into binary) ++ (
                $image | bytes at ($pixels + $s.top * $row)..<($pixels + $s.bottom * $row)
            ) | save -f ($pipes | get $i)
        }
    }

    # The strips' lines in the order tesseract finishes them.
    0..<$STRIPS | each {
        let read = job recv --tag 2
        let s = $strips | get $read.i
        $read.lines | each {|line|
            $line
            | each {|w|
                let y = $w.y1 / $enlarge + $s.top
                let h = ($w.y2 - $w.y1) / $enlarge
                {
                    text: $w.text
                    x: ($w.x1 / $enlarge / $scale)
                    y: ($y / $scale)
                    w: (($w.x2 - $w.x1) / $enlarge / $scale)
                    h: ($h / $scale)
                    middle: ($y + $h / 2)
                }
            }
            # By the word, not the line: a line's middle moves with whatever
            # tesseract groups into it (an icon, words of a pane beside it), so
            # two strips could each place it in the other and both drop it.
            | where {|w| $w.middle >= $s.core and $w.middle < $s.core + $step }
            | reject middle
        } | where {|line| ($line | is-not-empty) }
    } | prepend [[]]
}

# What tesseract read, from its hOCR with the box of each character: lines of
# words { x1, y1, x2, y2, text, chars }, a character { x1, y1, x2, y2, conf,
# c }, in the pixels of the image it read.
#
# Every command that runs a closure (each, where, reduce, any) costs Nushell
# some 80µs before its first item, as much as a hundred items through it, so
# what runs on each word or character here does without them: each runs on a
# line at the most, and the rest is for loops and commands on whole tables.
# So the hOCR is rewritten into a TSV, a row a character or a word, which from
# tsv reads typed all at once; the rows of -1s ahead of each line and of -2s
# ahead of each word are where split list cuts the table up.
def read-hocr [hocr: string] {
    let rows = (
        $hocr
        | str replace --all "&lt;" "<" | str replace --all "&gt;" ">"
        | str replace --all "&quot;" '"' | str replace --all "&#39;" "'" | str replace --all "&amp;" "&"
        # A character, after a quote so that from tsv keeps a "1" a string.
        | str replace --all --regex r#'(?m)^\s*<span class='ocrx_cinfo' title='x_bboxes (-?\d+) (-?\d+) (-?\d+) (-?\d+); x_conf ([\d.]+)'>(.*?)</span>.*$'# "$1\t$2\t$3\t$4\t$5\t'$6"
        | str replace --all --regex r#'(?m)^\s*<span class='ocrx_word' id='[^']*' title='bbox (\d+) (\d+) (\d+) (\d+); x_wconf (\d+)'>.*$'# "-2\t-2\t-2\t-2\t-2\t\n$1\t$2\t$3\t$4\t$5\t"
        | str replace --all --regex r#'(?m)^\s*<span class='ocr_(?:line|header|caption|textfloat)'.*$'# "-1\t-1\t-1\t-1\t-1\t"
        # The rest of the hOCR.
        | str replace --all --regex r#'(?m)^[ <].*\n?'# ""
        | from tsv --noheaders --quote "\u{1}"
        | rename x1 y1 x2 y2 conf c
        | into float conf
        | str substring 1.. c
    )
    $rows | split list { x1: -1, y1: -1, x2: -1, y2: -1, conf: -1.0, c: "" } | skip 1 | each {|line|
        let words = $line | split list { x1: -2, y1: -2, x2: -2, y2: -2, conf: -2.0, c: "" } | skip 1 | each {|word|
            let chars = $word | skip 1
            let text = $chars.c | str join
            if ($text | str trim) != "" {
                { x1: $word.0.x1, y1: $word.0.y1, x2: $word.0.x2, y2: $word.0.y2, text: $text, chars: $chars }
            }
        }
        if ($words | is-not-empty) { $words }
    }
}

# A PGM as magick writes one, "P5\n<width> <height>\n255\n" and then a byte a
# pixel, as { data, width, height, at }: `at` where the pixels start.
def pgm-pixels [pgm: binary] {
    let head = $pgm | bytes at 0..31
    let breaks = $head | bytes index-of --all 0x[0a]
    let size = $head | bytes at ($breaks.0 + 1)..<($breaks.1) | decode utf-8 | split row " " | into int
    { data: $pgm, width: $size.0, height: $size.1, at: ($breaks.2 + 1) }
}

# A line's words as fix-mono, join-underscores and join-split leave them.
def fix-line [words: list, pixels: record] {
    let pitch = monospaced $words
    let words = if $pitch == null { $words } else {
        $words | each {|w| fix-mono $w $pitch $pixels } | join-underscores $pitch $pixels
    }
    join-split $words
}

# The pitch of a line set in a monospaced font: the step from the middle of
# one character to the next is the same throughout. Null if the line is not
# such, or too short to tell.
def monospaced [words: list] {
    mut steps = []
    for w in $words {
        mut a = $w.chars.0
        for b in ($w.chars | skip 1) {
            $steps ++= [(($b.x1 + $b.x2 - $a.x1 - $a.x2) / 2)]
            $a = $b
        }
    }
    if ($steps | length) < 12 { return null }
    let pitch = $steps | math median
    mut even = 0
    for s in $steps { if ($s - $pitch | math abs) < 0.15 * $pitch { $even += 1 } }
    if $even >= 0.85 * ($steps | length) { $pitch }
}

# A word of a monospaced line with what tesseract gets wrong in such fonts put
# right. Some glyphs it reads twice, as two characters it confuses ("macO0S",
# "1limit"), closer than two characters can be: the one it is surer of stays. And
# it takes a capital O for a zero ("Nix0S", "#0verview"); a programming font
# marks its zero with a dot or a slash, so a "0" before a letter with nothing
# in its middle is an O.
def fix-mono [word: record, pitch: number, pixels: record] {
    let paired = $word.text =~ $TWINS
    if not $paired and not ($word.text | str contains "0") { return $word }
    let chars = if not $paired { $word.chars } else {
        $word.chars | reduce --fold [] {|ch, kept|
            if ($kept | is-empty) { return [$ch] }
            let last = $kept | last
            let twins = (
                ($ch.x1 + $ch.x2 - $last.x1 - $last.x2) / 2 < 0.6 * $pitch
                and $ch.c != $last.c
                and ($LOOKALIKES | any {|set| ($set | str contains $ch.c) and ($set | str contains $last.c) })
            )
            if not $twins { return ($kept | append $ch) }
            # Over the whole glyph, which each of the two has a part of.
            let sure = if $ch.conf > $last.conf { $ch } else { $last }
            $kept | drop | append ($sure | merge {
                x1: ([$ch.x1 $last.x1] | math min)
                y1: ([$ch.y1 $last.y1] | math min)
                x2: ([$ch.x2 $last.x2] | math max)
                y2: ([$ch.y2 $last.y2] | math max)
            })
        }
    }
    mut fixed = $chars
    for i in ($chars | enumerate | where item.c == "0" | get index) {
        let ch = $chars | get $i
        let next = $chars | get -o ($i + 1) | get -o c | default ""
        let after = $chars | get -o ($i + 2) | get -o c | default ""
        let previous = if $i > 0 { ($chars | get ($i - 1)).c } else { "" }
        # Not 0x7f, 0b10, 0o755: a zero, whatever its font.
        let prefix = $next in ["x" "b" "o"] and $after =~ '^[0-9a-fA-F]$'
        if $next =~ '^\p{L}$' and $previous !~ '^[0-9]$' and not $prefix and (inked-centre $pixels $ch) == false {
            $fixed = $fixed | update $i ($ch | update c "O")
        }
    }
    $word | update chars $fixed | update text ($fixed.c | str join)
}

# Whether the middle of a character's box is inked, as a dotted or a slashed
# zero's is and an O's is not: across the rows about its middle, from one
# stroke to the other. Null if the box is too faint, or has no hole, to tell.
def inked-centre [pixels: record, box: record] {
    let rows = $box.y1..<($box.y2) | each {|y|
        let at = $pixels.at + $y * $pixels.width
        $pixels.data | bytes at ($at + $box.x1)..<($at + $box.x2) | chunks 1 | into int
    }
    if ($rows | length) < 3 or ($rows.0 | length) < 3 { return null }
    # The ground in its corners, and the ink as far from it as the box goes.
    let corners = [($rows | first | first) ($rows | first | last) ($rows | last | first) ($rows | last | last)] | sort
    let ground = ($corners.1 + $corners.2) / 2
    let all = $rows | flatten
    let ink = [(($all | math max) - $ground) ($ground - ($all | math min))] | math max
    if $ink < 30 { return null }
    let middle = ($rows | length) // 2
    let reach = [1 (($rows | length) * 0.12 | math round)] | math max
    # The rows about the middle, from the ground (0) to the ink (1).
    let width = $rows.0 | length
    let holes = (
        $rows | slice ($middle - $reach)..($middle + $reach) | flatten
        | each {|v| ($v - $ground | math abs) / $ink }
        | chunks $width
        | each {|row| hole-ink $row }
    )
    if ($holes | is-empty) { null } else { ($holes | math max) >= 0.45 }
}

# The most ink in the hole of a row across a character, its pixels 0 for the
# ground to 1 for the ink: past the stroke on either side, and down its inner
# edge, where it fades into the hole. Null if the strokes meet.
def hole-ink [row: list] {
    let n = $row | length
    mut l = 0
    while $l < $n and ($row | get $l) < 0.5 { $l += 1 }
    while $l < $n and ($row | get $l) >= 0.5 { $l += 1 }
    mut r = $n - 1
    while $r >= 0 and ($row | get $r) < 0.5 { $r -= 1 }
    while $r >= 0 and ($row | get $r) >= 0.5 { $r -= 1 }
    while $l < $r and ($row | get $l) >= ($row | get ($l + 1)) { $l += 1 }
    while $r > $l and ($row | get $r) >= ($row | get ($r - 1)) { $r -= 1 }
    if $l <= $r { $row | slice $l..$r | math max }
}

# The words of a monospaced line that tesseract split at an underscore it
# missed ("nixos" "2411" for nixos_2411), joined by it: one cell apart, as
# words a space apart are, but with a stroke along the bottom of that cell. An
# underline would run under the characters beside it as well.
def join-underscores [pitch: number, pixels: record] {
    let words = $in
    if ($words | length) < 2 { return $words }
    let chars = $words | get chars | flatten
    let base = $chars | get y2 | math median
    let height = $chars | each {|ch| $ch.y2 - $ch.y1 } | math median
    mut out = [($words | first)]
    for b in ($words | skip 1) {
        let a = $out | last
        let left = $a.chars | last
        let right = $b.chars | first
        let middle = ($left.x1 + $left.x2 + $right.x1 + $right.x2) / 4
        let apart = ($right.x1 + $right.x2 - $left.x1 - $left.x2) / 2
        let joined = (
            ($apart - 2 * $pitch | math abs) < 0.25 * $pitch
            and (underscore $pixels ($middle - 0.35 * $pitch | math round) ($middle + 0.35 * $pitch | math round) (($left.x1 + $left.x2) // 2) ($base - 0.1 * $height | math round) ($base + 0.35 * $height | math round))
        )
        if $joined {
            $out = $out | drop | append {
                x1: $a.x1
                y1: ([$a.y1 $b.y1] | math min)
                x2: ([$a.x2 $b.x2] | math max)
                y2: ([$a.y2 $b.y2] | math max)
                text: $"($a.text)_($b.text)"
                chars: ($a.chars ++ $b.chars)
            }
        } else {
            $out ++= [$b]
        }
    }
    $out
}

# Whether the blank cell between two words, `from` to `to`, holds an
# underscore: a row between `top` and `bottom` inked across it, but not at
# `under`, the middle of the character before it, as an underline would be.
# Down the middle of the cell first, a pixel a row: most such cells are
# spaces.
def underscore [pixels: record, from: int, to: int, under: int, top: int, bottom: int] {
    let bottom = [$bottom ($pixels.height - 1)] | math min
    if $to - $from < 3 or $bottom <= $top { return false }
    let pixel = {|x, y| $pixels.data | bytes at ($pixels.at + $y * $pixels.width + $x)..($pixels.at + $y * $pixels.width + $x) | into int }
    let middle = ($from + $to) // 2
    # The ground, in the middle of the cell, above any underscore.
    let ground = do $pixel $middle ([($top - ($bottom - $top)) 0] | math max)
    let inked = {|v| ($v - $ground | math abs) > 80 }
    for y in $top..$bottom {
        let row = $pixels.at + $y * $pixels.width
        let v = $pixels.data | bytes at ($row + $middle)..($row + $middle) | into int
        if ($v - $ground | math abs) > 80 and not (do $inked (do $pixel $under $y)) and (
            $pixels.data | bytes at ($row + $from)..($row + $to) | chunks 1 | into int
            | where {|v| do $inked $v } | length
        ) >= 0.8 * ($to - $from + 1) {
            return true
        }
    }
    false
}

# The words tesseract split where there is no space, at a "/" ("https:"
# "//a.b/c", "https" "://a.b/c", "src/" "main.rs"), joined: they are closer
# than the line's spaces, and than a character is wide. By the boxes of the
# characters: a word's own can run on over what tesseract left out.
def join-split [words: list] {
    if ($words | length) < 2 { return $words }
    mut gaps = []
    for i in 1..<($words | length) {
        $gaps ++= [((($words | get $i).chars | first).x1 - (($words | get ($i - 1)).chars | last).x2)]
    }
    let space = $gaps | math median
    let width = {|w| (($w.chars | last).x2 - ($w.chars | first).x1) / ($w.chars | length) }
    mut out = [($words | first)]
    for i in 1..<($words | length) {
        let a = $out | last
        let b = $words | get $i
        let gap = $gaps | get ($i - 1)
        let char = [(do $width $a) (do $width $b)] | math min
        # With few gaps to go by, only what is far narrower than any space.
        let narrow = if ($gaps | length) >= 3 { $gap < 0.6 * $space } else { $gap < 0.4 * $char }
        let slash = ($a.text | str ends-with "/") or ($b.text | str starts-with "/") or ($b.text | str starts-with "://")
        if $slash and $narrow and $gap < 0.9 * $char {
            $out = $out | drop | append {
                x1: ([$a.x1 $b.x1] | math min)
                y1: ([$a.y1 $b.y1] | math min)
                x2: ([$a.x2 $b.x2] | math max)
                y2: ([$a.y2 $b.y2] | math max)
                text: ($a.text + $b.text)
                chars: ($a.chars ++ $b.chars)
            }
        } else {
            $out ++= [$b]
        }
    }
    $out
}
