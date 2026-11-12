// wl-find-ocr reads a small screen right: test/ocr-fixture.ppm.gz, a dark
// terminal over a light page at scale 2, holds the lines below, and each must
// come out as it is written, every word in its place.
//
//   node test/ocr.js            # wl-find-ocr on PATH, or WL_FIND_OCR
//   node test/ocr.js --render   # the fixture again, with chromium: prints the
//                               # words and their boxes, to paste in below
const { spawn, spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const zlib = require("node:zlib");

const fixture = path.join(__dirname, "ocr-fixture.ppm.gz");
const scale = 2;

// The lines of the fixture, a word [text, x, y, width, height] in logical
// pixels, as Chromium laid them out (--render). Between them they hold what
// OCR gets wrong on a screen: a capital O beside letters, which reads as a
// zero; letters that differ from their capitals by size alone; URLs; and
// punctuation that hangs off the ends of words.
const lines = [
    // The terminal: JetBrains Mono, light on dark.
    [["~/Projects", 12, 10.5, 88, 19.5], [">", 108.8, 10.5, 8.8, 19.5], ["git", 126.4, 10.5, 26.4, 19.5], ["clone", 161.6, 10.5, 44, 19.5], ["https://github.com/NixOS/nixpkgs.git", 214.4, 10.5, 316.8, 19.5]],
    [["warning:", 12, 31, 70.4, 19.5], ["unused", 91.2, 31, 52.8, 19.5], ["variable", 152.8, 31, 70.4, 19.5], ["`xOffset`", 232, 31, 79.2, 19.5], ["in", 320, 31, 17.6, 19.5], ["src/main.rs:12:5", 346.4, 31, 140.8, 19.5]],
    [["[INFO]", 12, 51.6, 52.8, 19.5], ["Listening", 73.6, 51.6, 79.2, 19.5], ["on", 161.6, 51.6, 17.6, 19.5], ["http://127.0.0.1:8080/api/v1/users?limit=50", 188, 51.6, 378.3, 19.5]],
    // The page: Adwaita Sans, dark on light.
    [["Sign", 12, 94, 30.6, 17.5], ["in", 46.7, 94, 12.2, 17.5], ["with", 63.1, 94, 29, 17.5], ["GitHub", 96.2, 94, 47.8, 17.5]],
    [["Written", 12, 117.5, 49.8, 17.5], ["by", 66, 117.5, 16.9, 17.5], ["Oscar", 87, 117.5, 41.2, 17.5], ["Cox,", 132.4, 117.5, 31.4, 17.5], ["Zoe", 167.9, 117.5, 26.6, 17.5], ["Vasquez", 198.6, 117.5, 59.4, 17.5], ["and", 262.2, 117.5, 25.9, 17.5], ["Xavier", 292.2, 117.5, 43.1, 17.5], ["on", 339.3, 117.5, 17.5, 17.5], ["macOS", 360.9, 117.5, 50.1, 17.5]],
    [["Visit", 12, 140.9, 29.8, 17.5], ["https://docs.example.org/v1/api?limit=20&sort=desc#Overview", 45.9, 140.9, 438.3, 17.5], ["now", 488.3, 140.9, 29.2, 17.5]],
    [["Copyright", 12, 164.4, 68.1, 17.5], ["2026", 84.2, 164.4, 36.2, 17.5], ["Example", 124.6, 164.4, 59.3, 17.5], ["Corp.", 188, 164.4, 38.5, 17.5], ["Contact", 230.6, 164.4, 54.5, 17.5], ["support@example.com", 289.1, 164.4, 160.8, 17.5]],
];

function page() {
    const esc = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    const line = words => `<div data-line>${words.map(w => `<span>${esc(w[0])}</span>`).join(" ")}</div>`;
    return `<!doctype html><html><head><meta charset="utf-8"><title></title><style>
html,body{margin:0;width:640px;height:200px;overflow:hidden}
body{font-size:14.67px}
#term{background:#1c1b19;color:#d8d0c0;font-family:"JetBrainsMono Nerd Font Mono",monospace;height:61.5px;padding:10px 12px;white-space:pre;line-height:1.4}
#page{background:#fff;color:#1f2328;font-family:"Adwaita Sans",sans-serif;height:98.5px;padding:10px 12px;line-height:1.6}
</style></head><body><div id="term">${lines.slice(0, 3).map(line).join("")}</div><div id="page">${lines.slice(3).map(line).join("")}</div>
<script>
document.title = JSON.stringify([...document.querySelectorAll("[data-line]")].map(l => [...l.querySelectorAll("span")].map(s => {
    const r = s.getBoundingClientRect(), n = v => Math.round(v * 10) / 10;
    return [s.textContent, n(r.x), n(r.y), n(r.width), n(r.height)];
})));
</script></body></html>`;
}

// The fixture, rendered again: its words go to stdout as the list above.
function render() {
    const { ppm, browser } = require("./ocr-bench.js");
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ocr-fixture-"));
    const html = path.join(dir, "fixture.html"), png = path.join(dir, "fixture.png");
    fs.writeFileSync(html, page());
    const flags = ["--headless=new", "--disable-gpu", "--hide-scrollbars", `--force-device-scale-factor=${scale}`, "--window-size=640,200"];
    const dom = spawnSync(browser, [...flags, "--dump-dom", `file://${html}`], { encoding: "utf8", timeout: 60000 });
    const title = dom.stdout.match(/<title>(.*?)<\/title>/s);
    if (!title) throw new Error(`${browser} dumped no page: ${dom.stderr}`);
    const unescape = s => s.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&amp;/g, "&");
    spawnSync(browser, [...flags, `--screenshot=${png}`, `file://${html}`], { encoding: "utf8", timeout: 60000 });
    fs.writeFileSync(fixture, zlib.gzipSync(ppm(fs.readFileSync(png)), { level: 9 }));
    fs.rmSync(dir, { recursive: true });
    for (const words of JSON.parse(unescape(title[1]))) console.log(`    ${JSON.stringify(words).replace(/\],\[/g, "], [").replace(/","?/g, m => m === '",' ? '", ' : m).replace(/,(\d)/g, ", $1")},`);
}

