.pragma library
// Neovim's regular expressions (:help pattern) for the find bar, with \v in
// force from the start. The parser follows nvim's src/nvim/regexp.c: peekchr()
// for what is magic where, then reg(), regbranch(), regconcat(), regpiece()
// and regatom(). test/vim-regex.js holds it against nvim itself.
//
// It matches by itself: QML's RegExp has no lookbehind, so it could not do
// @<= or @<!, nor \zs. Each node compiles to a closure that knows what follows
// it, and backtracks by returning false.
//
// A match lives in one line of text, so what needs a buffer has no meaning
// here and is an error: %# %V %'m %23l %23c %23v, and ~ as nothing was
// substituted. \_x is just x, and %^ %$ are ^ $.

const NONE = 1, OFF = 2, ON = 3, ALL = 4; // \V \M \m \v
const FOREVER = Infinity;
// Backtracking steps a whole find may take, so that (.*)*x cannot hang the bar:
// about half a second's worth.
const BUDGET = 1000000;

const isDigit = c => c >= "0" && c <= "9";
const isLower = c => c >= "a" && c <= "z";
const isUpper = c => c >= "A" && c <= "Z";
const isAlpha = c => isLower(c) || isUpper(c);
const isHex = c => isDigit(c) || (c >= "a" && c <= "f") || (c >= "A" && c <= "F");
const isOctal = c => c >= "0" && c <= "7";
const isWord = c => isAlpha(c) || isDigit(c) || c === "_";
const isBlank = c => c === " " || c === "\t";
const isLatinLetter = c => c >= "À" && c <= "ÿ";
const isPrint = c => (c >= " " && c <= "~") || c >= "¡";
const isIdent = c => isWord(c) || isLatinLetter(c);
// Wide punctuation and symbols are no more a word than ASCII's.
const isWidePunct = c => (c >= " " && c <= "⯿") || (c >= "　" && c <= "〿");
const isKeyword = c => isWord(c) || (c >= "À" && !isWidePunct(c));
const isFname = c => isWord(c) || "/.-+,#$%~=".includes(c) || c >= "À";

const not = test => c => !test(c);
const noDigit = test => c => test(c) && !isDigit(c);

// \i \k \f ... Unlike a collection, they do not bend to 'ignorecase'.
const CLASSES = {
    i: isIdent, I: noDigit(isIdent), k: isKeyword, K: noDigit(isKeyword),
    f: isFname, F: noDigit(isFname), p: isPrint, P: noDigit(isPrint),
    s: isBlank, S: not(isBlank), d: isDigit, D: not(isDigit),
    x: isHex, X: not(isHex), o: isOctal, O: not(isOctal),
    w: isWord, W: not(isWord),
    h: c => isAlpha(c) || c === "_", H: c => !isAlpha(c) && c !== "_",
    a: isAlpha, A: not(isAlpha), l: isLower, L: not(isLower), u: isUpper, U: not(isUpper)
};

// [:alpha:] and the like, inside a collection.
const NAMED = {
    alnum: c => isAlpha(c) || isDigit(c), alpha: isAlpha, blank: isBlank,
    cntrl: c => c < " " || c === "\x7f", digit: isDigit, graph: c => c > " " && c <= "~",
    lower: c => c.toLowerCase() === c && c.toUpperCase() !== c,
    upper: c => c.toUpperCase() === c && c.toLowerCase() !== c,
    print: isPrint, punct: c => c > " " && c <= "~" && !isAlpha(c) && !isDigit(c),
    space: c => c === " " || (c >= "\t" && c <= "\r"), xdigit: isHex,
    return: c => c === "\r", tab: c => c === "\t", escape: c => c === "\x1b",
    backspace: c => c === "\b", ident: isIdent, keyword: isKeyword, fname: isFname
};

const baseLetter = c => c.normalize("NFD")[0];

// What may be magic after a backslash (META_flags in regexp.c).
const META = "%&()*+.123456789<=>?@ACDFHIKLMOPSUVWXZ[_acdfhiklmnopsuvwxz{|~";
const MAGIC_IN_V = "(){%+=?@!&|<>#\"',-:;`/";
const ESCAPES = { r: "\r", t: "\t", e: "\x1b", b: "\b" };

// A magic character's token is the character after a backslash; a plain one's
// is itself, so "\\(" opens a group and "(" is a bracket.
const magic = c => "\\" + c;
const isMagic = token => token.length === 2;
const plain = token => isMagic(token) ? token[1] : token;

