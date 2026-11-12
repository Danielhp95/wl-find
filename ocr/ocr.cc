#include "ocr.hh"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>

namespace wlfind {

namespace {

// All of `fd` up to `size` bytes; the count read, short only at its end.
size_t read_all(int fd, uint8_t* into, size_t size) {
    size_t have = 0;
    while (have < size) {
        ssize_t got = read(fd, into + have, size - have);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) throw std::runtime_error(std::string("cannot read the image: ") + std::strerror(errno));
        if (got == 0) break;
        have += static_cast<size_t>(got);
    }
    return have;
}

}  // namespace

Image read_ppm(int fd) {
#ifdef F_SETPIPE_SZ
    // grim writes 27 MB for a 3840x2400 output: fewer, larger reads. It fails
    // on what is no pipe, which is as well.
    fcntl(fd, F_SETPIPE_SZ, 1 << 20);
#endif
    // The header is a few dozen bytes, but its length is not known until it
    // is read: a first piece that surely holds it, and the pixels after it.
    std::vector<uint8_t> head(4096);
    size_t have = read_all(fd, head.data(), head.size());
    size_t at = 0;
    // A number of the header, past any space and "# comments" before it.
    auto number = [&]() -> long {
        for (;;) {
            while (at < have && std::isspace(head[at])) at++;
            if (at < have && head[at] == '#') {
                while (at < have && head[at] != '\n') at++;
                continue;
            }
            break;
        }
        if (at >= have || !std::isdigit(head[at])) throw std::runtime_error("not a PPM: its header is cut short");
        long value = 0;
        while (at < have && std::isdigit(head[at]) && value < 1000000) value = value * 10 + (head[at++] - '0');
        return value;
    };
    if (have < 2 || head[0] != 'P' || head[1] != '6') throw std::runtime_error("not a PPM (P6), as grim -t ppm writes");
    at = 2;
    long width = number();
    long height = number();
    long depth = number();
    // A few screens side by side at the most: not the gigabytes a wrong
    // header may ask for.
    if (width < 1 || height < 1 || width > 32768 || height > 32768 || width * height > (1L << 27))
        throw std::runtime_error("a PPM of no size, or of too great a one");
    if (depth != 255) throw std::runtime_error("a PPM that is not of 8 bits a channel");
    // One space ends the header, whatever byte follows it.
    if (at >= have || !std::isspace(head[at])) throw std::runtime_error("not a PPM: its header is cut short");
    at++;

    Image image;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    size_t size = static_cast<size_t>(width) * height * 3;
    image.data.resize(size);
    size_t from_head = std::min(have - at, size);
    std::memcpy(image.data.data(), head.data() + at, from_head);
    if (from_head + read_all(fd, image.data.data() + from_head, size - from_head) < size)
        throw std::runtime_error("the PPM is cut short");
    return image;
}

namespace {

// How one axis is resampled: output pixel i is the sum of `count[i]` input
// pixels from `first[i]` on, by the weights from `offset[i]` on.
struct Taps {
    std::vector<int> first, count, offset;
    std::vector<float> weight;
};

Taps taps(int from, int to) {
    Taps t;
    t.first.resize(to);
    t.count.resize(to);
    t.offset.resize(to);
    double scale = static_cast<double>(from) / to;
    for (int i = 0; i < to; i++) {
        t.offset[i] = static_cast<int>(t.weight.size());
        if (to < from) {
            // Shrinking: the input pixels under [i, i + 1) × scale, each by
            // its share of that span.
            double a = i * scale, b = (i + 1) * scale;
            int first = static_cast<int>(std::floor(a));
            int last = std::min(static_cast<int>(std::ceil(b)), from);
            t.first[i] = first;
            t.count[i] = last - first;
            for (int j = first; j < last; j++)
                t.weight.push_back(static_cast<float>((std::min<double>(j + 1, b) - std::max<double>(j, a)) / scale));
        } else {
            // Enlarging: between the two input pixels about the middle of
            // the output one.
            double centre = (i + 0.5) * scale - 0.5;
            int first = static_cast<int>(std::floor(centre));
            float along = static_cast<float>(centre - first);
            if (first < 0) {
                first = 0;
                along = 0;
            }
            if (first >= from - 1) {
                t.first[i] = from - 1;
                t.count[i] = 1;
                t.weight.push_back(1);
            } else {
                t.first[i] = first;
                t.count[i] = 2;
                t.weight.push_back(1 - along);
                t.weight.push_back(along);
            }
        }
    }
    return t;
}

}  // namespace

void to_planes(const Image& image, Rect from, int width, int height, float* planes) {
    to_planes(image, from, width, height, planes, 0, height);
}

void to_planes(const Image& image, Rect from, int width, int height, float* planes, int first, int last) {
    Taps across = taps(from.x2 - from.x1, width);
    Taps down = taps(from.y2 - from.y1, height);
    // An input row resampled across, three samples a pixel as it is stored.
    // Enlarging takes each row for several output rows: the last few stay.
    const int kept = 4;
    std::vector<float> rows(static_cast<size_t>(kept) * width * 3);
    int row_of[kept];
    std::fill(row_of, row_of + kept, -1);
    int next = 0;
    auto resampled = [&](int y) -> const float* {
        for (int k = 0; k < kept; k++)
            if (row_of[k] == y) return &rows[static_cast<size_t>(k) * width * 3];
        int k = next;
        next = (next + 1) % kept;
        row_of[k] = y;
        float* out = &rows[static_cast<size_t>(k) * width * 3];
        const uint8_t* in = &image.data[(static_cast<size_t>(from.y1 + y) * image.width + from.x1) * 3];
        for (int x = 0; x < width; x++) {
            const uint8_t* pixel = in + static_cast<size_t>(across.first[x]) * 3;
            const float* weight = &across.weight[across.offset[x]];
            float r = 0, g = 0, b = 0;
            for (int j = 0; j < across.count[x]; j++, pixel += 3) {
                r += weight[j] * pixel[0];
                g += weight[j] * pixel[1];
                b += weight[j] * pixel[2];
            }
            out[x * 3] = r;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = b;
        }
        return out;
    };

    const size_t plane = static_cast<size_t>(width) * height;
    std::vector<float> sum(static_cast<size_t>(width) * 3);
    for (int y = first; y < last; y++) {
        std::fill(sum.begin(), sum.end(), 0.0f);
        for (int j = 0; j < down.count[y]; j++) {
            const float* row = resampled(down.first[y] + j);
            float weight = down.weight[down.offset[y] + j];
            for (int x = 0; x < width * 3; x++) sum[x] += weight * row[x];
        }
        float* blue = planes + static_cast<size_t>(y) * width;
        float* green = blue + plane;
        float* red = green + plane;
        const float to_unit = 2.0f / 255.0f;
        for (int x = 0; x < width; x++) {
            red[x] = sum[x * 3] * to_unit - 1;
            green[x] = sum[x * 3 + 1] * to_unit - 1;
            blue[x] = sum[x * 3 + 2] * to_unit - 1;
        }
    }
}

