/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <unistd.h>
#include <config.h>

#if HAVE_POSIX
#include <sys/ioctl.h>
#endif

#include <libswscale/swscale.h>

#include "options/m_config.h"
#include "config.h"
#include "osdep/terminal.h"
#include "osdep/io.h"
#include "vo.h"
#include "sub/osd.h"
#include "video/sws_utils.h"
#include "video/mp_image.h"

#define IMGFMT IMGFMT_BGR24

#define ALGO_PLAIN 1
#define ALGO_HALF_BLOCKS 2
#define ALGO_ALL_BLOCKS 3
#define ESC_HIDE_CURSOR "\033[?25l"
#define ESC_RESTORE_CURSOR "\033[?25h"
#define ESC_CLEAR_SCREEN "\033[2J"
#define ESC_CLEAR_COLORS "\033[0m"
#define ESC_GOTOXY "\033[%d;%df"
#define ESC_COLOR_BG "\033[48;2"
#define ESC_COLOR_FG "\033[38;2"
#define ESC_COLOR256_BG "\033[48;5"
#define ESC_COLOR256_FG "\033[38;5"
#define DEFAULT_WIDTH 80
#define DEFAULT_HEIGHT 25

struct vo_tct_opts {
    int algo;
    int width;   // 0 -> default
    int height;  // 0 -> default
    int term256;  // 0 -> true color
};

#define OPT_BASE_STRUCT struct vo_tct_opts
static const struct m_sub_options vo_tct_conf = {
    .opts = (const m_option_t[]) {
        {"vo-tct-algo", OPT_CHOICE(algo,
            {"plain", ALGO_PLAIN},
            {"half-blocks", ALGO_HALF_BLOCKS},
            {"all-blocks", ALGO_ALL_BLOCKS})},
        {"vo-tct-width", OPT_INT(width)},
        {"vo-tct-height", OPT_INT(height)},
        {"vo-tct-256", OPT_FLAG(term256)},
        {0}
    },
    .defaults = &(const struct vo_tct_opts) {
        .algo = ALGO_HALF_BLOCKS,
    },
    .size = sizeof(struct vo_tct_opts),
};

struct lut_item {
    char str[4];
    int width;
};

struct priv {
    struct vo_tct_opts *opts;
    size_t buffer_size;
    int swidth;
    int sheight;
    struct mp_image *frame;
    struct mp_rect src;
    struct mp_rect dst;
    struct mp_sws_context *sws;
    struct lut_item lut[256];
};

// Convert RGB24 to xterm-256 8-bit value
// For simplicity, assume RGB space is perceptually uniform.
// There are 5 places where one of two outputs needs to be chosen when the
// input is the exact middle:
// - The r/g/b channels and the gray value: the higher value output is chosen.
// - If the gray and color have same distance from the input - color is chosen.
static int rgb_to_x256(uint8_t r, uint8_t g, uint8_t b)
{
    // Calculate the nearest 0-based color index at 16 .. 231
#   define v2ci(v) (v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40)
    int ir = v2ci(r), ig = v2ci(g), ib = v2ci(b);   // 0..5 each
#   define color_index() (36 * ir + 6 * ig + ib)  /* 0..215, lazy evaluation */

    // Calculate the nearest 0-based gray index at 232 .. 255
    int average = (r + g + b) / 3;
    int gray_index = average > 238 ? 23 : (average - 3) / 10;  // 0..23

    // Calculate the represented colors back from the index
    static const int i2cv[6] = {0, 0x5f, 0x87, 0xaf, 0xd7, 0xff};
    int cr = i2cv[ir], cg = i2cv[ig], cb = i2cv[ib];  // r/g/b, 0..255 each
    int gv = 8 + 10 * gray_index;  // same value for r/g/b, 0..255

    // Return the one which is nearer to the original input rgb value
#   define dist_square(A,B,C, a,b,c) ((A-a)*(A-a) + (B-b)*(B-b) + (C-c)*(C-c))
    int color_err = dist_square(cr, cg, cb, r, g, b);
    int gray_err  = dist_square(gv, gv, gv, r, g, b);
    return color_err <= gray_err ? 16 + color_index() : 232 + gray_index;
}

