// wl-find's find bar. It stays loaded, and wl-find.nu opens it with a line
// down a socket, so the bar is up as soon as it is called. Once an output
// is captured it dims as wl-kbptr's hints do, and each match of the typed
// pattern, a Neovim regular expression under \v (vim-regex.js), is cut out
// of the dim and outlined. Enter leaves the matches for wl-find.nu to click,
// or, where there are no hints to pick one of several with, waits for a
// query with one match; Tab and Shift+Tab pick one of several here, and
// Enter then leaves that one alone. Ctrl+Enter does the same with a right
// click, Ctrl+Y has the text of the match copied instead, and Ctrl+O has it
// opened. Esc, a click anywhere, or Enter on an empty bar leave none. Ctrl+R
// has the screen read again, the pattern kept. wl-find --query or --type
// types a pattern in, and Enter is pressed for you once the screen is read,
// unless you edit it first. Ctrl+H shows and hides a cheat sheet of the
// patterns. wl-find --all-outputs searches every output, with the bar on
// the focused one.
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
    // The file whose making asks wl-find.nu to read the screen again.
    property string rereadPath: ""
    // The output the bar is on, by name.
    property string screenName: ""
    // The outputs searched, each { output, lines }: the pipe its lines come
    // down, the bar's own output first.
    property var reads: []
    // The lines { output, words } wl-find.nu has read off the outputs so far,
    // each word { text, x, y, w, h } in the logical pixels of its output.
    // They come a few at a time, after an empty first batch that says the
    // output is captured: its dim shows only then, so it stays out of the
    // capture.
    property var lines: []
    // Outputs by name: those captured, those whose pipe has closed, and those
    // whose dim is up. Reading again (Ctrl+R) leaves the dims up but clear,
    // and the pointer over them: Hyprland would hand the keyboard to the
    // window under a surface that went.
    property var captured: []
    property var finished: []
    property var shown: []
    // Ctrl+R asked for the screen to be read again, and wl-find.nu has not
    // yet sent the pipes it will come down.
    property bool rereading: false
    readonly property bool reading: rereading || finished.length < reads.length
    // Enter came before the OCR was done: submit once it is.
    property bool submitted: false
    // What wl-find.nu does with the match: click, right (Ctrl+Enter), copy
    // its text (Ctrl+Y), or open it (Ctrl+O). copyAlways is wl-find --copy:
    // Enter copies too.
    property string action: "click"
    property bool copyAlways: false
    // The find came with a pattern: Enter is pressed once the screen is read,
    // until the pattern is edited.
    property bool autoSubmit: false
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
    // The match Tab picked, kept by what it is: more lines may come before
    // it, and the boxes are new each time the pattern is searched for.
    property var currentBox: null
    readonly property int current: matches.findIndex(b => same(b, currentBox))
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

    // Text as find() compares it: compatibility forms folded (the "ﬁ" the
    // recogniser reads off a ligature is "fi"), and curly quotes, primes and
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
    // Case goes as in Neovim with 'ignorecase' and 'smartcase'. The boxes go
    // in the order Tab takes them: output by output, as in `reads`, and on
    // each as it is read, a row at a time.
    function find(query, lines) {
        if (query.trim() === "") return { boxes: [], bad: false };
        const boxes = [];
        try {
            const pattern = VimRegex.compile(fold(query));
            for (const line of lines) {
                const words = line.words.map(w => ({ text: fold(w.text), x: w.x, y: w.y, w: w.w, h: w.h }));
                const text = words.map(w => w.text).join(" ");
                for (let at = 0, match; (match = pattern.search(text, at)) !== null; ) {
                    if (match.end > match.start) boxes.push(box(line.output, words, match, text));
                    at = Math.max(match.end, at + 1);
                }
            }
        } catch (e) {
            return { boxes: [], bad: true };
        }
        const rank = b => reads.findIndex(r => r.output === b.output);
        // On one row when their middles are less than half a line apart.
        boxes.sort((a, b) => rank(a) - rank(b)
            || (Math.abs(a.y + a.h / 2 - b.y - b.h / 2) < Math.min(a.h, b.h) / 2 ? a.x - b.x : a.y - b.y));
        return { boxes: boxes, bad: false };
    }

    function same(a, b) {
        return !!a && !!b && a.output === b.output && a.x === b.x && a.y === b.y && a.text === b.text;
    }

    // The box of a match in a line's words, given as offsets into them joined
    // (`text`), with the text matched, as find() compares it, and the output
    // the line is on.
    function box(output, words, match, text) {
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
        return { output: output, x: x, y: y, w: right - x, h: bottom - y, text: text.slice(match.start, match.end) };
    }

    // `act` is the action to take on the match. One picked with Tab is taken
    // at once, wherever the reading is. Copying and opening pick one of
    // several matches by hints that need no pointer to click with (wl-kbptr
    // --only-print), so they have them everywhere; hints are on one output,
    // so matches on several have to be picked with Tab.
    function submit(act) {
        if (copyAlways && (act === "click" || act === "right")) act = "copy";
        action = act;
        if (input.text.trim() === "") {
            close([]);
        } else if (current >= 0) {
            close([matches[current]]);
        } else if (reading) {
            submitted = true;
        } else if (matches.length === 1) {
            close(matches);
        } else if (matches.length > 1 && (hints || act === "copy" || act === "open")
            && matches.every(b => b.output === matches[0].output)) {
            close(matches);
        } else if (matches.length > 1) {
            narrow = true;
        }
    }

    // Tab and Shift+Tab: the next match, or the one before, round and round.
    // Picking one takes back an Enter that waited for the reading.
    function step(by) {
        const n = matches.length;
        if (n === 0) return;
        autoSubmit = false;
        submitted = false;
        narrow = false;
        currentBox = matches[current < 0 ? (by > 0 ? 0 : n - 1) : (current + by + n) % n];
    }

    function stopReading() {
        for (let i = 0; i < readers.instances.length; i++) readers.instances[i].running = false;
    }

    // Hides the bar and leaves the boxes in resultPath, where wl-find.nu waits
    // for them: renamed into place a moment later, once the windows are off
    // the screen it may click.
    function close(boxes) {
        active = false;
        stopReading();
        reads = [];
        Quickshell.execDetached([
            "@sh@", "-c", '@coreutils@/sleep 0.03 && printf %s "$1" > "$2.part" && @coreutils@/mv "$2.part" "$2"',
            "sh", JSON.stringify({ boxes: boxes, action: action }), resultPath
        ]);
    }

    // Ctrl+R: the screen read again, the pattern kept. The dims are cleared,
    // and once they are clear on the screen, wl-find.nu is asked for another
    // reading; it stops the one going, and sends the pipes of the new one
    // (readAgain). Not while a capture is still to come.
    function reread() {
        if (!active || rereading || !reads.every(r => captured.includes(r.output) || finished.includes(r.output)))
            return;
        stopReading();
        rereading = true;
        reads = [];
        captured = [];
        finished = [];
        lines = [];
        currentBox = null;
        narrow = false;
        Quickshell.execDetached(["@sh@", "-c", '@coreutils@/sleep 0.03 && @coreutils@/touch "$1"', "sh", rereadPath]);
    }

    function readAgain(message) {
        if (!active || !rereading || message.result !== resultPath) return;
        reads = message.reads;
        rereading = false;
    }

    function isRead(read) {
        return active && reads.some(r => r.lines === read.lines);
    }

    // A batch of lines from `read`'s pipe: the first, empty or not, says its
    // output is captured.
    function take(read, data) {
        if (!isRead(read)) return;
        const home = read.output === screenName;
        const batch = JSON.parse(data)
            .map(words => home ? words.filter(w => !overBar(w)) : words)
            .filter(words => words.length > 0)
            .map(words => ({ output: read.output, words: words }));
        if (batch.length > 0) lines = lines.concat(batch);
        if (!captured.includes(read.output)) captured = captured.concat([read.output]);
        if (!shown.includes(read.output)) shown = shown.concat([read.output]);
    }

    // `read`'s pipe closed: its OCR is done, or its capture failed. With no
    // output captured there is nothing to find.
    function finish(read) {
        if (!isRead(read)) return;
        if (!finished.includes(read.output)) finished = finished.concat([read.output]);
        if (finished.length === reads.length && captured.length === 0 && !rereading) close([]);
    }

    function screenNamed(name) {
        for (let i = 0; i < Quickshell.screens.length; i++)
            if (Quickshell.screens[i].name === name) return Quickshell.screens[i];
        return null;
    }

    // A find, as wl-find.nu's message has it: on output `screen`, with or
    // without `hints`, reading the outputs of `reads` down their pipes; the
    // boxes chosen go to `result`, and Ctrl+R makes `reread`. `query` is
    // typed in already, and submitted once the screen is read. With `copy`,
    // the text of the match is copied rather than clicked. One still open is
    // closed with none.
    function open(message) {
        if (active) close([]);
        reads = [];
        lines = [];
        captured = [];
        finished = [];
        shown = [];
        rereading = false;
        currentBox = null;
        copyAlways = message.copy;
        action = message.copy ? "copy" : "click";
        hints = message.hints;
        narrow = false;
        resultPath = message.result;
        rereadPath = message.reread;
        screenName = message.screen;
        input.text = message.query;
        // After the text: typing it in is not an edit.
        autoSubmit = message.query.trim() !== "";
        submitted = autoSubmit;
        const output = screenNamed(message.screen);
        cheatsheet.screen = output;
        bar.screen = output;
        active = true;
        reads = message.reads;
        input.forceActiveFocus();
    }

    // Later, not inside the change: submitting may close, and that changes
    // `reading` again.
    onReadingChanged: if (active && submitted && !reading) Qt.callLater(() => {
        if (!active || !submitted || reading) return;
        submitted = false;
        submit(action);
    })

    // wl-find.nu talks to the bar with lines of JSON down this socket, named
    // for this file's directory, one per build: { kind: "open", ... } opens a
    // find, and { kind: "read", result, reads } sends the pipes of the
    // reading Ctrl+R asked for. Not `qs ipc`: that is a Qt program of its
    // own, ~60 ms to start before the bar could.
    SocketServer {
        active: true
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/wl-find-"
            + (Quickshell.env("WL_FIND_QML") || "/wl-find/").split("/").slice(-2)[0] + ".sock"
        handler: Socket {
            parser: SplitParser {
                onRead: line => {
                    const message = JSON.parse(line);
                    if (message.kind === "open") root.open(message);
                    else if (message.kind === "read") root.readAgain(message);
                }
            }
        }
    }

    // A reader per output searched, one JSON line per batch of lines. The
    // pipe closes when the OCR is done, or unwritten when the capture failed.
    Variants {
        id: readers
        model: root.reads

        Process {
            required property var modelData
            running: true
            command: ["@coreutils@/cat", modelData.lines]
            stdout: SplitParser {
                onRead: data => root.take(modelData, data)
            }
            onExited: root.finish(modelData)
        }
    }

    // An SVG path around a rectangle with rounded corners.
    function rounded(b, r) {
        return `M${b.x + r} ${b.y}h${b.width - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}`
            + `v${b.height - 2 * r}a${r} ${r} 0 0 1 ${-r} ${r}`
            + `h${-(b.width - 2 * r)}a${r} ${r} 0 0 1 ${-r} ${-r}`
            + `v${-(b.height - 2 * r)}a${r} ${r} 0 0 1 ${r} ${-r}Z`;
    }

    // The dims, one per output, each with a hole over each match on it
    // (even-odd fill). A dim maps after the bar, so it may be stacked over
    // it: a hole in the bar's shape keeps the bar clear wherever the
    // compositor puts it, and the cheat sheet too. While its output is read
    // again, a dim stays up but draws nothing.
    Variants {
        model: Quickshell.screens

        PanelWindow {
            id: dim
            required property var modelData
            // The bar's output.
            readonly property bool home: modelData.name === root.screenName
            readonly property bool lit: root.captured.includes(modelData.name)
            readonly property var here: lit ? root.matches.filter(b => b.output === modelData.name) : []
            screen: modelData
            visible: root.active && root.shown.includes(modelData.name)
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
                visible: dim.lit
                ShapePath {
                    fillColor: "@dim@"
                    strokeWidth: -1
                    fillRule: ShapePath.OddEvenFill
                    PathSvg {
                        path: {
                            let d = `M0 0H${dim.width}V${dim.height}H0Z`;
                            if (dim.home) {
                                d += root.rounded(root.barRect, barBox.radius);
                                if (cheatsheet.visible) d += root.rounded(root.cheatsheetRect, barBox.radius);
                            }
                            for (const b of dim.here)
                                d += `M${b.x - 3} ${b.y - 2}h${b.w + 6}v${b.h + 4}h${-(b.w + 6)}Z`;
                            return d;
                        }
                    }
                }
            }

            // The match Tab picked stands out, the others faded.
            Repeater {
                model: dim.here
                Rectangle {
                    required property var modelData
                    readonly property bool picked: root.same(modelData, root.currentBox)
                    x: modelData.x - 3
                    y: modelData.y - 2
                    width: modelData.w + 6
                    height: modelData.h + 4
                    radius: 3
                    color: "transparent"
                    opacity: picked || root.current < 0 ? 1 : 0.4
                    border.width: picked ? 3 : 2
                    border.color: "@accent@"
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: mouse => {
                    if (!(dim.home && root.overBar({ x: mouse.x, y: mouse.y, w: 0, h: 0 }))) root.close([]);
                }
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
                            const ctrl = event.modifiers & Qt.ControlModifier;
                            if (ctrl && event.key === Qt.Key_H) {
                                root.cheatsheetWanted = !root.cheatsheetWanted;
                            } else if (ctrl && event.key === Qt.Key_Y) {
                                // Not the TextInput's own redo.
                                root.submit("copy");
                            } else if (ctrl && event.key === Qt.Key_O) {
                                root.submit("open");
                            } else if (ctrl && event.key === Qt.Key_R) {
                                root.reread();
                            } else if (event.key === Qt.Key_Tab && !(event.modifiers & Qt.ShiftModifier)) {
                                // Not the move to the next item that takes focus.
                                root.step(1);
                            } else if (event.key === Qt.Key_Backtab || event.key === Qt.Key_Tab) {
                                root.step(-1);
                            } else {
                                return;
                            }
                            event.accepted = true;
                        }
                        Keys.onEscapePressed: root.close([])
                        Keys.onReturnPressed: event => root.submit(event.modifiers & Qt.ControlModifier ? "right" : "click")
                        Keys.onEnterPressed: event => root.submit(event.modifiers & Qt.ControlModifier ? "right" : "click")
                        onTextChanged: {
                            root.narrow = false;
                            root.currentBox = null;
                            // Editing a pattern given on the command line is
                            // taking it back from the Enter that was coming.
                            if (root.autoSubmit) {
                                root.autoSubmit = false;
                                root.submitted = false;
                            }
                        }
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
                        if (root.narrow) return `${n} matches: Tab or type more`;
                        if (root.found.bad) return "bad pattern";
                        const count = root.current >= 0 ? `${root.current + 1} of ${n}`
                            : n === 0 ? "no match" : n === 1 ? "1 match" : `${n} matches`;
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
    // and find matches in it; while the screen is read again it stays up,
    // clear, as the dims do.
    PanelWindow {
        id: cheatsheet
        visible: root.active && root.shown.includes(root.screenName) && root.cheatsheetWanted
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
            opacity: root.captured.includes(root.screenName) ? 1 : 0
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
                        ] },
                        { title: "Keys", rows: [
                            ["Enter", "click the match"],
                            ["Ctrl+Enter", "right click the match"],
                            ["Ctrl+Y", "copy the match"],
                            ["Ctrl+O", "open the match"],
                            ["Tab Shift+Tab", "pick the next, the last"],
                            ["Ctrl+R", "read the screen again"]
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