namespace {

// Pixels that touch, corners included, as one blob: its box, half open, how
// many pixels it has and what they add up to.
struct Blob {
    int x1, y1, x2, y2;
    double sum;
    long pixels;
};

// The blobs of a `width` by `height` picture: `value(x, y)` is above 0 for a
// pixel that is part of one, and is what it adds to its blob's sum.
template <typename Value>
std::vector<Blob> blobs_of(int width, int height, Value value) {
    // By runs: a run of pixels along a row joins the blobs of the runs it
    // touches in the row above.
    struct Growing {
        int parent;
        Blob blob;
    };
    std::vector<Growing> blobs;
    auto root = [&](int i) {
        while (blobs[i].parent != i) {
            blobs[i].parent = blobs[blobs[i].parent].parent;
            i = blobs[i].parent;
        }
        return i;
    };
    struct Run {
        int x1, x2, blob;
    };
    std::vector<Run> above, here;
    for (int y = 0; y < height; y++) {
        here.clear();
        size_t first_above = 0;
        for (int x = 0; x < width;) {
            double v = value(x, y);
            if (!(v > 0)) {
                x++;
                continue;
            }
            int x1 = x;
            double sum = 0;
            for (; x < width && (v = value(x, y)) > 0; x++) sum += v;
            int blob = static_cast<int>(blobs.size());
            blobs.push_back({blob, {x1, y, x, y + 1, sum, x - x1}});
            // Runs above that end before this one starts touch none of the
            // runs still to come either.
            while (first_above < above.size() && above[first_above].x2 < x1) first_above++;
            for (size_t k = first_above; k < above.size() && above[k].x1 <= x; k++) {
                int a = root(above[k].blob), b = root(blob);
                if (a == b) continue;
                Blob& into = blobs[a].blob;
                const Blob& other = blobs[b].blob;
                into.x1 = std::min(into.x1, other.x1);
                into.y1 = std::min(into.y1, other.y1);
                into.x2 = std::max(into.x2, other.x2);
                into.y2 = std::max(into.y2, other.y2);
                into.sum += other.sum;
                into.pixels += other.pixels;
                blobs[b].parent = a;
            }
            here.push_back({x1, x, blob});
        }
        std::swap(above, here);
    }
    std::vector<Blob> whole;
    for (size_t i = 0; i < blobs.size(); i++)
        if (blobs[i].parent == static_cast<int>(i)) whole.push_back(blobs[i].blob);
    return whole;
}

}  // namespace

Ink ink_of(const Image& image, Rect box) {
    Ink ink;
    ink.width = box.x2 - box.x1;
    ink.height = box.y2 - box.y1;
    const size_t pixels = static_cast<size_t>(ink.width) * ink.height;
    ink.data.resize(pixels);
    ink.bottom = ink.below = ink.height;
    if (pixels == 0) return ink;

    // The ground is the commonest shade of each channel: text covers less of
    // a line than what it is written on does. Counted a few shades together,
    // as a ground is seldom one shade exactly.
    int ground[3];
    for (int c = 0; c < 3; c++) {
        long count[256] = {};
        for (int y = 0; y < ink.height; y++) {
            const uint8_t* row = &image.data[(static_cast<size_t>(box.y1 + y) * image.width + box.x1) * 3 + c];
            for (int x = 0; x < ink.width; x++) count[row[x * 3]]++;
        }
        long most = -1;
        for (int v = 0; v < 256; v++) {
            long about = 0;
            for (int d = -2; d <= 2; d++)
                if (v + d >= 0 && v + d < 256) about += count[v + d];
            // Of shades as common, the one most common itself.
            if (about > most || (about == most && count[v] > count[ground[c]])) {
                most = about;
                ground[c] = v;
            }
        }
    }

    // A pixel is as inked as its furthest channel is from the ground, and
    // full ink is as far as more than a stray pixel or two get.
    long count[256] = {};
    for (int y = 0; y < ink.height; y++) {
        const uint8_t* row = &image.data[(static_cast<size_t>(box.y1 + y) * image.width + box.x1) * 3];
        uint8_t* out = &ink.data[static_cast<size_t>(y) * ink.width];
        for (int x = 0; x < ink.width; x++) {
            int far = 0;
            for (int c = 0; c < 3; c++) far = std::max(far, std::abs(row[x * 3 + c] - ground[c]));
            out[x] = static_cast<uint8_t>(far);
            count[far]++;
        }
    }
    long few = std::max<long>(3, static_cast<long>(pixels / 500));
    int full = 255;
    for (long beyond = 0; full > 0; full--) {
        beyond += count[full];
        if (beyond >= few) break;
    }
    const int faintest = 24;
    ink.legible = full >= faintest;
    if (!ink.legible) {
        std::fill(ink.data.begin(), ink.data.end(), 0);
        return ink;
    }
    for (uint8_t& v : ink.data) v = static_cast<uint8_t>(std::min(255, (v * 255 + full / 2) / full));

    // Rules: no stroke of a letter runs along a row for longer than the box
    // is tall. They are left faint, not blank: no part of a letter's shape,
    // but not ground on which a mark could only have been made up either,
    // as a row of underscores is a rule too.
    for (int y = 0; y < ink.height; y++) {
        uint8_t* row = &ink.data[static_cast<size_t>(y) * ink.width];
        for (int x = 0; x < ink.width;) {
            if (row[x] < 128) {
                x++;
                continue;
            }
            int x1 = x;
            while (x < ink.width && row[x] >= 128) x++;
            if (x - x1 > ink.height) std::fill(row + x1, row + x, 64);
        }
    }

    // The line is the band of rows with the most ink, and with it the bands
    // above and below that are clear of the box's edges, as the dot of an i
    // is. What reaches in from an edge is another line's.
    struct Band {
        int from, to;
        long weight;
    };
    std::vector<Band> bands;
    for (int y = 0; y < ink.height; y++) {
        const uint8_t* row = &ink.data[static_cast<size_t>(y) * ink.width];
        long weight = 0;
        for (int x = 0; x < ink.width; x++)
            if (row[x] >= 128) weight += row[x];
        if (weight == 0) continue;
        if (!bands.empty() && bands.back().to == y) {
            bands.back().to = y + 1;
            bands.back().weight += weight;
        } else {
            bands.push_back({y, y + 1, weight});
        }
    }
    if (bands.empty()) return ink;
    size_t main = 0;
    for (size_t i = 1; i < bands.size(); i++)
        if (bands[i].weight > bands[main].weight) main = i;
    size_t first = main, last = main;
    while (first > 0 && bands[first - 1].from > 0) first--;
    while (last + 1 < bands.size() && bands[last + 1].to < ink.height) last++;
    ink.top = bands[first].from;
    ink.bottom = bands[last].to;
    if (first > 0) ink.above = bands[first - 1].to;
    if (last + 1 < bands.size()) ink.below = bands[last + 1].from;
    for (int y = 0; y < ink.height; y++) {
        if (y >= ink.top && y < ink.bottom) continue;
        uint8_t* row = &ink.data[static_cast<size_t>(y) * ink.width];
        for (int x = 0; x < ink.width; x++)
            if (row[x] >= 128) row[x] = 0;
    }
    return ink;
}

