// wl-find's find bar. It stays loaded, and wl-find.nu opens it with a line
// down a socket, so the bar is up as soon as it is called. Once the screen
// is captured the output dims as wl-kbptr's hints do, and each match of the
// typed text is cut out of the dim and outlined. Enter leaves the matches for
// wl-find.nu to click, or, where there are no hints to pick one of several
// with, waits for a query with one match; Esc, a click anywhere, or Enter on
// an empty bar leave none. The @…@ colours, font and tools come from package.nix.
import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import Quickshell
import Quickshell.Io
import Quickshell.Wayland

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
    readonly property var matches: find(input.text, lines)
    // Where the bar sits on its output, in the output's logical pixels, as
    // the compositor centres it. It is up before the screen is captured, so
    // its own words may be read with the rest; overBar() drops them.
    readonly property rect barRect: bar.screen
        ? Qt.rect((bar.screen.width - bar.implicitWidth) / 2,
            bar.screen.height - bar.margins.bottom - bar.implicitHeight,
            bar.implicitWidth, bar.implicitHeight)
        : Qt.rect(0, 0, 0, 0)

    function overBar(b) {
        return b.x < barRect.x + barRect.width && b.x + b.w > barRect.x
            && b.y < barRect.y + barRect.height && b.y + b.h > barRect.y;
    }

    // Text as find() compares it: lower case, compatibility forms folded (the
    // "ﬁ" tesseract reads off a ligature is "fi"), and curly quotes, primes
    // and long dashes made the plain ones a keyboard types.
    function fold(text) {
        return text.normalize("NFKC").toLowerCase()
            .replace(/[\u2018\u2019\u201a\u201b\u2032]/g, "'")
            .replace(/[\u201c\u201d\u201e\u201f\u2033]/g, '"')
            .replace(/[\u2010-\u2015\u2212]/g, "-");
    }

    // Where the query occurs, one box per occurrence, as fold() compares
    // them. A query of several words spans as many words of a line: "ve as"
    // ends one and starts the next. A box covers only the matched letters,
    // sized by their share of the folded word.
    function find(query, lines) {
        const terms = fold(query).split(" ").filter(t => t !== "");
        const n = terms.length;
        if (n === 0) return [];
        const boxes = [];
        const part = (w, from, to) => ({
            x: w.x + w.w * from / w.text.length,
            right: w.x + w.w * to / w.text.length,
            y: w.y,
            bottom: w.y + w.h
        });
        const box = parts => {
            const x = Math.min(...parts.map(p => p.x));
            const y = Math.min(...parts.map(p => p.y));
            return {
                x: x,
                y: y,
                w: Math.max(...parts.map(p => p.right)) - x,
                h: Math.max(...parts.map(p => p.bottom)) - y
            };
        };
        for (const read of lines) {
            const line = read.map(w => ({ text: fold(w.text), x: w.x, y: w.y, w: w.w, h: w.h }));
            const texts = line.map(w => w.text);
            if (n === 1) {
                line.forEach((w, i) => {
                    for (let at = texts[i].indexOf(terms[0]); at !== -1; at = texts[i].indexOf(terms[0], at + terms[0].length))
                        boxes.push(box([part(w, at, at + terms[0].length)]));
                });
                continue;
            }
            for (let i = 0; i + n <= line.length; i++) {
                const words = texts.slice(i, i + n);
                if (!words[0].endsWith(terms[0]) || !words[n - 1].startsWith(terms[n - 1]))
                    continue;
                if (!words.slice(1, n - 1).every((w, k) => w === terms[k + 1]))
                    continue;
                const first = line[i];
                const last = line[i + n - 1];
                boxes.push(box([
                    part(first, first.text.length - terms[0].length, first.text.length),
                    ...line.slice(i + 1, i + n - 1).map(w => part(w, 0, w.text.length)),
                    part(last, 0, terms[n - 1].length)
                ]));
            }
        }
        return boxes;
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
        // On another output the bar is a new surface, which keeps the layer
        // it is made on: hidden while it moves, it is made raised.
        if (bar.screen !== output) {
            bar.visible = false;
            bar.screen = output;
            active = true;
            bar.visible = true;
        }
        active = true;
        input.forceActiveFocus();
    }

    onReadingChanged: if (active && submitted) submit()

    // wl-find.nu opens a find with a line down this socket, named for this
    // file: screen, hints, lines and result, between tabs. Not `qs ipc`: that
    // is a Qt program of its own, ~60 ms to start before the bar could.
    SocketServer {
        active: true
        path: Quickshell.env("XDG_RUNTIME_DIR") + "/wl-find-"
            + (Quickshell.env("WL_FIND_QML") || "wl-find.qml").split("/").pop() + ".sock"
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

    // The dim, with a hole over each match (even-odd fill). It maps after
    // the bar, so it may be stacked over it: a hole in the bar's shape keeps
    // the bar clear wherever the compositor puts it.
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
                        const r = barBox.radius;
                        const b = root.barRect;
                        d += `M${b.x + r} ${b.y}h${b.width - 2 * r}a${r} ${r} 0 0 1 ${r} ${r}`
                            + `v${b.height - 2 * r}a${r} ${r} 0 0 1 ${-r} ${r}`
                            + `h${-(b.width - 2 * r)}a${r} ${r} 0 0 1 ${-r} ${-r}`
                            + `v${-(b.height - 2 * r)}a${r} ${r} 0 0 1 ${r} ${-r}Z`;
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

    // The bar, up before the screen is captured (barRect). It stays mapped
    // between finds, parked: on the background layer, clear, and letting
    // clicks through, so a find only raises it. A new surface would take
    // ~40 ms to set up and draw first; one on another output still does.
    PanelWindow {
        id: bar
        anchors.bottom: true
        margins.bottom: 72
        implicitWidth: 560
        implicitHeight: 52
        exclusionMode: ExclusionMode.Ignore
        color: "transparent"
        WlrLayershell.layer: root.active ? WlrLayer.Overlay : WlrLayer.Background
        WlrLayershell.keyboardFocus: root.active ? WlrKeyboardFocus.Exclusive : WlrKeyboardFocus.None
        WlrLayershell.namespace: "wl-find-bar"
        mask: root.active ? null : parked

        Region {
            id: parked
        }

        Rectangle {
            id: barBox
            visible: root.active
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
                        const n = root.matches.length;
                        if (root.narrow) return `${n} matches: type more`;
                        const count = n === 0 ? "no match" : n === 1 ? "1 match" : `${n} matches`;
                        if (root.reading) return n === 0 || input.text.trim() === "" ? "reading…" : `${count}…`;
                        return input.text.trim() === "" ? "" : count;
                    }
                    color: root.narrow || (!root.reading && input.text.trim() !== "" && root.matches.length === 0) ? "@error@" : "@fgDim@"
                    font.family: "@font@"
                    font.pixelSize: 16
                }
            }
        }
    }
}