function parse(pattern) {
    let at = 0;
    let level = ALL;
    let groups = 0;
    const closed = [];
    let icase = null;
    // peekchr()'s memory.
    let token = null, tokenLength = 0;
    let atStart = true, prevAtStart = false, prev = "", prevprev = "", afterSlash = 0;

    function classify(c) {
        if (".[~".includes(c)) return level >= ON ? magic(c) : c;
        if (MAGIC_IN_V.includes(c)) return level === ALL ? magic(c) : c;
        if (c === "*") {
            const opening = prev === magic("(") || prev === magic("&") || prev === magic("|");
            return level >= ON && !atStart && !(prevAtStart && prev === magic("^"))
                && (afterSlash > 0 || !opening) ? magic(c) : c;
        }
        if (c === "^") {
            if (level >= OFF && (atStart || level === ALL || prev === magic("(")
                || prev === magic("|") || prev === magic("&") || prev === magic("n")
                || (plain(prev) === "(" && prevprev === magic("%")))) {
                atStart = true;
                prevAtStart = false;
                return magic(c);
            }
            return c;
        }
        if (c === "$" && level >= OFF) {
            // Skip the \c \C \m \M \v \V \Z that do not count as being in front.
            let p = at + 1;
            let all = level === ALL;
            while (pattern[p] === "\\" && "cCmMvVZ".includes(pattern[p + 1] || " ")) {
                if (pattern[p + 1] === "v") all = true;
                else if ("mMV".includes(pattern[p + 1])) all = false;
                p += 2;
            }
            const next = pattern[p];
            const closes = "|&)".includes(next || " ");
            if (next === undefined || (next === "\\" && (closes || "|&)n".includes(pattern[p + 1] || " ")))
                || (all && closes) || level === ALL)
                return magic(c);
        }
        return c;
    }

    function peek() {
        if (token !== null) return token;
        const c = pattern[at];
        tokenLength = 1;
        if (c === undefined) {
            tokenLength = 0;
            token = "";
        } else if (c !== "\\") {
            token = classify(c);
        } else {
            const next = pattern[at + 1];
            if (next === undefined) {
                token = c; // a trailing backslash
            } else if (META.includes(next)) {
                prevAtStart = atStart;
                atStart = false;
                afterSlash++;
                at++;
                const inner = classify(next);
                at--;
                afterSlash--;
                token = isMagic(inner) ? next : magic(next);
                tokenLength = 2;
            } else if (ESCAPES[next]) {
                token = ESCAPES[next];
                tokenLength = 2;
            } else if (level === NONE && (next === "$" || next === "^")) {
                token = magic(next);
                tokenLength = 2;
            } else {
                token = next; // backslashing it does nothing
                tokenLength = 2;
            }
        }
        return token;
    }

    function skip() {
        peek();
        at += tokenLength;
        prevAtStart = atStart;
        atStart = false;
        prevprev = prev;
        prev = token;
        token = null;
    }

    // skipchr_keepstart(): \c and the like do not count as the first atom.
    function skipKeepStart() {
        const was = [prevAtStart, prev, prevprev];
        skip();
        atStart = was[0];
        prev = was[1];
        prevprev = was[2];
    }

    function next() {
        const c = peek();
        skip();
        return c;
    }

    function digits(test, max) {
        const from = at;
        while (at - from < max && pattern[at] !== undefined && test(pattern[at])) at++;
        return pattern.slice(from, at);
    }

    // \d123 \o40 \x20 € \U0001F600, from the letter on; the code, or -1.
    function codeAt() {
        const kind = pattern[at++];
        const text = kind === "d" ? digits(isDigit, 10)
            : kind === "o" ? digits(isOctal, 3)
            : digits(isHex, kind === "x" ? 2 : kind === "u" ? 4 : 8);
        if (text === "") return -1;
        const code = parseInt(text, kind === "d" ? 10 : kind === "o" ? 8 : 16);
        return code > 0x10ffff ? -1 : code;
    }

    function literal(text) {
        const items = text.split("").map(c => ({ t: "chr", c: c }));
        return items.length === 1 ? items[0] : { t: "seq", items: items };
    }

    function namedClass() {
        for (const name in NAMED)
            if (pattern.startsWith(`[:${name}:]`, at)) {
                at += name.length + 4;
                return NAMED[name];
            }
        return null;
    }

    function delimited(mark) {
        const c = pattern[at + 2];
        if (pattern[at + 1] === mark && c !== undefined && pattern[at + 3] === mark && pattern[at + 4] === "]") {
            at += 5;
            return c;
        }
        return null;
    }

    // [...], from past the [; null if no ] closes it, when [ is just a [.
    function collection() {
        const from = at;
        const negated = pattern[at] === "^";
        if (negated) at++;
        // Letters bend to 'ignorecase'; [:upper:] and the like do not.
        const tests = [], named = [];
        const one = c => tests.push(x => x === c);
        let start = null;
        if (pattern[at] === "]" || pattern[at] === "-") {
            start = pattern[at++];
            one(start);
        }
        while (at < pattern.length && pattern[at] !== "]") {
            const c = pattern[at];
            if (c === "-") {
                at++;
                const end = pattern[at];
                if (end === undefined || end === "]" || start === null
                    || (end === "\\" && pattern[at + 1] === "n")) {
                    one("-");
                    start = "-";
                    continue;
                }
                let last = end === "[" ? delimited(".") : null;
                if (last === null) {
                    at++;
                    last = end;
                    if (end === "\\" && "doxuU".includes(pattern[at] || " ")) {
                        const code = codeAt();
                        if (code >= 0) last = String.fromCodePoint(code);
                    }
                }
                if (start > last) throw new Error("reverse range");
                const low = start;
                tests.push(x => x >= low && x <= last);
                start = null;
            } else if (c === "\\" && "]^-\\nrtebdoxuU".includes(pattern[at + 1] || " ")) {
                const e = pattern[++at];
                if (e === "n") {
                    at++;
                    one("\n");
                    start = null;
                } else if ("doxuU".includes(e)) {
                    const code = codeAt();
                    if (code < 0) at--; // a backslash, then the letter as itself
                    start = code < 0 ? "\\" : String.fromCodePoint(code);
                    one(start);
                } else {
                    at++;
                    start = ESCAPES[e] || e;
                    one(start);
                }
            } else if (c === "[") {
                start = null;
                const name = namedClass();
                if (name) {
                    named.push(name);
                    continue;
                }
                const equal = delimited("=");
                if (equal !== null) {
                    const base = baseLetter(equal);
                    tests.push(x => baseLetter(x) === base);
                    continue;
                }
                const element = delimited(".");
                if (element !== null) {
                    one(element);
                    continue;
                }
                at++;
                start = "[";
                one("[");
            } else {
                at++;
                start = c;
                one(c);
            }
        }
        if (pattern[at] !== "]") {
            at = from;
            return null;
        }
        at++;
        return {
            t: "set",
            test: x => tests.some(t => t(x)),
            named: x => named.some(t => t(x)),
            negated: negated
        };
    }

    function classAtom(c) {
        if (c !== "." && !CLASSES[c]) throw new Error("invalid use of \\_");
        return { t: "set", test: c === "." ? () => true : CLASSES[c] };
    }

    function atom() {
        const c = next();
        switch (c) {
        case magic("^"): return { t: "bol" };
        case magic("$"): return { t: "eol" };
        case magic("<"): return { t: "bow" };
        case magic(">"): return { t: "eow" };
        case magic("n"): return literal("\n");
        case magic("_"): {
            const d = plain(next());
            if (d === "^") return { t: "bol" };
            if (d === "$") return { t: "eol" };
            if (d === "n") return literal("\n");
            if (d === "[") return collection() || literal("[");
            return classAtom(d);
        }
        case magic("("):
            return alternatives(1);
        case "":
        case magic("|"):
        case magic("&"):
        case magic(")"):
            throw new Error("nothing to repeat");
        case magic("="):
        case magic("?"):
        case magic("+"):
        case magic("@"):
        case magic("{"):
        case magic("*"):
            throw new Error(`${plain(c)} follows nothing`);
        case magic("~"):
            throw new Error("no previous substitute");
        case magic("z"): {
            const d = plain(next());
            if (d !== "s" && d !== "e") throw new Error("invalid character after \\z");
            if (isMagic(peek()) && "*+{".includes(peek()[1])) throw new Error(`\\z${d} cannot be repeated`);
            return { t: d === "s" ? "zs" : "ze" };
        }
        case magic("%"):
            return percent();
        case magic("["):
            return collection() || literal("[");
        }
        if (isMagic(c) && isDigit(c[1])) {
            const n = +c[1];
            if (!closed[n] && !/@<[!=]/.test(pattern.slice(at)))
                throw new Error("illegal back reference");
            return { t: "ref", n: n };
        }
        if (isMagic(c) && CLASSES[c[1]]) return classAtom(c[1]);
        if (c === magic(".")) return classAtom(".");
        return literal(plain(c));
    }

    function percent() {
        const d = plain(next());
        switch (d) {
        case "(":
            return alternatives(2);
        case "^": return { t: "bol" };
        case "$": return { t: "eol" };
        case "C": return { t: "seq", items: [] };
        case "[": {
            const atoms = [];
            while (peek() !== "]") {
                if (peek() === "") throw new Error("missing ]");
                atoms.push(atom());
            }
            skip();
            if (atoms.length === 0) throw new Error("empty %[]");
            // a%[bc] is a(b(c)?)?
            return atoms.reduceRight((rest, x) => ({
                t: "loop", min: 0, max: 1, lazy: false,
                x: rest ? { t: "seq", items: [x, rest] } : x
            }), null);
        }
        case "d": case "o": case "x": case "u": case "U": {
            at--;
            const code = codeAt();
            if (code < 0) throw new Error("invalid character after %");
            return literal(String.fromCodePoint(code));
        }
        }
        throw new Error(`%${d} needs a buffer`);
    }

    function isMulti(t) {
        return isMagic(t) && "*+=?{@".includes(t[1]);
    }

    function limits() {
        const lazy = pattern[at] === "-";
        if (lazy) at++;
        const low = digits(isDigit, 10);
        let high = low === "" ? FOREVER : +low;
        if (pattern[at] === ",") {
            at++;
            const text = digits(isDigit, 10);
            high = text === "" ? FOREVER : +text;
        }
        if (pattern[at] === "\\") at++;
        if (pattern[at] !== "}") throw new Error("syntax error in {...}");
        at++;
        const min = low === "" ? 0 : +low;
        return { lo: Math.min(min, high), hi: Math.max(min, high), lazy: lazy };
    }

    function piece() {
        const x = atom();
        const op = peek();
        if (!isMulti(op)) return x;
        skip();
        let node;
        switch (op[1]) {
        case "*": node = { t: "loop", x: x, min: 0, max: FOREVER, lazy: false }; break;
        case "+": node = { t: "loop", x: x, min: 1, max: FOREVER, lazy: false }; break;
        case "=":
        case "?": node = { t: "loop", x: x, min: 0, max: 1, lazy: false }; break;
        case "{": {
            const l = limits();
            node = { t: "loop", x: x, min: l.lo, max: l.hi, lazy: l.lazy };
            break;
        }
        case "@": {
            const limit = digits(isDigit, 10);
            const kind = plain(next());
            const behind = kind === "<";
            const sense = behind ? plain(next()) : kind;
            if (sense !== "=" && sense !== "!" && !(sense === ">" && !behind))
                throw new Error("invalid character after @");
            node = sense === ">" ? { t: "atomic", x: x }
                : behind ? { t: "behind", x: x, negated: sense === "!", limit: limit === "" ? FOREVER : +limit }
                : { t: "look", x: x, negated: sense === "!" };
            break;
        }
        }
        if (isMulti(peek())) throw new Error("nested multi");
        return node;
    }

    function concat() {
        const items = [];
        for (;;) {
            const t = peek();
            if (t === "" || t === magic("|") || t === magic("&") || t === magic(")")) break;
            const flag = isMagic(t) ? t[1] : "";
            if (flag !== "" && "ZcCvmMV".includes(flag)) {
                if (flag === "c") icase = true;
                else if (flag === "C" && icase === null) icase = false;
                else if (flag === "v") level = ALL;
                else if (flag === "m") level = ON;
                else if (flag === "M") level = OFF;
                else if (flag === "V") level = NONE;
                skipKeepStart();
            } else {
                items.push(piece());
            }
        }
        return { t: "seq", items: items };
    }

    // a&b: b, where a matches too.
    function branch() {
        const parts = [concat()];
        while (peek() === magic("&")) {
            skip();
            parts.push(concat());
        }
        const last = parts.pop();
        return parts.length === 0 ? last
            : { t: "seq", items: parts.map(x => ({ t: "look", x: x, negated: false })).concat(last) };
    }

    // paren: 0 at the top, 1 a group, 2 a %() that captures nothing.
    function alternatives(paren) {
        let n = 0;
        if (paren === 1) {
            if (groups === 9) throw new Error("too many (");
            n = ++groups;
        }
        const branches = [branch()];
        while (peek() === magic("|")) {
            skip();
            branches.push(branch());
        }
        if (paren === 0 && peek() !== "") throw new Error("unmatched )");
        if (paren !== 0) {
            if (peek() !== magic(")")) throw new Error("unmatched (");
            skip();
        }
        closed[n] = true;
        const x = branches.length === 1 ? branches[0] : { t: "alt", branches: branches };
        return n > 0 ? { t: "group", n: n, x: x } : x;
    }

    return { tree: alternatives(0), icase: icase };
}