std::vector<Span> ink_columns(const Ink& ink) {
    std::vector<char> inked(static_cast<size_t>(ink.width), 0);
    for (int y = 0; y < ink.height; y++) {
        const uint8_t* row = &ink.data[static_cast<size_t>(y) * ink.width];
        for (int x = 0; x < ink.width; x++) inked[x] |= row[x] >= 128;
    }
    std::vector<Span> spans;
    for (int x = 0; x < ink.width;) {
        if (!inked[x]) {
            x++;
            continue;
        }
        int x1 = x;
        while (x < ink.width && inked[x]) x++;
        spans.push_back({x1, x});
    }
    return spans;
}

std::vector<Letter> join_pieces(const std::vector<std::vector<Letter>>& pieces, const std::vector<int>& cuts, int reach) {
    // How far apart letters are, on the whole: what "the same place" and "a
    // word apart" are measured by.
    std::vector<float> steps;
    for (const auto& piece : pieces)
        for (size_t i = 1; i < piece.size(); i++)
            if (piece[i].text != " " && piece[i - 1].text != " ") steps.push_back(piece[i].middle - piece[i - 1].middle);
    std::sort(steps.begin(), steps.end());
    float pitch = steps.empty() ? static_cast<float>(std::max(reach, 1)) : steps[steps.size() / 2];
    const float same_place = 0.75f * pitch, apart = 1.6f * pitch;

    std::vector<Letter> line;
    if (!pieces.empty()) line = pieces[0];
    for (size_t p = 1; p < pieces.size(); p++) {
        const std::vector<Letter>& next = pieces[p];
        float cut = p - 1 < cuts.size() ? static_cast<float>(cuts[p - 1]) : 0;

        // The longest run of letters that the line so far ends with and the
        // next piece starts with, about the cut: run[i][j] is how many end
        // at letter i of the one and j of the other.
        size_t first = line.size();
        while (first > 0 && line[first - 1].middle >= cut - static_cast<float>(reach) - pitch) first--;
        size_t last = 0;
        while (last < next.size() && next[last].middle <= cut + static_cast<float>(reach) + pitch) last++;
        std::vector<std::vector<int>> run(line.size() - first + 1, std::vector<int>(last + 1, 0));
        int longest = 0;
        size_t at_i = 0, at_j = 0;
        float nearest = 0;
        for (size_t i = first; i < line.size(); i++)
            for (size_t j = 0; j < last; j++) {
                const Letter& a = line[i];
                const Letter& b = next[j];
                if (a.text != b.text) continue;
                int length = run[i - first][j] + 1;
                if (a.text == " ") {
                    // A space is somewhere in its gap, not at a place of
                    // its own, and like any other space: two are the same
                    // one only after letters that are.
                    if (length > 1) run[i - first + 1][j + 1] = length;
                    continue;
                }
                if (std::fabs(a.middle - b.middle) > same_place) continue;
                run[i - first + 1][j + 1] = length;
                float off = std::fabs((a.middle + b.middle) / 2 - cut);
                if (length > longest || (length == longest && off < nearest)) {
                    longest = length;
                    at_i = i;
                    at_j = j;
                    nearest = off;
                }
            }

        std::vector<Letter> sewn;
        if (longest > 0) {
            // Half the run from each: the line up to the middle of it, the
            // next piece from there on.
            size_t back = static_cast<size_t>(longest / 2);
            sewn.assign(line.begin(), line.begin() + static_cast<long>(at_i - back + 1));
            sewn.insert(sewn.end(), next.begin() + static_cast<long>(at_j - back + 1), next.end());
        } else {
            // They agree on nothing. Every letter either of them read is
            // kept, once: one that both read, at about one place, as the
            // piece on whose side of the cut it is has it.
            std::vector<char> not_line(line.size(), 0), not_next(next.size(), 0);
            for (size_t i = 0; i < line.size(); i++) not_line[i] = line[i].text == " " && line[i].middle >= cut;
            for (size_t j = 0; j < next.size(); j++) {
                if (next[j].text == " ") {
                    not_next[j] = next[j].middle < cut;
                    continue;
                }
                for (size_t i = 0; i < line.size(); i++) {
                    if (line[i].text == " " || not_line[i] || std::fabs(line[i].middle - next[j].middle) > pitch / 2) continue;
                    if ((line[i].middle + next[j].middle) / 2 < cut) not_next[j] = 1;
                    else not_line[i] = 1;
                    break;
                }
            }
            size_t i = 0, j = 0;
            int last = -1;
            for (;;) {
                while (i < line.size() && not_line[i]) i++;
                while (j < next.size() && not_next[j]) j++;
                if (i >= line.size() && j >= next.size()) break;
                bool from_line = j >= next.size() || (i < line.size() && line[i].middle <= next[j].middle);
                const Letter& letter = from_line ? line[i++] : next[j++];
                // Where one piece's letters end and the other's begin, they
                // are words apart if they are far apart.
                if (last >= 0 && last != from_line && letter.text != " " && sewn.back().text != " " && letter.middle - sewn.back().middle > apart)
                    sewn.push_back({" ", 1, cut});
                sewn.push_back(letter);
                last = from_line;
            }
        }
        line = std::move(sewn);
    }

    // One space between words, and none at either end.
    std::vector<Letter> tidy;
    for (const Letter& letter : line) {
        if (letter.text == " " && (tidy.empty() || tidy.back().text == " ")) continue;
        tidy.push_back(letter);
    }
    if (!tidy.empty() && tidy.back().text == " ") tidy.pop_back();
    return tidy;
}

