#include "internal/launcher/grid_games.h"

#include <stdio.h>
#include <string.h>

/* Panel geometry, all in logical units so it scales with the panel. */
#define GG_MARGIN     16
#define GG_GAP        20
#define GG_TOPBAR     40
#define GG_ROW_PAD    10
#define GG_CHIP_H     54
#define GG_CHIP_GAP    8
#define GG_WORDMARK_H 96   /* tall enough that a typical wordmark spans the column */
#define GG_ART_PCT    50   /* cover owns the top half of the right column */

static int gg_list_w(void) {
    /* Just under half: the cover needs the wider side because the screen is
       landscape and most covers are not. */
    return (cat_get_screen_width() * 47) / 100;
}

/* The list is the small font: a game list is long and its names are long, so
   rows that fit beat rows that shout. */
static TTF_Font *gg_row_font(void) { return cat_get_font(CAT_FONT_SMALL); }

static int gg_row_h(void) {
    TTF_Font *f = gg_row_font();
    return TTF_FontHeight(f) + cat_scale(GG_ROW_PAD);
}

int jw_grid_games_visible_rows(void) {
    int top = cat_scale(GG_TOPBAR) + cat_scale(GG_MARGIN);
    int h   = cat_get_screen_height() - top - cat_scale(GG_MARGIN);
    int rows = h / gg_row_h();
    return rows > 0 ? rows : 1;
}

/* Text on a translucent panel is derived, never authored: a theme cannot end up
   with white on white by accident. Same rule as the status polarity over a
   wallpaper. */
static cat_draw_color gg_ink(cat_draw_color under) {
    unsigned luma = (under.r * 299u + under.g * 587u + under.b * 114u) / 1000u;
    /* A sheer panel shows the wallpaper through it, so bias toward the ink that
       survives either way rather than trusting the panel colour alone. */
    if (under.a < 110) return (cat_draw_color){ 0xF2, 0xF5, 0xEF, 0xFF };
    /* A soft neutral charcoal rather than near-black: on a sheer white panel over
       a wallpaper, true black reads harsh and fights the artwork. */
    return luma > 140 ? (cat_draw_color){ 0x41, 0x47, 0x4A, 0xFF }
                      : (cat_draw_color){ 0xEE, 0xF1, 0xEC, 0xFF };
}

/* SDL2's renderer has no blur, so the soft edge is four rounded rects at
   falling alpha. They are drawn as a ring around the panel, never under it: a
   translucent panel over black reads grey no matter how opaque it is. */
static void gg_panel(int x, int y, int w, int h, int r,
                     const jw_grid_games_style *st) {
    if (w <= 0 || h <= 0) return;
    if (st->shadow > 0) {
        for (int i = 3; i >= 1; i -= 2) {
            int g = cat_scale(3) * i;
            int a = (st->shadow * (4 - i)) / 14;
            if (a <= 0) continue;
            cat_draw_rounded_rect(x - g, y - g + cat_scale(3), w + g * 2, h + g * 2,
                                  r + g, (cat_draw_color){ 0, 0, 0, (Uint8)a });
        }
    }
    cat_draw_rounded_rect(x, y, w, h, r, st->underlay);
}

/* Bounded copy. snprintf("%s") from a wider buffer trips gcc's truncation
   analysis even when the result always fits, so copy by length instead. */
static void gg_copy(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len > n - 1) len = n - 1;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
}


/* Fit `src` to `maxw`, appending an ellipsis, and cache the result: the caller
   redraws every frame while a cover decodes, and re-fitting a long name means a
   binary search of harfbuzz shaping passes each time. Recomputed only when the
   text, width or font actually change. */
typedef struct {
    char      src[256];
    char      out[256];
    int       w, maxw;
    TTF_Font *font;
} gg_fit_slot;

static const char *gg_fit(gg_fit_slot *slot, TTF_Font *f, const char *src,
                          int maxw, int *out_w) {
    if (!src) src = "";
    if (slot->font == f && slot->maxw == maxw &&
        strncmp(slot->src, src, sizeof(slot->src) - 1) == 0) {
        if (out_w) *out_w = slot->w;
        return slot->out;
    }
    gg_copy(slot->src, sizeof(slot->src), src);
    slot->font = f;
    slot->maxw = maxw;
    gg_copy(slot->out, sizeof(slot->out), src);
    if (cat_measure_text(f, slot->out) > maxw) {
        size_t n = strlen(slot->out);
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)slot->out[n] & 0xC0) == 0x80) n--;
            if (n + 4 > sizeof(slot->out)) continue;
            slot->out[n] = '.'; slot->out[n+1] = '.'; slot->out[n+2] = '.';
            slot->out[n+3] = '\0';
            if (cat_measure_text(f, slot->out) <= maxw) break;
            slot->out[n] = '\0';
        }
    }
    slot->w = cat_measure_text(f, slot->out);
    if (out_w) *out_w = slot->w;
    return slot->out;
}