// 'smartcase': does the pattern hold a capital that is not part of an item?
function hasUppercase(pattern) {
    for (let i = 0; i < pattern.length; i++) {
        if (pattern[i] === "\\") {
            if ((pattern[i + 1] === "_" || pattern[i + 1] === "%") && i + 2 < pattern.length) i += 2;
            else i++;
        } else if (pattern[i].toLowerCase() !== pattern[i]) {
            return true;
        }
    }
    return false;
}

// The fewest and the most characters a node can match, which keep lookbehind
// from trying every earlier position.
function width(node) {
    switch (node.t) {
    case "chr":
    case "set":
        return [1, 1];
    case "seq":
        return node.items.map(width).reduce((a, b) => [a[0] + b[0], a[1] + b[1]], [0, 0]);
    case "alt": {
        const widths = node.branches.map(width);
        return [Math.min(...widths.map(w => w[0])), Math.max(...widths.map(w => w[1]))];
    }
    case "group":
    case "atomic":
        return width(node.x);
    case "loop": {
        const w = width(node.x);
        return [w[0] * node.min, node.max === 0 || w[1] === 0 ? 0 : w[1] * node.max];
    }
    case "ref":
        return [0, FOREVER];
    }
    return [0, 0];
}

// A letter's class as \< and \> see it: 0 blank, 1 punctuation, 2 word.
function wordClass(c) {
    if (c === undefined || isBlank(c) || c === " ") return 0;
    return isKeyword(c) ? 2 : 1;
}