std::vector<Word> words_of(const std::vector<Letter>& letters, const Ink& ink) {
    std::vector<Span> columns;
    if (ink.legible) columns = ink_columns(ink);
    float top = ink.legible ? static_cast<float>(ink.top) : 0;
    float bottom = ink.legible ? static_cast<float>(ink.bottom) : static_cast<float>(ink.height);

    // How far apart the letters of a word are, on the whole.
    std::vector<float> steps;
    for (size_t i = 1; i < letters.size(); i++)
        if (letters[i].text != " " && letters[i - 1].text != " ") steps.push_back(letters[i].middle - letters[i - 1].middle);
    std::sort(steps.begin(), steps.end());
    float pitch = steps.empty() ? 0.6f * (bottom - top) : steps[steps.size() / 2];
    // The recogniser may have a letter half a letter from where it is; and
    // ink further from a letter's middle than this is not that letter's.
    const float out = pitch / 2, widest = 1.25f * pitch;

    struct Range {
        size_t first, last;
        float x1, x2;
    };
    std::vector<Range> ranges;
    for (size_t i = 0; i < letters.size();) {
        if (letters[i].text == " ") {
            i++;
            continue;
        }
        size_t j = i;
        while (j < letters.size() && letters[j].text != " ") j++;
        // With no ink to go by, half a letter beyond its first and last,
        // and no further than the line goes.
        const float end = static_cast<float>(ink.width);
        ranges.push_back({i, j - 1, std::clamp(letters[i].middle - out, 0.0f, end), std::clamp(letters[j - 1].middle + out, 0.0f, end)});
        i = j;
    }

    for (size_t k = 0; k < ranges.size(); k++) {
        float first = letters[ranges[k].first].middle, last = letters[ranges[k].last].middle;
        if (k == 0) {
            // The line starts where the ink about its first letter does.
            for (const Span& c : columns)
                if (c.x2 > first - out && c.x1 < first + out && first - c.x1 <= widest) {
                    ranges[k].x1 = static_cast<float>(c.x1);
                    break;
                }
        }
        if (k + 1 == ranges.size()) {
            for (auto c = columns.rbegin(); c != columns.rend(); ++c)
                if (c->x2 > last - out && c->x1 < last + out && c->x2 - last <= widest) {
                    ranges[k].x2 = static_cast<float>(c->x2);
                    break;
                }
            continue;
        }
        // Two words part at the gap in the ink between the last letter of
        // one and the first of the next: of the gaps there, the one with
        // the most of itself between the two.
        float next = letters[ranges[k + 1].first].middle;
        float most = 0, from = 0, to = 0;
        // ...the empty ends of the line being gaps as well.
        for (size_t c = 0; c <= columns.size() && !columns.empty(); c++) {
            float gap_from = c == 0 ? 0 : static_cast<float>(columns[c - 1].x2);
            float gap_to = c == columns.size() ? static_cast<float>(ink.width) : static_cast<float>(columns[c].x1);
            float between = std::min(gap_to, next + out) - std::max(gap_from, last - out);
            if (between > most) {
                most = between;
                from = gap_from;
                to = gap_to;
            }
        }
        if (most > 0) {
            ranges[k].x2 = from;
            ranges[k + 1].x1 = to;
        }
    }

    std::vector<Word> words;
    for (Range& r : ranges) {
        // A word between two gaps that are one gap, or the wrong way round,
        // has no ink the gaps part from its neighbours': it is where it
        // was read.
        if (r.x2 <= r.x1) {
            const float end = static_cast<float>(ink.width);
            r.x1 = std::clamp(letters[r.first].middle - out, 0.0f, end);
            r.x2 = std::clamp(letters[r.last].middle + out, 0.0f, end);
        }
        std::string text;
        for (size_t i = r.first; i <= r.last; i++) text += letters[i].text;
        words.push_back({text, r.x1, top, r.x2 - r.x1, bottom - top});
    }
    return words;
}

namespace {

// How inked the middle of a letter is: across the rows about its middle, the
// most ink between its left stroke and its right one, past where each fades
// out. -1 if no row has strokes with anything between them.
float middle_of(const Ink& ink, const Shape& shape) {
    int w = shape.x2 - shape.x1, h = shape.y2 - shape.y1;
    if (w < 3 || h < 5) return -1;
    int centre = (shape.y1 + shape.y2) / 2;
    int reach = std::max(1, static_cast<int>(std::lround(0.12 * h)));
    float most = -1;
    for (int y = std::max(centre - reach, shape.y1); y <= std::min(centre + reach, shape.y2 - 1); y++) {
        const uint8_t* row = &ink.data[static_cast<size_t>(y) * ink.width + shape.x1];
        int l = 0, r = w - 1;
        while (l < w && row[l] < 128) l++;
        while (l < w && row[l] >= 128) l++;
        while (r >= 0 && row[r] < 128) r--;
        while (r >= 0 && row[r] >= 128) r--;
        while (l < r && row[l] >= row[l + 1]) l++;
        while (r > l && row[r] >= row[r - 1]) r--;
        if (l > r) continue;
        for (int x = l; x <= r; x++) most = std::max(most, row[x] / 255.0f);
    }
    return most;
}

}  // namespace

