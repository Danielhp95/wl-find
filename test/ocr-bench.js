// How well wl-find reads a screen: three made-up screens, rendered by headless
// Chromium at scale 1 and 2, read by `wl-find read`, and scored against the
// text they hold. A benchmark rather than a test: OCR moves with the models,
// ONNX Runtime's version and the fonts at hand, so it reports, and fails on
// nothing (test/ocr.js is the test, of a screen kept as it was rendered).
//
//   nix shell .#default nixpkgs#chromium -c node test/ocr-bench.js [-v]
//
// For each screen and scale: the words read exactly, those read but for their
// case, the URLs that --type url finds whole, the character error rate of the
// lines (each against the line read closest to it, so none where the lines
// repeat), and the time `wl-find read` took. -v lists what was missed.
//
// WL_FIND is the command to read with, wl-find by default; with nu and the
// package's tools on PATH, `WL_FIND="nu wl-find.nu"` reads with the working
// copy's script.
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const zlib = require("node:zlib");

const verbose = process.argv.includes("-v");
const wlFind = (process.env.WL_FIND || "wl-find").split(" ");
const browser = ["chromium", "chromium-browser", "google-chrome-stable"]
    .find(b => spawnSync("sh", ["-c", `command -v ${b}`]).status === 0);
if (!browser) throw new Error("no chromium on PATH");

