#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr uint32_t first_cp = 0x4E00;
constexpr uint32_t last_cp  = 0x9FFF;
constexpr uint64_t chars    = uint64_t(last_cp - first_cp + 1);
constexpr uint64_t count1   = chars;
constexpr uint64_t count2   = chars * chars;
constexpr uint64_t count3   = chars * chars * chars;
constexpr uint64_t total    = count1 + count2 + count3;

std::atomic<bool> stop_flag{false};

struct TermSize {
    int cols = 80;
    int rows = 24;
};

struct Combo {
    std::array<uint32_t, 3> cp{};
    int n = 0;
};

enum class Mode { Grid, Art };
enum class InfoStyle { Auto, Dec, Plain };

struct Opts {
    Mode mode = Mode::Grid;
    InfoStyle info = InfoStyle::Auto;
    double fps = 60.0;
    double art_width = 0.70;
    uint64_t start = 0;
    int cjk_width = 2;
    int gap = 1;
    bool alt_screen = true;
    std::string font;
};

struct Bitmap {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;

    uint8_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        return px[size_t(y) * w + x];
    }
};

struct Font {
    FT_Face face = nullptr;
    std::string path;
    std::string name;
    long face_no = 0;

    Font() = default;
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    Font(Font&& other) noexcept { *this = std::move(other); }
    Font& operator=(Font&& other) noexcept {
        if (this == &other) return *this;
        if (face) FT_Done_Face(face);
        face = other.face;
        path = std::move(other.path);
        name = std::move(other.name);
        face_no = other.face_no;
        other.face = nullptr;
        return *this;
    }

    ~Font() {
        if (face) FT_Done_Face(face);
    }
};

static void on_signal(int) {
    stop_flag.store(true);
}

static bool is_tty() {
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
#else
    return isatty(STDOUT_FILENO);
#endif
}

static void setup_terminal() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    auto h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(h, mode);
    }
#endif
}

static TermSize term_size() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO i{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i)) {
        return {i.srWindow.Right - i.srWindow.Left + 1,
                i.srWindow.Bottom - i.srWindow.Top + 1};
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row)
        return {int(ws.ws_col), int(ws.ws_row)};
#endif
    return {80, 24};
}