std::vector<Shape> shapes_of(const std::vector<Letter>& letters, const Ink& ink) {
    std::vector<Shape> shapes(letters.size());
    if (!ink.legible) return shapes;
    std::vector<Blob> blobs = blobs_of(ink.width, ink.height, [&](int x, int y) { return ink.data[static_cast<size_t>(y) * ink.width + x] >= 128 ? 1 : 0; });

    std::vector<float> steps;
    for (size_t i = 1; i < letters.size(); i++)
        if (letters[i].text != " " && letters[i - 1].text != " ") steps.push_back(letters[i].middle - letters[i - 1].middle);
    std::sort(steps.begin(), steps.end());
    float pitch = steps.empty() ? 0.6f * static_cast<float>(ink.bottom - ink.top) : steps[steps.size() / 2];

    // A blob is the letter's whose middle is nearest it, if that is within
    // half a letter; one with the middles of two letters in it is both of
    // them, touching, and the shape of neither.
    std::vector<char> touching(letters.size(), 0);
    for (const Blob& blob : blobs) {
        int nearest = -1, within = 0;
        float least = 0, least_off = 0;
        for (size_t i = 0; i < letters.size(); i++) {
            if (letters[i].text == " ") continue;
            float m = letters[i].middle;
            float away = m < blob.x1 ? blob.x1 - m : m >= blob.x2 ? m - blob.x2 : 0;
            float off = std::fabs(m - (blob.x1 + blob.x2) / 2.0f);
            if (away == 0) within++;
            if (nearest < 0 || away < least || (away == least && off < least_off)) {
                nearest = static_cast<int>(i);
                least = away;
                least_off = off;
            }
        }
        if (nearest < 0 || least > pitch / 2) continue;
        if (within > 1) {
            for (size_t i = 0; i < letters.size(); i++)
                if (letters[i].text != " " && letters[i].middle >= blob.x1 && letters[i].middle < blob.x2) touching[i] = 1;
            continue;
        }
        Shape& shape = shapes[nearest];
        if (!shape.known) {
            shape = {true, blob.x1, blob.y1, blob.x2, blob.y2, -1};
        } else {
            shape.x1 = std::min(shape.x1, blob.x1);
            shape.y1 = std::min(shape.y1, blob.y1);
            shape.x2 = std::max(shape.x2, blob.x2);
            shape.y2 = std::max(shape.y2, blob.y2);
        }
    }
    for (size_t i = 0; i < shapes.size(); i++) {
        if (touching[i]) shapes[i] = Shape{};
        if (shapes[i].known) shapes[i].middle = middle_of(ink, shapes[i]);
    }
    return shapes;
}