// Compiles a pattern; throws on a bad one. Case is ignored as with 'ignorecase'
// and 'smartcase' unless `ignorecase` says otherwise; \c and \C go first.
// search(text, from) gives the first match at or after `from` as { start, end },
// or null; it throws once a find has backtracked past BUDGET.
function compile(pattern, ignorecase) {
    const parsed = parse(pattern);
    const ic = parsed.icase !== null ? parsed.icase
        : ignorecase !== undefined ? ignorecase : !hasUppercase(pattern);
    const same = ic ? (a, b) => a.toLowerCase() === b.toLowerCase() : (a, b) => a === b;

    let text = "";
    let steps = 0;
    // Where each group opened (for its close), and what it last matched.
    const opened = [], starts = [], ends = [];
    let zs = -1, ze = -1, end = -1;
    const done = i => {
        end = i;
        return true;
    };
    const tick = () => {
        if (++steps > BUDGET) throw new Error("too much backtracking");
    };

    function charTest(node) {
        if (node.t === "chr") {
            const c = node.c;
            return x => same(x, c);
        }
        const test = node.test;
        if (!node.named) return test;
        const named = node.named, negated = node.negated;
        return x => negated !== (named(x) || test(x)
            || (ic && (test(x.toLowerCase()) || test(x.toUpperCase()))));
    }

    function saved() {
        return [starts.slice(), ends.slice(), zs, ze];
    }

    function restore(state) {
        for (let n = 1; n <= 9; n++) {
            starts[n] = state[0][n];
            ends[n] = state[1][n];
        }
        zs = state[2];
        ze = state[3];
    }

    function loop(node, next) {
        const min = node.min, max = node.max, lazy = node.lazy;
        // The usual case, x* of one character, needs no recursion to count.
        if (node.x.t === "chr" || node.x.t === "set") {
            const test = charTest(node.x);
            if (lazy) return i => {
                for (let j = i, n = 0; ; j++, n++) {
                    tick();
                    if (n >= min && next(j)) return true;
                    if (n >= max || j >= text.length || !test(text[j])) return false;
                }
            };
            return i => {
                let j = i;
                while (j - i < max && j < text.length && test(text[j])) j++;
                for (; j - i >= min; j--) {
                    tick();
                    if (next(j)) return true;
                }
                return false;
            };
        }
        let count = 0, last = -1;
        let body = null;
        const again = i => {
            tick();
            const c = count, l = last;
            const more = () => {
                // An empty round would go round forever.
                if (c >= max || (i === l && c >= min)) return false;
                count = c + 1;
                last = i;
                const matched = body(i);
                count = c;
                last = l;
                return matched;
            };
            const leave = () => {
                if (c < min) return false;
                const matched = next(i);
                count = c;
                last = l;
                return matched;
            };
            return lazy ? leave() || more() : more() || leave();
        };
        body = build(node.x, again);
        return i => {
            const c = count, l = last;
            count = 0;
            last = -1;
            const matched = again(i);
            count = c;
            last = l;
            return matched;
        };
    }

    function behind(node, next) {
        let target = -1;
        const inner = build(node.x, i => i === target);
        const span = width(node.x);
        const most = Math.min(span[1], node.limit);
        return i => {
            const was = target;
            const state = saved();
            target = i;
            let found = false;
            const stop = Math.max(0, i - most);
            for (let j = i - span[0]; j >= stop && !found; j--) found = inner(j);
            target = was;
            zs = state[2];
            ze = state[3];
            if (found === node.negated) {
                restore(state);
                return false;
            }
            if (node.negated) restore(state);
            if (next(i)) return true;
            restore(state);
            return false;
        };
    }

    function look(node, next) {
        const inner = build(node.x, () => true);
        return i => {
            const state = saved();
            const found = inner(i);
            zs = state[2];
            ze = state[3];
            if (found === node.negated) {
                restore(state);
                return false;
            }
            if (node.negated) restore(state);
            if (next(i)) return true;
            restore(state);
            return false;
        };
    }

    function build(node, next) {
        switch (node.t) {
        case "chr":
        case "set": {
            const test = charTest(node);
            return i => i < text.length && test(text[i]) && next(i + 1);
        }
        case "seq":
            return node.items.reduceRight((rest, x) => build(x, rest), next);
        case "alt": {
            const branches = node.branches.map(b => build(b, next));
            return i => {
                tick();
                return branches.some(b => b(i));
            };
        }
        case "group": {
            const n = node.n;
            const close = build({ t: "seq", items: [] }, i => {
                const s = starts[n], e = ends[n];
                starts[n] = opened[n];
                ends[n] = i;
                if (next(i)) return true;
                starts[n] = s;
                ends[n] = e;
                return false;
            });
            const inner = build(node.x, close);
            return i => {
                const o = opened[n];
                opened[n] = i;
                if (inner(i)) return true;
                opened[n] = o;
                return false;
            };
        }
        case "loop": return loop(node, next);
        case "look": return look(node, next);
        case "behind": return behind(node, next);
        case "atomic": {
            let stop = -1;
            const inner = build(node.x, i => {
                stop = i;
                return true;
            });
            return i => {
                const state = saved();
                const found = inner(i);
                zs = state[2];
                ze = state[3];
                if (found && next(stop)) return true;
                restore(state);
                return false;
            };
        }
        case "bol": return i => i === 0 && next(i);
        case "eol": return i => i === text.length && next(i);
        case "bow": return i => {
            const here = wordClass(text[i]);
            return here > 1 && wordClass(text[i - 1]) !== here && next(i);
        };
        case "eow": return i => {
            const before = wordClass(text[i - 1]);
            return i > 0 && before > 1 && wordClass(text[i]) !== before && next(i);
        };
        case "zs": return i => {
            const was = zs;
            zs = i;
            if (next(i)) return true;
            zs = was;
            return false;
        };
        case "ze": return i => {
            const was = ze;
            ze = i;
            if (next(i)) return true;
            ze = was;
            return false;
        };
        case "ref": {
            const n = node.n;
            return i => {
                if (starts[n] === undefined || starts[n] < 0) return next(i);
                const length = ends[n] - starts[n];
                if (i + length > text.length) return false;
                for (let k = 0; k < length; k++)
                    if (!same(text[starts[n] + k], text[i + k])) return false;
                return next(i + length);
            };
        }
        }
        throw new Error(`unknown node ${node.t}`);
    }

    const match = build(parsed.tree, done);

    return {
        search(subject, from) {
            text = subject;
            for (let i = from; i <= text.length; i++) {
                for (let n = 1; n <= 9; n++) starts[n] = ends[n] = undefined;
                zs = ze = -1;
                if (match(i)) {
                    const start = zs >= 0 ? zs : i;
                    return { start: start, end: Math.max(ze >= 0 ? ze : end, start) };
                }
            }
            return null;
        }
    };
}