/* Wrap on spaces to a pixel width; returns how many lines were produced. */
static int gg_wrap(TTF_Font *f, const char *text, int maxw,
                   char lines[][256], int max_lines) {
    if (!text || !text[0]) return 0;
    int n = 0;
    const char *p = text;
    char cur[256];
    cur[0] = '\0';
    while (*p && n < max_lines) {
        const char *sp = strchr(p, ' ');
        size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
        char word[128];
        if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
        memcpy(word, p, wlen);
        word[wlen] = '\0';

        char probe[256];
        size_t cl = strlen(cur);
        if (cl + 1 + wlen < sizeof(probe)) {
            memcpy(probe, cur, cl);
            if (cl) probe[cl++] = ' ';
            memcpy(probe + cl, word, wlen);
            probe[cl + wlen] = '\0';
        } else {
            probe[0] = '\0';
        }

        if (probe[0] && (cat_measure_text(f, probe) <= maxw || !cur[0])) {
            gg_copy(cur, sizeof(cur), probe);
        } else {
            gg_copy(lines[n++], 256, cur);
            gg_copy(cur, sizeof(cur), word);
        }
        p = sp ? sp + 1 : p + wlen;
    }
    if (cur[0] && n < max_lines) gg_copy(lines[n++], 256, cur);
    return n;
}