namespace {

bool is_digit(const std::string& text) { return text.size() == 1 && text[0] >= '0' && text[0] <= '9'; }

// A letter of any alphabet: what is not ASCII is one unless it is among the
// signs before the accented letters (´, ®, ·) or the punctuation, arrows and
// shapes from U+2000 on.
bool is_letter(const std::string& text) {
    if (text.empty()) return false;
    unsigned char c = static_cast<unsigned char>(text[0]);
    if (text.size() == 1) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    unsigned code = c >= 0xf0 ? 0x10000 : c >= 0xe0 ? ((c & 0x0f) << 12) | ((static_cast<unsigned char>(text[1]) & 0x3f) << 6)
        : ((c & 0x1f) << 6) | (static_cast<unsigned char>(text[1]) & 0x3f);
    return code >= 0xc0 && code < 0x2000 && code != 0xd7 && code != 0xf7;
}

bool among(const std::string& text, const char* set) { return text.size() == 1 && std::strchr(set, text[0]) != nullptr; }

float median(std::vector<float> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

}  // namespace

std::vector<Letter> proofread(std::vector<Letter> letters, const Ink& ink) {
    if (!ink.legible || letters.empty()) return letters;
    std::vector<Shape> shapes = shapes_of(letters, ink);
    const size_t count = letters.size();
    auto text = [&](size_t i) -> const std::string& {
        static const std::string none;
        return i < count ? letters[i].text : none;
    };

    // The line is monospaced if its letters are all one step apart, by the
    // middles of their ink: `cell` is that step.
    std::vector<float> steps, read_steps;
    for (size_t i = 1; i < count; i++) {
        if (text(i) == " " || text(i - 1) == " ") continue;
        read_steps.push_back(letters[i].middle - letters[i - 1].middle);
        // (An underscore's ink may lie under several cells.)
        if (shapes[i].known && shapes[i - 1].known && text(i) != "_" && text(i - 1) != "_")
            steps.push_back((shapes[i].x1 + shapes[i].x2 - shapes[i - 1].x1 - shapes[i - 1].x2) / 2.0f);
    }
    float pitch = read_steps.empty() ? 0.6f * static_cast<float>(ink.bottom - ink.top) : median(read_steps);
    bool monospaced = false;
    float cell = 0;
    if (steps.size() >= 8) {
        cell = median(steps);
        size_t even = 0;
        for (float step : steps) even += std::fabs(step - cell) < 0.15f * cell;
        monospaced = even * 100 >= steps.size() * 85;
    }

    // A mark read where there is no ink at all, not even a faint stroke, is
    // nothing but a gap.
    for (size_t i = 0; i < count; i++) {
        if (text(i) == " " || is_letter(text(i)) || is_digit(text(i))) continue;
        int x1 = std::max(static_cast<int>(letters[i].middle - 0.4f * pitch), 0);
        int x2 = std::min(static_cast<int>(letters[i].middle + 0.4f * pitch) + 1, ink.width);
        bool inked = false;
        for (int y = 0; y < ink.height && !inked; y++)
            for (int x = x1; x < x2 && !inked; x++) inked = ink.data[static_cast<size_t>(y) * ink.width + x] >= 64;
        if (!inked) letters[i].text = " ";
    }

    // How tall the line's capitals and its small letters are, from its
    // bottom: by those that are surely one or the other. With only one kind
    // to go by, a small letter is about three quarters of a capital.
    std::vector<float> talls, smalls, bottoms;
    for (size_t i = 0; i < count; i++) {
        if (!shapes[i].known) continue;
        if (among(text(i), "ABDEFGHIJKLMNPQRTYbdfhkl123456789")) talls.push_back(static_cast<float>(shapes[i].y1));
        if (among(text(i), "aegmnpqry")) smalls.push_back(static_cast<float>(shapes[i].y1));
        if ((is_letter(text(i)) || is_digit(text(i))) && !among(text(i), "gjpqyQJ")) bottoms.push_back(static_cast<float>(shapes[i].y2));
    }
    const float small_of_tall = 0.74f;
    float tall = 0, small = 0;
    bool measured = !bottoms.empty() && (!talls.empty() || !smalls.empty());
    float bottom = bottoms.empty() ? 0 : median(bottoms);
    if (measured) {
        tall = talls.empty() ? (bottom - median(smalls)) / small_of_tall : bottom - median(talls);
        small = smalls.empty() ? (bottom - median(talls)) * small_of_tall : bottom - median(smalls);
        measured = tall - small >= 2;
    }
    // 1 for a letter as tall as the capitals, -1 for one as tall as the
    // small letters, 0 for one between the two, or with nothing to tell by.
    auto height = [&](size_t i) {
        if (!measured || !shapes[i].known) return 0;
        float h = bottom - static_cast<float>(shapes[i].y1);
        // Ink well above the capitals is not a letter's alone: the end of
        // a button, an icon, a border.
        if (h > 1.25f * tall) return 0;
        float between = (tall + small) / 2, clear = 0.15f * (tall - small);
        return h > between + clear ? 1 : h < between - clear ? -1 : 0;
    };

    // One shape at two sizes.
    const std::string smalls_alike = "cosuvwxz", talls_alike = "COSUVWXZ";
    for (size_t i = 0; i < count; i++) {
        if (text(i).size() != 1) continue;
        size_t as_small = smalls_alike.find(text(i)[0]), as_tall = talls_alike.find(text(i)[0]);
        if (as_small != std::string::npos && height(i) > 0) letters[i].text = std::string(1, talls_alike[as_small]);
        if (as_tall != std::string::npos && height(i) < 0) letters[i].text = std::string(1, smalls_alike[as_tall]);
        // ...unless it is among digits: some fonts' figures are no taller
        // than their small letters.
        if (text(i) == "0" && height(i) < 0 && !is_digit(text(i - 1)) && !is_digit(text(i + 1))) letters[i].text = "o";
    }

    if (monospaced) {
        // Zeros and capital Os, by their middles.
        // A middle is surely inked from half way up, in a letter wide enough
        // to have a middle apart from its strokes.
        const float inked = 0.5f, empty = 0.2f;
        auto ring = [&](size_t i) {
            return (text(i) == "0" || text(i) == "O" || text(i) == "o") && shapes[i].known && height(i) >= 0 && shapes[i].x2 - shapes[i].x1 >= 5;
        };
        bool marks_zeros = false;
        for (size_t i = 0; i < count; i++)
            if (ring(i) && (text(i) != "o" || height(i) > 0) && shapes[i].middle >= inked) {
                letters[i].text = "0";
                marks_zeros = true;
            }
        auto hollow = [&](size_t i) { return i < count && text(i) == "0" && shapes[i].known && shapes[i].middle >= 0 && shapes[i].middle < empty; };
        // A digit that is surely one: not a zero that may be an O.
        auto digit = [&](size_t i) { return i < count && is_digit(text(i)) && !hollow(i); };
        std::vector<size_t> to_o;
        for (size_t i = 0; i < count; i++) {
            if (!hollow(i) || height(i) < 0) continue;
            if (marks_zeros) {
                to_o.push_back(i);
                continue;
            }
            // Before a letter, in a word with no digit in it: not 100, nor
            // E0308, nor the eth0 of a font that leaves its zeros empty...
            size_t first = i, last = i;
            while (first > 0 && (is_letter(text(first - 1)) || is_digit(text(first - 1)))) first--;
            while (last + 1 < count && (is_letter(text(last + 1)) || is_digit(text(last + 1)))) last++;
            bool number = false;
            for (size_t k = first; k <= last; k++) number = number || digit(k);
            // ...nor 0xf.
            bool prefix = among(text(i + 1), "xXbo") && (digit(i + 2) || hollow(i + 2) || among(text(i + 2), "abcdefABCDEF"));
            if (is_letter(text(i + 1)) && !number && !prefix) to_o.push_back(i);
        }
        for (size_t i : to_o) letters[i].text = "O";

        // A bare stroke from above the capitals to below the line.
        for (size_t i = 0; i < count; i++) {
            if (!among(text(i), "Il") || !shapes[i].known || !measured) continue;
            const Shape& shape = shapes[i];
            bool bare = static_cast<float>(shape.x2 - shape.x1) <= 0.3f * cell;
            bool through = static_cast<float>(shape.y2) >= bottom + 0.15f * tall && bottom - static_cast<float>(shape.y1) >= tall;
            if (bare && through) letters[i].text = "|";
        }

        // A backtick: a small stroke at the top of the line that leans down
        // from the left, which is read as an apostrophe or an accent, or not
        // read at all.
        auto backtick = [&](int x1, int y1, int x2, int y2) {
            float w = static_cast<float>(x2 - x1), h = static_cast<float>(y2 - y1);
            if (!measured || w < 3 || h < 3 || h > 0.45f * tall || w > 0.6f * cell) return false;
            if (static_cast<float>(y2) > bottom - 0.55f * small || static_cast<float>(y1) < bottom - 1.35f * tall) return false;
            // The middle of the ink in its lower half, less that in its upper.
            double sums[2] = {0, 0}, weights[2] = {0, 0};
            for (int y = y1; y < y2; y++)
                for (int x = x1; x < x2; x++) {
                    int v = ink.data[static_cast<size_t>(y) * ink.width + x];
                    if (v < 64) continue;
                    int half = (y - y1) * 2 >= y2 - y1;
                    sums[half] += v * (x + 0.5);
                    weights[half] += v;
                }
            if (weights[0] == 0 || weights[1] == 0) return false;
            return sums[1] / weights[1] - sums[0] / weights[0] >= 0.2 * w;
        };
        for (size_t i = 0; i < count; i++) {
            if (text(i) == " " || is_letter(text(i)) || is_digit(text(i)) || !shapes[i].known) continue;
            if (backtick(shapes[i].x1, shapes[i].y1, shapes[i].x2, shapes[i].y2)) letters[i].text = "`";
        }
        for (const Blob& blob : blobs_of(ink.width, ink.height, [&](int x, int y) { return ink.data[static_cast<size_t>(y) * ink.width + x] >= 128 ? 1 : 0; })) {
            if (!backtick(blob.x1, blob.y1, blob.x2, blob.y2)) continue;
            // Only where nothing was read: no letter within half a cell.
            bool read = false;
            size_t before = 0;
            for (size_t i = 0; i < letters.size(); i++) {
                float m = letters[i].middle;
                if (m < (blob.x1 + blob.x2) / 2.0f) before = i + 1;
                if (letters[i].text == " ") continue;
                float away = m < blob.x1 ? blob.x1 - m : m >= blob.x2 ? m - blob.x2 : 0;
                read = read || away <= cell / 2;
            }
            if (read) continue;
            letters.insert(letters.begin() + static_cast<long>(before), {"`", 1, (blob.x1 + blob.x2) / 2.0f});
            shapes.insert(shapes.begin() + static_cast<long>(before), {true, blob.x1, blob.y1, blob.x2, blob.y2, -1});
        }

        // Underscores side by side run into one stroke under the line, which
        // is read as one underscore, as a comma or a dash, or not at all: it
        // is as many underscores as the cells it lies under. A column is
        // such a stroke's if all its ink is below the letters.
        if (measured) {
            const int below = static_cast<int>(std::lround(bottom));
            auto at = [&](int x, int y) {
                return x >= 0 && x < ink.width && y >= 0 && y < ink.height ? ink.data[static_cast<size_t>(y) * ink.width + x] : 0;
            };
            auto bare = [&](int x) {
                bool under = false;
                for (int y = 0; y < ink.height; y++) {
                    if (at(x, y) < 64) continue;
                    if (y < below) return false;
                    under = true;
                }
                return under;
            };
            std::vector<Span> strokes;
            for (int x = 0; x < ink.width;) {
                if (!bare(x)) {
                    x++;
                    continue;
                }
                int x1 = x;
                while (x < ink.width && bare(x)) x++;
                strokes.push_back({x1, x});
            }
            // From the right, so that the letters before a stroke keep their
            // places while it is put right.
            for (auto stroke = strokes.rbegin(); stroke != strokes.rend(); ++stroke) {
                float length = static_cast<float>(stroke->x2 - stroke->x1);
                if (length < 0.5f * cell) continue;
                // A stroke that goes on under the letters either side of it
                // is an underline, seen where a space is: under the columns
                // of the next cell each way that have a letter in them.
                int centre = (stroke->x1 + stroke->x2) / 2, row = below;
                for (int y = below; y < ink.height; y++)
                    if (at(centre, y) > at(centre, row)) row = y;
                int lettered = 0, underlined = 0;
                for (int x = stroke->x1 - static_cast<int>(cell); x < stroke->x2 + static_cast<int>(cell); x++) {
                    if (x >= stroke->x1 && x < stroke->x2) continue;
                    bool letter = false;
                    for (int y = 0; y < below && !letter; y++) letter = at(x, y) >= 128;
                    lettered += letter;
                    underlined += letter && at(x, row) >= 64;
                }
                if (underlined * 2 > lettered) continue;
                // What was read along it makes way, unless that is a letter.
                size_t first = letters.size(), last = 0;
                bool marks = true;
                for (size_t i = 0; i < letters.size(); i++) {
                    float m = letters[i].middle;
                    if (m < static_cast<float>(stroke->x1) - 0.3f * cell || m >= static_cast<float>(stroke->x2) + 0.3f * cell) continue;
                    // Not a neighbour with ink of its own beside the stroke,
                    // put a little out of its place.
                    if (shapes[i].known && (shapes[i].x2 <= stroke->x1 || shapes[i].x1 >= stroke->x2)) continue;
                    marks = marks && !is_letter(letters[i].text) && !is_digit(letters[i].text);
                    first = std::min(first, i);
                    last = i + 1;
                }
                if (!marks) continue;
                if (first > last) {
                    first = 0;
                    while (first < letters.size() && letters[first].middle < static_cast<float>(stroke->x1)) first++;
                    last = first;
                }
                int underscores = std::max(1, static_cast<int>(std::lround(length / cell)));
                letters.erase(letters.begin() + static_cast<long>(first), letters.begin() + static_cast<long>(last));
                shapes.erase(shapes.begin() + static_cast<long>(first), shapes.begin() + static_cast<long>(last));
                for (int k = 0; k < underscores; k++) {
                    letters.insert(letters.begin() + static_cast<long>(first) + k, {"_", 1, static_cast<float>(stroke->x1) + (k + 0.5f) * length / underscores});
                    shapes.insert(shapes.begin() + static_cast<long>(first) + k, Shape{});
                }
            }
        }
    }

    // Letters a whole cell apart with nothing read between them.
    std::vector<Letter> spaced;
    for (size_t i = 0; i < letters.size(); i++) {
        if (monospaced && i > 0 && letters[i].text != " " && letters[i - 1].text != " " && shapes[i].known && shapes[i - 1].known
            && static_cast<float>(shapes[i].x1 - shapes[i - 1].x2) >= 0.93f * cell)
            spaced.push_back({" ", 1, (shapes[i].x1 + shapes[i - 1].x2) / 2.0f});
        spaced.push_back(letters[i]);
    }

    // One space between words, and none at either end.
    std::vector<Letter> tidy;
    for (const Letter& letter : spaced) {
        if (letter.text == " " && (tidy.empty() || tidy.back().text == " ")) continue;
        tidy.push_back(letter);
    }
    if (!tidy.empty() && tidy.back().text == " ") tidy.pop_back();
    return tidy;
}

Queue::Queue(std::vector<int> costs, int fast) : costs(std::move(costs)), front(0), left(0), fast(fast) {
    order.resize(this->costs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return this->costs[a] > this->costs[b]; });
    back = order.size();
    for (int cost : this->costs) left += cost;
}

