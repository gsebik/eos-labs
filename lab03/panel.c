/* EP-0172 display driver: a 3.5" ST7796 panel on SPI0.
 *
 * This renderer is deliberately blocking: the display task waits for every
 * transfer. That is the baseline the timing lab investigates, not a claim that
 * DMA would let a task sleep. Only the display task owns SPI0.
 *
 * Coordinates. The panel is mounted landscape on the carrier, so it is
 * addressed as 480 pixels across and 320 down, with the origin at the top-left:
 *
 *     x = 0 .. 479   left to right   (frequency: band 0 is at the left)
 *     y = 0 .. 319   top to bottom   (a bar grows upward, so it uses small y)
 *
 * MADCTL (0x36) decides how those coordinates map onto the glass. The four
 * landscape values are 0x28, 0x68, 0x88 and 0xE8; they are the four rotations
 * of the same image, and exactly one of them is upright for a given carrier.
 * On this station 0xE8 is correct. If the picture is ever rotated or mirrored,
 * change only this value and rebuild - nothing else needs to move. */
#include "panel.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

static uint32_t spi_hz;

/* Send one controller command: data/command low, one byte, then back high. */
static void command(uint8_t value)
{
    while (spi_is_busy(spi0)) tight_loop_contents();
    gpio_put(6, 0);
    spi_write_blocking(spi0, &value, 1);
    while (spi_is_busy(spi0)) tight_loop_contents();
    gpio_put(6, 1);
}
/* Send raw bytes in whichever mode data/command is already in. */
static void bytes(const uint8_t *data, size_t count)
{
    spi_write_blocking(spi0, data, count);
}
/* Write one controller register. */
static void setting(uint8_t reg, uint8_t value)
{
    command(reg);
    bytes(&value, 1);
}
/* Fill an axis-aligned rectangle with one colour.
 *
 * Used once, to clear the panel after reset: the controller's memory is
 * undefined at power-on, so the area the frame renderer does not touch would
 * otherwise show as noise.
 */
static void rectangle(unsigned x, unsigned y, unsigned width, unsigned height,
                      uint16_t color)
{
    const unsigned right = x + width - 1;
    const unsigned bottom = y + height - 1;
    uint8_t columns[] = { x >> 8, x & 255, right >> 8, right & 255 };
    uint8_t rows[] = { y >> 8, y & 255, bottom >> 8, bottom & 255 };
    command(0x2a); bytes(columns, sizeof columns);
    command(0x2b); bytes(rows, sizeof rows);
    command(0x2c);
    static uint8_t line[960];
    for (unsigned i = 0; i < width; ++i) {
        line[2 * i] = color >> 8;
        line[2 * i + 1] = color & 255;
    }
    for (unsigned y = 0; y < height; ++y) bytes(line, width * 2);
}

/* Reset sequence for the ST7796, then one full clear.
 *
 * The panel comes up with inverted pixel data, which is why INVON (0x21) is
 * sent: without it black shows as white. */
void sig_panel_init(void)
{
    spi_hz = spi_init(spi0, 40000000);
    gpio_set_function(2, GPIO_FUNC_SPI);
    gpio_set_function(3, GPIO_FUNC_SPI);
    for (unsigned pin = 5; pin <= 7; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 1);
    }
    sleep_ms(10); gpio_put(7, 0);
    sleep_ms(20); gpio_put(7, 1); sleep_ms(120);
    gpio_put(5, 0);
    command(0x01); sleep_ms(120);
    command(0x11); sleep_ms(120);
    setting(0xf0, 0xc3); setting(0xf0, 0x96);
    setting(0x36, 0xE8); setting(0x3a, 0x55); setting(0xb4, 0x01);
    const uint8_t b6[] = { 0x80, 0x02, 0x3b };
    command(0xb6); bytes(b6, sizeof b6);
    const uint8_t e8[] = { 0x40, 0x8a, 0, 0, 0x29, 0x19, 0xa5, 0x33 };
    command(0xe8); bytes(e8, sizeof e8);
    setting(0xc1, 0x06); setting(0xc2, 0xa7); setting(0xc5, 0x18);
    sleep_ms(120);
    const uint8_t positive[] = {
        0xf0, 9, 0x0b, 6, 4, 0x15, 0x2f, 0x54, 0x42, 0x3c, 0x17, 0x14, 0x18, 0x1b
    };
    const uint8_t negative[] = {
        0xe0, 9, 0x0b, 6, 4, 3, 0x2b, 0x43, 0x42, 0x3b, 0x16, 0x14, 0x17, 0x1b
    };
    command(0xe0); bytes(positive, sizeof positive);
    command(0xe1); bytes(negative, sizeof negative);
    setting(0xf0, 0x3c); setting(0xf0, 0x69);
    sleep_ms(120);
    /* INVON. This panel comes up with inverted pixel data: black shows as
     * white and green as magenta. 52Pi's own driver sends 0x21; the es201
     * reference init does not (inverted bars still look like bars, so it went
     * unnoticed). Without this the cleared background would be white. */
    command(0x21);
    command(0x29);
    /* GRAM is undefined after reset, so the area the renderer does not touch
     * would show as noise. Clear the panel once here; the periodic renderer
     * redraws a bounded plot region, so a single fill is enough. */
    sleep_ms(20);
    rectangle(0, 0, 480, 320, 0x0000);
}