const source = fs.readFileSync(path.join(__dirname, "..", "vim-regex.js"), "utf8");
const { compile } = new Function(source.replace(".pragma library", "") + "; return { compile };")();
const url = fs.readFileSync(path.join(__dirname, "..", "wl-find.nu"), "utf8").match(/^const HINT_URL = r#'(.*)'#$/m)[1];

const esc = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
const page = (style, body) => `<!doctype html><html><head><meta charset="utf-8"><style>
html,body{margin:0;width:1920px;height:1200px;overflow:hidden}${style}</style></head><body>${body}</body></html>`;

// Each screen: its page, and the lines of text on it to score.
const screens = {
    // A dark terminal beside a browser: a page, its tab and address bars, a
    // dark side bar, a code block, a button and a chat.
    desktop() {
        const term = [
            "~/Projects/wl-find main* > git clone https://github.com/Danielhp95/wl-find.git",
            "Cloning into 'wl-find'...",
            "remote: Enumerating objects: 142, done.",
            "~/Projects > curl -fsSL https://raw.githubusercontent.com/NixOS/nixpkgs/master/README.md",
            "[INFO] Listening on http://127.0.0.1:8080/api/v1/users?limit=50",
            "[WARN] Retrying https://registry.npmjs.org/@types/node in 3s",
            "src/main.rs:12:5: error[E0308]: mismatched types",
            "  at src/lib/OcrReader.rs:48:17",
            "warning: unused variable `xOffset` in WaylandSurface::commit",
            "~/Projects > nix run github:NixOS/nixpkgs#hello",
            "~/Projects > ssh git@github.com:Danielhp95/nix_config.git",
            "Docs: https://docs.rs/tokio/1.41.0/tokio/sync/struct.Mutex.html",
            "See https://en.wikipedia.org/wiki/Optical_character_recognition for more",
            "~/Projects > ls ~/.config/hypr/hyprland.lua /etc/nixos/configuration.nix",
            "PostgreSQL 16.4 on x86_64-pc-linux-gnu, compiled by GCC 13.2.0",
            "Connected to redis://cache.internal:6379/0 as CacheWorker",
            "~/Projects > kubectl get pods -n KubeSystem --context=prod-EU-west",
            "Pulled ghcr.io/OpenWebUI/open-webui:v0.6.5 in 12.4s",
            "Visit https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s",
            "ftp://ftp.gnu.org/gnu/coreutils/coreutils-9.5.tar.xz",
        ];
        const comments = [
            "# TODO(Oscar): handle the XWayland case in SwayIPC",
            "# see https://gitlab.freedesktop.org/wayland/wayland-protocols/-/issues/57",
        ];
        const address = "https://github.com/quickshell-mirror/quickshell/blob/master/src/wayland/WlrLayershell.cpp";
        const tabs = ["GitHub - quickshell", "Vercel Dashboard", "Zoom Meeting", "Wikipedia"];
        const heading = "Configuring Hyprland with Quickshell and Waybar";
        const prose = [
            "Written by Oskar Wojcik, Zoe Vasquez and Xavier McAllister on Sunday.",
            "Install the package from https://search.nixos.org/packages?query=quickshell first,",
            "then read the CHANGELOG at https://git.outfoxxed.me/quickshell/quickshell/src/branch/master.",
            "Both SvelteKit and VSCode work, as do Visual Studio, Xcode and WebStorm.",
            "Companies like Vercel, Cloudflare and Overleaf sponsor it.",
            "Ask Sven O'Connor or Wesley Cox in #wayland on irc://irc.libera.chat/wayland",
        ];
        const code = [
            "const socket = new WebSocket(\"wss://api.example.com/v2/stream\");",
            "fetch(\"https://api.github.com/repos/Danielhp95/wl-find/issues?state=open\")",
            "let cfg = OcrConfig::default().with_scale(2.0);",
        ];
        const side = ["Overview", "Issues 12", "Pull requests 3", "Actions", "Security", "Insights", "Settings"];
        const chat = [
            ["Vivian Oxley", "the build is at https://hydra.nixos.org/build/281234567/log"],
            ["Zachary Stone", "Works on my ThinkPad X1 Carbon with Kernel 6.11"],
            ["Owen Cooper", "PR: https://github.com/hyprwm/Hyprland/pull/8123#issuecomment-2400111"],
            ["Scott Wilcox", "use wl-kbptr --only-print, not wlrctl"],
        ];
        const footer = "Copyright 2026 Example Corp. Contact support@example.com or https://example.com/contact";
        const grey = "Last edited 3 hours ago by Victor Zavala";
        return {
            lines: [...term, ...comments, address, ...tabs, heading, ...prose, ...code, "Sign in with GitHub", ...side,
                ...chat.flat(), footer, grey],
            html: page(`body{background:#11100f;display:grid;grid-template-columns:900px 1020px;grid-template-rows:30px 1170px;font-family:"Adwaita Sans";font-size:14.67px}
#bar{grid-column:1/3;background:#1c1b19;color:#d8d0c0;display:flex;align-items:center;gap:24px;padding:0 12px;font-size:13px}
#term{background:#1c1b19;color:#d8d0c0;font-family:"JetBrainsMono Nerd Font Mono",monospace;font-size:14.67px;line-height:117%;padding:8px 10px;white-space:pre}
#term span{color:#7a7368}
#web{background:#fff;color:#1f2328;display:grid;grid-template-rows:36px 40px 1fr}
#tabs{background:#dee1e6;display:flex;gap:2px;padding:6px 8px 0}
#tabs div{background:#eef0f3;padding:6px 14px;border-radius:8px 8px 0 0;font-size:12px}
#addr{display:flex;align-items:center;padding:0 12px}
#addr div{background:#f1f3f4;border-radius:16px;padding:6px 14px;width:100%;font-size:14px}
#page{display:grid;grid-template-columns:200px 1fr}
#side{background:#24292f;color:#f0f6fc;padding:16px;font-size:14px;line-height:2.1}
#main{padding:18px 26px;font-size:16px;line-height:1.5}
h1{font-size:26px;margin:0 0 10px} a{color:#0969da}
pre{background:#f6f8fa;font-family:"JetBrainsMono Nerd Font Mono",monospace;font-size:13px;padding:10px}
button{background:#1f883d;color:#fff;border:0;border-radius:6px;padding:8px 16px;font:inherit;font-size:14px}
.grey{color:#8c959f;font-size:13px;margin-top:8px}
#chat{margin-top:16px;background:#2b2d31;color:#dbdee1;padding:10px 14px;border-radius:8px;font-size:15px}
#chat b{color:#f2f3f5;display:block;margin-top:6px}
footer{margin-top:14px;font-size:12px;color:#57606a}`,
                `<div id="bar"><span>1 2 3 4</span><span>Firefox</span><span style="margin-left:auto">Tue 3 Nov 14:05</span></div>
<div id="term">${term.map(esc).join("\n")}\n<span>${comments.map(esc).join("\n")}</span></div>
<div id="web"><div id="tabs">${tabs.map(t => `<div>${esc(t)}</div>`).join("")}</div>
<div id="addr"><div>${esc(address)}</div></div>
<div id="page"><div id="side">${side.map(esc).join("<br>")}</div><div id="main"><h1>${esc(heading)}</h1>
${prose.map(p => `<div>${esc(p).replace(/(https?|irc):\/\/\S+[^\s.,]/g, m => `<a>${m}</a>`)}</div>`).join("")}
<pre>${code.map(esc).join("\n")}</pre><button>Sign in with GitHub</button><div class="grey">${esc(grey)}</div>
<div id="chat">${chat.map(([who, what]) => `<b>${esc(who)}</b>${esc(what)}`).join("")}</div>
<footer>${esc(footer)}</footer></div></div></div>`),
        };
    },

    // A light, low-contrast terminal, an editor with syntax colours, a light
    // chat, coloured labels, a serif page with underlined links, a status
    // line and a notification.
    mixed() {
        const term = [
            "$ xdg-open https://www.reddit.com/r/NixOS/comments/1g2x7kq/nixos_2411_released/",
            "$ git remote -v",
            "origin  git@gitlab.com:ZooKeeper/ClickHouse-Operator.git (fetch)",
            "upstream https://bitbucket.org/OpenSUSE/zypper-docker/src/main/ (fetch)",
            "$ pip install --index-url https://pypi.org/simple/ SQLAlchemy==2.0.36",
            "Downloading https://files.pythonhosted.org/packages/a1/B2/sqlalchemy-2.0.36.tar.gz",
            "Successfully installed SQLAlchemy-2.0.36 MarkupSafe-3.0.2 Werkzeug-3.1.3",
            "$ open http://localhost:5173/settings/Profile?tab=Security#OAuth",
            "$ cat ~/.ssh/config | grep -i HostName",
            "  HostName vps-0x7f.example.net",
        ];
        const editor = [
            "// Fetches https://api.openai.com/v1/chat/completions with the WebSocket fallback",
            "import { ZodSchema } from \"zod\";",
            "const DOCS_URL = \"https://developer.mozilla.org/en-US/docs/Web/API/XMLHttpRequest\";",
            "export async function getVoxelOverlay(sceneId: string): Promise<VoxelOverlay> {",
            "  const res = await fetch(`https://cdn.jsdelivr.net/npm/three@0.169.0/examples/jsm/`);",
            "  return res.ok ? parseOverlay(await res.json()) : OVERLAY_FALLBACK;",
            "}",
            "// See https://github.com/vercel/next.js/blob/canary/packages/next/src/server/base-server.ts#L1520-L1544",
        ];
        const chat = [
            ["Ksenia Volkova", "Oluwaseun pushed the fix to https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/4021"],
            ["Wouter van Oss", "Does PowerShell or WezTerm handle OSC 8 links? Kitty and Ghostty do."],
            ["Xiaowen Zhou", "Slides: https://docs.google.com/presentation/d/1XyZ-AbCdEfGhIjKlMnOpQrStUvWx/edit#slide=id.p"],
            ["Siobhan O'Sullivan", "Meeting moved, see https://zoom.us/j/98765432109?pwd=SwXcVkOzP"],
        ];
        const labels = ["bug", "P0-critical", "macOS", "iPadOS", "WontFix", "needs-QA", "Kubernetes"];
        const colours = ["#d73a4a", "#b60205", "#5319e7", "#0075ca", "#6e7781", "#e99695", "#1d76db"];
        const serif = [
            "The SoundCloud, YouTube and TikTok embeds load from https://www.youtube-nocookie.com/embed/.",
            "PayPal, WhatsApp and LinkedIn were tested by Zbigniew Kowalski on Windows 11.",
        ];
        const links = ["DuckDuckGo", "CockroachDB docs", "OpenTofu registry", "https://registry.opentofu.org/providers/hashicorp/aws"];
        const status = ["NORMAL", "main", "src/components/VoxelOverlay.tsx", "utf-8", "TypeScript", "42:17"];
        const notice = ["Calendar", "Sync with Oscar and Zoe at 15:30 in Room 4B"];
        const syntax = l => /^\s*\/\//.test(l)
            ? `<span style="color:#6a9955">${esc(l)}</span>`
            : l.split(/("[^"]*"|`[^`]*`)/).map((part, i) => i % 2
                ? `<span style="color:#ce9178">${esc(part)}</span>`
                : esc(part).replace(/\b(import|from|const|export|async|function|return|await)\b/g, '<span style="color:#569cd6">$1</span>')).join("");
        return {
            lines: [...term, ...editor, ...chat.flat(), ...labels, ...serif, ...links, ...status, ...notice],
            html: page(`body{background:#e5e5e5;display:grid;grid-template-columns:760px 1160px;grid-template-rows:1176px 24px;font-family:"DejaVu Sans"}
#lt{background:#fdf6e3;color:#657b83;font-family:"DejaVu Sans Mono",monospace;font-size:13px;line-height:1.35;padding:8px;white-space:pre}
#right{display:grid;grid-template-rows:auto auto 1fr}
#ed{background:#1e1e1e;color:#9cdcfe;font-family:"JetBrainsMono Nerd Font Mono",monospace;font-size:13px;line-height:1.5;padding:10px 14px;white-space:pre}
#ed i{font-style:normal;color:#858585;display:inline-block;width:28px}
#mid{display:grid;grid-template-columns:800px 360px;background:#fff}
#chat{padding:12px;font-family:"Adwaita Sans";font-size:15px;line-height:1.4;color:#1d1c1d}
#chat b{display:block;margin-top:8px}
#lab{padding:12px;display:flex;flex-wrap:wrap;gap:6px;align-content:flex-start}
#lab span{font-family:"Adwaita Sans";font-size:12px;font-weight:600;color:#fff;padding:2px 8px;border-radius:10px}
#serif{padding:4px 14px;font-family:"DejaVu Serif";font-size:16px;color:#333;background:#faf8f5}
#links{padding:4px 14px 12px;background:#faf8f5;display:flex;gap:28px;font-size:14px}
#links a{color:#1a0dab;text-decoration:underline}
#notice{position:absolute;right:16px;bottom:60px;width:360px;background:#2e2e2e;color:#eee;border-radius:12px;padding:10px 14px;font-family:"Adwaita Sans";font-size:13px}
#notice b{display:block;font-size:14px}
#st{grid-column:1/3;background:#282c34;color:#abb2bf;font-family:"JetBrainsMono Nerd Font Mono",monospace;font-size:12px;display:flex;align-items:center;gap:16px}
#st span:first-child{background:#98c379;color:#282c34;font-weight:bold;padding:4px 8px}`,
                `<div id="lt">${term.map(esc).join("\n")}</div><div id="right">
<div id="ed">${editor.map((l, i) => `<i>${i + 1}</i>${syntax(l)}`).join("\n")}</div>
<div id="mid"><div id="chat">${chat.map(([a, b]) => `<b>${esc(a)}</b>${esc(b)}`).join("")}</div>
<div id="lab">${labels.map((l, i) => `<span style="background:${colours[i]}">${esc(l)}</span>`).join("")}</div></div>
<div><div id="serif">${serif.map(esc).join("<br>")}</div><div id="links">${links.map(l => `<a>${esc(l)}</a>`).join("")}</div></div></div>
<div id="st">${status.map(s => `<span>${esc(s)}</span>`).join("")}</div>
<div id="notice"><b>${esc(notice[0])}</b>${esc(notice[1])}</div>`),
        };
    },

    // The same three lines from 9 to 13 pixels, in the UI font and the
    // terminal one, on light and dark grounds.
    sizes() {
        const sample = [
            "Visit https://github.com/NixOS/nixpkgs/pull/351234 for SvelteKit and macOS",
            "Oscar McAllister, Zoe Vasquez, Xavier Owens: WebSocket OAuth ZooKeeper",
            "Sync https://docs.example.org/v1/api?limit=20&sort=desc#Overview now",
        ];
        const lines = [], blocks = [];
        for (const font of ["Adwaita Sans", "JetBrainsMono Nerd Font Mono"])
            for (const [bg, fg] of [["#ffffff", "#1f2328"], ["#1c1b19", "#d8d0c0"]]) {
                const rows = [];
                for (const size of [9, 10, 11, 12, 13])
                    for (const l of sample) {
                        rows.push(`<div style="font-size:${size}px">${esc(l)}</div>`);
                        lines.push(l);
                    }
                blocks.push(`<div style="background:${bg};color:${fg};font-family:'${font}';padding:6px;line-height:1.5">${rows.join("")}</div>`);
            }
        return { lines, html: page("body{background:#888;display:grid;grid-template-columns:1fr 1fr}", blocks.join("")) };
    },
};

// A PNG as a PPM: 8-bit RGB or RGBA, not interlaced, as Chromium writes them.
function ppm(png) {
    let width, height, channels;
    const idat = [];
    for (let at = 8; at < png.length; ) {
        const length = png.readUInt32BE(at), type = png.toString("ascii", at + 4, at + 8);
        const data = png.subarray(at + 8, at + 8 + length);
        if (type === "IHDR") {
            [width, height] = [data.readUInt32BE(0), data.readUInt32BE(4)];
            channels = { 2: 3, 6: 4 }[data[9]];
            if (data[8] !== 8 || !channels || data[12] !== 0) throw new Error("a PNG this cannot read");
        } else if (type === "IDAT") idat.push(data);
        at += 12 + length;
    }
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const stride = width * channels;
    const rgb = Buffer.alloc(width * height * 3);
    let previous = Buffer.alloc(stride);
    for (let y = 0; y < height; y++) {
        const filter = raw[y * (stride + 1)], line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
        const row = Buffer.alloc(stride);
        for (let x = 0; x < stride; x++) {
            const a = x >= channels ? row[x - channels] : 0, b = previous[x], c = x >= channels ? previous[x - channels] : 0;
            const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
            const predictor = [0, a, b, (a + b) >> 1, pa <= pb && pa <= pc ? a : pb <= pc ? b : c][filter];
            row[x] = (line[x] + predictor) & 0xff;
        }
        for (let x = 0; x < width; x++) row.copy(rgb, (y * width + x) * 3, x * channels, x * channels + 3);
        previous = row;
    }
    return Buffer.concat([Buffer.from(`P6\n${width} ${height}\n255\n`), rgb]);
}

const fold = t => t.normalize("NFKC")
    .replace(/[‘’‚‛′]/g, "'")
    .replace(/[“”„‟″]/g, '"')
    .replace(/[‐-―−]/g, "-");

function matches(subject) {
    const regex = compile(url), found = [];
    for (let at = 0, m; (m = regex.search(subject, at)) !== null; at = Math.max(m.end, at + 1))
        if (m.end > m.start) found.push(subject.slice(m.start, m.end));
    return found;
}

// How many of `want` are in `have`, each taken once, and those that are not.
function found(want, have) {
    const pool = new Map();
    for (const h of have) pool.set(h, (pool.get(h) || 0) + 1);
    const missed = want.filter(w => !(pool.get(w) > 0 && pool.set(w, pool.get(w) - 1)));
    return { count: want.length - missed.length, missed };
}

// The edits from `a` to the closest part of `b`.
function distance(a, b) {
    let previous = new Array(b.length + 1).fill(0);
    for (let i = 1; i <= a.length; i++) {
        const row = [i];
        for (let j = 1; j <= b.length; j++)
            row[j] = Math.min(previous[j] + 1, row[j - 1] + 1, previous[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
        previous = row;
    }
    return Math.min(...previous);
}

function main() {
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "ocr-bench-"));
const rows = [];
for (const [name, make] of Object.entries(screens)) {
    const screen = make();
    fs.writeFileSync(path.join(dir, `${name}.html`), screen.html);
    for (const scale of [1, 2]) {
        const png = path.join(dir, `${name}${scale}.png`), image = path.join(dir, `${name}${scale}.ppm`);
        const shot = spawnSync(browser, ["--headless=new", "--disable-gpu", "--hide-scrollbars", `--force-device-scale-factor=${scale}`,
            "--window-size=1920,1200", `--screenshot=${png}`, `file://${path.join(dir, `${name}.html`)}`], { encoding: "utf8", timeout: 60000 });
        if (!fs.existsSync(png)) throw new Error(`${browser} took no screenshot: ${shot.stderr}`);
        fs.writeFileSync(image, ppm(fs.readFileSync(png)));
        const start = Date.now();
        const read = spawnSync(wlFind[0], [...wlFind.slice(1), "read", image, "--scale", String(scale)], { encoding: "utf8", maxBuffer: 1 << 26 });
        const ms = Date.now() - start;
        if (read.status !== 0) throw new Error(`${wlFind.join(" ")} read failed: ${read.stderr}`);
        const lines = read.stdout.split("\n").map(fold);

        const words = found(screen.lines.flatMap(l => l.split(/\s+/).filter(Boolean)), lines.flatMap(l => l.split(" ")));
        const caseOnly = found(words.missed.map(w => w.toLowerCase()), lines.flatMap(l => l.split(" ")).map(w => w.toLowerCase())).count;
        const urls = found(screen.lines.flatMap(matches), lines.flatMap(matches));
        let errors = 0, length = 0;
        for (const l of screen.lines) {
            errors += Math.min(...lines.map(o => distance(l, o)));
            length += l.length;
        }
        const total = words.count + words.missed.length;
        const repeats = new Set(screen.lines).size < screen.lines.length;
        rows.push({ name, scale, words: `${words.count}/${total}`, caseOnly, urls: `${urls.count}/${urls.count + urls.missed.length}`,
            cer: repeats ? "—" : `${(100 * errors / length).toFixed(2)}%`, ms });
        if (verbose) {
            console.log(`${name} at scale ${scale}`);
            console.log(`  words missed: ${words.missed.join("  ")}`);
            console.log(`  URLs missed: ${urls.missed.join("  ")}`);
        }
    }
}
fs.rmSync(dir, { recursive: true });
console.table(rows);
}

// test/ocr.js renders its fixture with the same browser and PNG reader.
if (require.main === module) main();
else module.exports = { ppm, browser, screens };
