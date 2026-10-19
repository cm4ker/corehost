// === tests/test_screen_buffer.cpp ===
// Screen buffer unit tests (screen_buffer.hpp, screen_buffer_row.hpp)
// Coverage: read/write, fill, scroll, clear, resize, char32_t text
#include "test_common.hpp"
#include "screen_buffer.hpp"
#include <cstring>

using namespace corehost::conpty;

// ==================================================================
// Basic read/write
// ==================================================================

bool test_set_and_get_char()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 0}, U'A', 0x07);
    ASSERT_EQ(sb.at_u32({0, 0}), U'A');
    ASSERT_EQ(sb.attr_at({0, 0}), (WORD)0x07);
    ASSERT_EQ(sb.at_u32({0, 1}), U' ');
    return true;
}

bool test_set_and_get_attr()
{
    screen_buffer sb({80, 25});
    sb.set_attr({5, 3}, 0x1F);
    ASSERT_EQ(sb.attr_at({5, 3}), (WORD)0x1F);
    ASSERT_EQ(sb.at_u32({5, 3}), U' ');
    return true;
}

bool test_glyph_at()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 0}, U'A', 0x07);
    auto gv = sb.glyph_at({0, 0});
    ASSERT_EQ(gv.size(), (size_t)1);
    ASSERT_EQ(gv[0], U'A');
    return true;
}

bool test_glyph_width()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 0}, U'A', 0x07);
    ASSERT_EQ(sb.glyph_width({0, 0}), 1);
    return true;
}

// ==================================================================
// Fill
// ==================================================================

bool test_fill_char_partial()
{
    screen_buffer sb({80, 25});
    auto res = sb.fill_char(U'X', {10, 5}, 5);
    ASSERT_EQ(res.cells_modified, (ULONG)5);
    ASSERT_EQ(sb.at_u32({10, 5}), U'X');
    ASSERT_EQ(sb.at_u32({14, 5}), U'X');
    ASSERT_EQ(sb.at_u32({15, 5}), U' ');
    return true;
}

bool test_fill_char_full()
{
    screen_buffer sb({80, 25});
    auto res = sb.fill_char(U'#', {0, 0}, 80);
    ASSERT_EQ(res.cells_modified, (ULONG)80);
    ASSERT_EQ(sb.at_u32({0, 0}), U'#');
    ASSERT_EQ(sb.at_u32({79, 0}), U'#');
    return true;
}

bool test_fill_char_clamped()
{
    screen_buffer sb({80, 25});
    auto res = sb.fill_char(U'#', {70, 0}, 20);
    ASSERT_EQ(res.cells_modified, (ULONG)20);
    ASSERT_EQ(sb.at_u32({79, 0}), U'#');
    ASSERT_EQ(sb.at_u32({0, 1}), U'#');
    ASSERT_EQ(sb.at_u32({9, 1}), U'#');
    ASSERT_EQ(sb.at_u32({10, 1}), U' ');
    return true;
}

bool test_fill_attr()
{
    screen_buffer sb({80, 25});
    auto res = sb.fill_attr(0x2E, {78, 10}, 5);
    ASSERT_EQ(res.cells_modified, (ULONG)5);
    ASSERT_EQ(sb.attr_at({78, 10}), (WORD)0x2E);
    ASSERT_EQ(sb.attr_at({79, 10}), (WORD)0x2E);
    ASSERT_EQ(sb.attr_at({0, 11}), (WORD)0x2E);
    ASSERT_EQ(sb.attr_at({2, 11}), (WORD)0x2E);
    ASSERT_EQ(sb.attr_at({3, 11}), (WORD)0x07);
    return true;
}

// ==================================================================
// Write/Read sequences
// ==================================================================

bool test_write_char32()
{
    screen_buffer sb({80, 25});
    char32_t seq[] = {U'H', U'e', U'l', U'l', U'o'};
    size_t n = sb.write_char32({0, 0}, seq, 5);
    ASSERT_EQ(n, (size_t)5);
    ASSERT_EQ(sb.at_u32({0, 0}), U'H');
    ASSERT_EQ(sb.at_u32({4, 0}), U'o');
    return true;
}