bool Queue::empty() {
    std::lock_guard<std::mutex> lock(mutex);
    return front >= back;
}

int Queue::take(bool on_fast_core) {
    // A slow core takes this many times as long over a piece as a fast one.
    const int slower = 3;
    std::lock_guard<std::mutex> lock(mutex);
    if (front >= back) return -1;
    int piece;
    if (on_fast_core || fast < 1) {
        piece = order[front++];
    } else {
        piece = order[back - 1];
        if (static_cast<long>(costs[piece]) * slower >= left / fast) return -1;
        back--;
    }
    left -= costs[piece];
    return piece;
}

std::vector<Box> text_boxes(const float* map, int width, int height, const DetectOptions& options) {
    std::vector<Blob> blobs = blobs_of(width, height, [&](int x, int y) {
        float p = map[static_cast<size_t>(y) * width + x];
        return p > options.pixel ? p : 0.0f;
    });
    std::vector<Box> boxes;
    for (const Blob& b : blobs) {
        int w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (std::min(w, h) < options.thinnest) continue;
        float score = static_cast<float>(b.sum / b.pixels);
        if (score < options.box) continue;
        float grow = static_cast<float>(w) * h * options.unclip / (2.0f * (w + h));
        boxes.push_back({
            std::max(b.x1 - grow, 0.0f),
            std::max(b.y1 - grow, 0.0f),
            std::min(b.x2 + grow, static_cast<float>(width)),
            std::min(b.y2 + grow, static_cast<float>(height)),
            score,
        });
    }
    return boxes;
}

