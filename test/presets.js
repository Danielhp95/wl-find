// Holds the --type presets of wl-find.nu to what a line of the screen should give,
// and to Neovim: every match of each pattern in each line has to be the one
// listed here, in vim-regex.js and in nvim's matchstrlist() alike.
//
//   node test/presets.js   (with nvim on PATH)
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "..", "vim-regex.js"), "utf8");
const { compile } = new Function(source.replace(".pragma library", "") + "; return { compile };")();

// The presets, as wl-find.nu has them: `const HINT_URL = r#'…'#`, a line each.
const presets = {};
for (const [, name, pattern] of fs.readFileSync(path.join(__dirname, "..", "wl-find.nu"), "utf8")
    .matchAll(/^const HINT_([A-Z]+) = r#'(.*)'#$/gm))
    presets[name.toLowerCase()] = pattern;
for (const name of ["url", "path"])
    if (!presets[name]) throw new Error(`no HINT_${name.toUpperCase()} in wl-find.nu`);

// Each line, and the matches it should give, left to right.
const expected = {
    url: [
        ["", []],
        ["nothing to open here", []],
        ["see https://example.com/a/b?x=1&y=2.", ["https://example.com/a/b?x=1&y=2"]],
        ["http://localhost:8080/path", ["http://localhost:8080/path"]],
        // Quotes and brackets around it are not part of it.
        ["\"http://localhost:8080/path\", then", ["http://localhost:8080/path"]],
        ["'https://a.b/c'", ["https://a.b/c"]],
        ["<https://a.b/c>", ["https://a.b/c"]],
        ["[https://a.b/c]", ["https://a.b/c"]],
        ["(see https://a.b/c)", ["https://a.b/c"]],
        // Nor is the punctuation that ends a sentence.
        ["go to https://a.b/c, or https://d.e/f. Then", ["https://a.b/c", "https://d.e/f"]],
        ["is it https://a.b/c?", ["https://a.b/c"]],
        ["https://a.b/c;", ["https://a.b/c"]],
        ["https://a.b/c:", ["https://a.b/c"]],
        // A ")" is, when it closes a "(" inside.
        ["(https://en.wikipedia.org/wiki/Foo_(bar)) and more", ["https://en.wikipedia.org/wiki/Foo_(bar)"]],
        ["https://en.wikipedia.org/wiki/Foo_(bar).", ["https://en.wikipedia.org/wiki/Foo_(bar)"]],
        ["https://a.com/(x)/foo", ["https://a.com/(x)/foo"]],
        ["https://a.com/x)", ["https://a.com/x"]],
        // Schemes, in any case.
        ["visit HTTPS://EXAMPLE.COM now, or ftp://files.x.org/pub/file.tar.gz;",
            ["HTTPS://EXAMPLE.COM", "ftp://files.x.org/pub/file.tar.gz"]],
        ["ssh://git@host/repo and git://host/repo.git", ["ssh://git@host/repo", "git://host/repo.git"]],
        ["file:///etc/passwd", ["file:///etc/passwd"]],
        ["git+https://a.b/c", ["https://a.b/c"]],
        ["https://a.b/c#frag https://d.e/f%20g", ["https://a.b/c#frag", "https://d.e/f%20g"]],
        // What is not one.
        ["https://", []],
        ["https://.", []],
        ["xhttps://a.b", []],
        ["git@github.com:user/repo.git", []],
        ["example.com/path", []],
        ["mailto:me@example.com", []],
    ],
    path: [
        ["", []],
        ["nothing to open here", []],
        ["Open src/main.rs, then ~/.config/nvim/init.lua: done", ["src/main.rs", "~/.config/nvim/init.lua"]],
        ["/usr/bin/env ./run.sh ../up/file.c", ["/usr/bin/env", "./run.sh", "../up/file.c"]],
        ["/etc/ and /usr/lib/", ["/etc", "/usr/lib"]],
        // Names with an extension, with no "/".
        ["Cargo.lock package.json", ["Cargo.lock", "package.json"]],
        [".gitignore and .bashrc", [".gitignore", ".bashrc"]],
        ["foo.tar.gz", ["foo.tar.gz"]],
        ["Open README.md.", ["README.md"]],
        // Quotes, brackets and "=" around it are not part of it.
        ["'src/main.rs'", ["src/main.rs"]],
        ["(see lib/foo.py)", ["lib/foo.py"]],
        ["[docs/readme.md]", ["docs/readme.md"]],
        ["<src/main.rs>", ["src/main.rs"]],
        ["--file=/tmp/x.log key=val.txt", ["/tmp/x.log", "val.txt"]],
        ["src/main.rs, src/lib.rs; src/a.rs!", ["src/main.rs", "src/lib.rs", "src/a.rs"]],
        // Compiler output keeps the position after the file's name or loses it.
        ["error in foo.rs:12:5: expected", ["foo.rs"]],
        ["src/foo.rs:12:5: error", ["src/foo.rs:12:5"]],
        ["src/foo.rs: error", ["src/foo.rs"]],
        ["PATH=/usr/bin:/usr/local/bin", ["/usr/bin:/usr/local/bin"]],
        ["$HOME/bin", ["$HOME/bin"]],
        ["https://a.b/c", ["https://a.b/c"]],
        // What is not one: a comment, a bare "/", a number.
        ["// a comment /* block */ x", []],
        ["/ ~/ // */ /*", []],
        ["pi is 3.14 or 2.", []],
        ["end. Next", []],
    ],
};

// Every match of the pattern in the line, as the bar finds them.
function ours(pattern, subject) {
    const regex = compile(pattern);
    const found = [];
    for (let at = 0, match; (match = regex.search(subject, at)) !== null; ) {
        if (match.end > match.start) found.push(subject.slice(match.start, match.end));
        at = Math.max(match.end, at + 1);
    }
    return found;
}

const cases = [];
for (const [name, lines] of Object.entries(expected))
    for (const [subject, want] of lines)
        cases.push({ name, pattern: presets[name], subject, want });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "presets-"));
const input = path.join(dir, "in.json"), output = path.join(dir, "out.json");
fs.writeFileSync(input, JSON.stringify(cases));
const nvim = spawnSync("nvim", ["--clean", "--headless", "-l", path.join(__dirname, "nvim-matches.lua"), input, output],
    { stdio: "inherit", timeout: 120000 });
if (nvim.status !== 0) throw new Error(`nvim exited with ${nvim.status}`);
const fromNvim = JSON.parse(fs.readFileSync(output, "utf8"));
fs.rmSync(dir, { recursive: true });

let failed = 0;
cases.forEach((c, i) => {
    const here = ours(c.pattern, c.subject);
    // nvim's empty list comes back as {}.
    const there = Array.isArray(fromNvim[i]) ? fromNvim[i] : [];
    for (const [who, got] of [["here", here], ["nvim", there]]) {
        if (JSON.stringify(got) === JSON.stringify(c.want)) continue;
        failed++;
        console.log(`${c.name} on ${JSON.stringify(c.subject)}: want ${JSON.stringify(c.want)}, ${who} ${JSON.stringify(got)}`);
    }
});
console.log(`${cases.length * 2 - failed}/${cases.length * 2} as wanted (${cases.length} lines, here and in nvim)`);
if (failed) process.exit(1);