static void print_seq3(struct lut_item *lut, const char* prefix,
                       uint8_t r, uint8_t g, uint8_t b)
{
// The fwrite implementation is about 25% faster than the printf code
// (even if we use *.s with the lut values), however,
// on windows we need to use printf in order to translate escape sequences and
// UTF8 output for the console.
#ifndef _WIN32
    fputs(prefix, stdout);
    fwrite(lut[r].str, lut[r].width, 1, stdout);
    fwrite(lut[g].str, lut[g].width, 1, stdout);
    fwrite(lut[b].str, lut[b].width, 1, stdout);
    fputc('m', stdout);
#else
    printf("%s;%d;%d;%dm", prefix, (int)r, (int)g, (int)b);
#endif
}

static void print_seq1(struct lut_item *lut, const char* prefix, uint8_t c)
{
#ifndef _WIN32
    fputs(prefix, stdout);
    fwrite(lut[c].str, lut[c].width, 1, stdout);
    fputc('m', stdout);
#else
    printf("%s;%dm", prefix, (int)c);
#endif
}


static void write_plain(
    const int dwidth, const int dheight,
    const int swidth, const int sheight,
    const unsigned char *source, const int source_stride,
    bool term256, struct lut_item *lut)
{
    assert(source);
    const int tx = (dwidth - swidth) / 2;
    const int ty = (dheight - sheight) / 2;
    for (int y = 0; y < sheight; y++) {
        const unsigned char *row = source + y * source_stride;
        printf(ESC_GOTOXY, ty + y, tx);
        for (int x = 0; x < swidth; x++) {
            unsigned char b = *row++;
            unsigned char g = *row++;
            unsigned char r = *row++;
            if (term256) {
                print_seq1(lut, ESC_COLOR256_BG, rgb_to_x256(r, g, b));
            } else {
                print_seq3(lut, ESC_COLOR_BG, r, g, b);
            }
            printf(" ");
        }
        printf(ESC_CLEAR_COLORS);
    }
    printf("\n");
}

static void write_half_blocks(
    const int dwidth, const int dheight,
    const int swidth, const int sheight,
    unsigned char *source, int source_stride,
    bool term256, struct lut_item *lut)
{
    assert(source);
    const int tx = (dwidth - swidth) / 2;
    const int ty = (dheight - sheight) / 2;
    for (int y = 0; y < sheight * 2; y += 2) {
        const unsigned char *row_up = source + y * source_stride;
        const unsigned char *row_down = source + (y + 1) * source_stride;
        printf(ESC_GOTOXY, ty + y / 2, tx);
        for (int x = 0; x < swidth; x++) {
            unsigned char b_up = *row_up++;
            unsigned char g_up = *row_up++;
            unsigned char r_up = *row_up++;
            unsigned char b_down = *row_down++;
            unsigned char g_down = *row_down++;
            unsigned char r_down = *row_down++;
            if (term256) {
                print_seq1(lut, ESC_COLOR256_BG, rgb_to_x256(r_up, g_up, b_up));
                print_seq1(lut, ESC_COLOR256_FG, rgb_to_x256(r_down, g_down, b_down));
            } else {
                print_seq3(lut, ESC_COLOR_BG, r_up, g_up, b_up);
                print_seq3(lut, ESC_COLOR_FG, r_down, g_down, b_down);
            }
            printf("\xe2\x96\x84");  // UTF8 bytes of U+2584 (lower half block)
        }
        printf(ESC_CLEAR_COLORS);
    }
    printf("\n");
}


/*
   Block Elements data structure
*/