/* ---- Plot geometry -------------------------------------------------------
 * The panel is mounted landscape, but on this carrier its MADCTL cannot
 * produce an upright image: the mirror bits move the horizontal direction and
 * leave the picture upside down. The renderer therefore compensates in
 * software, and these three lines are the whole of it:
 *
 *   1. the layout below is written in the order the eye sees it;
 *   2. glyph rows are mirrored, so text is not upside down;
 *   3. send_row() reverses each row, so the picture is not left-to-right
 *      mirrored.
 *
 * Together those apply the 180 degree rotation the panel is missing. If the
 * panel is ever replaced, only this file needs revisiting.
 *
 * Everything below is written in "glass" rows, with the panel's flip already
 * applied, so the code reads in the order the eye sees:
 *
 *     y = 0                     +---------------------------+
 *                               |  frequency axis + labels  |
 *     y = AXIS_H                +---------------------------+
 *                               |                           |
 *                               |      spectrum plot        |  bar HEIGHT
 *                               |                           |  = band strength
 *     y = AXIS_H + rows         +---------------------------+
 *                               | frequency position bar    |
 *     y = AXIS_H+rows+SCALE_H   +---------------------------+
 *                               | tone readout and counters |
 *     y = ... + TEXT_H          +---------------------------+
 *
 * `rows` is the tallest bar in pixels, from EOS_DISPLAY_ROWS. It is both the
 * plot height and the pixel-transfer budget, so doubling it roughly doubles
 * the redraw time: an experiment control, not a cosmetic setting. */
enum { PANEL_W = 480, PANEL_H = 320, BAND_PITCH = 15, BAR_W = 11 };
enum { AXIS_H = 12, SCALE_H = 26, TEXT_H = 14 };
enum { PLOT_MAX = PANEL_H - AXIS_H - SCALE_H - TEXT_H };

/* Colours in RGB565: 5 bits red, 6 bits green, 5 bits blue. */
enum { COLOUR_BLACK = 0x0000, COLOUR_BAR = 0x07e0, COLOUR_AXIS = 0x7bef,
       COLOUR_TONE = 0xfd20, COLOUR_GAP = 0xf800 };

/* 5x7 glyphs: seven rows, five bits each, left column is the high bit. */
static const uint8_t glyphs[][7] = {
    {14, 17, 19, 21, 25, 17, 14}, /* 0 */
    {4, 12, 4, 4, 4, 4, 14},      /* 1 */
    {14, 17, 1, 2, 4, 8, 31},     /* 2 */
    {31, 2, 4, 2, 1, 17, 14},     /* 3 */
    {2, 6, 10, 18, 31, 2, 2},     /* 4 */
    {31, 16, 30, 1, 1, 17, 14},   /* 5 */
    {6, 8, 16, 30, 17, 17, 14},   /* 6 */
    {31, 1, 2, 4, 8, 8, 8},       /* 7 */
    {14, 17, 17, 14, 17, 17, 14}, /* 8 */
    {14, 17, 17, 15, 1, 2, 12},   /* 9 */
    {0, 0, 0, 0, 0, 12, 12},      /* . */
    {0, 0, 0, 0, 0, 0, 0},        /* anything unknown: blank */
    {17, 17, 17, 31, 17, 17, 17}, /* H */
    {31, 1, 2, 4, 8, 16, 31},     /* z */
    {0, 0, 0, 0, 0, 0, 0},        /* space */
    {16, 18, 20, 24, 20, 18, 17}, /* k */
    {16, 16, 16, 16, 16, 16, 31}, /* L */
    {14, 17, 17, 17, 17, 17, 14}, /* O */
    {14, 17, 16, 14, 1, 17, 14},  /* S */
    {31, 4, 4, 4, 4, 4, 4},       /* T */
    {14, 17, 17, 31, 17, 17, 17}, /* A */
    {31, 16, 16, 30, 16, 16, 31}, /* E */
};
enum { GLYPH_UNKNOWN = 11, GLYPH_H = 12, GLYPH_Z = 13, GLYPH_SPACE = 14, GLYPH_K = 15,
       GLYPH_L = 16, GLYPH_O = 17, GLYPH_S = 18, GLYPH_T = 19, GLYPH_A = 20,
       GLYPH_E = 21 };