bool test_write_attr_seq()
{
    screen_buffer sb({80, 25});
    WORD attrs[] = {0x01, 0x02, 0x03};
    size_t n = sb.write_attr_seq({0, 0}, attrs, 3);
    ASSERT_EQ(n, (size_t)3);
    ASSERT_EQ(sb.attr_at({0, 0}), (WORD)0x01);
    return true;
}

bool test_read_wchars()
{
    screen_buffer sb({80, 25});
    char32_t seq[] = {U'X', U'Y', U'Z'};
    sb.write_char32({0, 0}, seq, 3);
    wchar_t out[4]{};
    size_t n = sb.read_wchars({0, 0}, out, 3);
    ASSERT_EQ(n, (size_t)3);
    ASSERT_EQ(out[0], L'X');
    ASSERT_EQ(out[2], L'Z');
    return true;
}

bool test_read_attrs()
{
    screen_buffer sb({80, 25});
    WORD attrs[] = {0x0A, 0x0B};
    sb.write_attr_seq({5, 5}, attrs, 2);
    WORD out[3]{};
    size_t n = sb.read_attrs({5, 5}, out, 2);
    ASSERT_EQ(n, (size_t)2);
    ASSERT_EQ(out[0], (WORD)0x0A);
    return true;
}

// ==================================================================
// Scroll
// ==================================================================

bool test_scroll_up()
{
    screen_buffer sb({80, 25});
    for (SHORT y = 0; y < 25; ++y)
        sb.set_u32({0, y}, static_cast<char32_t>(U'A' + y), 0x07);
    SMALL_RECT sr{0, 0, 79, 24};
    sb.scroll(sr, sr, false, {0, -1}, U'Z', 0x0A);
    ASSERT_EQ(sb.at_u32({0, 0}), U'B');
    ASSERT_EQ(sb.at_u32({0, 24}), U'Z');
    ASSERT_EQ(sb.attr_at({0, 24}), (WORD)0x0A);
    return true;
}

bool test_scroll_down()
{
    screen_buffer sb({80, 25});
    for (SHORT y = 0; y < 25; ++y)
        sb.set_u32({0, y}, static_cast<char32_t>(U'A' + y), 0x07);
    SMALL_RECT sr{0, 0, 79, 24};
    sb.scroll(sr, sr, false, {0, 1}, U'Z', 0x0A);
    ASSERT_EQ(sb.at_u32({0, 0}), U'Z');
    ASSERT_EQ(sb.attr_at({0, 0}), (WORD)0x0A);
    ASSERT_EQ(sb.at_u32({0, 24}), U'X');
    return true;
}

bool test_scroll_noop()
{
    screen_buffer sb({80, 25});
    SMALL_RECT sr{10, 10, 9, 10};
    sb.scroll(sr, sr, false, {0, 1}, U' ', 0x07);
    ASSERT_EQ(sb.at_u32({0, 0}), U' ');
    return true;
}

bool test_scroll_fill_uses_fill_char()
{
    screen_buffer sb({4, 2});
    sb.set_u32({0, 0}, U'A', 0x07);
    SMALL_RECT sr{0, 0, 0, 0};
    sb.scroll(sr, sr, false, {4, 0}, U'Z', 0x0A);
    ASSERT_EQ(sb.at_u32({0, 0}), U'Z');
    ASSERT_EQ(sb.attr_at({0, 0}), (WORD)0x0A);
    return true;
}

// As in conhost: the clip limits where the scroll writes, not what it reads,
// so source rows above the clip still move into it.
bool test_scroll_copies_source_from_outside_clip()
{
    screen_buffer sb({8, 10});
    for (SHORT y = 0; y < 10; ++y)
        sb.set_u32({0, y}, static_cast<char32_t>(U'A' + y), 0x07);
    SMALL_RECT sr{0, 0, 7, 9};
    SMALL_RECT clip{0, 3, 7, 9};
    sb.scroll(sr, clip, true, {0, 2}, U'.', 0x07);
    ASSERT_EQ(sb.at_u32({0, 2}), U'C');
    ASSERT_EQ(sb.at_u32({0, 3}), U'B');
    ASSERT_EQ(sb.at_u32({0, 4}), U'C');
    ASSERT_EQ(sb.at_u32({0, 9}), U'H');
    return true;
}