enum block_elem_name {
    LOWER_HALF /* "▄" */, LEFT_HALF /* "▌" */,
    QUADRANT_LOWER_LEFT /* "▖" */, QUADRANT_LOWER_RIGHT /* "▗" */, QUADRANT_UPPER_LEFT /* "▘" */, QUADRANT_UPPER_RIGHT /* "▝" */,
    QUADRANT_UPPER_RIGHT_LOWER_LEFT /* "▞" */,
    LOWER_ONE_QUARTER /* "▂" */, LOWER_THREE_QUARTERS /* "▆" */, LEFT_ONE_QUARTER /* "▎" */, LEFT_THREE_QUARTERS /* "▊" */,
    LOWER_ONE_EIGHTH /* "▁" */, LOWER_THREE_EIGHTHS /* "▃" */, LOWER_FIVE_EIGHTHS /* "▅" */, LOWER_SEVEN_EIGHTHS /* "▇" */,
    LEFT_ONE_EIGHTH /* "▏" */, LEFT_THREE_EIGHTHS /* "▍" */, LEFT_FIVE_EIGHTHS /* "▋" */, LEFT_SEVEN_EIGHTHS /* "▉" */,
    NUM_ELEMENTS
};

struct block_element
{
    uint64_t bitmap;  // 8x8 bits window mapping 
    char* utf8; // UTF8 representation of ANSI code
};

static const struct block_element block_elements[NUM_ELEMENTS] = {
    {  // LOWER_HALF
        .bitmap=0b0000000000000000000000000000000011111111111111111111111111111111, 
        .utf8="\xe2\x96\x84" },
    {  // LEFT_HALF
        .bitmap=0b1111000011110000111100001111000011110000111100001111000011110000, 
        .utf8="\xe2\x96\x8c" },
    {  // QUADRANT_LOWER_LEFT
        .bitmap=0b0000000000000000000000000000000011110000111100001111000011110000, 
        .utf8="\xe2\x96\x96" },
    {  // QUADRANT_LOWER_RIGHT
        .bitmap=0b0000000000000000000000000000000000001111000011110000111100001111, 
        .utf8="\xe2\x96\x97" },
    {  // QUADRANT_UPPER_LEFT
        .bitmap=0b1111000011110000111100001111000000000000000000000000000000000000, 
        .utf8="\xe2\x96\x98" },
    {  // QUADRANT_UPPER_RIGHT
        .bitmap=0b0000111100001111000011110000111100000000000000000000000000000000, 
        .utf8="\xe2\x96\x9d" },
    {  // QUADRANT_UPPER_RIGHT_LOWER_LEFT
        .bitmap=0b0000111100001111000011110000111111110000111100001111000011110000, 
        .utf8="\xe2\x96\x9e" },
    {  // LOWER_ONE_QUARTER
        .bitmap=0b0000000000000000000000000000000000000000000000001111111111111111, 
        .utf8="\xe2\x96\x82" },
    {  // LOWER_THREE_QUARTERS
        .bitmap=0b0000000000000000111111111111111111111111111111111111111111111111, 
        .utf8="\xe2\x96\x86" },
    {  // LEFT_ONE_QUARTER
        .bitmap=0b1100000011000000110000001100000011000000110000001100000011000000, 
        .utf8="\xe2\x96\x8e" },
    {  // LEFT_THREE_QUARTERS
        .bitmap=0b1111110011111100111111001111110011111100111111001111110011111100, 
        .utf8="\xe2\x96\x8a" },
    {  // LOWER_ONE_EIGHTH
        .bitmap=0b0000000000000000000000000000000000000000000000000000000011111111, 
        .utf8="\xe2\x96\x81" },
    {  // LOWER_THREE_EIGHTHS
        .bitmap=0b0000000000000000000000000000000000000000111111111111111111111111, 
        .utf8="\xe2\x96\x83" },
    {  // LOWER_FIVE_EIGHTHS
        .bitmap=0b0000000000000000000000001111111111111111111111111111111111111111, 
        .utf8="\xe2\x96\x85" },
    {  // LOWER_SEVEN_EIGHTHS
        .bitmap=0b0000000011111111111111111111111111111111111111111111111111111111, 
        .utf8="\xe2\x96\x87" },
    {  // LEFT_ONE_EIGHTH
        .bitmap=0b1000000010000000100000001000000010000000100000001000000010000000, 
        .utf8="\xe2\x96\x8f" },
    {  // LEFT_THREE_EIGHTHS
        .bitmap=0b1110000011100000111000001110000011100000111000001110000011100000, 
        .utf8="\xe2\x96\x8d" },
    {  // LEFT_FIVE_EIGHTHS
        .bitmap=0b1111100011111000111110001111100011111000111110001111100011111000, 
        .utf8="\xe2\x96\x8b" },
    {  // LEFT_SEVEN_EIGHTHS
        .bitmap=0b1111111011111110111111101111111011111110111111101111111011111110, 
        .utf8="\xe2\x96\x89" },
};

