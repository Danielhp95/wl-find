// Holds vim-regex.js against Neovim itself: every pattern here, on every
// subject, with and without 'ignorecase', has to give the same first match as
// matchstrpos() does in nvim, or both must refuse it.
//
//   node test/vim-regex.js   (with nvim on PATH)
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const source = fs.readFileSync(path.join(__dirname, "..", "vim-regex.js"), "utf8");
const { compile } = new Function(source.replace(".pragma library", "") + "; return { compile };")();

const subjects = [
    "", "foo", "Foo bar", "foo bar baz", "foobar", "barfoo", "foo foo", "FOO", "aaa", "aab", "abab",
    "ab", "abc", "abcabc", "a.b", "a+b", "(x)", "[x]", "{x}", "a|b", "x=1", "x==1", "a b", "a  b",
    " lead", "trail ", "\ttab", "file.txt", "/usr/bin/env", "foo_bar", "foo-bar", "fooBar", "CamelCase",
    "2026-10-30", "12:34", "0xff", "v1.2.3", "$HOME", "^x", "a^b", "a$b", "100%", "#tag", "a@b.co",
    "Save changes", "save", "don't", "\"q\"", "é", "café", "naïve Ñu", "ΑΒΓ αβγ", "abba", "xyzzy", "aXbXc",
    "foo(bar)", "x-y-z", "\\", "a\\b", "~", "<tag>", "a,b;c", "`x`", "'", "*", "**", "a*b", "?", "a?b",
    "=", "&", "a&b", "%", "@", "!", "-", "/", ":", "]", "[", "}", "{", "a]b", "tab\there", "\x1b", "\x7f"
];

