// wl-find's find bar. It stays loaded, and wl-find.nu opens it with a line
// down a socket, so the bar is up as soon as it is called. Once the screen
// is captured the output dims as wl-kbptr's hints do, and each match of the
// typed pattern, a Neovim regular expression under \v (vim-regex.js), is cut
// out of the dim and outlined. Enter leaves the matches for
// wl-find.nu to click, or, where there are no hints to pick one of several
// with, waits for a query with one match; Esc, a click anywhere, or Enter on
// an empty bar leave none. Ctrl+H shows and hides a cheat sheet of the patterns.
// The @…@ colours, font and tools come from package.nix.
import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import Quickshell
import Quickshell.Io
import Quickshell.Wayland
import "vim-regex.js" as VimRegex

ShellRoot {
    id: root

    // One find at a time, from open() to close().
    property bool active: false
    property string resultPath: ""
    // The lines of words { text, x, y, w, h } wl-find.nu has read off the
    // screen so far, in logical pixels. They come a strip at a time, after an
    // empty first batch that says the screen is captured: the dim shows only
    // then, so it stays out of the capture.
    property var lines: []
    property bool captured: false
    property bool reading: true
    // Enter came before the OCR was done: submit once it is.
    property bool submitted: false
    // Whether wl-find.nu can show hints to pick one of several matches.
    property bool hints: true
    // Enter on several matches with no hints: "type more" until the query
    // changes.
    property bool narrow: false
    // Ctrl+H. It stays as it was left from one find to the next.
    property bool cheatsheetWanted: false
    // The pattern's boxes, or bad if it is no pattern, as "(foo" is on the
    // way to "(foo)".
    readonly property var found: find(input.text, lines)
    readonly property var matches: found.boxes
    // Where the bar sits on its output, in the output's logical pixels, as
    // the compositor centres it. It is up before the screen is captured, so
    // its own words may be read with the rest; overBar() drops them.
    readonly property rect barRect: bar.screen
        ? Qt.rect((bar.screen.width - bar.implicitWidth) / 2,
            bar.screen.height - bar.margins.bottom - bar.implicitHeight,
            bar.implicitWidth, bar.implicitHeight)
        : Qt.rect(0, 0, 0, 0)

    // As barRect: the compositor centres it on the right.
    readonly property rect cheatsheetRect: cheatsheet.screen
        ? Qt.rect(cheatsheet.screen.width - cheatsheet.margins.right - cheatsheet.implicitWidth,
            (cheatsheet.screen.height - cheatsheet.implicitHeight) / 2,
            cheatsheet.implicitWidth, cheatsheet.implicitHeight)
        : Qt.rect(0, 0, 0, 0)

    function overBar(b) {
        return b.x < barRect.x + barRect.width && b.x + b.w > barRect.x
            && b.y < barRect.y + barRect.height && b.y + b.h > barRect.y;
    }

    // Text as find() compares it: compatibility forms folded (the "ﬁ"
    // tesseract reads off a ligature is "fi"), and curly quotes, primes and
    // long dashes made the plain ones a keyboard types.
    function fold(text) {
        return text.normalize("NFKC")
            .replace(/[\u2018\u2019\u201a\u201b\u2032]/g, "'")
            .replace(/[\u201c\u201d\u201e\u201f\u2033]/g, '"')
            .replace(/[\u2010-\u2015\u2212]/g, "-");
    }

    // Where the pattern matches, one box per match. Each line is searched as
    // its words joined by a space, so a match may span them. A box covers
    // only the matched letters, sized by their share of the folded word.
    // Case goes as in Neovim with 'ignorecase' and 'smartcase'.
    function find(query, lines) {
        if (query.trim() === "") return { boxes: [], bad: false };
        const boxes = [];
        try {
            const pattern = VimRegex.compile(fold(query));
            for (const read of lines) {
                const words = read.map(w => ({ text: fold(w.text), x: w.x, y: w.y, w: w.w, h: w.h }));
                const text = words.map(w => w.text).join(" ");
                for (let at = 0, match; (match = pattern.search(text, at)) !== null; ) {
                    if (match.end > match.start) boxes.push(box(words, match));
                    at = Math.max(match.end, at + 1);
                }
            }
        } catch (e) {
            return { boxes: [], bad: true };
        }
        return { boxes: boxes, bad: false };
    }

    // The box of a match in a line's words, given as offsets into them joined.
    function box(words, match) {
        let x = Infinity, y = Infinity, right = -Infinity, bottom = -Infinity;
        let wordStart = 0;
        for (const w of words) {
            const from = Math.max(match.start - wordStart, 0);
            const to = Math.min(match.end - wordStart, w.text.length);
            if (to > from) {
                x = Math.min(x, w.x + w.w * from / w.text.length);
                right = Math.max(right, w.x + w.w * to / w.text.length);
                y = Math.min(y, w.y);
                bottom = Math.max(bottom, w.y + w.h);
            }
            wordStart += w.text.length + 1;
        }
        return { x: x, y: y, w: right - x, h: bottom - y };
    }

    function submit() {
        if (input.text.trim() === "") {
            close([]);
        } else if (reading) {
            submitted = true;
        } else if (matches.length === 1 || (matches.length > 1 && hints)) {
            close(matches);
        } else if (matches.length > 1) {
            narrow = true;
        }
    }

    // Hides the bar and leaves the boxes in resultPath, where wl-find.nu waits
    // for them: renamed into place a moment later, once the windows are off
    // the screen it may click.
    function close(boxes) {
        active = false;
        reader.running = false;
        Quickshell.execDetached([
            "@sh@", "-c", '@coreutils@/sleep 0.03 && printf %s "$1" > "$2.part" && @coreutils@/mv "$2.part" "$2"',
            "sh", JSON.stringify(boxes), resultPath
        ]);
    }

    function screenNamed(name) {
        for (let i = 0; i < Quickshell.screens.length; i++)
            if (Quickshell.screens[i].name === name) return Quickshell.screens[i];
        return null;
    }

    // A find on output `screen`, with or without `hints`: the lines come from
    // the pipe at `lines`, the boxes chosen go to `result`. One still open is
    // closed with none.
    function open(screen, hints, lines, result) {
        if (active) close([]);
        root.lines = [];
        captured = false;
        reading = true;
        submitted = false;
        root.hints = hints;
        narrow = false;
        resultPath = result;
        input.text = "";
        reader.exec(["@coreutils@/cat", lines]);
        const output = screenNamed(screen);
        dim.screen = output;
        cheatsheet.screen = output;
        bar.screen = output;
        active = true;
        input.forceActiveFocus();
    }

    onReadingChanged: if (active && submitted) submit()

    // wl-find.nu opens a find with a line down this socket, named for this
    // file's directory, one per build: screen, hints, lines and result, between tabs. Not `qs ipc`: that
    // is a Qt program of its own, ~60 ms to start before the bar could.
    SocketServer {
        active: true
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/wl-find-"
            + (Quickshell.env("WL_FIND_QML") || "/wl-find/").split("/").slice(-2)[0] + ".sock"
        handler: Socket {
            parser: SplitParser {
                onRead: line => {
                    const [screen, hints, lines, result] = line.split("\t");
                    root.open(screen, hints === "true", lines, result);
                }
            }
        }
    }

    // One JSON line per batch of lines. The pipe closes when the OCR is done,
    // or unwritten when the capture failed.
    Process {
        id: reader
        stdout: SplitParser {
            onRead: data => {
                if (!root.active) return;
                const read = JSON.parse(data).map(line => line.filter(w => !root.overBar(w)));
                root.lines = root.lines.concat(read.filter(line => line.length > 0));
                root.captured = true;
            }
        }
        onExited: {
            if (!root.active) return;
            root.reading = false;
            if (!root.captured) root.close([]);
        }
    }

    // An SVG path around a rectangle with rounded corners.
    function rounded(b, r) {
        return `M${b.x + r} ${b.y}h${b.width - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}`
            + `v${b.height - 2 * r}a${r} ${r} 0 0 1 ${-r} ${r}`
            + `h${-(b.width - 2 * r)}a${r} ${r} 0 0 1 ${-r} ${-r}`
            + `v${-(b.height - 2 * r)}a${r} ${r} 0 0 1 ${r} ${-r}Z`;
    }

    // The dim, with a hole over each match (even-odd fill). It maps after
    // the bar, so it may be stacked over it: a hole in the bar's shape keeps
    // the bar clear wherever the compositor puts it, and the cheat sheet too.
    PanelWindow {
        id: dim
        visible: root.active && root.captured
        anchors {
            top: true
            bottom: true
            left: true
            right: true
        }
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        WlrLayershell.namespace: "wl-find"

        Shape {
            anchors.fill: parent
            ShapePath {
                fillColor: "@dim@"
                strokeWidth: -1
                fillRule: ShapePath.OddEvenFill
                PathSvg {
                    path: {
                        let d = `M0 0H${dim.width}V${dim.height}H0Z`;
                        d += root.rounded(root.barRect, barBox.radius);
                        if (cheatsheet.visible) d += root.rounded(root.cheatsheetRect, barBox.radius);
                        for (const b of root.matches)
                            d += `M${b.x - 3} ${b.y - 2}h${b.w + 6}v${b.h + 4}h${-(b.w + 6)}Z`;
                        return d;
                    }
                }
            }
        }

        Repeater {
            model: root.matches
            Rectangle {
                required property var modelData
                x: modelData.x - 3
                y: modelData.y - 2
                width: modelData.w + 6
                height: modelData.h + 4
                radius: 3
                color: "transparent"
                border.width: 2
                border.color: "@accent@"
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: mouse => {
                if (!root.overBar({ x: mouse.x, y: mouse.y, w: 0, h: 0 })) root.close([]);
            }
        }
    }

    // The bar, up before the screen is captured (barRect). Its window comes
    // and goes with each find: left mapped behind opaque windows between finds,
    // it would get no frame callbacks, and Qt would never commit the next raise.
    PanelWindow {
        id: bar
        visible: root.active
        anchors.bottom: true
        margins.bottom: 72
        implicitWidth: 560
        implicitHeight: 52
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.Exclusive
        WlrLayershell.namespace: "wl-find-bar"

        Rectangle {
            id: barBox
            anchors.fill: parent
            radius: 14
            color: "@bg@"
            border.width: 1
            border.color: "@border@"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 12

                Text {
                    text: "/"
                    color: "@accent@"
                    font.family: "@font@"
                    font.pixelSize: 20
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    TextInput {
                        id: input
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        clip: true
                        focus: true
                        color: "@fg@"
                        selectionColor: "@accentDim@"
                        font.family: "@font@"
                        font.pixelSize: 20
                        // No ligatures: "@<=" is three characters here.
                        font.features: { "calt": 0, "liga": 0 }
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_H && (event.modifiers & Qt.ControlModifier)) {
                                root.cheatsheetWanted = !root.cheatsheetWanted;
                                event.accepted = true;
                            }
                        }
                        Keys.onEscapePressed: root.close([])
                        Keys.onReturnPressed: root.submit()
                        Keys.onEnterPressed: root.submit()
                        onTextChanged: root.narrow = false
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: input.text === ""
                        text: "Find on screen"
                        color: "@fgDim@"
                        font: input.font
                    }
                }

                Text {
                    // Matches found while the rest is still read end in "…".
                    text: {
                        if (input.text.trim() === "") return "<c-h> for help";
                        const n = root.matches.length;
                        if (root.narrow) return `${n} matches: type more`;
                        if (root.found.bad) return "bad pattern";
                        const count = n === 0 ? "no match" : n === 1 ? "1 match" : `${n} matches`;
                        if (root.reading) return n === 0 ? "reading…" : `${count}…`;
                        return count;
                    }
                    color: root.narrow || root.found.bad || (!root.reading && input.text.trim() !== "" && root.matches.length === 0) ? "@error@" : "@fgDim@"
                    font.family: "@font@"
                    font.pixelSize: 16
                    textFormat: Text.PlainText
                }
            }
        }
    }

    // Ctrl+H's cheat sheet. It waits for the capture, or the OCR would read it
    // and find matches in it.
    PanelWindow {
        id: cheatsheet
        visible: root.active && root.captured && root.cheatsheetWanted
        anchors.right: true
        margins.right: 24
        implicitWidth: 400
        implicitHeight: sheet.implicitHeight + 32
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.layer: WlrLayer.Overlay
        WlrLayershell.keyboardFocus: WlrKeyboardFocus.None
        WlrLayershell.namespace: "wl-find-cheatsheet"

        Rectangle {
            anchors.fill: parent
            radius: barBox.radius
            color: "@bg@"
            border.width: 1
            border.color: "@border@"

            Column {
                id: sheet
                x: 16
                y: 16
                width: parent.width - 32
                spacing: 12

                Repeater {
                    model: [
                        { title: "One character", rows: [
                            [".", "any"],
                            ["\\d \\D", "digit, not a digit"],
                            ["\\w \\W", "word character, not one"],
                            ["\\s \\S", "space or tab, neither"],
                            ["\\a \\l \\u", "letter, lower, upper"],
                            ["[abc] [^a-z]", "one of, none of"],
                            ["[[:punct:]]", "named class"],
                            ["\\. \\( \\<", "the character itself"]
                        ] },
                        { title: "Repeats", rows: [
                            ["* +", "0 or more, 1 or more"],
                            ["= ?", "0 or 1"],
                            ["{2} {2,5} {,5}", "exactly, between, at most"],
                            ["{-} {-1,}", "as few as possible"]
                        ] },
                        { title: "Where", rows: [
                            ["^ $", "start, end of the line"],
                            ["< >", "start, end of a word"],
                            ["\\zs \\ze", "match starts, ends here"]
                        ] },
                        { title: "Groups", rows: [
                            ["a|b", "a or b"],
                            ["(a) %(a)", "group, without capture"],
                            ["\\1", "what group 1 matched"],
                            ["fu%[nction]", "fu, fun, func..."],
                            [".*a&.*b", "a and b, in any order"]
                        ] },
                        { title: "Look around", rows: [
                            ["a(b)@=", "a, before b"],
                            ["a(b)@!", "a, not before b"],
                            ["(b)@<=a", "a, after b"],
                            ["(b)@<!a", "a, not after b"]
                        ] },
                        { title: "Modes", rows: [
                            ["\\c \\C", "ignore case, match case"],
                            ["\\V", "the rest is literal"]
                        ] }
                    ]

                    Column {
                        required property var modelData
                        width: sheet.width
                        spacing: 3

                        Text {
                            text: modelData.title
                            color: "@accent@"
                            font.family: "@font@"
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Repeater {
                            model: modelData.rows

                            Row {
                                required property var modelData
                                spacing: 0

                                Text {
                                    width: 150
                                    text: modelData[0]
                                    color: "@fg@"
                                    font.family: "@font@"
                                    font.pixelSize: 14
                                    font.features: { "calt": 0, "liga": 0 }
                                    textFormat: Text.PlainText
                                }

                                Text {
                                    text: modelData[1]
                                    color: "@fgDim@"
                                    font.family: "@font@"
                                    font.pixelSize: 14
                                    textFormat: Text.PlainText
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