#define WINDOW_H 8
#define WINDOW_W 8
#define WINDOW_SZ (WINDOW_W * WINDOW_H)

// selects i-th foreground bit out of bitmap
#define is_fg(bits, i) (bits>>(WINDOW_SZ-1-i) & 0b1)
// square operation
#define square(v)  ((v)*(v))

struct rgb_color {
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

static struct rgb_color get_rgb(const unsigned char *base_pixel) {
    struct rgb_color color;
    color.b = *base_pixel++;
    color.g = *base_pixel++;
    color.r = *base_pixel++;
    return color;
}

// Computes forground/background colors that best fit window of pixels, given a block element
static unsigned int best_fg_bg(
        //input
        const enum block_elem_name elem,  const struct rgb_color win_pixels[WINDOW_SZ],
        // output
        struct rgb_color *mean_fg, struct rgb_color *mean_bg
    ) {
    // sum fg and bg values
    unsigned int sum_fg_r=0, sum_fg_g=0, sum_fg_b=0;
    unsigned int sum_bg_r=0, sum_bg_g=0, sum_bg_b=0;
    // sum fg^2 and bg^2 values
    unsigned int sum_fg2_r=0, sum_fg2_g=0, sum_fg2_b=0;
    unsigned int sum_bg2_r=0, sum_bg2_g=0, sum_bg2_b=0;
    unsigned int num_fg=0, num_bg=0;
    for (int i=0; i<WINDOW_SZ ;++i) {
        if ( is_fg(block_elements[elem].bitmap, i) ) {
            ++num_fg;
            sum_fg_r += win_pixels[i].r;
            sum_fg_g += win_pixels[i].g;
            sum_fg_b += win_pixels[i].b;
            sum_fg2_r += square( win_pixels[i].r );
            sum_fg2_g += square( win_pixels[i].g );
            sum_fg2_b += square( win_pixels[i].b );
        } else {
            ++num_bg;
            sum_bg_r += win_pixels[i].r;
            sum_bg_g += win_pixels[i].g;
            sum_bg_b += win_pixels[i].b;
            sum_bg2_r += square( win_pixels[i].r );
            sum_bg2_g += square( win_pixels[i].g );
            sum_bg2_b += square( win_pixels[i].b );
        }
    }
    // compute mean values for fg and bg
    mean_fg->r = sum_fg_r/num_fg; mean_fg->g = sum_fg_g/num_fg; mean_fg->b = sum_fg_b/num_fg; 
    mean_bg->r = sum_bg_r/num_bg; mean_bg->g = sum_bg_g/num_bg; mean_bg->b = sum_bg_b/num_bg;

    // compute loss as variance of rgb values.  
    // NB. to reduce computation we define: loss = N * Variance(X)
    //     So we have:
    //      loss = Sum_i(X[i]^2) - N * Mean(X)^2
    unsigned int loss = 0;
    // foregroud pixels
    loss += sum_fg2_r - num_fg * square( mean_fg->r );
    loss += sum_fg2_g - num_fg * square( mean_fg->g );
    loss += sum_fg2_b - num_fg * square( mean_fg->b );
    // backgroud pixels
    loss += sum_bg2_r - num_bg * square( mean_bg->r );
    loss += sum_bg2_g - num_bg * square( mean_bg->g );
    loss += sum_bg2_b - num_bg * square( mean_bg->b );
    
    return loss;
}

// Returns UTF8 representation for best block element that fits window of pixels
static const char* guess_best_block_element(
        // input
        struct rgb_color win_pixels[WINDOW_SZ],
        // output
        struct rgb_color *best_fg, struct rgb_color *best_bg
    ) {
    int best_loss = INT_MAX;
    enum block_elem_name best_elem;
    struct rgb_color fg, bg;
    for (enum block_elem_name elem=0; elem<NUM_ELEMENTS ;++elem) {
        unsigned int loss = best_fg_bg(elem, win_pixels, //input 
                                        &fg, &bg // output
                                      );
        if (loss < best_loss) {
            best_elem = elem;
            best_loss = loss;
            *best_fg = fg; 
            *best_bg = bg;
        }
    }
    return block_elements[best_elem].utf8; // UTF8 bytes of Unicode char
}

static void write_all_blocks(
    const int dwidth, const int dheight,
    const int swidth, const int sheight,
    unsigned char *source, int source_stride,
    bool term256, struct lut_item *lut)
{
    assert(source);
    const int tx = (dwidth - swidth) / 2;
    const int ty = (dheight - sheight) / 2;
    // window of rgb pixels
    struct rgb_color win_pixels[WINDOW_SZ];
    struct rgb_color fg, bg;

    for (int y = 0; y < sheight * WINDOW_H; y += WINDOW_H) {
        unsigned char *rows[WINDOW_H];
        for (int i=0; i<WINDOW_H ;++i)
            rows[i] = source + (y + i) * source_stride;

        printf(ESC_GOTOXY, ty + y / WINDOW_H, tx);
        for (int x = 0; x < swidth; x++) {
            // fill window
            int j = 0;
            for (int i=0; i<WINDOW_H ;++i) // rows 
                for (; j<WINDOW_W*(i+1) ;++j) {
                    win_pixels[j] = get_rgb(rows[i]);
                    rows[i] += 3; // next pixel on this row
                }

            const char *utf8_block_element = guess_best_block_element(win_pixels, &fg, &bg);
            if (term256) {
                print_seq1(lut, ESC_COLOR256_BG, rgb_to_x256(bg.r, bg.g, bg.b));
                print_seq1(lut, ESC_COLOR256_FG, rgb_to_x256(fg.r, fg.g, fg.b)); 
            } else {
                print_seq3(lut, ESC_COLOR_BG, bg.r, bg.g, bg.b); 
                print_seq3(lut, ESC_COLOR_FG, fg.r, fg.g, fg.b);  
            }
            printf("%s", utf8_block_element );
        }
        printf(ESC_CLEAR_COLORS);
    }
    printf("\n");
}

static void get_win_size(struct vo *vo, int *out_width, int *out_height) {
    struct priv *p = vo->priv;
    *out_width = DEFAULT_WIDTH;
    *out_height = DEFAULT_HEIGHT;

    terminal_get_size(out_width, out_height);

    if (p->opts->width > 0)
        *out_width = p->opts->width;
    if (p->opts->height > 0)
        *out_height = p->opts->height;
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;

    get_win_size(vo, &vo->dwidth, &vo->dheight);

    struct mp_osd_res osd;
    vo_get_src_dst_rects(vo, &p->src, &p->dst, &osd);
    p->swidth = p->dst.x1 - p->dst.x0;
    p->sheight = p->dst.y1 - p->dst.y0;

    p->sws->src = *params;
    p->sws->dst = (struct mp_image_params) {
        .imgfmt = IMGFMT,
        .w = p->swidth,
        .h = p->sheight,
        .p_w = 1,
        .p_h = 1,
    };

    int mul_w, mul_h;
    switch (p->opts->algo) {
        case ALGO_PLAIN:
            mul_w = 1;
            mul_h = 1;
            break;
        case ALGO_ALL_BLOCKS:
            mul_w = WINDOW_W;
            mul_h = WINDOW_H;
            break;
        default: // ALGO_HALF_BLOCKS
            mul_w = 1;
            mul_h = 2;
    }
    if (p->frame)
        talloc_free(p->frame);
    p->frame = mp_image_alloc(IMGFMT, p->swidth * mul_w, p->sheight * mul_h);
    if (!p->frame)
        return -1;

    if (mp_sws_reinit(p->sws) < 0)
        return -1;

    printf(ESC_HIDE_CURSOR);
    printf(ESC_CLEAR_SCREEN);
    vo->want_redraw = true;
    return 0;
}

static void draw_image(struct vo *vo, mp_image_t *mpi)
{
    struct priv *p = vo->priv;
    struct mp_image src = *mpi;
    // XXX: pan, crop etc.
    mp_sws_scale(p->sws, p->frame, &src);
    talloc_free(mpi);
}

static void flip_page(struct vo *vo)
{
    struct priv *p = vo->priv;

    int width, height;
    get_win_size(vo, &width, &height);

    if (vo->dwidth != width || vo->dheight != height)
        reconfig(vo, vo->params);

    switch (p->opts->algo) {
        case ALGO_PLAIN:
            write_plain(
                vo->dwidth, vo->dheight, p->swidth, p->sheight,
                p->frame->planes[0], p->frame->stride[0],
                p->opts->term256, p->lut);
            break;
        case ALGO_ALL_BLOCKS:
            write_all_blocks(
                vo->dwidth, vo->dheight, p->swidth, p->sheight,
                p->frame->planes[0], p->frame->stride[0],
                p->opts->term256, p->lut);
            break;
        default: // ALGO_HALF_BLOCKS
            write_half_blocks(
                vo->dwidth, vo->dheight, p->swidth, p->sheight,
                p->frame->planes[0], p->frame->stride[0],
                p->opts->term256, p->lut);
    }
    fflush(stdout);
}

static void uninit(struct vo *vo)
{
    printf(ESC_RESTORE_CURSOR);
    printf(ESC_CLEAR_SCREEN);
    printf(ESC_GOTOXY, 0, 0);
    struct priv *p = vo->priv;
    if (p->frame)
        talloc_free(p->frame);
}

static int preinit(struct vo *vo)
{
    // most terminal characters aren't 1:1, so we default to 2:1.
    // if user passes their own value of choice, it'll be scaled accordingly.
    vo->monitor_par = vo->opts->monitor_pixel_aspect * 2;

    struct priv *p = vo->priv;
    p->opts = mp_get_config_group(vo, vo->global, &vo_tct_conf);
    p->sws = mp_sws_alloc(vo);
    p->sws->log = vo->log;
    mp_sws_enable_cmdline_opts(p->sws, vo->global);

    for (int i = 0; i < 256; ++i) {
        char buff[8];
        p->lut[i].width = sprintf(buff, ";%d", i);
        memcpy(p->lut[i].str, buff, 4); // some strings may not end on a null byte, but that's ok.
    }

    return 0;
}

static int query_format(struct vo *vo, int format)
{
    return format == IMGFMT;
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    return VO_NOTIMPL;
}

const struct vo_driver video_out_tct = {
    .name = "tct",
    .description = "true-color terminals",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_image = draw_image,
    .flip_page = flip_page,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
    .global_opts = &vo_tct_conf,
};