// The offset comes from the rectangle as given, before it is clamped to the
// buffer: rows 0..4 of sr{-2..4} land at dest.Y + 2.
bool test_scroll_offset_from_unclamped_source()
{
    screen_buffer sb({8, 10});
    for (SHORT y = 0; y < 10; ++y)
        sb.set_u32({0, y}, static_cast<char32_t>(U'A' + y), 0x07);
    SMALL_RECT sr{0, -2, 7, 4};
    sb.scroll(sr, sr, false, {0, 3}, U'.', 0x07);
    ASSERT_EQ(sb.at_u32({0, 4}), U'.');
    ASSERT_EQ(sb.at_u32({0, 5}), U'A');
    ASSERT_EQ(sb.at_u32({0, 9}), U'E');
    return true;
}

bool test_scroll_fill_row_write_and_readback()
{
    screen_buffer sb({8, 2});
    SMALL_RECT sr{0, 0, 7, 1};
    sb.scroll(sr, sr, false, {0, -1}, U'.', 0x0A);

    std::u32string text = U"abc";
    COORD cursor{0, 1};
    auto result = sb.write_text_row(cursor, text, 0x1E, text_measurement_mode::console, false);
    ASSERT_EQ(result.consumed, text.size());
    ASSERT(!result.row_end);
    ASSERT_EQ(cursor.X, (SHORT)3);

    wchar_t chars[8]{};
    WORD attrs[8]{};
    ASSERT_EQ(sb.read_wchars({0, 1}, chars, 8), (size_t)8);
    ASSERT_EQ(sb.read_attrs({0, 1}, attrs, 8), (size_t)8);
    ASSERT_EQ(chars[0], L'a');
    ASSERT_EQ(chars[2], L'c');
    ASSERT_EQ(chars[3], L'.');
    ASSERT_EQ(attrs[0], (WORD)0x1E);
    ASSERT_EQ(attrs[2], (WORD)0x1E);
    ASSERT_EQ(attrs[3], (WORD)0x0A);

    sb.set_attr({1, 1}, 0x2F);
    ASSERT_EQ(sb.at_u32({0, 1}), U'a');
    ASSERT_EQ(sb.at_u32({1, 1}), U'b');
    ASSERT_EQ(sb.attr_at({0, 1}), (WORD)0x1E);
    ASSERT_EQ(sb.attr_at({1, 1}), (WORD)0x2F);
    ASSERT_EQ(sb.attr_at({3, 1}), (WORD)0x0A);
    return true;
}

bool test_single_width_row_promotes_for_wide_glyph()
{
    screen_buffer sb({8, 2});
    std::u32string text = U"abcdef";
    COORD cursor{0, 0};
    auto result = sb.write_text_row(cursor, text, 0x1E, text_measurement_mode::console, false);
    ASSERT_EQ(result.consumed, text.size());
    ASSERT_EQ(cursor.X, (SHORT)6);

    sb.set_u32({2, 0}, U'你', 0x2F);
    ASSERT_EQ(sb.at_u32({0, 0}), U'a');
    ASSERT_EQ(sb.at_u32({2, 0}), U'你');
    ASSERT_EQ(sb.glyph_width({2, 0}), 2);
    ASSERT_EQ(sb.glyph_width({3, 0}), 0);
    ASSERT_EQ(sb.at_u32({4, 0}), U'e');
    ASSERT_EQ(sb.attr_at({2, 0}), (WORD)0x2F);
    ASSERT_EQ(sb.attr_at({4, 0}), (WORD)0x1E);
    return true;
}

bool test_fill_char_replaces_wide_glyph()
{
    screen_buffer sb({8, 2});
    sb.set_u32({2, 0}, U'你', 0x2F);
    auto res = sb.fill_char(U'X', {2, 0}, 2);
    ASSERT_EQ(res.cells_modified, (ULONG)2);
    ASSERT_EQ(sb.at_u32({2, 0}), U'X');
    ASSERT_EQ(sb.glyph_width({2, 0}), 1);
    ASSERT_EQ(sb.at_u32({3, 0}), U'X');
    ASSERT_EQ(sb.glyph_width({3, 0}), 1);
    ASSERT_EQ(sb.attr_at({2, 0}), (WORD)0x07);
    ASSERT_EQ(sb.attr_at({3, 0}), (WORD)0x07);
    return true;
}

// ==================================================================
// Clear
// ==================================================================

