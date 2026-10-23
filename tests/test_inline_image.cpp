// === tests/test_inline_image.cpp ===
// Inline images: pixel sizes from image headers and the rows an iTerm2
// image takes, as the terminal (mite) computes them.
#include "test_common.hpp"
#include "inline_image.hpp"

#include <string>

using namespace corehost::conpty;

// A 1x1 PNG, with its IHDR patched to `w` x `h`.
static std::u32string png_b64(uint32_t w, uint32_t h)
{
    unsigned char bytes[24] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    for (int i = 0; i < 4; ++i)
    {
        bytes[16 + i] = static_cast<unsigned char>(w >> (24 - 8 * i));
        bytes[20 + i] = static_cast<unsigned char>(h >> (24 - 8 * i));
    }
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::u32string out;
    for (int i = 0; i < 24; i += 3)
    {
        const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        for (int k = 3; k >= 0; --k)
            out.push_back(static_cast<char32_t>(alphabet[(v >> (6 * k)) & 63]));
    }
    return out;
}

static std::u32string osc(std::u32string_view args, const std::u32string &b64)
{
    return std::u32string(U"\x1b]1337;File=") + std::u32string(args) + U":" + b64 + U"\x07";
}

bool test_png_size()
{
    const auto s = image_size_from_base64(png_b64(640, 480));
    ASSERT(s.has_value());
    ASSERT_EQ(s->width, 640u);
    ASSERT_EQ(s->height, 480u);
    return true;
}

bool test_gif_size()
{
    // "GIF89a" + width 20 + height 10, little endian: R0lGODlhFAAKAA==
    const auto s = image_size_from_base64(U"R0lGODlhFAAKAA==");
    ASSERT(s.has_value());
    ASSERT_EQ(s->width, 20u);
    ASSERT_EQ(s->height, 10u);
    return true;
}

bool test_unknown_format()
{
    ASSERT(!image_size_from_base64(U"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA").has_value());
    return true;
}

bool test_rows_native()
{
    // 40x60 px in 10x20 cells: 3 rows.
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1", png_b64(40, 60)), 10, 20, 80), 3);
    // 41x61: rounded up, 4 rows.
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1", png_b64(41, 61)), 10, 20, 80), 4);
    return true;
}

bool test_rows_width_cells()
{
    // 12 cells wide (120 px) of a 640x480 image: 90 px high, 4.5 rows -> 5.
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1;width=12", png_b64(640, 480)), 10, 20, 80), 5);
    return true;
}

bool test_rows_explicit_height()
{
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1;height=7", png_b64(640, 480)), 0, 0, 80), 7);
    // In pixels: 100 px over 20 px cells.
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1;height=100px", png_b64(640, 480)), 10, 20, 80), 5);
    return true;
}

bool test_rows_unknown_cell()
{
    ASSERT_EQ(iterm2_inline_image_rows(osc(U"inline=1", png_b64(40, 60)), 0, 0, 80), 0);
    return true;
}

int main()
{
    std::wcout << L"Inline image tests:\n";
    RUN_TEST(test_png_size, L"PNG size");
    RUN_TEST(test_gif_size, L"GIF size");
    RUN_TEST(test_unknown_format, L"Unknown format");
    RUN_TEST(test_rows_native, L"Rows from the native size");
    RUN_TEST(test_rows_width_cells, L"Rows from a width in cells");
    RUN_TEST(test_rows_explicit_height, L"Explicit height");
    RUN_TEST(test_rows_unknown_cell, L"No cell size");
    std::wcout << L"  " << tests_passed << L" passed, " << tests_failed << L" failed, " << (tests_passed + tests_failed)
               << L" total." << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
