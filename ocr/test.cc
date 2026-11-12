// The parts of wl-find-ocr that need no model: `meson test` runs them.
#include "ocr.hh"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wlfind;

namespace {

int failures = 0;

struct Test {
    const char* name;
    std::function<void()> run;
};

std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

struct Register {
    Register(const char* name, std::function<void()> run) { tests().push_back({name, std::move(run)}); }
};

#define TEST(name)                                         \
    static void test_##name();                             \
    static Register register_##name(#name, test_##name);   \
    static void test_##name()

template <typename A, typename B>
void expect_eq(const A& got, const B& want, const char* expression, const char* file, int line) {
    if (got == want) return;
    std::ostringstream out;
    out << file << ":" << line << ": " << expression << " is " << got << ", not " << want;
    std::fprintf(stderr, "  %s\n", out.str().c_str());
    failures++;
}

void expect_near(double got, double want, double within, const char* expression, const char* file, int line) {
    if (std::fabs(got - want) <= within) return;
    std::fprintf(stderr, "  %s:%d: %s is %g, not %g\n", file, line, expression, got, want);
    failures++;
}

#define EXPECT_EQ(got, want) expect_eq((got), (want), #got, __FILE__, __LINE__)
#define EXPECT_NEAR(got, want, within) expect_near((got), (want), (within), #got, __FILE__, __LINE__)
#define EXPECT_THROWS(expression)                                                         \
    do {                                                                                  \
        bool threw = false;                                                               \
        try {                                                                             \
            expression;                                                                   \
        } catch (const std::runtime_error&) {                                             \
            threw = true;                                                                 \
        }                                                                                 \
        if (!threw) {                                                                     \
            std::fprintf(stderr, "  %s:%d: %s did not throw\n", __FILE__, __LINE__, #expression); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

// A pipe that holds `bytes`, closed for writing: what read_ppm reads.
int pipe_of(const std::string& bytes) {
    int ends[2];
    if (pipe(ends) != 0) throw std::runtime_error("pipe");
    if (write(ends[1], bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) throw std::runtime_error("write");
    close(ends[1]);
    return ends[0];
}

Image ppm(const std::string& bytes) {
    int fd = pipe_of(bytes);
    try {
        Image image = read_ppm(fd);
        close(fd);
        return image;
    } catch (...) {
        close(fd);
        throw;
    }
}

Image grey(int width, int height, std::vector<int> values) {
    Image image{width, height, {}};
    for (int v : values)
        for (int c = 0; c < 3; c++) image.data.push_back(static_cast<uint8_t>(v));
    return image;
}

// ---- read_ppm

TEST(ppm_is_read_as_grim_writes_it) {
    Image image = ppm(std::string("P6\n2 1\n255\n") + "\x01\x02\x03\x0a\x0b\x0c");
    EXPECT_EQ(image.width, 2);
    EXPECT_EQ(image.height, 1);
    EXPECT_EQ(image.data.size(), 6u);
    if (image.data.size() != 6) return;
    EXPECT_EQ(static_cast<int>(image.data[0]), 1);
    // A pixel byte that is a newline is no part of the header.
    EXPECT_EQ(static_cast<int>(image.data[3]), 10);
    EXPECT_EQ(static_cast<int>(image.data[5]), 12);
}

TEST(ppm_header_may_hold_comments_and_any_space) {
    Image image = ppm(std::string("P6 # made by hand\n1\t1\n# depth\n255\n") + "abc");
    EXPECT_EQ(image.width, 1);
    EXPECT_EQ(image.height, 1);
    EXPECT_EQ(image.data.size(), 3u);
    if (image.data.size() != 3) return;
    EXPECT_EQ(static_cast<int>(image.data[2]), static_cast<int>('c'));
}

TEST(ppm_of_another_kind_is_refused) {
    EXPECT_THROWS(ppm(std::string("P5\n1 1\n255\n") + "a"));
    EXPECT_THROWS(ppm(std::string("P6\n1 1\n65535\n") + "aabbcc"));
    EXPECT_THROWS(ppm(""));
    EXPECT_THROWS(ppm("P6\n0 0\n255\n"));
}

TEST(ppm_cut_short_is_refused) {
    EXPECT_THROWS(ppm(std::string("P6\n2 1\n255\n") + "abcde"));
}

// ---- to_planes

TEST(planes_are_blue_green_red_from_minus_one_to_one) {
    Image image{2, 1, {255, 0, 0, 0, 0, 255}};
    float planes[6] = {9, 9, 9, 9, 9, 9};
    to_planes(image, {0, 0, 2, 1}, 2, 1, planes);
    // blue: none in the red pixel, all in the blue one
    EXPECT_NEAR(planes[0], -1, 1e-6);
    EXPECT_NEAR(planes[1], 1, 1e-6);
    // green: none in either
    EXPECT_NEAR(planes[2], -1, 1e-6);
    EXPECT_NEAR(planes[3], -1, 1e-6);
    // red
    EXPECT_NEAR(planes[4], 1, 1e-6);
    EXPECT_NEAR(planes[5], -1, 1e-6);
}

TEST(planes_shrunk_average_the_pixels_they_cover) {
    Image image = grey(4, 2, {0, 40, 80, 120, 20, 60, 100, 140});
    float planes[6] = {9, 9, 9, 9, 9, 9};
    to_planes(image, {0, 0, 4, 2}, 2, 1, planes);
    // (0 + 40 + 20 + 60) / 4 = 30 and (80 + 120 + 100 + 140) / 4 = 110
    EXPECT_NEAR(planes[0], 30 / 255.0 * 2 - 1, 1e-5);
    EXPECT_NEAR(planes[1], 110 / 255.0 * 2 - 1, 1e-5);
}

TEST(planes_shrunk_by_a_fraction_weigh_pixels_by_their_share) {
    // Three pixels into two: each takes one whole pixel and half the middle one.
    Image image = grey(3, 1, {0, 90, 240});
    float planes[6] = {9, 9, 9, 9, 9, 9};
    to_planes(image, {0, 0, 3, 1}, 2, 1, planes);
    EXPECT_NEAR(planes[0], (0 + 45) / 1.5 / 255.0 * 2 - 1, 1e-5);
    EXPECT_NEAR(planes[1], (45 + 240) / 1.5 / 255.0 * 2 - 1, 1e-5);
}

TEST(planes_enlarged_interpolate_between_pixels) {
    Image image = grey(2, 1, {0, 255});
    float planes[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    to_planes(image, {0, 0, 2, 1}, 4, 1, planes);
    // Pixel centres at -0.25, 0.25, 0.75 and 1.25 of the two.
    EXPECT_NEAR(planes[0], -1, 1e-5);
    EXPECT_NEAR(planes[1], -0.5, 1e-5);
    EXPECT_NEAR(planes[2], 0.5, 1e-5);
    EXPECT_NEAR(planes[3], 1, 1e-5);
}

TEST(planes_come_from_the_rectangle_asked_for) {
    Image image = grey(3, 2, {10, 20, 30, 40, 50, 60});
    float planes[6] = {9, 9, 9, 9, 9, 9};
    to_planes(image, {1, 1, 3, 2}, 2, 1, planes);
    EXPECT_NEAR(planes[0], 50 / 255.0 * 2 - 1, 1e-5);
    EXPECT_NEAR(planes[1], 60 / 255.0 * 2 - 1, 1e-5);
}

TEST(planes_can_be_made_a_band_of_rows_at_a_time) {
    // Four rows into two: 15 and 35. Asked for the second alone, the first
    // is left as it was.
    Image image = grey(2, 4, {10, 10, 20, 20, 30, 30, 40, 40});
    float planes[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    to_planes(image, {0, 0, 2, 4}, 2, 2, planes, 1, 2);
    EXPECT_NEAR(planes[0], 9, 1e-6);
    EXPECT_NEAR(planes[1], 9, 1e-6);
    EXPECT_NEAR(planes[2], 35 / 255.0 * 2 - 1, 1e-5);
    EXPECT_NEAR(planes[3], 35 / 255.0 * 2 - 1, 1e-5);
    // ...in each of the three planes.
    EXPECT_NEAR(planes[4], 9, 1e-6);
    EXPECT_NEAR(planes[6], 35 / 255.0 * 2 - 1, 1e-5);
    EXPECT_NEAR(planes[10], 35 / 255.0 * 2 - 1, 1e-5);
}

// ---- ink_of

// An image of one colour with rectangles of others on it.
struct Patch {
    int x1, y1, x2, y2;
    int r, g, b;
};

Image painted(int width, int height, Patch ground, std::vector<Patch> patches) {
    Image image{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 3)};
    patches.insert(patches.begin(), {0, 0, width, height, ground.r, ground.g, ground.b});
    for (const Patch& p : patches)
        for (int y = p.y1; y < p.y2; y++)
            for (int x = p.x1; x < p.x2; x++) {
                uint8_t* at = &image.data[(static_cast<size_t>(y) * width + x) * 3];
                at[0] = static_cast<uint8_t>(p.r);
                at[1] = static_cast<uint8_t>(p.g);
                at[2] = static_cast<uint8_t>(p.b);
            }
    return image;
}

Patch shade(int v) { return {0, 0, 0, 0, v, v, v}; }

int ink_at(const Ink& ink, int x, int y) { return ink.data[static_cast<size_t>(y) * ink.width + x]; }

TEST(ink_is_what_differs_from_the_ground_dark_on_light) {
    Image image = painted(12, 6, shade(240), {{4, 2, 6, 4, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 12, 6});
    EXPECT_EQ(ink.width, 12);
    EXPECT_EQ(ink.height, 6);
    EXPECT_EQ(ink.legible, true);
    EXPECT_EQ(ink.data.size(), 72u);
    if (ink.data.size() != 72) return;
    EXPECT_EQ(ink_at(ink, 4, 2), 255);
    EXPECT_EQ(ink_at(ink, 5, 3), 255);
    EXPECT_EQ(ink_at(ink, 3, 2), 0);
    EXPECT_EQ(ink_at(ink, 0, 0), 0);
}

TEST(ink_is_the_same_light_on_dark) {
    Image image = painted(12, 6, shade(20), {{4, 2, 6, 4, 220, 220, 220}});
    Ink ink = ink_of(image, {0, 0, 12, 6});
    EXPECT_EQ(ink.legible, true);
    EXPECT_EQ(ink.data.size(), 72u);
    if (ink.data.size() != 72) return;
    EXPECT_EQ(ink_at(ink, 4, 2), 255);
    EXPECT_EQ(ink_at(ink, 3, 2), 0);
}

TEST(ink_of_a_colour_as_bright_as_its_ground_still_shows) {
    // Pure blue on grey of the same lightness: only the colour differs.
    Image image = painted(12, 6, {0, 0, 0, 0, 30, 30, 30}, {{4, 2, 6, 4, 30, 30, 230}});
    Ink ink = ink_of(image, {0, 0, 12, 6});
    EXPECT_EQ(ink.legible, true);
    EXPECT_EQ(ink.data.size(), 72u);
    if (ink.data.size() != 72) return;
    EXPECT_EQ(ink_at(ink, 4, 2), 255);
    EXPECT_EQ(ink_at(ink, 7, 2), 0);
}

TEST(ink_of_a_smoothed_edge_is_part_ink) {
    // A pixel half way between the ground (240) and the ink (40).
    Image image = painted(12, 6, shade(240), {{4, 2, 6, 4, 40, 40, 40}, {6, 2, 7, 4, 140, 140, 140}});
    Ink ink = ink_of(image, {0, 0, 12, 6});
    EXPECT_EQ(ink.data.size(), 72u);
    if (ink.data.size() != 72) return;
    EXPECT_NEAR(ink_at(ink, 6, 2), 127.5, 1);
}

TEST(ink_too_faint_to_tell_is_not_legible) {
    Image image = painted(12, 6, shade(100), {{4, 2, 6, 4, 108, 108, 108}});
    EXPECT_EQ(ink_of(image, {0, 0, 12, 6}).legible, false);
    Image blank = painted(12, 6, shade(100), {});
    EXPECT_EQ(ink_of(blank, {0, 0, 12, 6}).legible, false);
}

TEST(ink_is_measured_in_the_box_asked_for) {
    // The left half is black, but the box is the right half: its ground is
    // the white there, and the black next door no part of it.
    Image image = painted(24, 6, shade(240), {{0, 0, 12, 6, 0, 0, 0}, {16, 2, 18, 4, 40, 40, 40}});
    Ink ink = ink_of(image, {12, 0, 24, 6});
    EXPECT_EQ(ink.width, 12);
    EXPECT_EQ(ink.data.size(), 72u);
    if (ink.data.size() != 72) return;
    EXPECT_EQ(ink_at(ink, 4, 2), 255);
    EXPECT_EQ(ink_at(ink, 0, 0), 0);
}

TEST(ink_of_the_lines_above_and_below_is_wiped) {
    // Four letters on rows 4 to 7, the foot of a letter above them coming in
    // from the top edge and the head of one below from the bottom edge.
    Image image = painted(30, 12, shade(240), {
        {4, 4, 8, 8, 40, 40, 40}, {10, 4, 14, 8, 40, 40, 40}, {16, 4, 20, 8, 40, 40, 40}, {22, 4, 26, 8, 40, 40, 40},
        {12, 0, 14, 2, 40, 40, 40}, {20, 10, 22, 12, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 30, 12});
    EXPECT_EQ(ink.data.size(), 360u);
    if (ink.data.size() != 360) return;
    EXPECT_EQ(ink_at(ink, 12, 0), 0);
    EXPECT_EQ(ink_at(ink, 20, 11), 0);
    EXPECT_EQ(ink_at(ink, 5, 5), 255);
    EXPECT_EQ(ink.top, 4);
    EXPECT_EQ(ink.bottom, 8);
    // The other lines' ink is on rows 0 and 1, and 10 and 11.
    EXPECT_EQ(ink.above, 2);
    EXPECT_EQ(ink.below, 10);
}

TEST(a_box_with_one_line_in_it_is_that_lines_from_top_to_bottom) {
    Image image = painted(30, 12, shade(240), {{4, 4, 8, 8, 40, 40, 40}, {10, 4, 14, 8, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 30, 12});
    EXPECT_EQ(ink.above, 0);
    EXPECT_EQ(ink.below, 12);
    // ...as is one too faint to tell.
    Ink faint = ink_of(painted(30, 12, shade(100), {}), {0, 0, 30, 12});
    EXPECT_EQ(faint.above, 0);
    EXPECT_EQ(faint.below, 12);
}

TEST(ink_of_a_dot_over_a_letter_is_kept) {
    // An "i": its stem on rows 5 to 8 and its dot on rows 2 and 3, clear of
    // the edges of the box. Beside it an "l", to row 1.
    Image image = painted(30, 12, shade(240), {{10, 5, 12, 9, 40, 40, 40}, {10, 2, 12, 4, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 30, 12});
    EXPECT_EQ(ink.data.size(), 360u);
    if (ink.data.size() != 360) return;
    EXPECT_EQ(ink_at(ink, 10, 2), 255);
    EXPECT_EQ(ink.top, 2);
    EXPECT_EQ(ink.bottom, 9);
}

TEST(ink_of_a_rule_is_wiped) {
    // Three letters, underlined: the rule is 26 pixels long, in a box 12
    // tall, which no stroke of a letter is.
    Image image = painted(30, 12, shade(240), {
        {4, 3, 8, 8, 40, 40, 40}, {10, 3, 14, 8, 40, 40, 40}, {16, 3, 20, 8, 40, 40, 40}, {2, 9, 28, 10, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 30, 12});
    EXPECT_EQ(ink.data.size(), 360u);
    if (ink.data.size() != 360) return;
    // Faint from then on: no part of a letter's shape, but not the bare
    // ground either, where a mark could only have been made up. A row of
    // underscores is such a rule.
    EXPECT_EQ(ink_at(ink, 15, 9), 64);
    EXPECT_EQ(ink_at(ink, 5, 5), 255);
    EXPECT_EQ(ink.bottom, 8);
}

TEST(ink_of_a_stroke_no_longer_than_the_box_is_tall_is_a_letters) {
    // The bar of a wide "T", 10 long in a box 12 tall.
    Image image = painted(30, 12, shade(240), {{6, 3, 16, 4, 40, 40, 40}, {10, 4, 12, 9, 40, 40, 40}});
    Ink ink = ink_of(image, {0, 0, 30, 12});
    EXPECT_EQ(ink.data.size(), 360u);
    if (ink.data.size() != 360) return;
    EXPECT_EQ(ink_at(ink, 7, 3), 255);
    EXPECT_EQ(ink.top, 3);
}

// ---- ink_columns

// Ink with rectangles of one strength each on it.
struct Dab {
    int x1, y1, x2, y2;
    int ink;
};

Ink ink_with(int width, int height, std::vector<Dab> dabs) {
    Ink ink{width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height, 0), true};
    for (const Dab& d : dabs)
        for (int y = d.y1; y < d.y2; y++)
            for (int x = d.x1; x < d.x2; x++) ink.data[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(d.ink);
    return ink;
}

std::string spans_as_text(const std::vector<Span>& spans) {
    std::string text;
    for (const Span& s : spans) text += std::to_string(s.x1) + "-" + std::to_string(s.x2) + " ";
    return text;
}

TEST(columns_of_ink_are_runs_with_gaps_between) {
    // Two letters, and a third that starts in the first column.
    Ink ink = ink_with(20, 8, {{0, 1, 3, 7, 255}, {5, 2, 9, 6, 255}, {12, 0, 13, 8, 255}});
    EXPECT_EQ(spans_as_text(ink_columns(ink)), std::string("0-3 5-9 12-13 "));
}

TEST(columns_take_ink_that_is_at_least_half_there) {
    // The faint column (100 of 255) is a gap, the stronger one (140) is ink.
    Ink ink = ink_with(20, 8, {{2, 1, 4, 7, 255}, {4, 1, 5, 7, 100}, {8, 1, 9, 7, 140}});
    EXPECT_EQ(spans_as_text(ink_columns(ink)), std::string("2-4 8-9 "));
}

TEST(columns_of_no_ink_are_none) { EXPECT_EQ(ink_columns(ink_with(20, 8, {})).size(), 0u); }

// ---- join_pieces

std::string letters_as_text(const std::vector<Letter>& letters) {
    std::string text;
    for (const Letter& l : letters) text += l.text;
    return text;
}

// `text` as letters 10 apart, the first at `from`.
std::vector<Letter> read_as(const std::string& text, float from) {
    std::vector<Letter> letters;
    for (size_t i = 0; i < text.size(); i++) letters.push_back({std::string(1, text[i]), 1, from + 10 * static_cast<float>(i)});
    return letters;
}

TEST(pieces_are_sewn_where_they_agree) {
    // "abcdefgh", cut at 40 and read 20 past it either way: "abcdef" and
    // "cdefgh". Every letter once.
    auto joined = join_pieces({read_as("abcdef", 5), read_as("cdefgh", 25)}, {40}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefgh"));
}

TEST(pieces_agree_on_a_letter_they_place_a_little_apart) {
    // The second piece has every letter 4 further along than the first.
    auto joined = join_pieces({read_as("abcdef", 5), read_as("cdefgh", 29)}, {40}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefgh"));
}

TEST(a_run_of_one_letter_is_sewn_by_where_its_letters_are) {
    // "abllllcd": the four l are alike, and only their places say which of
    // one piece is which of the other.
    auto joined = join_pieces({read_as("abllll", 5), read_as("llllcd", 25)}, {40}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abllllcd"));
}

TEST(a_letter_cut_through_at_the_end_of_a_piece_is_the_other_pieces_to_read) {
    // Each piece ends in the middle of a letter, and misreads it: "abcdet"
    // for abcdef, and "odefgh" for cdefgh. They agree on "de".
    auto joined = join_pieces({read_as("abcdet", 5), read_as("odefgh", 25)}, {40}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefgh"));
}

TEST(a_space_at_a_cut_is_one_space) {
    // "ab cd", cut at the space: "ab c" and "b cd".
    auto joined = join_pieces({read_as("ab c", 5), read_as("b cd", 15)}, {25}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("ab cd"));
}

TEST(a_space_alone_is_nothing_to_sew_at) {
    // "a > b" cut at the ">": each piece has a space, but not the same one,
    // and a space is alike wherever it is. Only the ">" is in both.
    std::vector<std::vector<Letter>> pieces = {
        {{"a", 1, 5}, {" ", 1, 13}, {">", 1, 25}},
        {{">", 1, 25}, {" ", 1, 35}, {"b", 1, 45}}};
    EXPECT_EQ(letters_as_text(join_pieces(pieces, {24}, 10)), std::string("a > b"));
}

TEST(a_letter_two_pieces_read_differently_is_kept_once) {
    // "abc", a letter one piece took for an x at 42 and the other for a y
    // at 38, then "fgh": cut at 40, each has it on the other's side. It is
    // one letter, as its side of the cut has it.
    std::vector<std::vector<Letter>> pieces = {
        {{"a", 1, 5}, {"b", 1, 15}, {"c", 1, 25}, {"x", 1, 42}},
        {{"y", 1, 38}, {"f", 1, 55}, {"g", 1, 65}, {"h", 1, 75}}};
    EXPECT_EQ(letters_as_text(join_pieces(pieces, {40}, 20)), std::string("abcyfgh"));
}

TEST(what_only_one_piece_read_is_kept) {
    // The middle piece read nothing at all. What its neighbours read of it,
    // past their cuts, is all there is: "abcdef" and "ghijkl".
    auto joined = join_pieces({read_as("abcdef", 5), {}, read_as("ghijkl", 65)}, {40, 80}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefghijkl"));
}

TEST(pieces_with_a_blank_between_them_are_words_apart) {
    auto joined = join_pieces({read_as("ab", 5), read_as("cd", 95)}, {55}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("ab cd"));
    // The space is at the cut.
    if (joined.size() == 5) EXPECT_NEAR(joined[2].middle, 55, 1e-6);
}

TEST(pieces_side_by_side_that_agree_on_nothing_are_one_word) {
    // Read with no reach at all: "abcd" and "efgh", 10 apart throughout.
    auto joined = join_pieces({read_as("abcd", 5), read_as("efgh", 45)}, {40}, 0);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefgh"));
}

TEST(a_line_in_three_pieces_is_sewn_twice) {
    auto joined = join_pieces({read_as("abcdef", 5), read_as("cdefghij", 25), read_as("ghijkl", 65)}, {40, 80}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("abcdefghijkl"));
}

TEST(a_line_starts_and_ends_on_a_letter) {
    EXPECT_EQ(letters_as_text(join_pieces({read_as(" a ", 5)}, {}, 20)), std::string("a"));
}

TEST(pieces_with_nothing_read_in_them_leave_no_spaces) {
    auto joined = join_pieces({read_as("a", 5), {}, read_as("b", 145)}, {50, 100}, 20);
    EXPECT_EQ(letters_as_text(joined), std::string("a b"));
}

// ---- words_of
std::string words_as_text(const std::vector<Word>& words) {
    std::ostringstream out;
    for (const Word& w : words) out << w.text << "@" << w.x << "," << w.y << " " << w.w << "x" << w.h << " ";
    return out.str();
}

TEST(a_word_is_boxed_by_the_ink_of_its_letters) {
    // "abc de": letters 8 wide and 2 apart on rows 4 to 15, the words 12
    // apart. The recogniser has their middles a few pixels out.
    Ink ink = ink_with(100, 20, {{10, 4, 18, 16, 255}, {20, 4, 28, 16, 255}, {30, 4, 38, 16, 255}, {50, 4, 58, 16, 255}, {60, 4, 68, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"a", 1, 16}, {"b", 1, 23}, {"c", 1, 31}, {" ", 1, 44}, {"d", 1, 56}, {"e", 1, 62}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("abc@10,4 28x12 de@50,4 18x12 "));
}

TEST(a_letter_in_two_strokes_is_boxed_whole) {
    // A double quote before a letter: two strokes 2 wide and 2 apart, the
    // recogniser's middle between them.
    Ink ink = ink_with(60, 20, {{10, 4, 12, 8, 255}, {14, 4, 16, 8, 255}, {20, 4, 28, 16, 255}, {30, 4, 38, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"\"", 1, 13}, {"a", 1, 24}, {"b", 1, 34}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("\"ab@10,4 28x12 "));
}

TEST(a_word_ends_at_the_gap_before_the_next_wherever_its_last_letter_was_put) {
    // "ab, cd": the comma is 2 wide, at 30, and the recogniser has it at 36,
    // past its own ink. The words still part at the gap between them, from
    // 32 to 46.
    Ink ink = ink_with(100, 20, {{10, 4, 18, 16, 255}, {20, 4, 28, 16, 255}, {30, 12, 32, 18, 255}, {46, 4, 54, 16, 255}, {56, 4, 64, 16, 255}});
    ink.top = 4;
    ink.bottom = 18;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {",", 1, 36}, {" ", 1, 41}, {"c", 1, 50}, {"d", 1, 60}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("ab,@10,4 22x14 cd@46,4 18x14 "));
}

TEST(a_line_ends_at_the_ink_of_its_last_letter_wherever_that_was_put) {
    // "ab." with the full stop at 30, read at 36.
    Ink ink = ink_with(100, 20, {{10, 4, 18, 16, 255}, {20, 4, 28, 16, 255}, {30, 14, 32, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {".", 1, 36}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("ab.@10,4 22x12 "));
}

TEST(a_word_with_no_ink_of_its_own_is_boxed_by_where_its_letters_are) {
    // "ab , cd" as the recogniser has it, the comma against the b on the
    // screen: no gap parts it from "ab", and the one after it is the same gap
    // that parts "ab," from "cd". The comma is where it was read, 10 wide
    // as the letters are apart, not from one side of that gap back to the
    // other.
    Ink ink = ink_with(100, 20, {{10, 4, 18, 16, 255}, {20, 4, 28, 16, 255}, {28, 12, 30, 18, 255}, {46, 4, 54, 16, 255}, {56, 4, 64, 16, 255}});
    ink.top = 4;
    ink.bottom = 18;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {" ", 1, 27}, {",", 1, 29}, {" ", 1, 38}, {"c", 1, 50}, {"d", 1, 60}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("ab@10,4 20x14 ,@24,4 10x14 cd@46,4 18x14 "));
}

TEST(a_word_is_boxed_inside_its_line_wherever_its_letters_were_put) {
    // A mark read past the end of the line, 100 wide: its box ends with the
    // line, and is never the wrong way round.
    Ink ink = ink_with(100, 20, {{10, 4, 18, 16, 255}, {20, 4, 28, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {" ", 1, 60}, {",", 1, 120}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("ab@10,4 18x12 ,@100,4 0x12 "));
}

TEST(a_word_with_no_ink_to_go_by_is_boxed_by_where_its_letters_are) {
    // Letters 10 apart: half a letter beyond the first and the last.
    Ink ink = ink_with(100, 20, {});
    ink.legible = false;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {"c", 1, 34}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("abc@9,0 30x20 "));
}

TEST(a_word_whose_ink_runs_on_past_it_is_boxed_by_where_its_letters_are) {
    // Ink from one side of the line to the other, as under a highlight the
    // ground was not taken for: the ink says nothing of where a word ends.
    Ink ink = ink_with(100, 20, {{0, 4, 100, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"a", 1, 14}, {"b", 1, 24}, {"c", 1, 34}, {" ", 1, 44}, {"d", 1, 54}, {"e", 1, 64}};
    EXPECT_EQ(words_as_text(words_of(letters, ink)), std::string("abc@9,4 30x12 de@49,4 20x12 "));
}

TEST(words_of_no_letters_are_none) {
    EXPECT_EQ(words_of({}, ink_with(10, 10, {})).size(), 0u);
    EXPECT_EQ(words_of({{" ", 1, 5}}, ink_with(10, 10, {})).size(), 0u);
}

// ---- shapes_of

std::string shapes_as_text(const std::vector<Shape>& shapes) {
    std::ostringstream out;
    for (const Shape& s : shapes) {
        if (s.known) out << s.x1 << "," << s.y1 << "-" << s.x2 << "," << s.y2 << " ";
        else out << "? ";
    }
    return out.str();
}

TEST(a_letters_shape_is_the_box_of_its_ink) {
    // A tall letter, a short one and a tall one, and a space after them.
    Ink ink = ink_with(60, 20, {{10, 4, 18, 16, 255}, {20, 8, 28, 16, 255}, {30, 4, 38, 16, 255}});
    auto shapes = shapes_of({{"d", 1, 14}, {"a", 1, 25}, {"b", 1, 33}, {" ", 1, 44}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("10,4-18,16 20,8-28,16 30,4-38,16 ? "));
}

TEST(a_letter_in_two_parts_is_one_shape) {
    // An "i": its dot over its stem, and a letter either side.
    Ink ink = ink_with(60, 20, {{10, 8, 18, 16, 255}, {22, 8, 24, 16, 255}, {22, 4, 24, 6, 255}, {28, 8, 36, 16, 255}});
    auto shapes = shapes_of({{"a", 1, 14}, {"i", 1, 23}, {"n", 1, 32}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("10,8-18,16 22,4-24,16 28,8-36,16 "));
}

TEST(letters_that_touch_have_no_shape_of_their_own) {
    // Two letters in one blob, and a third clear of them.
    Ink ink = ink_with(60, 20, {{10, 8, 28, 16, 255}, {32, 8, 40, 16, 255}});
    auto shapes = shapes_of({{"r", 1, 14}, {"n", 1, 24}, {"a", 1, 36}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("? ? 32,8-40,16 "));
}

TEST(ink_that_no_letter_was_read_at_is_no_letters) {
    // An icon before the text, further from the first letter than letters
    // are from each other.
    Ink ink = ink_with(80, 20, {{0, 4, 8, 16, 255}, {30, 8, 38, 16, 255}, {40, 8, 48, 16, 255}});
    auto shapes = shapes_of({{"a", 1, 34}, {"b", 1, 44}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("30,8-38,16 40,8-48,16 "));
}

TEST(a_letter_read_where_there_is_no_ink_has_no_shape) {
    Ink ink = ink_with(80, 20, {{10, 8, 18, 16, 255}, {20, 8, 28, 16, 255}});
    auto shapes = shapes_of({{"a", 1, 14}, {"b", 1, 24}, {"c", 1, 60}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("10,8-18,16 20,8-28,16 ? "));
    Ink blank = ink_with(80, 20, {});
    blank.legible = false;
    EXPECT_EQ(shapes_as_text(shapes_of({{"a", 1, 14}}, blank)), std::string("? "));
}

// A ring 10 wide and 12 tall at `x`, its stroke 2 thick: the letter O.
std::vector<Dab> ring(int x) {
    return {{x, 4, x + 10, 6, 255}, {x, 14, x + 10, 16, 255}, {x, 4, x + 2, 16, 255}, {x + 8, 4, x + 10, 16, 255}};
}

TEST(the_middle_of_a_hollow_letter_is_empty_and_of_a_dotted_zero_inked) {
    std::vector<Dab> dabs = ring(10);
    for (const Dab& d : ring(30)) dabs.push_back(d);
    // The dot of the zero.
    dabs.push_back({34, 9, 36, 11, 255});
    Ink ink = ink_with(60, 20, dabs);
    auto shapes = shapes_of({{"O", 1, 15}, {"0", 1, 35}}, ink);
    EXPECT_EQ(shapes_as_text(shapes), std::string("10,4-20,16 30,4-40,16 "));
    if (shapes.size() != 2) return;
    EXPECT_NEAR(shapes[0].middle, 0, 1e-6);
    EXPECT_NEAR(shapes[1].middle, 1, 1e-6);
}

TEST(a_letter_with_no_hollow_has_no_middle) {
    // A solid stem, as of an l.
    Ink ink = ink_with(60, 20, {{10, 4, 13, 16, 255}});
    auto shapes = shapes_of({{"l", 1, 11}}, ink);
    EXPECT_EQ(shapes.size(), 1u);
    if (shapes.size() == 1) EXPECT_NEAR(shapes[0].middle, -1, 1e-6);
}

// ---- proofread

// A line as typed in a monospaced font and as read: a cell every 12 pixels,
// `read[i]` the letter read in cell i ('_' for none) and `drawn[i]` what is
// there: 's' a small letter and 'T' a tall one (solid, 8 wide, on rows 8 or
// 4 to 15), 'o' a small ring and 'O' a tall one, '0' a tall ring with a dot
// in it, '|' a bare stroke from row 1 to 19, '`' a small stroke at the top
// that slants down from the left and '\'' an upright one, 'f' a faint mark
// there, 'B' a block the whole height of the box, '_' a stroke under the
// whole cell, below the letters, and 'n' one no wider than a letter, 'u' a
// small letter with a stroke under it, and '.' nothing. In `read`, '=' is an
// underscore read there.
struct Typed {
    Ink ink;
    std::vector<Letter> letters;
};

Typed typed(const std::string& read, const std::string& drawn) {
    std::vector<Dab> dabs;
    std::vector<Letter> letters;
    for (size_t i = 0; i < drawn.size(); i++) {
        int x = static_cast<int>(i) * 12 + 2;
        auto ring = [&](int top) {
            dabs.push_back({x, top, x + 8, top + 2, 255});
            dabs.push_back({x, 14, x + 8, 16, 255});
            dabs.push_back({x, top, x + 2, 16, 255});
            dabs.push_back({x + 6, top, x + 8, 16, 255});
        };
        switch (drawn[i]) {
            case 's': dabs.push_back({x, 8, x + 8, 16, 255}); break;
            case 'T': dabs.push_back({x, 4, x + 8, 16, 255}); break;
            case 'o': ring(8); break;
            case 'O': ring(4); break;
            case '0':
                ring(4);
                dabs.push_back({x + 3, 9, x + 5, 11, 255});
                break;
            case '|': dabs.push_back({x + 3, 1, x + 5, 20, 255}); break;
            case '`':
                for (int k = 0; k < 4; k++) dabs.push_back({x + 1 + k, 3 + k, x + 3 + k, 4 + k, 255});
                break;
            case '\'': dabs.push_back({x + 3, 3, x + 5, 7, 255}); break;
            case 'f': dabs.push_back({x + 3, 3, x + 5, 7, 100}); break;
            case 'B': dabs.push_back({x, 0, x + 8, 22, 255}); break;
            case '_': dabs.push_back({x - 2, 17, x + 10, 19, 255}); break;
            case 'n': dabs.push_back({x, 17, x + 8, 19, 255}); break;
            case 'u':
                dabs.push_back({x, 8, x + 8, 16, 255});
                dabs.push_back({x - 2, 17, x + 10, 19, 255});
                break;
            default: break;
        }
        if (read[i] != '_') letters.push_back({std::string(1, read[i] == '=' ? '_' : read[i]), 1, static_cast<float>(x + 4)});
    }
    Typed line{ink_with(static_cast<int>(drawn.size()) * 12 + 4, 22, dabs), letters};
    line.ink.top = 1;
    line.ink.bottom = 20;
    return line;
}

std::string proofread_as_text(const std::string& read, const std::string& drawn) {
    Typed line = typed(read, drawn);
    return letters_as_text(proofread(line.letters, line.ink));
}

TEST(a_letter_as_tall_as_the_capitals_is_a_capital) {
    // "a" is a small letter and "b" as tall as a capital; the "s" read after
    // them is as tall as the "b".
    EXPECT_EQ(proofread_as_text("abs", "sTT"), std::string("abS"));
    EXPECT_EQ(proofread_as_text("abcosuvwxz", "sTTTTTTTTT"), std::string("abCOSUVWXZ"));
}

TEST(a_capital_no_taller_than_the_small_letters_is_a_small_one) {
    EXPECT_EQ(proofread_as_text("abS", "sTs"), std::string("abs"));
    EXPECT_EQ(proofread_as_text("abCOSUVWXZ", "sTssssssss"), std::string("abcosuvwxz"));
}

TEST(a_letter_and_its_capital_of_two_shapes_are_told_apart_by_the_recogniser) {
    // An "m" as tall as a capital is still what was read: "M" is another
    // shape, which the recogniser would have seen.
    EXPECT_EQ(proofread_as_text("abm", "sTT"), std::string("abm"));
    EXPECT_EQ(proofread_as_text("abM", "sTs"), std::string("abM"));
}

TEST(ink_taller_than_any_letter_is_not_a_letters_height) {
    // The end of a button, or an icon, in one blob with the last letter:
    // nearly twice as tall as the capitals. The "x" is no capital for it.
    EXPECT_EQ(proofread_as_text("abx", "sTB"), std::string("abx"));
}

TEST(with_nothing_to_measure_it_against_a_letter_stays_as_read) {
    // No letter here is surely small or surely tall.
    EXPECT_EQ(proofread_as_text("so", "Ts"), std::string("so"));
    EXPECT_EQ(proofread_as_text("SO", "sT"), std::string("SO"));
}

TEST(with_only_capitals_to_measure_against_small_letters_are_three_quarters_as_tall) {
    // "B" is 12 tall. An "s" 8 tall is a small letter, and one 12 tall a
    // capital.
    EXPECT_EQ(proofread_as_text("BS", "Ts"), std::string("Bs"));
    EXPECT_EQ(proofread_as_text("Bs", "TT"), std::string("BS"));
}

TEST(with_only_small_letters_to_measure_against_capitals_are_a_third_taller) {
    EXPECT_EQ(proofread_as_text("as", "sT"), std::string("aS"));
    EXPECT_EQ(proofread_as_text("aS", "ss"), std::string("as"));
}

TEST(a_letter_that_touches_the_next_stays_as_read) {
    // "as" in one blob: neither has a height of its own.
    Ink ink = ink_with(60, 22, {{2, 8, 10, 16, 255}, {14, 4, 22, 16, 255}, {26, 4, 46, 16, 255}});
    ink.top = 4;
    ink.bottom = 16;
    std::vector<Letter> letters = {{"a", 1, 6}, {"b", 1, 18}, {"s", 1, 31}, {"s", 1, 41}};
    EXPECT_EQ(letters_as_text(proofread(letters, ink)), std::string("abss"));
}

TEST(in_a_monospaced_line_a_zero_with_an_empty_middle_before_a_letter_is_an_O) {
    EXPECT_EQ(proofread_as_text("abcdefghnix0s", "sTsTsTsTsssOs"), std::string("abcdefghnixOs"));
    EXPECT_EQ(proofread_as_text("abcdefgh 0cr", "sTsTsTsT.Oss"), std::string("abcdefgh Ocr"));
}

TEST(a_zero_with_an_empty_middle_at_the_end_of_a_word_stays_unless_the_line_marks_its_zeros) {
    // "eth0", in a font that may leave its zeros empty.
    EXPECT_EQ(proofread_as_text("abcdefgh[inf0]", "sTsTsTsTTssTOT"), std::string("abcdefgh[inf0]"));
    // With a dotted zero further along the line, the empty one is an O.
    EXPECT_EQ(proofread_as_text("abcdefgh[inf0] 10", "sTsTsTsTTssTOT.T0"), std::string("abcdefgh[infO] 10"));
}

TEST(a_small_zero_among_digits_stays_a_zero) {
    // Old-style figures: a zero no taller than the small letters, in 2010.
    EXPECT_EQ(proofread_as_text("abcdefgh2010", "sTsTsTsTToTo"), std::string("abcdefgh2010"));
}

TEST(a_zero_with_an_empty_middle_in_a_number_stays_a_zero) {
    // A font that does not mark its zero: this may be 100, v0.1 or 0xf.
    EXPECT_EQ(proofread_as_text("abcdefgh100", "sTsTsTsTTOO"), std::string("abcdefgh100"));
    EXPECT_EQ(proofread_as_text("abcdefghv0.1", "sTsTsTsTsOsT"), std::string("abcdefghv0.1"));
    EXPECT_EQ(proofread_as_text("abcdefgh 0xf", "sTsTsTsT.OsT"), std::string("abcdefgh 0xf"));
}

TEST(where_a_line_marks_its_zeros_an_empty_one_is_an_O_whatever_is_beside_it) {
    // The first zero has a dot in it, so this font marks them: the second,
    // without, is an O though it is beside a digit.
    EXPECT_EQ(proofread_as_text("abcdefgh0 10", "sTsTsTsT0.TO"), std::string("abcdefgh0 1O"));
}

TEST(an_O_with_an_inked_middle_in_a_monospaced_line_is_a_zero) {
    EXPECT_EQ(proofread_as_text("abcdefgh1O", "sTsTsTsTT0"), std::string("abcdefgh10"));
    EXPECT_EQ(proofread_as_text("abcdefgh1o", "sTsTsTsTT0"), std::string("abcdefgh10"));
}

TEST(a_small_ring_is_an_o_whatever_it_was_read_as) {
    EXPECT_EQ(proofread_as_text("abcdefghf0o", "sTsTsTsTTos"), std::string("abcdefghfoo"));
}

TEST(in_a_line_that_is_not_monospaced_a_zero_stays_as_read) {
    // Letters 12, 16 and 20 apart, by turns.
    std::vector<Dab> dabs;
    std::vector<Letter> letters;
    int x = 2;
    const std::string read = "abcdefghnix0s";
    for (size_t i = 0; i < read.size(); i++) {
        if (read[i] == '0') {
            for (const Dab& d : std::vector<Dab>{{x, 4, x + 8, 6, 255}, {x, 14, x + 8, 16, 255}, {x, 4, x + 2, 16, 255}, {x + 6, 4, x + 8, 16, 255}}) dabs.push_back(d);
        } else {
            dabs.push_back({x, std::string("bdfh").find(read[i]) == std::string::npos ? 8 : 4, x + 8, 16, 255});
        }
        letters.push_back({std::string(1, read[i]), 1, static_cast<float>(x + 4)});
        x += 12 + 4 * static_cast<int>(i % 3);
    }
    Ink ink = ink_with(x + 10, 22, dabs);
    ink.top = 4;
    ink.bottom = 16;
    EXPECT_EQ(letters_as_text(proofread(letters, ink)), read);
}

TEST(a_mark_read_where_there_is_no_ink_is_a_space) {
    EXPECT_EQ(proofread_as_text("ab~cd", "sT.sT"), std::string("ab cd"));
    // ...and nothing, at the end of a line or beside a space.
    EXPECT_EQ(proofread_as_text("ab'", "sT."), std::string("ab"));
    EXPECT_EQ(proofread_as_text("ab ~cd", "sT..sT"), std::string("ab cd"));
}

TEST(a_faint_mark_is_still_a_mark) {
    // Too faint to count as ink where letters are measured, but there.
    EXPECT_EQ(proofread_as_text("ab'cd", "sTfsT"), std::string("ab'cd"));
}

TEST(a_letter_read_where_there_is_no_ink_stays) {
    // Only marks are made up out of nothing; a letter with no ink under
    // where it was put has its ink somewhere near.
    EXPECT_EQ(proofread_as_text("abxcd", "sT.sT"), std::string("abxcd"));
}

TEST(in_a_monospaced_line_a_bare_stroke_through_the_line_is_a_pipe) {
    EXPECT_EQ(proofread_as_text("abcdefgh I ij", "sTsTsTsT.|.ss"), std::string("abcdefgh | ij"));
    EXPECT_EQ(proofread_as_text("abcdefgh l ij", "sTsTsTsT.|.ss"), std::string("abcdefgh | ij"));
    // An I with serifs is as wide as any letter.
    EXPECT_EQ(proofread_as_text("abcdefgh I ij", "sTsTsTsT.T.ss"), std::string("abcdefgh I ij"));
}

TEST(in_a_monospaced_line_a_small_stroke_slanting_down_from_the_left_is_a_backtick) {
    // Read as an apostrophe, or an accent.
    EXPECT_EQ(proofread_as_text("abcdefgh 'ab", "sTsTsTsT.`sT"), std::string("abcdefgh `ab"));
    Typed accent = typed("abcdefgh 'ab", "sTsTsTsT.`sT");
    accent.letters[9].text = "\xc2\xb4";
    EXPECT_EQ(letters_as_text(proofread(accent.letters, accent.ink)), std::string("abcdefgh `ab"));
    // An upright one is the apostrophe it was read as.
    EXPECT_EQ(proofread_as_text("abcdefgh 'ab", "sTsTsTsT.'sT"), std::string("abcdefgh 'ab"));
}

TEST(a_backtick_that_was_not_read_at_all_is_one_all_the_same) {
    // Nothing read in the cells before and after "ab": "`ab`".
    EXPECT_EQ(proofread_as_text("abcdefgh __ab_", "sTsTsTsT..`sT`"), std::string("abcdefgh `ab`"));
    // Ink that no letter was read at is otherwise left alone.
    EXPECT_EQ(proofread_as_text("abcdefgh __ab_", "sTsTsTsT..'sT'"), std::string("abcdefgh ab"));
}

TEST(in_a_monospaced_line_a_stroke_under_several_cells_is_as_many_underscores) {
    // Underscores side by side run into one stroke, which is read as one
    // underscore, or as a comma and one, or not at all.
    EXPECT_EQ(proofread_as_text("abcdefghij x=_y", "sTsTsTsTss.s__s"), std::string("abcdefghij x__y"));
    EXPECT_EQ(proofread_as_text("abcdefghij x,_=y", "sTsTsTsTss.s___s"), std::string("abcdefghij x___y"));
    EXPECT_EQ(proofread_as_text("abcdefghij x___y", "sTsTsTsTss.s___s"), std::string("abcdefghij x___y"));
    // One is one.
    EXPECT_EQ(proofread_as_text("abcdefghij x=y", "sTsTsTsTss.s_s"), std::string("abcdefghij x_y"));
    // In a font whose underscores do not touch, each is a stroke of its own,
    // and the one in the middle here was not read.
    EXPECT_EQ(proofread_as_text("abcdefghij x=_=y", "sTsTsTsTss.snnns"), std::string("abcdefghij x___y"));
}

TEST(a_stroke_that_goes_on_under_the_letters_is_an_underline_not_underscores) {
    // "ab cd", underlined: the stroke under the space is no underscore.
    EXPECT_EQ(proofread_as_text("abcdefghij ab cd", "sTsTsTsTss.uu_uu"), std::string("abcdefghij ab cd"));
}

TEST(in_a_monospaced_line_letters_a_cell_apart_have_a_space_between_them) {
    // Nothing was read in the ninth cell, which is empty.
    EXPECT_EQ(proofread_as_text("abcdefgh_ij", "sTsTsTsT.ss"), std::string("abcdefgh ij"));
    // Letters in cells side by side have none.
    EXPECT_EQ(proofread_as_text("abcdefghij", "sTsTsTsTss"), std::string("abcdefghij"));
}

TEST(a_line_with_no_legible_ink_stays_as_read) {
    Typed line = typed("abs0", "sTTO");
    line.ink.legible = false;
    EXPECT_EQ(letters_as_text(proofread(line.letters, line.ink)), std::string("abs0"));
}

// ---- Queue

TEST(a_fast_core_takes_the_costliest_piece_and_a_slow_one_the_cheapest) {
    Queue queue({50, 100, 10, 80, 20}, 2);
    EXPECT_EQ(queue.take(true), 1);
    EXPECT_EQ(queue.take(false), 2);
    EXPECT_EQ(queue.take(true), 3);
}

TEST(a_slow_core_stops_when_the_fast_ones_would_be_done_first) {
    // A slow core takes three times as long. With 100 taken, 160 is left for
    // two fast cores, 80 each: the 10 would take a slow core 30, so it takes
    // it. Then 150 is left, 75 each: the 20 would take it 60. Then 130, 65
    // each: the 50 would take it 150, so the fast cores are left to it.
    Queue queue({100, 80, 50, 20, 10}, 2);
    EXPECT_EQ(queue.take(true), 0);
    EXPECT_EQ(queue.take(false), 4);
    EXPECT_EQ(queue.take(false), 3);
    EXPECT_EQ(queue.take(false), -1);
    EXPECT_EQ(queue.take(true), 1);
    EXPECT_EQ(queue.take(true), 2);
    EXPECT_EQ(queue.take(true), -1);
}

TEST(every_piece_is_taken_once) {
    Queue queue({5, 5, 5}, 1);
    int taken[3] = {0, 0, 0};
    for (int i; (i = queue.take(true)) >= 0;) taken[i]++;
    EXPECT_EQ(taken[0], 1);
    EXPECT_EQ(taken[1], 1);
    EXPECT_EQ(taken[2], 1);
}

TEST(a_queue_is_empty_once_every_piece_is_taken_not_once_a_slow_core_is_refused) {
    Queue queue({100, 90}, 1);
    EXPECT_EQ(queue.empty(), false);
    EXPECT_EQ(queue.take(false), -1);
    EXPECT_EQ(queue.empty(), false);
    queue.take(true);
    queue.take(true);
    EXPECT_EQ(queue.empty(), true);
}

TEST(with_no_fast_cores_every_core_is_one) {
    // A machine whose cores are all alike says so with none marked fast.
    Queue queue({100, 10}, 0);
    EXPECT_EQ(queue.take(false), 0);
    EXPECT_EQ(queue.take(false), 1);
    EXPECT_EQ(queue.take(false), -1);
}

// ---- text_boxes

// A map of `width` × `height` with rectangles of one probability each.
struct Blob {
    int x1, y1, x2, y2;
    float p;
};

std::vector<float> map_of(int width, int height, std::vector<Blob> blobs) {
    std::vector<float> map(static_cast<size_t>(width) * height, 0.0f);
    for (const Blob& b : blobs)
        for (int y = b.y1; y < b.y2; y++)
            for (int x = b.x1; x < b.x2; x++) map[static_cast<size_t>(y) * width + x] = b.p;
    return map;
}

TEST(a_blob_is_a_box_grown_by_its_area_over_its_perimeter) {
    auto map = map_of(64, 32, {{10, 8, 30, 16, 0.9f}});
    auto boxes = text_boxes(map.data(), 64, 32);
    EXPECT_EQ(boxes.size(), 1u);
    if (boxes.size() != 1) return;
    // 20 × 8: 160 × 1.6 / (2 × 28) = 4.571 on every side
    EXPECT_NEAR(boxes[0].x1, 10 - 4.571, 0.01);
    EXPECT_NEAR(boxes[0].y1, 8 - 4.571, 0.01);
    EXPECT_NEAR(boxes[0].x2, 30 + 4.571, 0.01);
    EXPECT_NEAR(boxes[0].y2, 16 + 4.571, 0.01);
    EXPECT_NEAR(boxes[0].score, 0.9, 1e-5);
}

TEST(a_box_stays_inside_the_map) {
    auto map = map_of(64, 32, {{0, 0, 20, 8, 0.9f}, {50, 26, 64, 32, 0.9f}});
    auto boxes = text_boxes(map.data(), 64, 32);
    EXPECT_EQ(boxes.size(), 2u);
    for (const Box& b : boxes) {
        EXPECT_EQ(b.x1 >= 0 && b.y1 >= 0 && b.x2 <= 64 && b.y2 <= 32, true);
    }
}

TEST(a_faint_blob_is_no_box) {
    // Text by the pixel (over 0.3), but not on average (under 0.5).
    auto map = map_of(64, 32, {{10, 8, 30, 16, 0.4f}});
    EXPECT_EQ(text_boxes(map.data(), 64, 32).size(), 0u);
}

TEST(a_speck_is_no_box) {
    auto map = map_of(64, 32, {{10, 8, 30, 10, 0.9f}, {40, 8, 42, 20, 0.9f}});
    EXPECT_EQ(text_boxes(map.data(), 64, 32).size(), 0u);
}

TEST(blobs_apart_are_boxes_apart_and_blobs_touching_are_one) {
    // Two a pixel apart, and two that meet at a corner.
    auto apart = map_of(64, 32, {{4, 4, 14, 10, 0.9f}, {15, 4, 25, 10, 0.9f}});
    EXPECT_EQ(text_boxes(apart.data(), 64, 32).size(), 2u);
    auto corner = map_of(64, 32, {{4, 4, 14, 10, 0.9f}, {14, 10, 24, 16, 0.9f}});
    auto boxes = text_boxes(corner.data(), 64, 32);
    EXPECT_EQ(boxes.size(), 1u);
}

TEST(an_l_shaped_blob_is_scored_over_its_own_pixels) {
    // A full row and a full column of 0.9: its bounding box is mostly empty,
    // but the blob itself is sure.
    auto map = map_of(64, 48, {{4, 4, 44, 9, 0.9f}, {4, 9, 9, 44, 0.9f}});
    auto boxes = text_boxes(map.data(), 64, 48);
    EXPECT_EQ(boxes.size(), 1u);
    if (boxes.size() == 1) EXPECT_NEAR(boxes[0].score, 0.9, 1e-5);
}

// ---- join_boxes

std::string boxes_as_text(const std::vector<Box>& boxes) {
    std::ostringstream out;
    for (const Box& b : boxes) out << b.x1 << "," << b.y1 << "-" << b.x2 << "," << b.y2 << " ";
    return out.str();
}

TEST(boxes_that_nearly_touch_on_a_row_are_one) {
    // 20 tall and 6 apart: one line, read in two.
    EXPECT_EQ(boxes_as_text(join_boxes({{10, 10, 60, 30, 0.9f}, {66, 11, 120, 31, 0.8f}})), std::string("10,10-120,31 "));
    // Three in a row, given out of order.
    EXPECT_EQ(boxes_as_text(join_boxes({{130, 10, 160, 30, 0.9f}, {10, 10, 60, 30, 0.9f}, {64, 10, 126, 30, 0.9f}})), std::string("10,10-160,30 "));
}

TEST(boxes_apart_or_on_other_rows_stay_as_they_are) {
    // More than half their height apart.
    EXPECT_EQ(join_boxes({{10, 10, 60, 30, 0.9f}, {75, 10, 120, 30, 0.9f}}).size(), 2u);
    // One above the other.
    EXPECT_EQ(join_boxes({{10, 10, 60, 30, 0.9f}, {10, 32, 60, 52, 0.9f}}).size(), 2u);
    // A small box beside a tall one: not one line.
    EXPECT_EQ(join_boxes({{10, 10, 60, 30, 0.9f}, {62, 10, 120, 70, 0.9f}}).size(), 2u);
}

// ---- read_glyphs

TEST(glyphs_are_the_likeliest_class_of_each_run) {
    // Classes: 0 blank, 1 "a", 2 "b". Steps: a a _ a b b.
    const float output[] = {
        0.1f, 0.8f, 0.1f,
        0.2f, 0.7f, 0.1f,
        0.9f, 0.05f, 0.05f,
        0.3f, 0.6f, 0.1f,
        0.1f, 0.1f, 0.8f,
        0.05f, 0.0f, 0.95f,
    };
    auto glyphs = read_glyphs(output, 6, 3);
    EXPECT_EQ(glyphs.size(), 3u);
    if (glyphs.size() != 3) return;
    EXPECT_EQ(glyphs[0].index, 1);
    EXPECT_EQ(glyphs[0].first, 0);
    EXPECT_EQ(glyphs[0].last, 1);
    EXPECT_NEAR(glyphs[0].confidence, 0.8, 1e-6);
    // The same letter again, past a blank, is a second one.
    EXPECT_EQ(glyphs[1].index, 1);
    EXPECT_EQ(glyphs[1].first, 3);
    EXPECT_EQ(glyphs[1].last, 3);
    EXPECT_NEAR(glyphs[1].confidence, 0.6, 1e-6);
    EXPECT_EQ(glyphs[2].index, 2);
    EXPECT_EQ(glyphs[2].first, 4);
    EXPECT_EQ(glyphs[2].last, 5);
    EXPECT_NEAR(glyphs[2].confidence, 0.95, 1e-6);
}

TEST(glyphs_of_nothing_but_blanks_are_none) {
    const float output[] = {0.9f, 0.1f, 0.8f, 0.2f};
    EXPECT_EQ(read_glyphs(output, 2, 2).size(), 0u);
    EXPECT_EQ(read_glyphs(output, 0, 2).size(), 0u);
}

TEST(two_letters_side_by_side_without_a_blank_are_two) {
    // a b: no blank between different classes.
    const float output[] = {0.1f, 0.8f, 0.1f, 0.1f, 0.2f, 0.7f};
    auto glyphs = read_glyphs(output, 2, 3);
    EXPECT_EQ(glyphs.size(), 2u);
    if (glyphs.size() == 2) {
        EXPECT_EQ(glyphs[0].index, 1);
        EXPECT_EQ(glyphs[1].index, 2);
        EXPECT_EQ(glyphs[1].first, 1);
    }
}

// ---- rows_of

std::string rows_as_text(const std::vector<std::vector<int>>& rows) {
    std::string text;
    for (const auto& row : rows) {
        for (int i : row) text += std::to_string(i) + " ";
        text += "| ";
    }
    return text;
}

TEST(rows_run_top_to_bottom_and_left_to_right) {
    // Given out of order: 0 below, 1 top right, 2 top left.
    std::vector<Box> boxes = {{0, 20, 10, 30, 1}, {20, 2, 30, 12, 1}, {0, 0, 10, 10, 1}};
    EXPECT_EQ(rows_as_text(rows_of(boxes)), std::string("2 1 | 0 | "));
}

TEST(a_tall_box_beside_two_lines_is_a_row_of_its_own) {
    // 0 and 1 are two lines; 2 spans both, so it belongs with neither, and
    // does not make one row of them.
    std::vector<Box> boxes = {{0, 0, 10, 10, 1}, {0, 12, 10, 22, 1}, {20, 0, 30, 22, 1}};
    auto rows = rows_of(boxes);
    EXPECT_EQ(rows.size(), 3u);
}

TEST(rows_of_nothing_are_none) { EXPECT_EQ(rows_of({}).size(), 0u); }

// ---- to_json

TEST(json_is_lines_of_words) {
    std::vector<std::vector<Word>> lines = {{{"git", 1, 2, 3, 4}, {"clone", 10.5f, 2, 30.25f, 4}}, {{"x", 0, 0, 1, 1}}};
    EXPECT_EQ(to_json(lines),
        std::string(R"([[{"text":"git","x":1,"y":2,"w":3,"h":4},{"text":"clone","x":10.5,"y":2,"w":30.25,"h":4}],)"
                    R"([{"text":"x","x":0,"y":0,"w":1,"h":1}]])"));
}

TEST(json_of_no_lines_is_an_empty_list) { EXPECT_EQ(to_json({}), std::string("[]")); }

TEST(json_escapes_what_a_string_cannot_hold) {
    std::vector<std::vector<Word>> lines = {{{"a\"b\\c\td\x01", 0, 0, 1, 1}}};
    EXPECT_EQ(to_json(lines), std::string(R"([[{"text":"a\"b\\c\td\u0001","x":0,"y":0,"w":1,"h":1}]])"));
}

TEST(json_keeps_text_that_is_not_ascii) {
    std::vector<std::vector<Word>> lines = {{{"caf\xc3\xa9", 0, 0, 1, 1}}};
    EXPECT_EQ(to_json(lines), std::string("[[{\"text\":\"caf\xc3\xa9\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]]"));
}

}  // namespace

int main(int argc, char** argv) {
    std::string only = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (const Test& test : tests()) {
        if (!only.empty() && std::string(test.name).find(only) == std::string::npos) continue;
        int before = failures;
        try {
            test.run();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  threw: %s\n", e.what());
            failures++;
        }
        std::fprintf(stderr, "%s %s\n", failures == before ? "ok  " : "FAIL", test.name);
        ran++;
    }
    std::fprintf(stderr, "%d tests, %d failed checks\n", ran, failures);
    return failures == 0 ? 0 : 1;
}
