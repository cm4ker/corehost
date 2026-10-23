#pragma once
// Inline images (iTerm2 OSC 1337 File=...) passed through to the terminal.
// The terminal draws the image at its cursor and moves the cursor below it,
// as many rows as the image takes; the screen model has to move the same
// way, or the next output (placed from the model) lands on the image. The
// rows follow from the File= arguments, the image's own pixel size (read
// from its header: PNG, GIF, JPEG, BMP, WebP) and the terminal's cell size
// in pixels (asked with CSI 16 t), computed as the terminal does (mite's
// images.zig resolveSpan).

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace corehost::conpty
{

struct image_pixel_size
{
    uint32_t width = 0;
    uint32_t height = 0;
};

namespace inline_image_detail
{

inline int base64_value(char32_t c) noexcept
{
    if (c >= U'A' && c <= U'Z')
        return static_cast<int>(c - U'A');
    if (c >= U'a' && c <= U'z')
        return static_cast<int>(c - U'a') + 26;
    if (c >= U'0' && c <= U'9')
        return static_cast<int>(c - U'0') + 52;
    if (c == U'+' || c == U'-')
        return 62;
    if (c == U'/' || c == U'_')
        return 63;
    return -1;
}

// The first bytes `b64` stands for, at most `limit` of them; stops at the
// padding or anything that isn't base64.
inline std::vector<uint8_t> decode_prefix(std::u32string_view b64, size_t limit)
{
    std::vector<uint8_t> out;
    out.reserve(std::min(limit, b64.size() / 4 * 3 + 3));
    uint32_t acc = 0;
    int bits = 0;
    for (const char32_t c : b64)
    {
        const int v = base64_value(c);
        if (v < 0)
            break;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
            if (out.size() >= limit)
                break;
        }
    }
    return out;
}

inline uint32_t be16(const std::vector<uint8_t> &d, size_t i) noexcept
{
    return (static_cast<uint32_t>(d[i]) << 8) | d[i + 1];
}
inline uint32_t be32(const std::vector<uint8_t> &d, size_t i) noexcept
{
    return (be16(d, i) << 16) | be16(d, i + 2);
}
inline uint32_t le16(const std::vector<uint8_t> &d, size_t i) noexcept
{
    return d[i] | (static_cast<uint32_t>(d[i + 1]) << 8);
}
inline uint32_t le24(const std::vector<uint8_t> &d, size_t i) noexcept
{
    return le16(d, i) | (static_cast<uint32_t>(d[i + 2]) << 16);
}
inline uint32_t le32(const std::vector<uint8_t> &d, size_t i) noexcept
{
    return le16(d, i) | (le16(d, i + 2) << 16);
}

} // namespace inline_image_detail

// The pixel size an image file declares in its header, from the start of
// its base64. A JPEG's size can sit past a large EXIF block, so up to
// 256 KiB is looked at.
inline std::optional<image_pixel_size> image_size_from_base64(std::u32string_view b64)
{
    using namespace inline_image_detail;
    const auto d = decode_prefix(b64, 256 * 1024);
    const size_t n = d.size();
    // PNG: signature, then the IHDR chunk.
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return image_pixel_size{be32(d, 16), be32(d, 20)};
    // GIF: the logical screen.
    if (n >= 10 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F')
        return image_pixel_size{le16(d, 6), le16(d, 8)};
    // BMP: BITMAPINFOHEADER (a negative height is top-down).
    if (n >= 26 && d[0] == 'B' && d[1] == 'M')
    {
        const auto h = static_cast<int32_t>(le32(d, 22));
        return image_pixel_size{le32(d, 18), static_cast<uint32_t>(h < 0 ? -h : h)};
    }
    // WebP: lossy (VP8), lossless (VP8L) or extended (VP8X).
    if (n >= 30 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' && d[9] == 'E' &&
        d[10] == 'B' && d[11] == 'P')
    {
        if (d[12] == 'V' && d[13] == 'P' && d[14] == '8' && d[15] == 'X')
            return image_pixel_size{le24(d, 24) + 1, le24(d, 27) + 1};
        if (d[12] == 'V' && d[13] == 'P' && d[14] == '8' && d[15] == 'L' && d[20] == 0x2f)
        {
            const uint32_t b = le32(d, 21);
            return image_pixel_size{(b & 0x3fff) + 1, ((b >> 14) & 0x3fff) + 1};
        }
        if (d[12] == 'V' && d[13] == 'P' && d[14] == '8' && d[15] == ' ' && d[23] == 0x9d && d[24] == 0x01 &&
            d[25] == 0x2a)
            return image_pixel_size{le16(d, 26) & 0x3fff, le16(d, 28) & 0x3fff};
        return std::nullopt;
    }
    // JPEG: the first start-of-frame segment.
    if (n >= 4 && d[0] == 0xff && d[1] == 0xd8)
    {
        size_t i = 2;
        while (i + 9 <= n)
        {
            if (d[i] != 0xff)
                return std::nullopt;
            const uint8_t marker = d[i + 1];
            if (marker == 0xff)
            {
                ++i;
                continue;
            }
            const bool sof = marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
            if (sof)
                return image_pixel_size{be16(d, i + 7), be16(d, i + 5)};
            if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01)
            {
                i += 2;
                continue;
            }
            i += 2 + be16(d, i + 2);
        }
    }
    return std::nullopt;
}