/* The axis is absolute, in kilohertz, spanning the whole 0-25 kHz range at
 * round 5 kHz steps. It must be absolute rather than relative to the tone: a
 * harmonic-labelled axis would move with the signal, so two different tones
 * would look identical and could not be compared. The amber marker under the
 * plot shows where the current fundamental sits on that fixed scale.
 * Each mark names a frequency; it is drawn at the band that contains it. */
static const struct { uint32_t hz; const char *text; } axis_marks[] = {
    {0, "0"}, {5000, "5"}, {10000, "10"}, {15000, "15"}, {20000, "20"},
};
enum { AXIS_MARK_COUNT = sizeof axis_marks / sizeof axis_marks[0] };

/* Where a frequency sits on this axis: FFT bin = f * 256 / 50000, and each
 * band groups four consecutive bins. */
static unsigned band_of(uint32_t hz)
{
    const unsigned bin = (unsigned)(((uint64_t)hz * 256u) / 50000u);
    const unsigned band = bin / 4u;
    return band < SIG_BANDS ? band : SIG_BANDS - 1;
}

static uint16_t row_pixels[PANEL_W];
static uint8_t row_bytes[PANEL_W * 2];
static unsigned bar_height[SIG_BANDS];
static uint32_t tone_hz;

/* Index into the glyph table, or the blank glyph for anything unsupported. */
static unsigned glyph_index(char c)
{
    if (c >= '0' && c <= '9') return (unsigned)(c - '0');
    if (c == '.') return 10;
    if (c == 'H') return GLYPH_H;
    if (c == 'z') return GLYPH_Z;
    if (c == 'L') return GLYPH_L;
    if (c == 'O') return GLYPH_O;
    if (c == 'S') return GLYPH_S;
    if (c == 'T') return GLYPH_T;
    if (c == 'A') return GLYPH_A;
    if (c == 'E') return GLYPH_E;
    if (c == 'k') return GLYPH_K;
    if (c == ' ') return GLYPH_SPACE;
    return GLYPH_UNKNOWN;
}

/* Which glyph row (0..6) belongs on screen row y, or 7 when y is outside the
 * band that starts at band_top. Not mirrored: the panel rotation is undone when
 * the whole frame is written bottom row first. */
static unsigned text_row(unsigned y, unsigned band_top)
{
    if (y < band_top + 3 || y > band_top + 9) return 7;
    return y - band_top - 3;
}

/* Paint one row of a string, six pixels per character. */
/* Paint one row of a string into the current row buffer, six pixels per
 * character. Characters outside the table are drawn blank. */
static void put_text(unsigned x0, unsigned glyph_row_index, const char *text)
{
    for (unsigned i = 0; text[i] != '\0'; ++i) {
        const unsigned bits = glyphs[glyph_index(text[i])][glyph_row_index];
        for (unsigned column = 0; column < 5; ++column) {
            const unsigned px = x0 + i * 6 + column;
            if ((bits & (16u >> column)) && px < PANEL_W) row_pixels[px] = COLOUR_AXIS;
        }
    }
}

/* Append `value` as at least `width` decimal digits, most significant first. */
static unsigned append_digits(char *out, unsigned at, unsigned value, unsigned width)
{
    char digits[12];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 || count < width);
    while (count) out[at++] = digits[--count];
    return at;
}

/* Render a frequency as kilohertz with two decimals, e.g. 3125 -> "3.13".
 * The axis and the tone readout share this, so their units always agree. */
static unsigned format_khz(uint32_t hz, char *out)
{
    const unsigned hundredths = (unsigned)((hz + 5u) / 10u);
    unsigned at = append_digits(out, 0, hundredths / 100u, 1);
    out[at++] = '.';
    at = append_digits(out, at, hundredths % 100u, 2);
    out[at] = '\0';
    return at;
}

