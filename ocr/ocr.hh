// What wl-find-ocr reads a screen with, apart from the two models themselves
// (main.cc runs those): the image in, the boxes of text out of the detector's
// map, the characters out of the recogniser's, and the lines of words the
// find bar searches.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wlfind {

// Interleaved 8-bit samples, three a pixel (RGB).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
};

// Half open: x2 and y2 are past the last pixel.
struct Rect {
    int x1, y1, x2, y2;
};

// A PPM as `grim -t ppm` writes one, "P6\n<width> <height>\n255\n" and then
// three bytes a pixel, read off `fd`. Throws std::runtime_error on anything
// else.
Image read_ppm(int fd);

// `from` of `image`, scaled to width × height, as the models take it: three
// planes, blue first, of (v / 255 - 0.5) / 0.5. Shrinking averages the pixels
// each one covers, and enlarging interpolates between them. `planes` holds
// 3 × width × height floats.
void to_planes(const Image& image, Rect from, int width, int height, float* planes);

// The same, but only rows `first` up to `last` of the planes: a band of
// them, for threads to share an image out between them.
void to_planes(const Image& image, Rect from, int width, int height, float* planes, int first, int last);

// How much ink each pixel of a box of the image holds, from 0 for its ground
// to 255: the text and what it is written on told apart, whichever is the
// darker and whatever their colours. Not `legible` where the two are too
// close to tell.
//
// A box is cut generously about its line, so it may hold more than that: the
// feet of the line above and the heads of the one below, which reach in from
// its edges, and rules (an underline, a border), which run on for longer than
// any stroke of a letter. Both are wiped, and `top` and `bottom` are the rows
// the line itself is on. `above` and `below` are the rows between which the
// box holds nothing of another line: where to cut it for the recogniser,
// which a few rows of the next line's heads can stop reading altogether.
struct Ink {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
    bool legible = false;
    int top = 0;
    int bottom = 0;
    int above = 0;
    int below = 0;
};

Ink ink_of(const Image& image, Rect box);

// Half open, along a line.
struct Span {
    int x1, x2;
};

// The runs of columns with ink in them, left to right: letters, or several
// that touch. A column has ink if a pixel of it is at least half inked.
std::vector<Span> ink_columns(const Ink& ink);

// A character the recogniser read on a line: what it is, how sure it was, and
// where its middle is along the line, in pixels. A space is one too.
struct Letter {
    std::string text;
    float confidence;
    float middle;
};

// The letters of a line that was read in pieces, as one line. The line is cut
// at `cuts`, wherever those fall, even through a letter; so each piece is read
// with `reach` pixels of its neighbours on either side, and the letters about
// a cut are read twice, whole at least once. Two pieces are sewn where they
// agree: at the letters both read alike, at the same place, nearest the cut.
// Where they agree on nothing, each keeps the letters on its own side of it.
std::vector<Letter> join_pieces(const std::vector<std::vector<Letter>>& pieces, const std::vector<int>& cuts, int reach);

// What the ink says of a letter the recogniser read: the box of its ink, half
// open, and how inked the middle of that is, from 0 for the hollow of an O to
// 1 for the dot or the slash of a zero (-1 where it has no middle to speak
// of, as an l has none). Not `known` for a letter with no ink of its own:
// one that touches the next, or was read where there is nothing.
struct Shape {
    bool known = false;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float middle = -1;
};

// The shape of each of a line's letters, spaces too (which have none): a
// letter's ink is the blobs nearest it, the dot of an i with its stem.
std::vector<Shape> shapes_of(const std::vector<Letter>& letters, const Ink& ink);