// The lines of text in wl-find-ocr's output, in order of their text.
const texts = stdout => stdout.split("\n").filter(Boolean).flatMap(l => JSON.parse(l))
    .map(words => words.map(w => w.text).join(" ")).sort();

// --after FILE, as wl-find reads the outputs after the first: "[]" comes out
// as soon as the image is in, then nothing until FILE is made, and then the
// same lines as without it.
async function waits(command, image, want) {
    const go = path.join(path.dirname(image), "go");
    const child = spawn(command, ["--scale", String(scale), "--after", go, image]);
    let out = "", err = "";
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", d => out += d);
    child.stderr.on("data", d => err += d);
    const closed = new Promise(resolve => child.on("close", resolve));
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    for (let t = 0; out === "" && child.exitCode === null && t < 200; t++) await sleep(50);
    if (out === "") {
        child.kill();
        return [`with --after, nothing came out: ${err.trim()}`];
    }
    if (out !== "[]\n") {
        child.kill();
        return [`with --after, ${JSON.stringify(out.slice(0, 60))} came out before the file was made, not []`];
    }
    await sleep(500);
    if (out !== "[]\n") {
        child.kill();
        return ["with --after, it read before the file was made"];
    }
    fs.writeFileSync(go, "");
    const status = await closed;
    if (status !== 0) return [`with --after, it failed (${status})`];
    return JSON.stringify(texts(out)) === JSON.stringify(want) ? [] : ["with --after, it read other lines than without"];
}

async function test() {
    const command = process.env.WL_FIND_OCR || "wl-find-ocr";
    const image = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "ocr-test-")), "fixture.ppm");
    fs.writeFileSync(image, zlib.gunzipSync(fs.readFileSync(fixture)));
    const run = spawnSync(command, ["--scale", String(scale), image], { encoding: "utf8", maxBuffer: 1 << 26 });
    const failures = [];
    if (run.error) failures.push(`${command} did not run: ${run.error.message}`);
    else if (run.status !== 0) failures.push(`${command} failed (${run.status}): ${run.stderr}`);
    const batches = (run.stdout || "").split("\n").filter(Boolean).map(l => JSON.parse(l));

    // The first line says only that the screen is in: the find bar dims the
    // screen on it, before anything is read.
    if (batches.length > 0 && JSON.stringify(batches[0]) !== "[]") failures.push(`the first line out is ${JSON.stringify(batches[0]).slice(0, 60)}, not []`);

    const read = batches.flat();
    const text = words => words.map(w => w.text).join(" ");
    for (const want of lines) {
        const wanted = want.map(w => w[0]).join(" ");
        // The line read at the height of this one, and furthest along it.
        const middle = want[0][2] + want[0][4] / 2;
        const got = read.filter(l => l.length > 0 && l[0].y < middle && l[0].y + l[0].h > middle).sort((a, b) => text(b).length - text(a).length)[0];
        if (!got) {
            failures.push(`not read at all: ${wanted}`);
            continue;
        }
        if (text(got) !== wanted) {
            failures.push(`read wrong:\n    want ${wanted}\n    got  ${text(got)}`);
            continue;
        }
        // Each word where the browser drew it: its middle inside the line,
        // and its sides within 4 pixels. The browser's box is the room the
        // word takes up, and the one read the ink in it, which starts and
        // ends up to half a narrow letter (a ":", a "`") further in.
        for (let i = 0; i < want.length; i++) {
            const [word, x, y, w, h] = want[i], g = got[i];
            const off = Math.max(Math.abs(g.x - x), Math.abs(g.x + g.w - (x + w)));
            const inLine = g.y + g.h / 2 > y && g.y + g.h / 2 < y + h && g.h > h * 0.4 && g.h < h * 1.6;
            if (off > 4 || !inLine)
                failures.push(`misplaced: "${word}" is at ${x},${y} ${w}x${h}, read at ${g.x},${g.y} ${g.w}x${g.h}`);
        }
    }
    if (run.status === 0) failures.push(...await waits(command, image, texts(run.stdout)));
    fs.rmSync(path.dirname(image), { recursive: true });
    for (const f of failures) console.error(f);
    console.error(failures.length === 0 ? `ok: ${lines.length} lines read right` : `${failures.length} failures`);
    process.exit(failures.length === 0 ? 0 : 1);
}

if (process.argv.includes("--render")) render();
else test();