/* Program the drawing window (CASET/RASET) and start a memory write (RAMWR).
 * The controller then advances the write position itself, so every following
 * pixel goes to the next position inside the window. */
static void open_window(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
    const uint8_t columns[] = { x0 >> 8, x0 & 255, x1 >> 8, x1 & 255 };
    const uint8_t rows[] = { y0 >> 8, y0 & 255, y1 >> 8, y1 & 255 };
    command(0x2a); bytes(columns, sizeof columns);
    command(0x2b); bytes(rows, sizeof rows);
    command(0x2c);
}

/* Send the composed row. The window is already open, so this is pure pixel
 * data: no commands, no address setup.
 *
 * Each row is reversed on the way out. That is the horizontal half of the
 * panel's 180 degree rotation; the vertical half is that the frame is composed
 * from its last row to its first, so the controller's increasing row address
 * runs from the bottom of the picture to the top. Both halves are needed: with
 * only the horizontal one, the bars and the axis come out upside down. */
/* Push the composed row out as big-endian RGB565 pixels. */
static void send_row(void)
{
    for (unsigned x = 0; x < PANEL_W; ++x) {
        const uint16_t pixel = row_pixels[PANEL_W - 1 - x];
        row_bytes[2 * x] = (uint8_t)(pixel >> 8);
        row_bytes[2 * x + 1] = (uint8_t)(pixel & 255);
    }
    bytes(row_bytes, sizeof row_bytes);
}

/* Convert one band's strength into a bar height in pixels.
 *
 * Band strengths are log1p values, and an idle band still measures about 2-4
 * because of converter noise and window leakage. Drawn from zero, that floor
 * would fill a third of the plot with bars that carry no signal, so the bottom
 * FLOOR units are treated as "nothing here". The scale is therefore
 * FLOOR..10 rather than 0..10; it is a display choice, not a measurement.
 *
 * The first test also rejects NaN and the second rejects infinity, so the cast
 * cannot overflow. */
enum { BAR_FLOOR = 4 };            /* log1p units treated as the noise floor */
static unsigned bar_pixels(float level, unsigned rows)
{
    if (!(level > (float)BAR_FLOOR)) return 0;
    if (level >= 10.0f) return rows;
    return (unsigned)((level - (float)BAR_FLOOR) * (float)rows / (10.0f - BAR_FLOOR));
}

/* The renderer keeps the tone for the axis tick and the readout, so the frame
 * states the frequency it was taken at. */
void sig_panel_set_tone(uint32_t hz) { tone_hz = hz; }

/* Compose and send every row of one frame: axis and labels, the plot, then the
 * tone readout.
 *
 * Each row is composed into one buffer and written with a single SPI transfer,
 * so the transfer count is fixed and only the row count changes with `rows`.
 * Every plot row is written whether the bar reaches it or not, which is what
 * erases a bar that has shrunk.
 */
/* Where a band sits across the panel. The frequency bar uses the same scale as
 * the axis above it, so the marker lines up with the axis tick. */
static unsigned band_column(unsigned band)
{
    if (band >= SIG_BANDS) band = SIG_BANDS - 1;
    return band * (PANEL_W - 1) / (SIG_BANDS - 1);
}

/* Centred label at a column, kept inside the panel. */
static unsigned label_at(unsigned column, unsigned length)
{
    const unsigned width = length * 6u;
    const unsigned left = column > width / 2 ? column - width / 2 : 0;
    return left + width > PANEL_W ? PANEL_W - width : left;
}

/* ---- Incremental drawing ------------------------------------------------
 *
 * The panel refreshes its own glass continuously, and a whole-frame write lasts
 * about 39 ms of SPI time at the default size - several panel refreshes. A
 * full rewrite therefore lets the panel show the top of one frame and the
 * bottom of the next: the tearing seam. Writing only what changed on each frame
 * keeps every write far shorter than a panel refresh, so the seam is gone
 * without needing the panel's TE signal.
 *
 * The static parts of the frame (axis, labels, frequency scale) are drawn once.
 * Each frame then redraws the bars whose height changed, the marker if it
 * moved, and the readout if its text changed.
 */
static unsigned frame_top, plot_top, plot_bottom, scale_top, text_top;
static int last_marker = -1;      /* band the amber marker was drawn at */
static char last_readout[64];     /* text currently on the readout row */

