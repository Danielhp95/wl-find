// wl-find-ocr: the text on a screenshot, for the find bar to search.
//
//   grim -t ppm - | wl-find-ocr --scale 2
//
// It reads a PPM, from a file or stdin, and writes lines of JSON, each a list
// of lines of text, a line a list of words { text, x, y, w, h } in logical
// pixels: an empty list as soon as the image is in, then the lines as they
// are read, a few at a time.
//
// With --after FILE it reads nothing until FILE is made, its image and
// recogniser in already: so wl-find reads one output after another, each
// ready to start the moment the last is done. Two read at once take as long
// as one after the other, as each has its threads spin on every core.
//
// A detector finds the boxes of text on the screen at its logical size. Each
// box is a line, cut into pieces of one length, and a recogniser reads the
// pieces off the screen's own pixels, on every core at once (ocr.hh has what
// happens around the two models).
#include "ocr.hh"

#include <onnxruntime_cxx_api.h>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef WL_FIND_MODELS
#define WL_FIND_MODELS "."
#endif

using namespace wlfind;
using Clock = std::chrono::steady_clock;

namespace {

// The recogniser takes lines this tall.
const int LINE_HEIGHT = 48;
// It was trained on lines 320 wide: a line is read in pieces no longer than
// that, and a piece shorter than NARROWEST is padded out with grey.
const int PIECE = 320;
const int NARROWEST = 96;
// How far past a cut a piece is read, in heights of its line: a letter or
// two, for the pieces to be sewn by.
const double REACH = 0.5;
// The detector halves its input five times over.
const int DETECTOR_STEP = 32;
// The find bar searches again with every line of output: no more often than
// this many milliseconds.
const int FLUSH_EVERY = 30;

struct Options {
    double scale = 1;
    std::string detector = WL_FIND_MODELS "/det.onnx";
    std::string recogniser = WL_FIND_MODELS "/rec.onnx";
    std::string image;
    int threads = 0;
    bool timing = false;
    std::string after;
};

[[noreturn]] void usage(const char* problem) {
    std::fprintf(stderr,
        "wl-find-ocr: %s\n"
        "usage: wl-find-ocr [--scale N] [--det MODEL] [--rec MODEL] [--threads N] [--after FILE] [--timing] [IMAGE.ppm]\n",
        problem);
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto value = [&]() -> const char* {
            if (i + 1 >= argc) usage(("no value for " + arg).c_str());
            return argv[++i];
        };
        if (arg == "--scale") options.scale = std::atof(value());
        else if (arg == "--det") options.detector = value();
        else if (arg == "--rec") options.recogniser = value();
        else if (arg == "--threads") options.threads = std::atoi(value());
        else if (arg == "--after") options.after = value();
        else if (arg == "--timing") options.timing = true;
        else if (!arg.empty() && arg[0] == '-' && arg != "-") usage(("no such option: " + arg).c_str());
        else options.image = arg;
    }
    if (!(options.scale > 0)) usage("--scale takes a number above 0");
    return options;
}

// The cores this may run on, and which of them are slow ones.
struct Cores {
    int count = 1;
    std::set<int> all, slow;
    int fast() const { return count - static_cast<int>(slow.size()); }
};

// A list of cores as the kernel writes one, "0-7,16-19".
std::set<int> core_list(const std::string& path) {
    std::set<int> cores;
    std::ifstream file(path);
    std::string range;
    while (std::getline(file, range, ',')) {
        int first = 0, last = 0;
        int read = std::sscanf(range.c_str(), "%d-%d", &first, &last);
        if (read < 1) continue;
        if (read == 1) last = first;
        for (int core = first; core <= last; core++) cores.insert(core);
    }
    return cores;
}

Cores cores() {
    Cores cores;
    std::set<int> allowed;
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int core = 0; core < CPU_SETSIZE; core++)
            if (CPU_ISSET(core, &set)) allowed.insert(core);
    }
    if (allowed.empty()) {
        cores.count = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        return cores;
    }
    cores.count = static_cast<int>(allowed.size());
    cores.all = allowed;
    // Intel names its efficiency cores; Arm gives each core a capacity.
    std::set<int> slow = core_list("/sys/devices/cpu_atom/cpus");
    if (slow.empty()) {
        int most = 0;
        std::vector<std::pair<int, int>> capacities;
        for (int core : allowed) {
            std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(core) + "/cpu_capacity");
            int capacity = 0;
            if (file >> capacity) {
                capacities.push_back({core, capacity});
                most = std::max(most, capacity);
            }
        }
        for (auto [core, capacity] : capacities)
            if (capacity * 4 < most * 3) slow.insert(core);
    }
    for (int core : slow)
        if (allowed.count(core)) cores.slow.insert(core);
    // All slow is none slow.
    if (cores.fast() == 0) cores.slow.clear();
    return cores;
}