static void cursor(std::string& out, int row, int col) {
    out += "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

struct TermSession {
    bool alt;

    explicit TermSession(bool use_alt) : alt(use_alt) {
        if (alt) std::fputs("\x1b[?1049h", stdout);
        std::fputs("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H", stdout);
        std::fflush(stdout);
    }

    ~TermSession() {
        std::fputs("\x1b#5\x1b[0m\x1b[?7h\x1b[?25h", stdout);
        if (alt) std::fputs("\x1b[?1049l", stdout);
        std::fflush(stdout);
    }
};

static Combo combo_at(uint64_t idx) {
    Combo c;
    if (idx < count1) {
        c.n = 1;
        c.cp[0] = first_cp + uint32_t(idx);
        return c;
    }

    idx -= count1;
    if (idx < count2) {
        c.n = 2;
        c.cp[0] = first_cp + uint32_t(idx / chars);
        c.cp[1] = first_cp + uint32_t(idx % chars);
        return c;
    }

    idx -= count2;
    c.n = 3;
    auto cc = chars * chars;
    c.cp[0] = first_cp + uint32_t(idx / cc);
    idx %= cc;
    c.cp[1] = first_cp + uint32_t(idx / chars);
    c.cp[2] = first_cp + uint32_t(idx % chars);
    return c;
}

static void put_utf8(std::string& s, uint32_t cp) {
    if (cp <= 0x7F) {
        s += char(cp);
    } else if (cp <= 0x7FF) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

static std::string combo_utf8(const Combo& c) {
    std::string s;
    for (int i = 0; i < c.n; ++i) put_utf8(s, c.cp[i]);
    return s;
}

static std::string commas(uint64_t n) {
    auto s = std::to_string(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
    return s;
}

static std::string fmt_rate(double n) {
    std::ostringstream s;
    s << std::fixed;
    if (n >= 1e9) s << std::setprecision(2) << n / 1e9 << "G/s";
    else if (n >= 1e6) s << std::setprecision(2) << n / 1e6 << "M/s";
    else if (n >= 1e3) s << std::setprecision(2) << n / 1e3 << "K/s";
    else s << std::setprecision(1) << n << "/s";
    return s.str();
}

static std::string fmt_time(double sec) {
    if (!std::isfinite(sec) || sec < 0) return "--";
    constexpr double day = 86400.0;
    constexpr double year = 365.2425 * day;
    std::ostringstream s;

    if (sec >= year) {
        double y = sec / year;
        s << std::fixed << std::setprecision(y < 10 ? 2 : 1) << y << "y";
    } else if (sec >= day) {
        s << std::fixed << std::setprecision(1) << sec / day << "d";
    } else if (sec >= 3600) {
        int h = int(sec / 3600);
        int m = int(std::fmod(sec, 3600.0) / 60);
        s << h << "h" << m << "m";
    } else if (sec >= 60) {
        int m = int(sec / 60);
        int q = int(std::fmod(sec, 60.0));
        s << m << "m" << q << "s";
    } else {
        s << std::fixed << std::setprecision(1) << sec << "s";
    }
    return s.str();
}

static std::string cut(std::string s, int n) {
    if (n <= 0) return {};
    if (int(s.size()) <= n) return s;
    if (n <= 3) return s.substr(0, size_t(n));
    return s.substr(0, size_t(n - 3)) + "...";
}

static std::string progress(uint64_t done, int cols) {
    long double p = std::clamp((long double)done / total, 0.0L, 1.0L);
    std::ostringstream ps;
    ps << std::fixed << std::setprecision(9) << double(p * 100) << "%";
    auto pct = ps.str();

    int w = cols - int(pct.size()) - 3;
    if (w < 3) return cut(pct, cols);

    int fill = std::clamp(int(std::floor(p * w)), 0, w);
    std::string s = "[";
    if (fill == w) {
        s.append(size_t(w), '=');
    } else {
        s.append(size_t(fill), '=');
        s += '>';
        s.append(size_t(std::max(0, w - fill - 1)), ' ');
    }
    return s + "] " + pct;
}

static std::string hex_cp(uint32_t cp) {
    std::ostringstream s;
    s << "U+" << std::uppercase << std::hex << std::setfill('0')
      << std::setw(cp <= 0xFFFF ? 4 : 6) << cp;
    return s.str();
}

static std::string unicode_line(const Combo& c) {
    std::string s = "Unicode  ";
    for (int i = 0; i < c.n; ++i) {
        if (i) s += ' ';
        s += hex_cp(c.cp[i]);
    }
    return s;
}

static std::vector<unsigned char> utf8_bytes(uint32_t cp) {
    std::string s;
    put_utf8(s, cp);
    return {s.begin(), s.end()};
}

static std::string utf8_line(const Combo& c) {
    std::ostringstream s;
    s << "UTF-8    " << std::uppercase << std::hex << std::setfill('0');
    for (int i = 0; i < c.n; ++i) {
        if (i) s << "  ";
        auto bytes = utf8_bytes(c.cp[i]);
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (j) s << ' ';
            s << std::setw(2) << unsigned(bytes[j]);
        }
    }
    return s.str();
}

static bool env_has(const char* key, const char* text) {
    const char* p = std::getenv(key);
    if (!p) return false;
    std::string s = p;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::string q = text;
    std::transform(q.begin(), q.end(), q.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s.find(q) != std::string::npos;
}

static bool dec_likely_supported() {
    // There is no portable capability query for DECDHL/DECDWL. Keep auto mode
    // deliberately conservative; --info-style dec can force it.
    if (std::getenv("WT_SESSION")) return false;
#ifdef _WIN32
    return false;
#else
    if (env_has("TERM", "xterm") || env_has("TERM", "rxvt") ||
        env_has("TERM", "vt100") || env_has("TERM", "vt220")) return true;
    if (env_has("TERM_PROGRAM", "apple_terminal") || env_has("TERM_PROGRAM", "iterm")) return true;
    return false;
#endif
}

static void center_plain(std::string& out, int row, int cols, std::string s) {
    s = cut(std::move(s), cols);
    int col = std::max(1, (cols - int(s.size())) / 2 + 1);
    cursor(out, row, col);
    out += "\x1b#5";
    out += s;
    out += "\x1b[K";
}

static void center_dec(std::string& out, int row, int cols, const std::string& s) {
    int logical = cols / 2;
    int pad = std::max(0, (logical - int(s.size())) / 2);

    cursor(out, row, 1);
    out += "\x1b#3";
    out.append(size_t(pad), ' ');
    out += s;
    out += "\x1b[K";

    cursor(out, row + 1, 1);
    out += "\x1b#4";
    out.append(size_t(pad), ' ');
    out += s;
    out += "\x1b[K";
}

static void clear_rows(std::string& out, int a, int b) {
    for (int r = a; r <= b; ++r) {
        cursor(out, r, 1);
        out += "\x1b#5\x1b[2K";
    }
}

static std::vector<fs::path> font_dirs() {
    std::vector<fs::path> d;
#ifdef _WIN32
    if (const char* p = std::getenv("WINDIR")) d.emplace_back(fs::path(p) / "Fonts");
    if (const char* p = std::getenv("LOCALAPPDATA")) d.emplace_back(fs::path(p) / "Microsoft/Windows/Fonts");
#elif defined(__APPLE__)
    d.emplace_back("/System/Library/Fonts");
    d.emplace_back("/Library/Fonts");
    if (const char* p = std::getenv("HOME")) d.emplace_back(fs::path(p) / "Library/Fonts");
#else
    d.emplace_back("/usr/share/fonts");
    d.emplace_back("/usr/local/share/fonts");
    if (const char* p = std::getenv("HOME")) {
        d.emplace_back(fs::path(p) / ".fonts");
        d.emplace_back(fs::path(p) / ".local/share/fonts");
    }
#endif
    return d;
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static int name_score(const fs::path& p) {
    auto s = lower(p.filename().string());
    int n = 0;
    auto has = [&](const char* q) { return s.find(q) != std::string::npos; };

    if (has("notosanscjk") || has("noto sans cjk")) n += 120;
    if (has("sourcehansans") || has("source han sans")) n += 115;
    if (has("pingfang")) n += 110;
    if (has("msyh") || has("yahei")) n += 105;
    if (has("simhei")) n += 95;
    if (has("simsun")) n += 90;
    if (has("deng")) n += 85;
    if (has("wqy") || has("wenquanyi")) n += 80;
    if (has("cjk")) n += 60;
    if (has("sc") || has("cn")) n += 15;
    if (has("sans")) n += 5;
    if (has("emoji") || has("symbol")) n -= 200;
    return n;
}

static std::vector<fs::path> font_files() {
    std::vector<fs::path> files;
    std::error_code ec;

    for (auto& root : font_dirs()) {
        if (!fs::exists(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            auto ext = lower(it->path().extension().string());
            if (ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc")
                files.push_back(it->path());
        }
    }

    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        int sa = name_score(a), sb = name_score(b);
        if (sa != sb) return sa > sb;
        return a.string() < b.string();
    });
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

static int cjk_coverage(FT_Face face) {
    int n = 0;
    for (uint32_t cp = first_cp; cp <= last_cp; ++cp)
        n += FT_Get_Char_Index(face, cp) != 0;
    return n;
}

static Font open_best_face(FT_Library lib, const fs::path& path) {
    Font best;
    int best_score = -1;

    FT_Face first = nullptr;
    if (FT_New_Face(lib, path.string().c_str(), 0, &first)) return best;
    long faces = std::max<long>(1, first->num_faces);
    FT_Done_Face(first);

    for (long i = 0; i < faces; ++i) {
        FT_Face f = nullptr;
        if (FT_New_Face(lib, path.string().c_str(), i, &f)) continue;
        int score = cjk_coverage(f);
        if (score > best_score) {
            if (best.face) FT_Done_Face(best.face);
            best.face = f;
            best.path = path.string();
            best.face_no = i;
            std::string family = f->family_name ? f->family_name : path.filename().string();
            if (f->style_name && std::string(f->style_name) != "Regular") family += " " + std::string(f->style_name);
            best.name = family;
            best_score = score;
        } else {
            FT_Done_Face(f);
        }
    }
    return best;
}

static Font find_font(FT_Library lib, const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        auto f = open_best_face(lib, explicit_path);
        if (!f.face) throw std::runtime_error("could not open font: " + explicit_path);
        if (cjk_coverage(f.face) == 0) throw std::runtime_error("font has no CJK glyphs: " + explicit_path);
        return f;
    }

    auto files = font_files();
    if (files.empty()) throw std::runtime_error("no .ttf/.ttc/.otf fonts found; use --font PATH");

    Font best;
    int best_score = -1;
    int tried = 0;

    for (auto& path : files) {
        auto f = open_best_face(lib, path);
        if (!f.face) continue;
        ++tried;
        int score = cjk_coverage(f.face);
        int combined = score * 1000 + name_score(path);

        if (combined > best_score) {
            best = std::move(f);
            best_score = combined;
        }

        // The list is name-prioritized. A full-coverage CJK-looking font is
        // good enough; don't open every font on the machine.
        if (best.face && cjk_coverage(best.face) == int(chars) && name_score(best.path) >= 60) break;
        if (tried >= 250 && best.face && cjk_coverage(best.face) >= int(chars * 9 / 10)) break;
    }

    if (!best.face || cjk_coverage(best.face) == 0)
        throw std::runtime_error("could not find a usable CJK font; use --font PATH");
    return best;
}

struct GlyphBmp {
    int w = 0, h = 0;
    int left = 0, top = 0, advance = 0;
    std::vector<uint8_t> px;
};

static GlyphBmp load_glyph(FT_Face face, uint32_t cp) {
    GlyphBmp g;
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (!gi) gi = FT_Get_Char_Index(face, 0x25A1); // white square, if the font has it
    if (!gi) gi = FT_Get_Char_Index(face, '?');
    if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT)) return g;
    if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return g;

    auto& b = face->glyph->bitmap;
    g.w = int(b.width);
    g.h = int(b.rows);
    g.left = face->glyph->bitmap_left;
    g.top = face->glyph->bitmap_top;
    g.advance = int(face->glyph->advance.x >> 6);
    if (g.advance <= 0) g.advance = std::max(1, g.w);
    g.px.assign(size_t(g.w) * g.h, 0);

    for (int y = 0; y < g.h; ++y) {
        int sy = b.pitch >= 0 ? y : (g.h - 1 - y);
        const unsigned char* row = b.buffer + sy * std::abs(b.pitch);
        if (b.pixel_mode == FT_PIXEL_MODE_GRAY) {
            for (int x = 0; x < g.w; ++x) g.px[size_t(y) * g.w + x] = row[x];
        } else if (b.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int x = 0; x < g.w; ++x)
                g.px[size_t(y) * g.w + x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        }
    }
    return g;
}

static Bitmap raster_text(Font& font, const Combo& c, int px_size) {
    if (FT_Set_Pixel_Sizes(font.face, 0, FT_UInt(std::max(8, px_size))))
        throw std::runtime_error("FT_Set_Pixel_Sizes failed");

    std::vector<GlyphBmp> glyphs;
    glyphs.reserve(size_t(c.n));

    int pen = 0, min_x = 0, max_x = 0, top = 0, bottom = 0;
    bool first = true;

    for (int i = 0; i < c.n; ++i) {
        auto g = load_glyph(font.face, c.cp[i]);
        int x0 = pen + g.left;
        int x1 = x0 + g.w;
        if (first) { min_x = x0; max_x = x1; first = false; }
        else { min_x = std::min(min_x, x0); max_x = std::max(max_x, x1); }
        top = std::max(top, g.top);
        bottom = std::max(bottom, g.h - g.top);
        pen += g.advance;
        glyphs.push_back(std::move(g));
    }

    Bitmap bm;
    bm.w = std::max(1, max_x - min_x);
    bm.h = std::max(1, top + bottom);
    bm.px.assign(size_t(bm.w) * bm.h, 0);

    pen = 0;
    for (auto& g : glyphs) {
        int ox = pen + g.left - min_x;
        int oy = top - g.top;
        for (int y = 0; y < g.h; ++y) {
            for (int x = 0; x < g.w; ++x) {
                int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= bm.w || dy >= bm.h) continue;
                auto v = g.px[size_t(y) * g.w + x];
                auto& d = bm.px[size_t(dy) * bm.w + dx];
                d = std::max(d, v);
            }
        }
        pen += g.advance;
    }
    return bm;
}

static double sample(const Bitmap& b, double x, double y) {
    if (b.w <= 0 || b.h <= 0) return 0;
    x = std::clamp(x, 0.0, double(b.w - 1));
    y = std::clamp(y, 0.0, double(b.h - 1));
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    int x1 = std::min(x0 + 1, b.w - 1), y1 = std::min(y0 + 1, b.h - 1);
    double fx = x - x0, fy = y - y0;
    double a = b.at(x0, y0) * (1 - fx) + b.at(x1, y0) * fx;
    double q = b.at(x0, y1) * (1 - fx) + b.at(x1, y1) * fx;
    return a * (1 - fy) + q * fy;
}

static void draw_art(std::string& out, const Bitmap& bm, int top_row, int rows, int cols, double width_frac) {
    if (rows <= 0 || cols <= 0 || bm.w <= 0 || bm.h <= 0) return;

    int max_w = std::max(1, int(std::floor(cols * width_frac)));
    int max_h = rows * 2; // upper/lower half block gives two vertical pixels per terminal row
    double scale = std::min(double(max_w) / bm.w, double(max_h) / bm.h);
    int w = std::max(1, int(std::round(bm.w * scale)));
    int h = std::max(1, int(std::round(bm.h * scale)));
    int out_rows = (h + 1) / 2;
    int x0 = std::max(0, (cols - w) / 2);
    int y0 = top_row + std::max(0, (rows - out_rows) / 2);
    constexpr double threshold = 88.0;

    for (int r = 0; r < out_rows; ++r) {
        cursor(out, y0 + r, 1);
        out += "\x1b#5";
        out.append(size_t(x0), ' ');

        for (int x = 0; x < w; ++x) {
            auto sx = (x + 0.5) * bm.w / w - 0.5;
            int py0 = r * 2;
            int py1 = py0 + 1;
            auto sy0 = (py0 + 0.5) * bm.h / h - 0.5;
            auto sy1 = (py1 + 0.5) * bm.h / h - 0.5;
            bool up = py0 < h && sample(bm, sx, sy0) >= threshold;
            bool dn = py1 < h && sample(bm, sx, sy1) >= threshold;

            if (up && dn) out += "█";
            else if (up) out += "▀";
            else if (dn) out += "▄";
            else out += ' ';
        }
        out += "\x1b[K";
    }
}

static bool parse_u64(const char* s, uint64_t& n) {
    try {
        size_t p = 0;
        auto x = std::stoull(s, &p, 10);
        if (s[p]) return false;
        n = uint64_t(x);
        return true;
    } catch (...) { return false; }
}

static bool parse_double(const char* s, double& n) {
    try {
        size_t p = 0;
        n = std::stod(s, &p);
        return !s[p];
    } catch (...) { return false; }
}

static void help(const char* exe) {
    std::cout
        << "Usage: " << exe << " [options]\n\n"
        << "  --mode grid|art         grid = many combos/frame; art = one large combo/frame\n"
        << "  --fps N                 target refresh rate, 0 = unlimited (default 60)\n"
        << "  --start N               resume from global combination index\n"
        << "  --art-width N           target art width as fraction of terminal (default 0.70)\n"
        << "  --font PATH             override automatic CJK font discovery\n"
        << "  --info-style auto|dec|plain\n"
        << "                           DEC double-width/double-height info when possible\n"
        << "  --cjk-width N           terminal columns per CJK char in grid mode (default 2)\n"
        << "  --gap N                 spaces between grid cells (default 1)\n"
        << "  --no-alt                don't use alternate screen buffer\n"
        << "  -h, --help              show this help\n\n"
        << "Combinations: " << commas(total) << "\n";
}

static Opts parse_opts(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (++i >= argc) throw std::runtime_error("missing value after " + a);
            return argv[i];
        };

        if (a == "-h" || a == "--help") { help(argv[0]); std::exit(0); }
        else if (a == "--mode") {
            std::string v = next();
            if (v == "grid") o.mode = Mode::Grid;
            else if (v == "art" || v == "focus") o.mode = Mode::Art;
            else throw std::runtime_error("--mode must be grid or art");
        } else if (a == "--fps") {
            if (!parse_double(next(), o.fps) || o.fps < 0 || o.fps > 10000)
                throw std::runtime_error("bad --fps");
        } else if (a == "--start") {
            if (!parse_u64(next(), o.start) || o.start >= total)
                throw std::runtime_error("bad --start");
        } else if (a == "--art-width") {
            if (!parse_double(next(), o.art_width) || o.art_width < 0.10 || o.art_width > 0.95)
                throw std::runtime_error("--art-width must be 0.10..0.95");
        } else if (a == "--font") {
            o.font = next();
        } else if (a == "--info-style") {
            std::string v = next();
            if (v == "auto") o.info = InfoStyle::Auto;
            else if (v == "dec") o.info = InfoStyle::Dec;
            else if (v == "plain") o.info = InfoStyle::Plain;
            else throw std::runtime_error("--info-style must be auto, dec or plain");
        } else if (a == "--cjk-width") {
            o.cjk_width = std::atoi(next());
            if (o.cjk_width < 1 || o.cjk_width > 4) throw std::runtime_error("bad --cjk-width");
        } else if (a == "--gap") {
            o.gap = std::atoi(next());
            if (o.gap < 0 || o.gap > 16) throw std::runtime_error("bad --gap");
        } else if (a == "--no-alt") {
            o.alt_screen = false;
        } else {
            throw std::runtime_error("unknown option: " + a);
        }
    }
    return o;
}