std::vector<Box> join_boxes(std::vector<Box> boxes) {
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.x1 < b.x1; });
    // Each box into the one it follows, if any, until none follows another.
    for (bool joined = true; joined;) {
        joined = false;
        for (size_t i = 0; i < boxes.size() && !joined; i++)
            for (size_t j = i + 1; j < boxes.size() && !joined; j++) {
                Box& a = boxes[i];
                const Box& b = boxes[j];
                float shorter = std::min(a.y2 - a.y1, b.y2 - b.y1), taller = std::max(a.y2 - a.y1, b.y2 - b.y1);
                float beside = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
                if (beside < 0.7f * taller || b.x1 - a.x2 > 0.5f * shorter || b.x1 < a.x1) continue;
                a = {a.x1, std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2), std::min(a.score, b.score)};
                boxes.erase(boxes.begin() + static_cast<long>(j));
                joined = true;
            }
    }
    return boxes;
}

std::vector<Glyph> read_glyphs(const float* output, int steps, int classes) {
    std::vector<Glyph> glyphs;
    int previous = 0;
    for (int t = 0; t < steps; t++) {
        const float* row = output + static_cast<size_t>(t) * classes;
        int best = static_cast<int>(std::max_element(row, row + classes) - row);
        if (best != 0) {
            if (best == previous) {
                glyphs.back().last = t;
                glyphs.back().confidence = std::max(glyphs.back().confidence, row[best]);
            } else {
                glyphs.push_back({best, row[best], t, t});
            }
        }
        previous = best;
    }
    return glyphs;
}

std::vector<std::vector<int>> rows_of(const std::vector<Box>& boxes) {
    std::vector<int> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    auto middle = [&](int i) { return (boxes[i].y1 + boxes[i].y2) / 2; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return middle(a) < middle(b); });
    // A row is its first box: the others are measured against that one, so
    // a row does not drift down a column of text.
    std::vector<std::vector<int>> rows;
    for (int i : order) {
        bool placed = false;
        for (auto& row : rows) {
            const Box& first = boxes[row[0]];
            if (middle(i) > first.y1 && middle(i) < first.y2 && middle(row[0]) > boxes[i].y1 && middle(row[0]) < boxes[i].y2) {
                row.push_back(i);
                placed = true;
                break;
            }
        }
        if (!placed) rows.push_back({i});
    }
    for (auto& row : rows)
        std::stable_sort(row.begin(), row.end(), [&](int a, int b) { return boxes[a].x1 < boxes[b].x1; });
    return rows;
}

namespace {

void append_number(std::string& out, float value) {
    char text[32];
    int length = std::snprintf(text, sizeof text, "%.2f", static_cast<double>(value));
    // "12.50" is 12.5 and "3.00" is 3.
    while (length > 0 && text[length - 1] == '0') length--;
    if (length > 0 && text[length - 1] == '.') length--;
    out.append(text, static_cast<size_t>(length));
}

void append_string(std::string& out, const std::string& text) {
    out += '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char escape[8];
                    std::snprintf(escape, sizeof escape, "\\u%04x", c);
                    out += escape;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

}  // namespace

std::string to_json(const std::vector<std::vector<Word>>& lines) {
    std::string out = "[";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) out += ',';
        out += '[';
        for (size_t j = 0; j < lines[i].size(); j++) {
            const Word& word = lines[i][j];
            if (j > 0) out += ',';
            out += "{\"text\":";
            append_string(out, word.text);
            out += ",\"x\":";
            append_number(out, word.x);
            out += ",\"y\":";
            append_number(out, word.y);
            out += ",\"w\":";
            append_number(out, word.w);
            out += ",\"h\":";
            append_number(out, word.h);
            out += '}';
        }
        out += ']';
    }
    out += ']';
    return out;
}

}  // namespace wlfind