// `work(i)` for every i below `count`, on up to `threads` threads.
template <typename Work>
void in_parallel(int count, int threads, Work work) {
    std::atomic<int> next{0};
    auto run = [&] {
        for (int i; (i = next.fetch_add(1)) < count;) work(i);
    };
    std::vector<std::thread> others;
    for (int t = 1; t < std::min(threads, count); t++) others.emplace_back(run);
    run();
    for (std::thread& other : others) other.join();
}

// A model, and the names it calls what goes in and what comes out.
struct Model {
    Ort::Session session{nullptr};
    std::string input, output;

    // A model run on several threads has them spin while they wait for
    // each other, which is quicker; one run on a single thread has none to
    // wait.
    Model(Ort::Env& env, const std::string& path, int threads) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(threads);
        options.SetInterOpNumThreads(1);
        options.AddConfigEntry("session.intra_op.allow_spinning", threads > 1 ? "1" : "0");
        session = Ort::Session(env, path.c_str(), options);
        Ort::AllocatorWithDefaultOptions allocator;
        input = session.GetInputNameAllocated(0, allocator).get();
        output = session.GetOutputNameAllocated(0, allocator).get();
    }

    // The model's output for `data`, a tensor of `shape`.
    Ort::Value run(float* data, const std::array<int64_t, 4>& shape) {
        static const Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        size_t count = static_cast<size_t>(shape[0] * shape[1] * shape[2] * shape[3]);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, data, count, shape.data(), shape.size());
        const char* in = input.c_str();
        const char* out = output.c_str();
        auto outputs = session.Run(Ort::RunOptions{nullptr}, &in, &tensor, 1, &out, 1);
        return std::move(outputs[0]);
    }
};

// The characters the recogniser's classes stand for, which the model carries
// with it, a line each: class 0 is the blank, and the last one a space.
std::vector<std::string> characters(Model& model) {
    Ort::AllocatorWithDefaultOptions allocator;
    auto list = model.session.GetModelMetadata().LookupCustomMetadataMapAllocated("character", allocator);
    if (!list) throw std::runtime_error("the recogniser has no list of its characters");
    std::vector<std::string> all = {""};
    const char* at = list.get();
    while (*at) {
        const char* end = std::strchr(at, '\n');
        if (!end) end = at + std::strlen(at);
        size_t length = static_cast<size_t>(end - at);
        if (length > 0 && at[length - 1] == '\r') length--;
        all.emplace_back(at, length);
        at = *end ? end + 1 : end;
    }
    all.push_back(" ");
    return all;
}

// The boxes of text on the image, in its own pixels.
std::vector<Box> detect(Model& detector, const Image& image, double scale, int threads) {
    int width = std::max(1, static_cast<int>(std::lround(image.width / scale)));
    int height = std::max(1, static_cast<int>(std::lround(image.height / scale)));
    // The detector takes sizes it can halve: the last row and column are
    // drawn out to the next one.
    int wide = (width + DETECTOR_STEP - 1) / DETECTOR_STEP * DETECTOR_STEP;
    int tall = (height + DETECTOR_STEP - 1) / DETECTOR_STEP * DETECTOR_STEP;
    // Not zeroed: the threads that fill them are the first to touch them.
    std::unique_ptr<float[]> planes(new float[static_cast<size_t>(3) * width * height]);
    int bands = std::min(threads * 2, height);
    in_parallel(bands, threads, [&](int band) {
        to_planes(image, {0, 0, image.width, image.height}, width, height, planes.get(),
            static_cast<int>(static_cast<long>(height) * band / bands), static_cast<int>(static_cast<long>(height) * (band + 1) / bands));
    });
    std::unique_ptr<float[]> input(new float[static_cast<size_t>(3) * wide * tall]);
    in_parallel(3 * tall, threads, [&](int i) {
        int c = i / tall, y = i % tall;
        const float* row = &planes[(static_cast<size_t>(c) * height + std::min(y, height - 1)) * width];
        float* out = &input[(static_cast<size_t>(c) * tall + y) * wide];
        std::copy(row, row + width, out);
        std::fill(out + width, out + wide, row[width - 1]);
    });
    Ort::Value map = detector.run(input.get(), {1, 3, tall, wide});
    if (map.GetTensorTypeAndShapeInfo().GetElementCount() != static_cast<size_t>(wide) * tall)
        throw std::runtime_error("the detector's map is not the size of what it was shown");
    std::vector<Box> boxes = join_boxes(text_boxes(map.GetTensorData<float>(), wide, tall));
    float sx = static_cast<float>(image.width) / width, sy = static_cast<float>(image.height) / height;
    for (Box& b : boxes) {
        b.x1 = std::min(b.x1 * sx, static_cast<float>(image.width));
        b.x2 = std::min(b.x2 * sx, static_cast<float>(image.width));
        b.y1 = std::min(b.y1 * sy, static_cast<float>(image.height));
        b.y2 = std::min(b.y2 * sy, static_cast<float>(image.height));
    }
    return boxes;
}

