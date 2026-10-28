// Start-up screens (runtime/startup_check.hpp, ui/startup_canvas.hpp): disc image classification, the
// message chosen for each problem and that it fits the screen, word wrap, the font and the progress bar.
// With a directory argument, every screen is also written there as a PPM image for review.
#include "runtime/startup_check.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace twv::vita;
namespace ui = twv::ui;

namespace {
const char* outDir = nullptr;

void put_be32(std::vector<uint8_t>& b, std::size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}

// A GameCube-like image: header, DOL at 0x10000, FST at 0x20000 listing `files` (offset, size) pairs.
struct Image {
    std::vector<uint8_t> head = std::vector<uint8_t>(DiscHeaderBytes, 0);
    std::vector<uint8_t> fst;
    uint64_t end = 0;
};
Image make_image(const char* id, std::vector<std::pair<uint32_t, uint32_t>> files) {
    Image img;
    std::memcpy(img.head.data(), id, 6);
    put_be32(img.head, 0x1C, GameCubeMagic);
    const uint32_t entries = uint32_t(files.size()) + 1;
    img.fst.assign(entries * 12 + 16, 0);
    img.fst[0] = 1;
    put_be32(img.fst, 8, entries);
    img.end = 0x20000 + img.fst.size();
    for (std::size_t i = 0; i < files.size(); ++i) {
        put_be32(img.fst, (i + 1) * 12 + 4, files[i].first);
        put_be32(img.fst, (i + 1) * 12 + 8, files[i].second);
        if (uint64_t(files[i].first) + files[i].second > img.end) img.end = uint64_t(files[i].first) + files[i].second;
    }
    put_be32(img.head, 0x420, 0x10000);
    put_be32(img.head, 0x424, 0x20000);
    put_be32(img.head, 0x428, uint32_t(img.fst.size()));
    return img;
}
DiscCheck check(const Image& img, uint64_t size) {
    DiscCheck d = check_disc_header(img.head.data(), img.head.size(), size);
    if (d.problem == DiscProblem::None) check_disc_files(d, img.fst.data(), img.fst.size());
    return d;
}

void write_ppm(const char* name, const std::vector<uint32_t>& px) {
    if (!outDir) return;
    const std::string path = std::string(outDir) + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    assert(f);
    std::fprintf(f, "P6\n%d %d\n255\n", ui::ScreenWidth, ui::ScreenHeight);
    for (uint32_t p : px) {
        const unsigned char rgb[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

void render(const char* name, const MessageText& t) {
    assert(t.message.title && *t.message.title && t.kind[0]);
    const ui::Layout measured = ui::draw_message(nullptr, t.message);
    if (!measured.fits) std::fprintf(stderr, "%s: %d lines, bottom %d\n", name, measured.lines, measured.bottom);
    assert(measured.fits);
    std::vector<uint32_t> px(std::size_t(ui::ScreenWidth) * ui::ScreenHeight, 0);
    const ui::Canvas c{px.data(), ui::ScreenWidth, ui::ScreenHeight, ui::ScreenWidth};
    const ui::Layout drawn = ui::draw_message(&c, t.message);
    assert(drawn.lines == measured.lines && drawn.bottom == measured.bottom);
    std::size_t lit = 0;
    for (uint32_t p : px) {
        assert((p >> 24) == 0xff); // every pixel written, opaque
        lit += p != ui::palette::Background;
    }
    assert(lit > 2000);
    write_ppm(name, px);
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1) outDir = argv[1];

    // Font: built at compile time; spot checks and the '?' fallback.
    static_assert(ui::Font['A' - ui::FontFirst][3] == 0x1F && ui::Font['A' - ui::FontFirst][0] == 0x0E);
    static_assert(ui::Font['g' - ui::FontFirst][8] == 0x0E); // descender
    static_assert(&ui::glyph('\x7f') == &ui::glyph('?') && &ui::glyph('\xe9') == &ui::glyph('?'));
    for (int c = ui::FontFirst + 1; c <= ui::FontLast; ++c) {
        int bits = 0;
        for (uint8_t row : ui::Font[c - ui::FontFirst]) bits += __builtin_popcount(row);
        assert(bits > 0); // every printable character but space has pixels
    }
    {   // Pixels of "I!" at scale 2: four per font pixel, nothing outside the two cells.
        std::vector<uint32_t> px(64 * 32, 0);
        const ui::Canvas c{px.data(), 64, 32, 64};
        assert(ui::draw_text(c, 1, 1, 2, 0xffffffffu, "I!") == ui::text_width(2, 2));
        int bits = 0;
        for (char ch : {'I', '!'})
            for (uint8_t row : ui::glyph(ch)) bits += __builtin_popcount(row);
        std::size_t lit = 0;
        for (uint32_t p : px) lit += p != 0;
        assert(lit == std::size_t(bits) * 4);
        ui::fill_rect(c, -5, -5, 200, 200, 7); // clipped, no overflow under ASan
        for (uint32_t p : px) assert(p == 7);
    }

    // Word wrap: spaces, a long word (a path) cut at the line end, the line limit.
    {
        ui::TextLine l[8];
        bool cut = true;
        assert(ui::wrap_text("one two three", 7, l, 8, &cut) == 2 && !cut);
        assert(l[0].start == 0 && l[0].length == 7 && l[1].start == 8 && l[1].length == 5);
        assert(ui::wrap_text("ux0:data/twilight-vita/game.iso", 10, l, 8) == 4 && l[3].length == 1);
        assert(ui::wrap_text("  ", 10, l, 8) == 0);
        assert(ui::wrap_text("a b c d e f", 1, l, 3, &cut) == 3 && cut);
    }

    // Disc images.
    const char* path = "ux0:data/twilight-vita/game.iso";
    const Image pal = make_image("GZ2P01", {{0x30000, 0x1000}, {0x50000000, 0x200000}});
    DiscCheck d = check(pal, GameCubeDiscBytes);
    assert(d.problem == DiscProblem::None && d.tested && std::strcmp(d.gameId, "GZ2P01") == 0);
    assert(d.needed == 0x50200000ull);
    d = check(pal, 0x50200000ull); // trimmed after the last file: fine
    assert(d.problem == DiscProblem::None);
    d = check(pal, 0x50200000ull - 1); // the last file is cut
    assert(d.problem == DiscProblem::Truncated && d.needed == 0x50200000ull);
    d = check(pal, 0x20010); // the FST itself is cut: caught from the header alone
    assert(d.problem == DiscProblem::Truncated && d.fstSize);
    for (const char* id : {"GZ2E01", "GZ2J01"}) {
        d = check(make_image(id, {{0x30000, 16}}), GameCubeDiscBytes);
        assert(d.problem == DiscProblem::None && !d.tested); // Dusklight accepts them; untested on the Vita
    }
    for (const char* id : {"GALE01", "GZ2P8P", "GZ2X01", "RZDE01"}) {
        d = check(make_image(id, {{0x30000, 16}}), GameCubeDiscBytes);
        assert(d.problem == DiscProblem::WrongGame && std::strcmp(d.gameId, id) == 0);
    }
    const struct {const char* magic; const char* name;} containers[] = {
        {"CISO", "CISO"}, {"WBFS", "WBFS"}, {"RVZ\x01", "RVZ"}, {"WIA\x01", "WIA"}, {"EGGS", "NFS"},
        {"\xB1\x0B\xC0\x01", "GCZ"}, {"\xAE\x0F\x38\xA2", "TGC"}};
    for (const auto& c : containers) {
        std::vector<uint8_t> h(DiscHeaderBytes, 0);
        std::memcpy(h.data(), c.magic, 4);
        d = check_disc_header(h.data(), h.size(), 1u << 30);
        assert(d.problem == DiscProblem::Container && std::strcmp(d.format, c.name) == 0);
    }
    {
        Image nkit = make_image("GZ2P01", {{0x30000, 16}});
        std::memcpy(nkit.head.data() + 0x200, "NKIT", 4);
        assert(check(nkit, 900u << 20).problem == DiscProblem::NKit);
        Image wii = make_image("RZDP01", {{0x30000, 16}});
        put_be32(wii.head, 0x1C, 0);
        put_be32(wii.head, 0x18, WiiMagic);
        d = check(wii, 4ull << 30);
        assert(d.problem == DiscProblem::Wii && std::strcmp(d.gameId, "RZDP01") == 0);
        std::vector<uint8_t> text(DiscHeaderBytes, 'x'); // a renamed text file
        assert(check_disc_header(text.data(), text.size(), 5000).problem == DiscProblem::NotGameCube);
        std::vector<uint8_t> zeros(DiscHeaderBytes, 0);
        assert(check_disc_header(zeros.data(), zeros.size(), GameCubeDiscBytes).problem == DiscProblem::NotGameCube);
        assert(check_disc_header(zeros.data(), 0x10, 0x10).problem == DiscProblem::Unreadable);
        Image shortHead = make_image("GZ2P01", {});
        assert(check_disc_header(shortHead.head.data(), 0x100, 0x100).problem == DiscProblem::Truncated);
        Image badFst = make_image("GZ2P01", {{0x30000, 16}});
        badFst.fst[0] = 0; // root is not a directory
        assert(check(badFst, GameCubeDiscBytes).problem == DiscProblem::NotGameCube);
        Image hugeFst = make_image("GZ2P01", {});
        put_be32(hugeFst.head, 0x428, FstLimit + 1);
        assert(check(hugeFst, GameCubeDiscBytes).problem == DiscProblem::NotGameCube);
    }

    // Messages: one per problem, each fits the screen (with long worst cases) and names the path.
    MessageText t;
    for (int p = 1; p <= int(DiscProblem::Truncated); ++p) {
        DiscCheck dc = check(pal, 0x50200000ull - 1);
        dc.problem = DiscProblem(p);
        dc.format = "CISO";
        std::strcpy(dc.gameId, "RZDP01");
        // The longest list main_vita.cpp passes: SimilarLimit characters.
        const char* similar = "The Legend of Zelda - Twilight Princess (Europe) (En,Fr,De,Es,It).iso, game.iso.iso";
        static_assert(std::char_traits<char>::length("The Legend of Zelda - Twilight Princess (Europe) (En,Fr,De,Es,It).iso, game.iso.iso") >= SimilarLimit - 4);
        disc_message(t, dc, path, p == int(DiscProblem::Missing) ? similar : nullptr);
        bool namesPath = false;
        for (const auto& item : t.message.items) namesPath |= item.text && item.style == ui::Style::Path && std::strcmp(item.text, path) == 0;
        assert(namesPath);
        assert(std::strcmp(t.kind + 5, DiscProblemNames[p]) == 0);
        render(t.kind, t);
    }
    disc_message(t, check(make_image("GALE01", {{0x30000, 16}}), GameCubeDiscBytes), path, nullptr);
    assert(std::strstr(t.message.items[0].text, "GALE01") && std::strstr(t.message.items[2].text, "GZ2E01"));
    shader_compiler_message(t);
    assert(std::strcmp(t.message.items[1].text, "ur0:data/libshacccg.suprx") == 0);
    render(t.kind, t);
    memory_message(t, "video memory (CDRAM)", 20u << 20, 32u << 20);
    assert(std::strstr(t.message.items[0].text, "32 MB of video memory (CDRAM); 20 MB is free"));
    render("memory", t);
    memory_message(t, "main memory", 0, 0);
    render("memory_unknown", t);
    arguments_message(t, "args.txt", "--vita-frame-overlap needs on, off or verify, once",
                      "--vita-frame-overlap=sometimes-maybe-with-a-very-long-value-that-wraps-twice");
    assert(std::strcmp(t.message.title, "Problem in args.txt") == 0 &&
           std::strcmp(t.message.items[3].text, "ux0:data/twilight-vita/args.txt") == 0);
    render("args_error", t);
    arguments_message(t, "sequence.txt", "sequence.txt exceeds 4096 bytes", nullptr);
    assert(std::strcmp(t.message.title, "Problem in sequence.txt") == 0 && !t.message.items[1].text);
    render("args_sequence", t);
    arguments_message(t, nullptr, "--vita-frame-overlap=on needs the GX worker (--vita-gx-thread=on, as in the play "
                      "profile) in a vitaGL/GXM build", "--vita-gpu=gxm");
    render("args_builtin", t);
    graphics_message(t, "sceGxmCreateRenderTarget 0x805b0004");
    render("graphics", t);

    // Progress: "Loading..." alone, then the bar; a partial redraw touches only the bar, count and label.
    std::vector<uint32_t> px(std::size_t(ui::ScreenWidth) * ui::ScreenHeight, 0);
    const ui::Canvas c{px.data(), ui::ScreenWidth, ui::ScreenHeight, ui::ScreenWidth};
    ui::Progress p;
    ui::draw_progress(c, p, true);
    assert(px[std::size_t(ui::BarY) * ui::ScreenWidth + ui::BarX] == ui::palette::Background);
    write_ppm("progress_loading", px);
    p.label = "Preparing graphics (first launch only)...";
    p.bar = true;
    p.done = 255;
    p.total = 510;
    p.note = "This takes about a minute once. Later launches skip it.";
    ui::draw_progress(c, p, true);
    const auto at = [&](int x, int y) { return px[std::size_t(y) * ui::ScreenWidth + std::size_t(x)]; };
    assert(at(ui::BarX + ui::BarW / 2 - 1, ui::BarY + 5) == ui::palette::BarFill);
    assert(at(ui::BarX + ui::BarW / 2 + 1, ui::BarY + 5) == ui::palette::BarEmpty);
    assert(at(ui::BarX - 2, ui::BarY + 5) == ui::palette::BarFrame);
    write_ppm("progress_half", px);
    const std::vector<uint32_t> before = px;
    p.done = 510;
    p.label = "Starting the game...";
    ui::draw_progress(c, p, false);
    assert(at(ui::BarX + ui::BarW - 1, ui::BarY + 5) == ui::palette::BarFill);
    for (int y = 0; y < ui::ScreenHeight; ++y) {
        const bool redrawn = (y >= ui::LabelY && y < ui::LabelY + ui::FontRows * 3) ||
                             (y >= ui::BarY && y < ui::BarY + ui::BarH) ||
                             (y >= ui::CountY && y < ui::CountY + ui::FontRows * 3);
        if (!redrawn)
            for (int x = 0; x < ui::ScreenWidth; ++x) assert(at(x, y) == before[std::size_t(y) * ui::ScreenWidth + std::size_t(x)]);
    }
    write_ppm("progress_done", px);
    p.done = 600; // more than the total: clamped
    ui::draw_progress(c, p, false);
    std::printf("startup_check_test: font, wrap, disc header/FST classification, messages fit, progress bar passed\n");
}