bool test_clear()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 0}, U'X', 0x1F);
    sb.clear();
    ASSERT_EQ(sb.at_u32({0, 0}), U' ');
    ASSERT_EQ(sb.attr_at({0, 0}), (WORD)0x07);
    return true;
}

bool test_clear_cell()
{
    screen_buffer sb({80, 25});
    sb.set_u32({10, 5}, U'Z', 0x2F);
    sb.clear_cell({10, 5});
    ASSERT_EQ(sb.at_u32({10, 5}), U' ');
    ASSERT_EQ(sb.attr_at({10, 5}), (WORD)0x07);
    return true;
}

// ==================================================================
// Resize
// ==================================================================

bool test_resize_expand()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 0}, U'H', 0x07);
    sb.resize({120, 30});
    ASSERT_EQ(sb.size.X, (SHORT)120);
    ASSERT_EQ(sb.size.Y, (SHORT)30);
    ASSERT_EQ(sb.at_u32({0, 0}), U'H');
    return true;
}

bool test_resize_shrink()
{
    screen_buffer sb({80, 25});
    sb.set_u32({79, 24}, U'Z', 0x07);
    sb.resize({40, 10});
    ASSERT_EQ(sb.size.X, (SHORT)40);
    ASSERT_EQ(sb.size.Y, (SHORT)10);
    return true;
}

// ==================================================================
// Edge cases
// ==================================================================

bool test_out_of_bounds()
{
    screen_buffer sb({80, 25});
    sb.set_u32({80, 0}, U'X', 0x07);
    sb.set_u32({0, 25}, U'X', 0x07);
    ASSERT(true);
    return true;
}

bool test_ci_roundtrip()
{
    screen_buffer sb({80, 25});
    sb.set_u32({0, 3}, U'H', 0x1F);
    sb.set_u32({1, 3}, U'i', 0x2E);

    CHAR_INFO ci[80]{};
    sb.row_to_ci(3, ci);
    ASSERT_EQ(ci[0].Attributes, (WORD)0x1F);

    sb.clear();
    sb.row_from_ci(3, ci, 2);
    ASSERT_EQ(sb.at_u32({0, 3}), U'H');
    ASSERT_EQ(sb.attr_at({0, 3}), (WORD)0x1F);
    ASSERT_EQ(sb.at_u32({1, 3}), U'i');
    return true;
}

// A wide glyph's halves both carry its char, told apart by
// COMMON_LVB_LEADING_BYTE/TRAILING_BYTE, as from conhost's ReadConsoleOutput
// and ReadConsoleOutputAttribute. The model itself never keeps the flags.
bool test_ci_marks_wide_glyph_halves()
{
    screen_buffer sb({8, 1});
    sb.set_u32({0, 0}, U'a', 0x07);
    sb.set_u32({1, 0}, U'你', 0x1E);
    CHAR_INFO ci[8]{};
    sb.row_to_ci(0, ci);
    ASSERT_EQ(ci[0].Attributes, (WORD)0x07);
    ASSERT_EQ(ci[1].Char.UnicodeChar, L'你');
    ASSERT_EQ(ci[1].Attributes, (WORD)(0x1E | COMMON_LVB_LEADING_BYTE));
    ASSERT_EQ(ci[2].Char.UnicodeChar, L'你');
    ASSERT_EQ(ci[2].Attributes, (WORD)(0x1E | COMMON_LVB_TRAILING_BYTE));
    ASSERT_EQ(ci[3].Attributes, (WORD)0x07);

    WORD attrs[4]{};
    ASSERT_EQ(sb.read_attrs_linear({0, 0}, attrs, 4), (size_t)4);
    ASSERT_EQ(attrs[0], (WORD)0x07);
    ASSERT_EQ(attrs[1], (WORD)(0x1E | COMMON_LVB_LEADING_BYTE));
    ASSERT_EQ(attrs[2], (WORD)(0x1E | COMMON_LVB_TRAILING_BYTE));

    sb.set_attr({4, 0}, 0x0317);
    ASSERT_EQ(sb.attr_at({4, 0}), (WORD)0x17);
    return true;
}