static uint64_t draw_grid(std::string& out, TermSize ts, const Opts& o, uint64_t idx) {
    int content_rows = ts.rows - 2;
    int combo_w = 3 * o.cjk_width;
    int cell_w = std::max(1, combo_w + o.gap);
    int grid_cols = std::max(1, ts.cols / cell_w);
    uint64_t cap = uint64_t(content_rows) * grid_cols;
    uint64_t n = std::min<uint64_t>(cap, total - idx);

    clear_rows(out, 1, content_rows);
    uint64_t shown = 0;
    for (int row = 0; row < content_rows && shown < n; ++row) {
        cursor(out, row + 1, 1);
        out += "\x1b#5";
        for (int col = 0; col < grid_cols && shown < n; ++col, ++shown) {
            auto c = combo_at(idx + shown);
            out += combo_utf8(c);
            int pad = combo_w - c.n * o.cjk_width;
            if (pad > 0) out.append(size_t(pad), ' ');
            if (col + 1 < grid_cols) out.append(size_t(o.gap), ' ');
        }
        out += "\x1b[K";
    }
    return n;
}

static void draw_status(std::string& out, TermSize ts, int row, uint64_t done,
                        uint64_t session_start, Clock::time_point t0, const std::string& extra = {}) {
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    double rate = elapsed > 0.05 ? double(done - session_start) / elapsed : 0.0;
    double eta = rate > 0 ? double(total - done) / rate : INFINITY;

    std::ostringstream s;
    s << commas(done) << " / " << commas(total);
    if (rate > 0) s << " | " << fmt_rate(rate) << " | ETA " << fmt_time(eta);
    if (!extra.empty()) s << " | " << extra;
    center_plain(out, row, ts.cols, s.str());

    cursor(out, row + 1, 1);
    out += "\x1b#5" + progress(done, ts.cols) + "\x1b[K";
}