void jw_grid_games_draw(const cat_list_state *ls, int count,
                        jw_grid_games_name_fn name_fn,
                        jw_grid_games_art_fn art_fn, void *ctx,
                        const jw_grid_games_meta *meta, int meta_count,
                        const char *synopsis,
                        SDL_Texture *wordmark, int wm_w, int wm_h,
                        const char *system_name, int top_bar,
                        const jw_grid_games_style *st) {
    if (!ls || !st) return;

    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *rowf  = gg_row_font();
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    TTF_Font *tiny  = cat_get_font(CAT_FONT_TINY);
    if (!body || !small) return;

    const int M   = cat_scale(GG_MARGIN);
    const int GAP = cat_scale(GG_GAP);
    const int top = top_bar > 0 ? top_bar : cat_scale(GG_TOPBAR);
    const int sw  = cat_get_screen_width();
    const int sh  = cat_get_screen_height();
    const int r   = st->radius;

    /* A theme may state its text colour; otherwise it is derived from the
       underlay so a theme cannot end up with white on white. */
    cat_draw_color ink = st->ink.a ? st->ink : gg_ink(st->underlay);

    /* ── left: the list ─────────────────────────────────────────────────── */
    int lx = M, ly = top, lw = gg_list_w(), lh = sh - top - M;
    gg_panel(lx, ly, lw, lh, r, st);

    int row_h = gg_row_h();
    int rows  = lh / row_h;
    if (rows < 1) rows = 1;
    ((cat_list_state *)ls)->visible_rows = rows;

    int first = ls->scroll_offset;
    if (first > count - rows) first = count - rows;
    if (first < 0) first = 0;

    int body_h = TTF_FontHeight(body);
    int text_x = lx + cat_scale(20);
    int text_w = lw - cat_scale(40);
    for (int i = 0; i < rows && first + i < count; ++i) {
        int idx = first + i;
        int ry = ly + cat_scale(8) + i * row_h;
        bool sel = (idx == ls->cursor);
        if (sel)
            cat_draw_rounded_rect(lx + cat_scale(8), ry - cat_scale(2),
                                  lw - cat_scale(16), row_h - cat_scale(2),
                                  (row_h - cat_scale(2)) / 2, st->highlight);
        /* Fitted through the per-row cache, then drawn by the plain path so the
           rendered texture is cached too. Re-fitting these every frame is what
           kept the launcher pinned at 100% and starved input. */
        static gg_fit_slot row_fit[32];
        const char *label = gg_fit(&row_fit[i % 32], rowf,
                                   name_fn ? name_fn(ctx, idx) : "", text_w, NULL);
        cat_draw_text(rowf, label, text_x,
                      ry + (row_h - cat_scale(2) - TTF_FontHeight(rowf)) / 2 - cat_scale(1),
                      sel ? st->highlight_text : ink);
    }

    /* ── right: cover, then whatever facts we have ──────────────────────── */
    int rx = lx + lw + GAP;
    int rw = sw - rx - M;

    /* The right column is three fixed zones, top to bottom: the cover in the
       top half, the game's info under it, then the wordmark. Fixed on purpose --
       sizing the cover from what is left over made it swell whenever a game had
       no metadata, so the same cover changed size depending on whether it had
       been played. */
    const int col_h  = sh - top - M;
    const int wm_slot = cat_scale(GG_WORDMARK_H);
    const int art_h   = col_h * GG_ART_PCT / 100;
    const int info_y  = top + art_h + GAP;
    const int info_h  = sh - M - wm_slot - info_y;

    int chip_rows = (meta_count + 2) / 3;
    int chip_h    = chip_rows > 0
                  ? chip_rows * cat_scale(GG_CHIP_H) + (chip_rows - 1) * cat_scale(GG_CHIP_GAP)
                  : 0;
    if (chip_h > info_h) chip_h = info_h;

    /* The synopsis takes whatever the chips leave, and wraps to fit it. */
    int syn_avail = info_h - chip_h - (chip_h ? cat_scale(GG_CHIP_GAP) : 0);
    int syn_line  = TTF_FontHeight(small) + cat_scale(4);
    int syn_max   = syn_avail > cat_scale(34) ? (syn_avail - cat_scale(34)) / syn_line : 0;
    if (syn_max > 4) syn_max = 4;
    char syn_lines[4][256];
    int syn_n = (synopsis && syn_max > 0)
              ? gg_wrap(small, synopsis, rw - cat_scale(26), syn_lines, syn_max) : 0;
    int syn_h = syn_n ? cat_scale(30) + syn_n * syn_line + cat_scale(4) : 0;

    /* The cover draws pure: no plate, no shadow, no rounding of its own. A
       portrait cover in a landscape slot simply sits narrower, which is the
       nature of the shape rather than something to crop away. */
    int aw = 0, ah = 0;
    SDL_Texture *art = art_fn ? art_fn(ctx, ls->cursor, &aw, &ah) : NULL;
    if (art && aw > 0 && ah > 0) {
        float s = (float)rw / (float)aw;
        float sy = (float)art_h / (float)ah;
        if (sy < s) s = sy;
        int dw = (int)(aw * s), dh = (int)(ah * s);
        SDL_Rect dst = { rx + (rw - dw) / 2, top + (art_h - dh) / 2, dw, dh };
        SDL_RenderCopy(cat_get_renderer(), art, NULL, &dst);
    }

    int y = info_y;
    int cw = (rw - cat_scale(GG_CHIP_GAP) * 2) / 3;
    for (int i = 0; i < meta_count && i < JW_GRID_GAMES_MAX_META; ++i) {
        int cx = rx + (i % 3) * (cw + cat_scale(GG_CHIP_GAP));
        int cy = y + (i / 3) * (cat_scale(GG_CHIP_H) + cat_scale(GG_CHIP_GAP));
        gg_panel(cx, cy, cw, cat_scale(GG_CHIP_H), cat_scale(10), st);
        cat_draw_color dim = ink;
        dim.a = 160;
        if (tiny) cat_draw_text(tiny, meta[i].label, cx + cat_scale(12), cy + cat_scale(8), dim);
        static gg_fit_slot chip_fit[JW_GRID_GAMES_MAX_META];
        const char *cv = gg_fit(&chip_fit[i % JW_GRID_GAMES_MAX_META], small,
                                meta[i].value, cw - cat_scale(24), NULL);
        cat_draw_text(small, cv, cx + cat_scale(12), cy + cat_scale(26), ink);
    }
    y += chip_h + (chip_h ? cat_scale(GG_CHIP_GAP) : 0);

    if (syn_n) {
        gg_panel(rx, y, rw, syn_h, cat_scale(10), st);
        cat_draw_color dim = ink; dim.a = 160;
        if (tiny) cat_draw_text(tiny, "SYNOPSIS", rx + cat_scale(12), y + cat_scale(8), dim);
        int ty = y + cat_scale(28);
        for (int i = 0; i < syn_n; ++i) {
            cat_draw_text(small, syn_lines[i], rx + cat_scale(13), ty, ink);
            ty += TTF_FontHeight(small) + cat_scale(4);
        }
        y += syn_h;
    }

    /* System identity, bottom right, on the wallpaper with no panel of its own,
       tinted to match the list text. */
    int wy = sh - M - wm_slot;   /* inside the bottom margin, like every other zone */
    if (wordmark && wm_w > 0 && wm_h > 0) {
        /* Width first: a logo should span the column, and only give that up when
           its own proportions would push it past the slot's height. A wordmark
           is wide by nature, so most of them take the full width. */
        int dw = rw;
        int dh = (wm_h * dw + wm_w / 2) / wm_w;
        if (dh > wm_slot) {
            dh = wm_slot;
            dw = (wm_w * dh + wm_h / 2) / wm_h;
        }
        if (dh < 1) dh = 1;
        SDL_Rect dst = { rx + (rw - dw) / 2, wy + (wm_slot - dh) / 2, dw, dh };
        /* Tinted to the list's own ink. The art is authored white for exactly
           this, so the logo reads as part of the interface rather than as a
           sticker, and it stays legible when the underlay flips polarity. */
        SDL_SetTextureBlendMode(wordmark, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(wordmark, ink.r, ink.g, ink.b);
        SDL_SetTextureAlphaMod(wordmark, 255);
        SDL_RenderCopy(cat_get_renderer(), wordmark, NULL, &dst);
    } else if (system_name && system_name[0]) {
        static gg_fit_slot name_fit;
        int nw = 0;
        const char *fitted = gg_fit(&name_fit, body, system_name, rw, &nw);
        cat_draw_text(body, fitted, rx + (rw - nw) / 2,
                      wy + (wm_slot - body_h) / 2,
                      (cat_draw_color){ 0xEE, 0xF4, 0xEA, 0xFF });
    }

}