// WriteConsoleOutput's rules for flagged cells, from conhost: a leading half
// writes the glyph over two columns, a trailing half only its attribute
// unless it is the write's first cell, where it restores the whole glyph one
// column to the left.
bool test_ci_write_follows_wide_glyph_flags()
{
    screen_buffer sb({10, 6});
    for (SHORT y = 0; y < 6; ++y)
        for (SHORT x = 0; x < 10; ++x)
            sb.set_u32({x, y}, static_cast<char32_t>(U'0' + x), 0x07);
    const CHAR_INFO a{{L'a'}, 0x07};
    const CHAR_INFO b{{L'b'}, 0x07};
    const CHAR_INFO lead{{L'你'}, 0x0102};
    const CHAR_INFO trail{{L'你'}, 0x0204};

    const CHAR_INFO lone_trail[3] = {a, trail, b};
    sb.row_from_ci(0, lone_trail, 3);
    ASSERT_EQ(sb.at_u32({1, 0}), U'1');
    ASSERT_EQ(sb.attr_at({1, 0}), (WORD)0x04);
    sb.row_from_ci(0, &lead, 1, 9);
    ASSERT_EQ(sb.at_u32({9, 0}), U' ');

    const CHAR_INFO trail_first[2] = {trail, b};
    sb.row_from_ci(1, trail_first, 2, 3);
    ASSERT_EQ(sb.at_u32({2, 1}), U'你');
    ASSERT_EQ(sb.glyph_width({2, 1}), 2);
    ASSERT_EQ(sb.attr_at({2, 1}), (WORD)0x07);
    ASSERT_EQ(sb.attr_at({3, 1}), (WORD)0x04);
    ASSERT_EQ(sb.at_u32({4, 1}), U'b');
    sb.row_from_ci(2, trail_first, 2);
    ASSERT_EQ(sb.at_u32({0, 2}), U' ');
    ASSERT_EQ(sb.glyph_width({0, 2}), 1);

    const CHAR_INFO lead_last[2] = {a, lead};
    sb.row_from_ci(3, lead_last, 2);
    ASSERT_EQ(sb.at_u32({1, 3}), U'你');
    ASSERT_EQ(sb.glyph_width({1, 3}), 2);
    ASSERT_EQ(sb.attr_at({2, 3}), (WORD)0x07);
    ASSERT_EQ(sb.at_u32({3, 3}), U'3');

    const CHAR_INFO pair[2] = {lead, trail};
    sb.row_from_ci(4, pair, 2, 1);
    CHAR_INFO ci[10]{};
    sb.row_to_ci(4, ci);
    ASSERT_EQ(ci[1].Attributes, (WORD)0x0102);
    ASSERT_EQ(ci[2].Attributes, (WORD)0x0204);
    ASSERT_EQ(ci[3].Char.UnicodeChar, L'3');

    const CHAR_INFO lone_lead[3] = {a, lead, b};
    sb.row_from_ci(5, lone_lead, 3);
    ASSERT_EQ(sb.at_u32({1, 5}), U' ');
    ASSERT_EQ(sb.at_u32({2, 5}), U'b');
    ASSERT_EQ(sb.glyph_width({2, 5}), 1);
    return true;
}

// Overwriting half of a wide glyph blanks the other half, as in conhost. An
// overwritten leading half left the glyph's char in the trailing column, a
// write ending where a wide glyph starts took that glyph's trailing half,
// and a wide glyph written over narrow ones kept their text in its own.
bool test_overwrite_half_of_wide_glyph_blanks_other_half()
{
    screen_buffer sb({8, 3});
    const std::u32string two = U"你好";
    sb.write_char32_linear({0, 0}, two.data(), two.size());
    sb.write_char32_linear({1, 0}, U"Q", 1);
    ASSERT_EQ(sb.at_u32({0, 0}), U' ');
    ASSERT_EQ(sb.at_u32({1, 0}), U'Q');
    ASSERT_EQ(sb.at_u32({2, 0}), U'好');
    ASSERT_EQ(sb.glyph_width({2, 0}), 2);

    sb.write_char32_linear({0, 1}, two.data(), two.size());
    sb.write_char32_linear({0, 1}, U"Q", 1);
    ASSERT_EQ(sb.at_u32({1, 1}), U' ');
    ASSERT_EQ(sb.glyph_width({1, 1}), 1);
    ASSERT_EQ(sb.at_u32({2, 1}), U'好');

    sb.write_char32_linear({0, 2}, U"abc", 3);
    sb.set_attr({1, 2}, 0x2E);
    sb.write_char32_linear({0, 2}, U"你", 1);
    ASSERT(sb.glyph_at({0, 2}) == std::u32string_view{U"你"});
    ASSERT_EQ(sb.attr_at({1, 2}), (WORD)0x2E);
    ASSERT_EQ(sb.at_u32({2, 2}), U'c');
    return true;
}