int main(int argc, char** argv) {
    try {
        auto o = parse_opts(argc, argv);
        if (!is_tty()) throw std::runtime_error("stdout is not a terminal; redirected output is disabled");

        setup_terminal();
        std::signal(SIGINT, on_signal);
#ifdef SIGTERM
        std::signal(SIGTERM, on_signal);
#endif

        FT_Library ft = nullptr;
        std::optional<Font> font;
        if (o.mode == Mode::Art) {
            if (FT_Init_FreeType(&ft)) throw std::runtime_error("FT_Init_FreeType failed");
            font.emplace(find_font(ft, o.font));
        }

        uint64_t idx = o.start;
        uint64_t session_start = idx;
        uint64_t frames = 0;
        auto t0 = Clock::now();
        auto next_frame = t0;
        TermSize old{-1, -1};

        {
            TermSession term(o.alt_screen);

            while (!stop_flag.load() && idx < total) {
                auto ts = term_size();
                if (ts.cols < 20 || ts.rows < 5) {
                    std::string out = "\x1b[H\x1b[2JTerminal too small";
                    std::fwrite(out.data(), 1, out.size(), stdout);
                    std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                if (ts.cols != old.cols || ts.rows != old.rows) {
                    old = ts;
                    std::fputs("\x1b[2J\x1b[H", stdout);
                    std::fflush(stdout);
                }

                std::string out;
                out.reserve(size_t(ts.cols) * ts.rows * 4);
                uint64_t advance = 0;

                if (o.mode == Mode::Grid) {
                    advance = draw_grid(out, ts, o, idx);
                    draw_status(out, ts, ts.rows - 1, idx + advance, session_start, t0);
                } else {
                    auto c = combo_at(idx);
                    auto uline = unicode_line(c);
                    auto bline = utf8_line(c);

                    bool dec = o.info == InfoStyle::Dec ||
                               (o.info == InfoStyle::Auto && dec_likely_supported());
                    if (dec && (int(uline.size()) > ts.cols / 2 || int(bline.size()) > ts.cols / 2)) dec = false;
                    if (dec && ts.rows < 9) dec = false;

                    int info_rows = dec ? 4 : 2;
                    int footer = info_rows + 2;
                    int art_rows = ts.rows - footer;
                    if (art_rows < 2) {
                        dec = false;
                        info_rows = 2;
                        footer = 4;
                        art_rows = ts.rows - footer;
                    }

                    clear_rows(out, 1, ts.rows);

                    int target_w = std::max(1, int(ts.cols * o.art_width));
                    int target_h = std::max(2, art_rows * 2);
                    int px_size = std::clamp(std::max(target_w / std::max(1, c.n), target_h) * 2, 64, 512);
                    auto bm = raster_text(*font, c, px_size);
                    draw_art(out, bm, 1, art_rows, ts.cols, o.art_width);

                    int info_row = art_rows + 1;
                    if (dec) {
                        center_dec(out, info_row, ts.cols, uline);
                        center_dec(out, info_row + 2, ts.cols, bline);
                    } else {
                        center_plain(out, info_row, ts.cols, uline);
                        center_plain(out, info_row + 1, ts.cols, bline);
                    }

                    std::string extra = font->name;
                    draw_status(out, ts, ts.rows - 1, idx + 1, session_start, t0, extra);
                    advance = 1;
                }

                std::fwrite(out.data(), 1, out.size(), stdout);
                std::fflush(stdout);
                idx += advance;
                ++frames;

                if (o.fps > 0 && idx < total && !stop_flag.load()) {
                    auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / o.fps));
                    next_frame += step;
                    auto now = Clock::now();
                    if (next_frame > now) std::this_thread::sleep_until(next_frame);
                    else next_frame = now;
                }
            }
        }

        auto elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        auto done = idx - session_start;
        double rate = elapsed > 0 ? double(done) / elapsed : 0;

        std::cout << "\n" << (idx >= total ? "Completed.\n" : "Stopped.\n")
                  << "Processed: " << commas(done) << "\n"
                  << "Next index: " << commas(idx) << "\n"
                  << "Frames: " << commas(frames) << "\n"
                  << "Elapsed: " << fmt_time(elapsed) << "\n"
                  << "Average: " << fmt_rate(rate) << "\n";
        if (font) std::cout << "Font: " << font->name << " (" << font->path << ")\n";
        if (idx < total) std::cout << "Resume: " << argv[0] << " --start " << idx
                                   << " --mode " << (o.mode == Mode::Art ? "art" : "grid")
                                   << " --fps " << o.fps << "\n";

        font.reset();
        if (ft) FT_Done_FreeType(ft);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