/* Send one horizontal strip of the frame. `compose` paints the strip's pixels:
 * x is relative to the strip, y is the absolute frame row. Rows are written in
 * the controller's address order, which runs upward on this panel, so the last
 * frame row of the strip is sent first. */
typedef void (*row_painter)(unsigned x, unsigned y, uint16_t *out);

static void send_strip(unsigned x0, unsigned y0, unsigned w, unsigned h,
                       row_painter compose)
{
    if (!w || !h) return;
    open_window(x0, y0, x0 + w - 1, y0 + h - 1);
    for (unsigned step = 0; step < h; ++step) {
        const unsigned y = y0 + h - 1 - step;
        for (unsigned x = 0; x < w; ++x) row_pixels[x] = COLOUR_BLACK;
        compose(0, y, row_pixels);
        for (unsigned x = 0; x < w; ++x) {
            row_bytes[2 * x] = (uint8_t)(row_pixels[x] >> 8);
            row_bytes[2 * x + 1] = (uint8_t)(row_pixels[x] & 255);
        }
        bytes(row_bytes, w * 2);
    }
}

/* --- static frame --- */
static void compose_axis(unsigned x, unsigned y, uint16_t *out)
{
    const unsigned glyph = text_row(y, frame_top);
    for (unsigned i = 0; i < AXIS_MARK_COUNT; ++i) {
        const unsigned start = band_column(band_of(axis_marks[i].hz));
        if (y == frame_top + 1 && x + 0 >= start && x < start + 2) out[x] = COLOUR_AXIS;
        if (glyph < 7) {
            const unsigned bits = glyphs[glyph_index(axis_marks[i].text[0])][glyph];
            const unsigned column = x - start;
            if (column < 5 && (bits & (16u >> column))) out[x] = COLOUR_AXIS;
        }
    }
    if (glyph < 7 && x >= PANEL_W - 24 && x < PANEL_W - 24 + 18) {
        const char *unit = "kHz";
        const unsigned column = x - (PANEL_W - 24);
        const unsigned bits = glyphs[glyph_index(unit[column / 6])][glyph];
        if (column % 6 < 5 && (bits & (16u >> (column % 6)))) out[x] = COLOUR_AXIS;
    }
}

static void compose_scale(unsigned x, unsigned y, uint16_t *out)
{
    const unsigned row = y - scale_top;
    const unsigned first = band_column(SIG_TONE_BAND_MIN);
    const unsigned last = band_column(SIG_TONE_BAND_MAX);
    if (row >= 2 && row < 4 && x >= first && x <= last) out[x] = COLOUR_AXIS;
    if (row >= 2 && row < 9) {
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned ends[3] = { SIG_TONE_BAND_MIN, SIG_TONE_BAND_MID,
                                       SIG_TONE_BAND_MAX };
            const unsigned tick = band_column(ends[i]);
            if (x == tick || x == tick + 1) out[x] = COLOUR_AXIS;
        }
    }
    const unsigned glyph = text_row(y, scale_top + 14);
    if (glyph < 7) {
        const char *labels[3] = { "2.3", "12.9", "23.4" };
        const unsigned ends[3] = { SIG_TONE_BAND_MIN, SIG_TONE_BAND_MID,
                                   SIG_TONE_BAND_MAX };
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned start = label_at(band_column(ends[i]), 3);
            const unsigned column = x >= start ? x - start : 99;
            if (column < 15) {
                const unsigned bits = glyphs[glyph_index(labels[i][column / 6])][glyph];
                if (column % 6 < 5 && (bits & (16u >> (column % 6)))) out[x] = COLOUR_AXIS;
            }
        }
    }
}

/* --- per-frame pieces --- */
static unsigned paint_band, paint_rows, paint_height;
static void compose_bar(unsigned x, unsigned y, uint16_t *out)
{
    const unsigned above_floor = plot_bottom - 1 - y;
    (void) paint_band;
    if (above_floor < paint_height && x < BAR_W) out[x] = COLOUR_BAR;
    (void) paint_rows;
}

static int paint_marker_band;
static void compose_marker(unsigned x, unsigned y, uint16_t *out)
{
    const unsigned column = x;
    const unsigned target = band_column((unsigned)paint_marker_band);
    if (y < scale_top) {
        if (y == frame_top + 1 && (column == target || column == target + 1)) {
            out[x] = COLOUR_TONE;
        }
    } else if (y >= scale_top + 1 && y < scale_top + 9
               && column + 0 >= target - 1 && column <= target + 1) {
        out[x] = COLOUR_TONE;
    }
}