// A line's letters with what its ink says of them put right. The recogniser
// reads a line squeezed to one height, so it is unsure of what only size or
// a detail tells apart, and the ink is not:
//
// - a c, o, s, u, v, w, x or z and its capital are one shape at two sizes:
//   as tall as the line's capitals and ascenders it is the capital, and as
//   tall as its small letters the small one;
// - in a monospaced line a zero has a dot or a slash in it, and an O has
//   not: an "O" with an inked middle is a zero, and a "0" with an empty one
//   an O, where another zero of the line is marked, or where it is before a
//   letter and no part of a number ("Nix0S", but not 100, eth0 or 0xf: a
//   few such fonts leave their zeros empty);
// - a bare stroke from above the capitals to below the line is a "|", not
//   the I or l it is read as, which have serifs in such a font;
// - underscores side by side are one stroke under the line, read as one of
//   them, or as a comma, or not at all: it is as many as the cells it lies
//   under, unless it goes on under the letters beside it, as an underline
//   does;
// - a mark read where there is no ink at all is a space, and two letters a
//   whole cell apart in a monospaced line have one between them.
std::vector<Letter> proofread(std::vector<Letter> letters, const Ink& ink);

// A box of text, in the pixels of the detector's map.
struct Box {
    float x1, y1, x2, y2;
    float score;
};

struct DetectOptions {
    // A pixel is text above this.
    float pixel = 0.3f;
    // A box is kept if its pixels average this.
    float box = 0.5f;
    // The detector marks the core of a line, shrunk from its edges; a box is
    // grown back by its area × this over its perimeter.
    float unclip = 1.6f;
    // Boxes thinner than this are noise.
    int thinnest = 3;
};

// The boxes of text in the detector's map, `width` × `height` probabilities:
// one for each blob of pixels that touch, corners included.
std::vector<Box> text_boxes(const float* map, int width, int height, const DetectOptions& options = {});

// Boxes side by side that nearly touch, as one: the detector parts a line at
// a run of underscores, which lie too low for it, and a line read in two
// boxes has a space where they meet. Two boxes are one if they are on one
// row, most of the height of each beside the other, and less than half that
// height apart.
std::vector<Box> join_boxes(std::vector<Box> boxes);

// A character the recogniser read: its class, how sure it was, and the steps
// of its output it spans.
struct Glyph {
    int index;
    float confidence;
    int first, last;
};

// The characters in the recogniser's output, `steps` rows of `classes`
// probabilities: the likeliest class of each step, runs of one class taken
// once, and class 0, the blank that parts them, left out.
std::vector<Glyph> read_glyphs(const float* output, int steps, int classes);

// The boxes as rows of the screen, top to bottom, each left to right: by
// index into `boxes`. Two boxes share a row if the middle of each is within
// the height of the other.
std::vector<std::vector<int>> rows_of(const std::vector<Box>& boxes);

// The pieces of the screen still to be read, by what each costs to read, for
// the threads reading them to take from. A processor may have fast cores and
// slow ones (Intel's P and E, Arm's big and little): a long piece on a slow
// core would be the last one done, and hold up everything. So a fast core
// takes the costliest piece left, and a slow one the cheapest, and only while
// it would be done with it before the fast cores are through the rest.
class Queue {
public:
    // `fast` is how many of the cores are fast ones.
    Queue(std::vector<int> costs, int fast);
    // The piece for a core to read next, by its place in `costs`, or -1 if
    // there is none for it, as yet or at all.
    int take(bool on_fast_core);
    // Whether every piece is taken.
    bool empty();

private:
    std::mutex mutex;
    std::vector<int> costs;
    // By cost, the costliest first; those from `front` up to `back` are left.
    std::vector<int> order;
    size_t front, back;
    long left;
    int fast;
};

// A word as the find bar takes it, in logical pixels.
struct Word {
    std::string text;
    float x, y, w, h;
};

// The words of a line, in the pixels of its box: the letters between spaces,
// each word boxed by the ink of its letters, from the top of the line to its
// bottom. The recogniser only says about where a letter is, so a word's sides
// are where the ink nearest its first and last letters ends; with no ink to
// go by, they are half a letter beyond the middles of those.
std::vector<Word> words_of(const std::vector<Letter>& letters, const Ink& ink);

// Lines of words as a line of JSON, without the newline.
std::string to_json(const std::vector<std::vector<Word>>& lines);

}  // namespace wlfind