const patterns = [
    // Literals and escapes.
    "foo", "Foo", "FOO", "foo bar", "a\\.b", "a\\+b", "\\(x\\)", "\\[x\\]", "\\{x\\}", "a\\|b", "x\\=1", "\\$HOME",
    "\\^x", "100\\%", "\\#tag", "a\\@b", "\\\\", "a\\\\b", "\\~", "\\<tag\\>", "\\*", "\\?", "\\&", "\\!", "\\-", "\\/",
    "\\:", "\\]", "\\}", "\\'", "\\\"", "\\`", "\\,", "\\;", "#tag", "a,b", "a;b", "!", "-", "/", ":", "'", "\"", "`",
    ",", ";", "#", "]", "}", "a]b", "a}b", "\\t", "tab\\there", "\\e", "\\r", "\\b", "\\n", "\\_s", "\\_.", "\\_d",
    // . and the character classes.
    ".", "..", "a.b", ".*", ".+", "^.*$", "\\d", "\\d+", "\\D+", "\\w+", "\\W+", "\\s", "\\S+", "\\a+", "\\A+", "\\l+",
    "\\L+", "\\u+", "\\U+", "\\h+", "\\H+", "\\x+", "\\X+", "\\o+", "\\O+", "\\i+", "\\I+", "\\k+", "\\K+", "\\f+",
    "\\F+", "\\p+", "\\P+", "\\d{4}-\\d{2}-\\d{2}", "\\u\\l+", "\\w+\\.\\w+", "\\_a+", "\\_u", "\\_[ab]", "\\_^a", "a\\_$",
    // Multis.
    "a*", "a+", "a=", "a?", "a{2}", "a{1,2}", "a{,2}", "a{2,}", "a{}", "a{-}", "a{-1,}", "a{-1,2}", "a{-2}", "a{-,2}",
    "a{2,1}", "a{-2,1}", "a{2\\}", "a{x}", "a{", "a{1", "ab*", "(ab)*", "(ab)+", "(ab)=", "(ab){2}", "(ab){-1,}", "a.{-}b",
    "a.*b", "a.*c", "a.{-}c", "(a|b)*", "(a|b)+c", "(a*)*", "(a*)+b", "(a|ab)(c|bcd)", ".{-}", ".{-}b", "a*a", "a*ab",
    "(a=)*", "(a*){2}", "(a{-})b", "x*", "x+", "x{0}", "b{0,0}c",
    // Alternation and concats.
    "a|b", "foo|bar", "bar|foo", "foo|foobar", "foobar|foo", "|a", "a|", "a||b", "(foo|bar)baz", "(|a)b", "(a|)b",
    ".*bar&foo.*", ".*foo&.*bar", "foo&bar", ".*a&.*b&.*c", "foo&", "&foo", "a&|b",
    // Groups and references.
    "(a)", "(a)(b)", "(a)\\1", "(.)\\1", "(.)(.)\\2\\1", "(a|b)\\1", "(a)|\\1b", "%(a)", "%(ab)+", "%(a|b)c", "(%(a)b)\\1",
    "(foo) \\1", "\\1", "(a)\\2", "(a\\1)", "(", ")", "(a", "a)", "%(", "%(a", "()", "%()", "()a", "(())", "(a)(b)(a)(b)(a)(b)(a)(b)(a)",
    "(a)(b)(a)(b)(a)(b)(a)(b)(a)(b)",
    // Anchors and word edges.
    "^", "$", "^foo", "foo$", "^foo$", "^$", "a^b", "a$b", "^^", "$$", "a|^b", "(^a)", "%(^a)", "a$|b", "(a$)", "^*", "^a*", "$a",
    "<foo", "foo>", "<foo>", "<", ">", "<>", "<\\w", "\\w>", "<bar", "bar>", "<\\u", "<.", ".>", "%^foo", "foo%$", "%^", "%$", "\\_^",
    "\\_$", "<*", ">+",
    // Lookaround.
    "foo(bar)@=", "foo(bar)@!", "(foo)@<=bar", "(foo)@<!bar", "(foo )@<=bar", "(a)@=a", "(a)@!b", "a(b)@!", "(a)@<=b", "(b)@<!a",
    "(^)@<=a", "(foo)@<=", "(foo)@=", ".*(bar)@=", "(a*)@>a", "(a+)@>b", "(a|ab)@>c", "(ab)@>", "\\d+(px)@!", "(\\$)@<=\\w+",
    "(foo)@3<=bar", "(foo)@2<=bar", "(foo)@3<!bar", "(.*)@<=b", "%(foo)@<=bar", "(a)@", "(a)@x", "(a)@<x", "(a)@<", "@=", "(a)@=*",
    "(a)@=+", "(a)@<=*", "a@=", "a@!b", "b@<=a", "a@<=b", "a@>a", "\\w@<!\\w+", "\\s@<=\\w+", "(\\w)@<=\\u",
    // \zs and \ze.
    "foo\\zsbar", "foo\\zebar", "foo\\zs", "\\zsfoo", "foo\\ze", "\\zefoo", "fo\\zso\\ze ", "a\\zsb\\zec", "(a\\zs)b", "(a\\ze)b",
    "a\\zs*", "a\\ze+", "\\zs", "\\ze", "a\\zsb|cd", "foo\\zs bar", "\\w+\\.\\zs\\w+", "\\zx", "\\z", "a\\zsb\\zsc", "a\\zeb\\zec", "(foo\\zs)@=f", "(foo\\ze)@=f", "(\\zef)@<=o",
    // Collections.
    "[abc]", "[a-c]", "[a-c]+", "[^a-c]+", "[^abc]", "[]]", "[]a]", "[^]]", "[-a]", "[a-]", "[a-c-e]", "[c-a]", "[", "[a", "[]", "[^", "[^]",
    "[a\\]b]", "[\\]]", "[\\^]", "[\\-]", "[\\\\]", "[\\e]", "[\\t]", "[\\r]", "[\\b]", "[\\n]", "[\\d97]", "[\\x61]", "[\\u0061]", "[\\U00000061]",
    "[\\o141]", "[\\a]", "[\\d]", "[\\w]", "[\\.]", "[a\\-c]", "[a-\\x63]", "[\\x61-c]", "[\\x61-\\x63]", "[[:alpha:]]+", "[[:digit:]]+", "[[:alnum:]]+",
    "[[:lower:]]+", "[[:upper:]]+", "[[:punct:]]+", "[[:space:]]", "[[:blank:]]", "[[:cntrl:]]", "[[:graph:]]+", "[[:print:]]+", "[[:xdigit:]]+", "[[:return:]]",
    "[[:tab:]]", "[[:escape:]]", "[[:backspace:]]", "[[:ident:]]+", "[[:keyword:]]+", "[[:fname:]]+", "[[:nope:]]", "[[:alpha:]0-9]+", "[^[:alpha:]]+", "[[]",
    "[[a]", "[[=a=]]", "[[=e=]]+", "[[.a.]]", "[=a=]", "[.a.]", "[:alpha:]", "[a-z]{2}", "[a-z]+\\.[a-z]+", "[.]", "[*]+", "[$]", "[(]x",
    "[éa]", "[à-ÿ]+", "[α-ω]+",
    // Optional sequences and codes.
    "fu%[nction]", "foo%[bar]", "r%[[eo]ad]", "%[a]", "%[]", "%[", "%[a", "%[(a)]", "a%[%[b]]", "%[a]*", "%d97", "%d", "%x61", "%x", "%u0061", "%u", "%U00000061", "%U",
    "%o141", "%o", "%x6", "%d9", "%u61", "%x61b", "%C", "a%Cb", "%x", "%z", "%",
    // Errors and leftovers.
    "*", "+", "=", "?", "{", "@", "{1}", "a**", "a*+", "a+*", "a=?", "a{1}{2}", "a@=@=", "~", "a~", "\\%", "%%", "\\", "a\\", "\\z(a\\)", "\\z1", "\\_", "\\_x", "\\_1", "\\_(",
    // Case flags, and the other magic levels.
    "\\cfoo", "\\Cfoo", "foo\\c", "foo\\C", "\\cFOO", "\\CFOO", "\\c\\Cfoo", "\\C\\cfoo", "\\c[A-Z]+", "\\C[a-z]+", "\\c\\u+", "\\c\\l+", "\\c\\U+", "\\c\\L+", "\\c[^a]", "\\c(a)\\1",
    "\\c[[:upper:]]+", "\\c[[:lower:]]+", "\\Zfoo", "\\v(a)", "\\V(x)", "\\Va.b", "\\V\\(x\\)", "\\V^foo", "\\Vfoo$", "\\Va$b", "\\V\\^foo", "\\Vfoo\\$", "\\V\\.", "\\V*",
    "\\m(x)", "\\m\\(a\\)", "\\ma*", "\\m*a", "\\m^*", "\\m\\(*\\)", "\\ma\\|b", "\\ma\\+", "\\ma\\=", "\\ma\\{2}", "\\ma\\{-1,}", "\\m.", "\\m\\.", "\\m[ab]", "\\m\\<foo\\>", "\\m\\(foo\\)\\@<=bar",
    "\\m%(a", "\\m\\%(a\\)", "\\ma$b", "\\ma^b", "\\m\\(^a\\)", "\\m\\(a$\\)", "\\mfoo$\\|bar", "\\m~", "\\M.", "\\M\\.", "\\M*", "\\M\\*", "\\Ma\\*", "\\M[ab]", "\\M\\[ab]", "\\M^foo",
    "foo\\mbar", "\\m(a\\v(b)", "a\\m*", "\\v\\m\\(a\\)", "^\\cfoo", "$\\c", "$\\v", "\\m$\\|a", "\\m$\\vb", "\\M$\\c"
];