// A box of text, as it is read: cut into pieces, each read on its own and
// `reach` pixels past the cuts at its ends, so that a letter beside a cut, or
// under it, is read whole by one piece at least (join_pieces).
struct Line {
    Rect box;
    Ink ink;
    std::vector<int> cuts;
    int reach = 0;
    std::vector<std::vector<Letter>> pieces;
    std::atomic<int> unread{0};
    std::vector<Word> words;
};

// A piece of a line: the columns `x1` up to `x2` of its box.
struct Piece {
    int line, index;
    int x1, x2;
};

// The letters in a piece of a line, their middles along the line.
std::vector<Letter> recognise(Model& recogniser, const std::vector<std::string>& characters, const Image& image, const Line& line, const Piece& piece) {
    // Its own rows of the box, without what reaches in of the lines above
    // and below.
    Rect from = {line.box.x1 + piece.x1, line.box.y1 + line.ink.above, line.box.x1 + piece.x2, line.box.y1 + line.ink.below};
    int w = from.x2 - from.x1, h = from.y2 - from.y1;
    int width = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * LINE_HEIGHT / h)));
    int padded = std::max(width, NARROWEST);
    std::vector<float> planes(static_cast<size_t>(3) * width * LINE_HEIGHT);
    to_planes(image, from, width, LINE_HEIGHT, planes.data());
    std::vector<float> input(static_cast<size_t>(3) * padded * LINE_HEIGHT, 0.0f);
    for (int row = 0; row < 3 * LINE_HEIGHT; row++)
        std::copy(&planes[static_cast<size_t>(row) * width], &planes[static_cast<size_t>(row + 1) * width],
            &input[static_cast<size_t>(row) * padded]);

    Ort::Value output = recogniser.run(input.data(), {1, 3, LINE_HEIGHT, padded});
    auto shape = output.GetTensorTypeAndShapeInfo().GetShape();
    int steps = static_cast<int>(shape[1]), classes = static_cast<int>(shape[2]);
    if (classes != static_cast<int>(characters.size())) throw std::runtime_error("the recogniser's characters are not its classes");
    // A step of the output is this many pixels of the line.
    double step = static_cast<double>(padded) / steps * w / width;
    std::vector<Letter> letters;
    for (const Glyph& g : read_glyphs(output.GetTensorData<float>(), steps, classes)) {
        double middle = (g.first + g.last + 1) / 2.0 * step;
        // Not what it made of the grey a short piece is padded with.
        if (middle < w) letters.push_back({characters[g.index], g.confidence, static_cast<float>(piece.x1 + middle)});
    }
    return letters;
}