static const char *paint_text;
static unsigned paint_text_x;
static void compose_readout(unsigned x, unsigned y, uint16_t *out)
{
    const unsigned glyph = text_row(y, text_top);
    if (glyph >= 7) return;
    const unsigned column = (x >= paint_text_x) ? x - paint_text_x : 999;
    for (unsigned i = 0; paint_text[i] != '\0'; ++i) {
        if (column < i * 6 + 5 && column >= i * 6) {
            const unsigned bit = column - i * 6;
            const unsigned bits = glyphs[glyph_index(paint_text[i])][glyph];
            if (bits & (16u >> bit)) out[x] = COLOUR_AXIS;
        }
    }
}

/* Draw the parts of the frame that never change. */
static void draw_static(void)
{
    send_strip(0, frame_top, PANEL_W, AXIS_H, compose_axis);
    send_strip(0, scale_top, PANEL_W, SCALE_H, compose_scale);
}

/* Redraw one band's column between its old and new height. */
static void draw_bar(unsigned band, unsigned rows, unsigned old_height,
                     unsigned new_height)
{
    const unsigned tallest = old_height > new_height ? old_height : new_height;
    if (!tallest) return;
    paint_height = new_height;
    paint_rows = rows;
    send_strip(band * BAND_PITCH, plot_bottom - tallest, BAR_W, tallest,
               compose_bar);
}

/* Move the amber marker: erase at the old band, draw at the new one. */
static void draw_marker(int band)
{
    if (band == last_marker) return;
    const int previous = last_marker;
    paint_marker_band = previous;
    if (previous >= 0) {
        send_strip(0, frame_top + 1, PANEL_W, 1, compose_marker);
        send_strip(0, scale_top + 1, PANEL_W, 8, compose_marker);
    }
    paint_marker_band = band;
    send_strip(0, frame_top + 1, PANEL_W, 1, compose_marker);
    send_strip(0, scale_top + 1, PANEL_W, 8, compose_marker);
    last_marker = band;
}

/* Redraw the readout row, but only when its text changed. */
static void draw_readout(const char *text)
{
    if (strcmp(text, last_readout) == 0) return;
    paint_text = text;
    paint_text_x = 6;
    send_strip(0, text_top, 300, TEXT_H, compose_readout);
    strncpy(last_readout, text, sizeof last_readout - 1);
    last_readout[sizeof last_readout - 1] = '\0';
}

void sig_panel_draw(const spectrum_result_t *result, unsigned rows,
                    unsigned lost, unsigned late)
{
    if (rows > PLOT_MAX) rows = PLOT_MAX;

    static unsigned drawn_rows = 0;
    if (rows != drawn_rows) {
        /* Geometry changed: start from a clean panel and redraw the static
         * parts, rather than trying to patch a differently sized frame. */
        rectangle(0, 0, PANEL_W, PANEL_H, COLOUR_BLACK);
        const unsigned frame_height = AXIS_H + rows + SCALE_H + TEXT_H;
        frame_top = (PANEL_H - frame_height) / 2;
        plot_top = frame_top + AXIS_H;
        plot_bottom = plot_top + rows;
        scale_top = plot_bottom;
        text_top = scale_top + SCALE_H;
        for (unsigned band = 0; band < SIG_BANDS; ++band) bar_height[band] = 0;
        last_marker = -1;
        last_readout[0] = '\0';
        drawn_rows = rows;
        draw_static();
    }

    for (unsigned band = 0; band < SIG_BANDS; ++band) {
        const unsigned height = bar_pixels(result->bands[band], rows);
        if (height != bar_height[band]) {
            draw_bar(band, rows, bar_height[band], height);
            bar_height[band] = height;
        }
    }

    draw_marker((int)band_of(tone_hz));

    char readout[64];
    unsigned length = format_khz(tone_hz, readout);
    const char unit[] = " kHz   LOST ";
    for (unsigned i = 0; unit[i] != '\0'; ++i) readout[length++] = unit[i];
    length = append_digits(readout, length, lost, 1);
    const char late_text[] = "   LATE ";
    for (unsigned i = 0; late_text[i] != '\0'; ++i) readout[length++] = late_text[i];
    length = append_digits(readout, length, late, 1);
    readout[length] = '\0';
    draw_readout(readout);
}

uint32_t sig_panel_spi_hz(void) { return spi_hz; }