// Left out on purpose: nvim's "-" beside a collating element, as in [[.a.]-c]
// and [a-[.c.]], which it drops or takes for the end of the collection.
//
// These look at the buffer or the cursor, which a line of a screen has none of.
const needBuffer = ["%#", "%V", "%'m", "%23l", "%23c", "%23v", "%<23c", "%>2l"];
for (const pattern of needBuffer) {
    let refused = false;
    try {
        compile(pattern);
    } catch (e) {
        refused = true;
    }
    if (!refused) throw new Error(`${pattern} should be refused`);
}

const cases = [];
for (const pattern of patterns)
    for (const subject of subjects)
        for (const ic of [false, true])
            cases.push({ pattern, subject, ic });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), "vim-regex-"));
const input = path.join(dir, "in.json"), output = path.join(dir, "out.json");
fs.writeFileSync(input, JSON.stringify(cases));
const nvim = spawnSync("nvim", ["--clean", "--headless", "-l", path.join(__dirname, "nvim-oracle.lua"), input, output],
    { stdio: "inherit", timeout: 120000 });
if (nvim.status !== 0) throw new Error(`nvim exited with ${nvim.status}`);
const expected = JSON.parse(fs.readFileSync(output, "utf8"));
fs.rmSync(dir, { recursive: true });

// UTF-8 byte offsets, as nvim counts, of the UTF-16 ones here.
const bytes = (text, index) => Buffer.byteLength(text.slice(0, index));

function ours({ pattern, subject, ic }) {
    try {
        const found = compile(pattern, ic).search(subject, 0);
        return found ? [bytes(subject, found.start), bytes(subject, found.end)] : null;
    } catch (e) {
        return "error";
    }
}

let failed = 0;
const failedPatterns = new Set();
cases.forEach((c, i) => {
    const got = ours(c);
    const want = expected[i] === "none" ? null : expected[i];
    if (JSON.stringify(got) !== JSON.stringify(want)) {
        failed++;
        if (!failedPatterns.has(c.pattern))
            console.log(`${JSON.stringify(c.pattern)} on ${JSON.stringify(c.subject)} ic=${c.ic}: `
                + `nvim ${JSON.stringify(want)}, here ${JSON.stringify(got)}`);
        failedPatterns.add(c.pattern);
    }
});
console.log(`${cases.length - failed}/${cases.length} agree with nvim (${patterns.length} patterns)`);
if (failed) {
    console.log("differ:", JSON.stringify([...failedPatterns]));
    process.exit(1);
}