void write_line(const std::string& json) {
    std::string line = json + "\n";
    size_t done = 0;
    while (done < line.size()) {
        ssize_t wrote = write(STDOUT_FILENO, line.data() + done, line.size() - done);
        if (wrote < 0 && errno == EINTR) continue;
        // The find bar has gone: there is no one to read for. At once, with
        // the other threads still reading.
        if (wrote < 0) _exit(0);
        done += static_cast<size_t>(wrote);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parse(argc, argv);
    auto started = Clock::now();
    auto timing = [&](const char* what) {
        if (options.timing)
            std::fprintf(stderr, "%7.1f ms  %s\n", std::chrono::duration<double, std::milli>(Clock::now() - started).count(), what);
    };
    try {
        Cores processor = cores();
        if (options.threads > 0) {
            processor.count = options.threads;
            processor.slow.clear();
        }

        // The models load while the screen is captured. With --after, the
        // detector loads only once the wait is over: its threads spin for a
        // while after it is loaded, half a second of every fast core, and
        // that would come out of the reading this one waits for.
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "wl-find-ocr");
        std::unique_ptr<Model> detector, recogniser;
        std::vector<std::string> alphabet;
        std::exception_ptr no_detector, no_recogniser;
        bool waits = !options.after.empty();
        auto load_detector = [&] {
            detector = std::make_unique<Model>(env, options.detector, processor.fast());
        };
        std::thread loading_detector;
        if (!waits)
            loading_detector = std::thread([&] {
                try {
                    load_detector();
                } catch (...) {
                    no_detector = std::current_exception();
                }
            });
        std::thread load_recogniser([&] {
            try {
                recogniser = std::make_unique<Model>(env, options.recogniser, 1);
                alphabet = characters(*recogniser);
                // Its classes, where the model says how many, are those
                // characters: said once here, not once a line.
                auto classes = recogniser->session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
                if (!classes.empty() && classes.back() > 0 && classes.back() != static_cast<int64_t>(alphabet.size()))
                    throw std::runtime_error("the recogniser's characters are not its classes");
            } catch (...) {
                no_recogniser = std::current_exception();
            }
        });

        Image image;
        std::exception_ptr unread;
        try {
            int fd = options.image.empty() || options.image == "-" ? STDIN_FILENO : open(options.image.c_str(), O_RDONLY);
            if (fd < 0) throw std::runtime_error("cannot open " + options.image + ": " + std::strerror(errno));
            image = read_ppm(fd);
            if (fd != STDIN_FILENO) close(fd);
        } catch (...) {
            unread = std::current_exception();
        }
        timing("image read");
        // The screen is captured: the find bar may dim it.
        if (!unread) write_line("[]");
        if (loading_detector.joinable()) loading_detector.join();
        load_recogniser.join();
        if (unread) std::rethrow_exception(unread);
        if (no_detector) std::rethrow_exception(no_detector);
        if (no_recogniser) std::rethrow_exception(no_recogniser);
        timing(waits ? "recogniser loaded" : "models loaded");
        if (waits) {
            while (access(options.after.c_str(), F_OK) != 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            timing("waited");
            load_detector();
            timing("detector loaded");
        }

        std::vector<Box> boxes = detect(*detector, image, options.scale, processor.fast());
        detector.reset();
        std::vector<std::vector<int>> rows = rows_of(boxes);
        timing("text found");

        // Each box a line, cut into pieces of one length.
        std::vector<Line> lines(boxes.size());
        in_parallel(static_cast<int>(boxes.size()), processor.count, [&](int i) {
            Line& line = lines[i];
            line.box = {
                std::max(static_cast<int>(std::floor(boxes[i].x1)), 0),
                std::max(static_cast<int>(std::floor(boxes[i].y1)), 0),
                std::min(static_cast<int>(std::ceil(boxes[i].x2)), image.width),
                std::min(static_cast<int>(std::ceil(boxes[i].y2)), image.height),
            };
            int w = line.box.x2 - line.box.x1, h = line.box.y2 - line.box.y1;
            if (w < 2 || h < 2) return;
            line.ink = ink_of(image, line.box);
            // Pieces of one length, as many as leave none longer than PIECE.
            h = line.ink.below - line.ink.above;
            int count = std::max(1, (w * LINE_HEIGHT / h + PIECE - 1) / PIECE);
            for (int k = 1; k < count; k++) line.cuts.push_back(static_cast<int>(static_cast<long>(w) * k / count));
            line.reach = static_cast<int>(h * REACH);
            line.pieces.resize(line.cuts.size() + 1);
            line.unread = static_cast<int>(line.pieces.size());
        });
        std::vector<Piece> pieces;
        std::vector<int> costs;
        for (size_t i = 0; i < lines.size(); i++) {
            const Line& line = lines[i];
            int w = line.box.x2 - line.box.x1;
            for (size_t k = 0; k < line.pieces.size(); k++) {
                int x1 = k == 0 ? 0 : std::max(line.cuts[k - 1] - line.reach, 0);
                int x2 = k < line.cuts.size() ? std::min(line.cuts[k] + line.reach, w) : w;
                pieces.push_back({static_cast<int>(i), static_cast<int>(k), x1, x2});
                costs.push_back(std::max((x2 - x1) * LINE_HEIGHT / (line.ink.below - line.ink.above), NARROWEST));
            }
        }
        timing("lines cut");

        Queue queue(costs, processor.slow.empty() ? 0 : processor.fast());
        std::mutex mutex;
        std::condition_variable progress;
        std::vector<char> done(lines.size(), 0);
        size_t lines_done = 0;
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].pieces.empty()) {
                done[i] = 1;
                lines_done++;
            }
        std::vector<std::thread> workers;
        int count = std::min<int>(processor.count, static_cast<int>(pieces.size()));
        for (int t = 0; t < count; t++) {
            // A worker for each core, the first of them the fast cores'. Each
            // is held to its kind: left to itself the kernel moves threads
            // between the two, and a fast core's worker that wakes on a slow
            // one would take, or be refused, as a slow one's.
            bool on_slow = t >= processor.fast();
            workers.emplace_back([&, on_slow] {
                if (!processor.slow.empty()) {
                    cpu_set_t kind;
                    CPU_ZERO(&kind);
                    for (int core : processor.all)
                        if ((processor.slow.count(core) > 0) == on_slow) CPU_SET(core, &kind);
                    sched_setaffinity(0, sizeof kind, &kind);
                }
                for (;;) {
                    int at = queue.take(!on_slow);
                    if (at < 0) {
                        if (queue.empty()) return;
                        // A slow core with only long pieces left: the fast
                        // ones will have them.
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    const Piece& piece = pieces[at];
                    Line& line = lines[piece.line];
                    try {
                        line.pieces[piece.index] = recognise(*recogniser, alphabet, image, line, piece);
                    } catch (const std::exception& e) {
                        std::fprintf(stderr, "wl-find-ocr: %s\n", e.what());
                    }
                    if (--line.unread > 0) continue;
                    // The last piece of its line: the line is read.
                    line.words = words_of(proofread(join_pieces(line.pieces, line.cuts, line.reach), line.ink), line.ink);
                    for (Word& word : line.words) {
                        // Logical pixels from here on.
                        word.x = static_cast<float>((line.box.x1 + word.x) / options.scale);
                        word.y = static_cast<float>((line.box.y1 + word.y) / options.scale);
                        word.w = static_cast<float>(word.w / options.scale);
                        word.h = static_cast<float>(word.h / options.scale);
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    done[piece.line] = 1;
                    lines_done++;
                    progress.notify_one();
                }
            });
        }

        // The rows as their lines are read, a few to a line of output.
        std::vector<char> written(rows.size(), 0);
        size_t rows_written = 0;
        auto last_flush = Clock::now();
        std::unique_lock<std::mutex> lock(mutex);
        while (rows_written < rows.size()) {
            bool all = lines_done == lines.size();
            if (!all) {
                progress.wait_for(lock, std::chrono::milliseconds(FLUSH_EVERY));
                all = lines_done == lines.size();
                if (!all && Clock::now() - last_flush < std::chrono::milliseconds(FLUSH_EVERY)) continue;
            }
            std::vector<std::vector<Word>> out;
            for (size_t r = 0; r < rows.size(); r++) {
                if (written[r]) continue;
                bool complete = true;
                for (int i : rows[r]) complete = complete && done[i];
                if (!complete) continue;
                written[r] = 1;
                rows_written++;
                std::vector<Word> row;
                for (int i : rows[r])
                    for (Word& word : lines[i].words) row.push_back(std::move(word));
                if (!row.empty()) out.push_back(std::move(row));
            }
            if (out.empty()) continue;
            lock.unlock();
            write_line(to_json(out));
            last_flush = Clock::now();
            lock.lock();
        }
        lock.unlock();
        for (std::thread& worker : workers) worker.join();
        timing("text read");
        if (options.timing)
            std::fprintf(stderr, "%zu boxes in %zu rows, %zu pieces, on %d cores (%zu slow)\n", boxes.size(), rows.size(), pieces.size(),
                processor.count, processor.slow.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "wl-find-ocr: %s\n", e.what());
        return 1;
    }
    return 0;
}