// How an iTerm2 File= argument sizes one side.
struct image_dim
{
    enum class kind : uint8_t
    {
        automatic,
        cells,
        pixels,
        percent,
    };
    kind k = kind::automatic;
    uint32_t n = 0;

    static image_dim parse(std::u32string_view v) noexcept
    {
        if (v.empty() || v == U"auto")
            return {};
        image_dim d{kind::cells, 0};
        if (v.ends_with(U"px"))
        {
            d.k = kind::pixels;
            v.remove_suffix(2);
        }
        else if (v.ends_with(U"%"))
        {
            d.k = kind::percent;
            v.remove_suffix(1);
        }
        if (v.empty())
            return {};
        for (const char32_t c : v)
        {
            if (c < U'0' || c > U'9' || d.n > 100000)
                return {};
            d.n = d.n * 10 + static_cast<uint32_t>(c - U'0');
        }
        return d;
    }

    // Cells this side takes; nullopt for automatic.
    std::optional<uint32_t> cells(double cell_px, uint32_t term_cols, uint32_t native, bool is_width) const noexcept
    {
        switch (k)
        {
        case kind::automatic:
            return std::nullopt;
        case kind::cells:
            return n;
        case kind::pixels:
            return std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(static_cast<double>(n) / cell_px)));
        case kind::percent:
            return std::max<uint32_t>(1, is_width ? term_cols * n / 100 : native * n / 100);
        }
        return std::nullopt;
    }
};

// The parsed arguments and payload of `ESC ] 1337 ; File = args : base64`.
struct iterm2_file
{
    image_dim width;
    image_dim height;
    std::u32string_view payload;
};

inline std::optional<iterm2_file> parse_iterm2_file(std::u32string_view raw) noexcept
{
    constexpr std::u32string_view prefix = U"\x1b]1337;File=";
    if (!raw.starts_with(prefix))
        return std::nullopt;
    auto rest = raw.substr(prefix.size());
    const auto colon = rest.find(U':');
    if (colon == std::u32string_view::npos)
        return std::nullopt;
    iterm2_file f;
    f.payload = rest.substr(colon + 1);
    auto args = rest.substr(0, colon);
    while (!args.empty())
    {
        const auto semi = args.find(U';');
        const auto kv = args.substr(0, semi);
        args = semi == std::u32string_view::npos ? std::u32string_view{} : args.substr(semi + 1);
        const auto eq = kv.find(U'=');
        if (eq == std::u32string_view::npos)
            continue;
        const auto key = kv.substr(0, eq);
        const auto val = kv.substr(eq + 1);
        if (key == U"width")
            f.width = image_dim::parse(val);
        else if (key == U"height")
            f.height = image_dim::parse(val);
    }
    return f;
}

// Rows an iTerm2 image (`raw`: the whole OSC) takes in a terminal with
// cells of `cell_w` x `cell_h` px (0 = not known) and `term_cols` columns;
// 0 when that can't be told (no cell size, an unknown image format).
inline int iterm2_inline_image_rows(std::u32string_view raw, uint32_t cell_w, uint32_t cell_h, uint32_t term_cols)
{
    const auto f = parse_iterm2_file(raw);
    if (!f)
        return 0;
    if (f->height.k == image_dim::kind::cells)
        return f->height.n <= 1000 ? static_cast<int>(std::max<uint32_t>(f->height.n, 1)) : 0;
    if (cell_w == 0 || cell_h == 0)
        return 0;
    const auto px = image_size_from_base64(f->payload);
    if (!px || px->width == 0 || px->height == 0)
        return 0;
    const double iw = px->width, ih = px->height, cw = cell_w, ch = cell_h;
    const uint32_t native_cols = (px->width + cell_w - 1) / cell_w;
    const uint32_t native_rows = (px->height + cell_h - 1) / cell_h;
    const auto box_cols = f->width.cells(cw, term_cols, native_cols, true);
    const auto box_rows = f->height.cells(ch, term_cols, native_rows, false);
    uint32_t rows = 0;
    if (box_rows)
        rows = *box_rows;
    else if (box_cols)
        rows = static_cast<uint32_t>(std::round(static_cast<double>(*box_cols) * cw * ih / iw / ch));
    else
        rows = native_rows;
    rows = std::max<uint32_t>(rows, 1);
    return rows <= 1000 ? static_cast<int>(rows) : 0;
}

} // namespace corehost::conpty