// ReadConsoleOutputCharacterW gives one char per glyph, and a half cut off by
// either end of the read comes back as a space, as from conhost.
bool test_read_wchars_linear_one_per_glyph()
{
    screen_buffer sb({8, 1});
    const std::u32string text = U"a你好" U"b";
    sb.write_char32_linear({0, 0}, text.data(), text.size());
    wchar_t out[8]{};
    ASSERT_EQ(sb.read_wchars_linear({0, 0}, out, 6), (size_t)4);
    ASSERT(std::wstring_view(out, 4) == L"a你好" L"b");
    ASSERT_EQ(sb.read_wchars_linear({2, 0}, out, 3), (size_t)2);
    ASSERT(std::wstring_view(out, 2) == L" 好");
    ASSERT_EQ(sb.read_wchars_linear({2, 0}, out, 2), (size_t)2);
    ASSERT(std::wstring_view(out, 2) == L"  ");
    return true;
}

// ==================================================================
// Test Runner
// ==================================================================

int main()
{
    std::wcout << L"=== screen_buffer Tests ===" << std::endl;

    RUN_TEST(test_set_and_get_char, L"Set/Get char");
    RUN_TEST(test_set_and_get_attr, L"Set/Get attr");
    RUN_TEST(test_glyph_at, L"glyph_at");
    RUN_TEST(test_glyph_width, L"glyph_width");

    RUN_TEST(test_fill_char_partial, L"fill char partial");
    RUN_TEST(test_fill_char_full, L"fill char full");
    RUN_TEST(test_fill_char_clamped, L"fill char clamped");
    RUN_TEST(test_fill_attr, L"fill attr");

    RUN_TEST(test_write_char32, L"write_char32");
    RUN_TEST(test_write_attr_seq, L"write_attr_seq");

    RUN_TEST(test_read_wchars, L"read_wchars");
    RUN_TEST(test_read_attrs, L"read_attrs");

    RUN_TEST(test_scroll_up, L"scroll up");
    RUN_TEST(test_scroll_down, L"scroll down");
    RUN_TEST(test_scroll_noop, L"scroll noop");
    RUN_TEST(test_scroll_fill_uses_fill_char, L"scroll fill char");
    RUN_TEST(test_scroll_copies_source_from_outside_clip, L"scroll copies source from outside the clip");
    RUN_TEST(test_scroll_offset_from_unclamped_source, L"scroll offset from the unclamped source");
    RUN_TEST(test_scroll_fill_row_write_and_readback, L"scroll fill row write/readback");
    RUN_TEST(test_single_width_row_promotes_for_wide_glyph, L"single-width row promotes for wide glyph");
    RUN_TEST(test_fill_char_replaces_wide_glyph, L"fill char replaces wide glyph");
    RUN_TEST(test_ci_marks_wide_glyph_halves, L"CHAR_INFO marks wide glyph halves");
    RUN_TEST(test_ci_write_follows_wide_glyph_flags, L"CHAR_INFO write follows wide glyph flags");
    RUN_TEST(test_overwrite_half_of_wide_glyph_blanks_other_half, L"overwriting half a wide glyph blanks the other half");
    RUN_TEST(test_read_wchars_linear_one_per_glyph, L"read_wchars_linear: one char per glyph");

    RUN_TEST(test_clear, L"clear");
    RUN_TEST(test_clear_cell, L"clear_cell");

    RUN_TEST(test_resize_expand, L"resize expand");
    RUN_TEST(test_resize_shrink, L"resize shrink");

    RUN_TEST(test_out_of_bounds, L"out of bounds");
    RUN_TEST(test_ci_roundtrip, L"CHAR_INFO roundtrip");

    std::wcout << L"  " << tests_passed << L" passed, " << tests_failed << L" failed, " << (tests_passed + tests_failed)
               << L" total." << std::endl;
    return tests_failed > 0 ? 1 : 0;
}
