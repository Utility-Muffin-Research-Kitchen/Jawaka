#include "internal/launcher/grid.h"
#include "internal/launcher/coverflow.h"   /* jw_cf_ease_in_out_cubic: one easing for all carousels */

#include "catastrophe.h"
#include <SDL2/SDL.h>

static int jw__grid_max(int a, int b) { return a > b ? a : b; }
static int jw__grid_min(int a, int b) { return a < b ? a : b; }

void jw_grid_layout(jw_grid *g, const cat_stylesheet_launcher *l,
                    int screen_w, int screen_h, int status_h) {
    if (!g || !l) return;
    int cols   = jw__grid_max(1, l->grid_cols);
    int rows   = jw__grid_max(1, l->grid_rows);
    int margin = cat_scale(l->grid_margin);
    int gutter = cat_scale(l->grid_gutter);

    /* Largest square satisfying both axes; the non-binding axis gets slack. */
    int w_fit = (screen_w - 2 * margin - (cols - 1) * gutter) / cols;
    int avail_h = screen_h - status_h;
    int h_fit = (avail_h - 2 * margin - (rows - 1) * gutter) / rows;
    int tile  = jw__grid_max(8, jw__grid_min(w_fit, h_fit));

    int block_w = cols * tile + (cols - 1) * gutter;
    int block_h = rows * tile + (rows - 1) * gutter;

    g->cols     = cols;
    g->rows     = rows;
    g->tile     = tile;
    g->gutter   = gutter;
    g->radius   = tile * l->grid_radius_pct / 100;
    g->status_h = status_h;
    g->x0       = (screen_w - block_w) / 2;
    /* Top-anchored: the first row sits one gutter below the status band and
       the vertical slack goes to the bottom, per the plan. */
    g->y0       = status_h + gutter;
    (void)block_h;
}

void jw_grid_reset(jw_grid *g) {
    if (!g) return;
    g->scroll_row    = 0;
    g->anim_from_row = 0.0f;
    g->anim_start_ms = 0;
    g->anim_active   = false;
    g->focus_prev    = -1;
    g->focus_anim_start_ms = 0;
}

static float jw__grid_scroll_now(const jw_grid *g, uint32_t now, uint32_t anim_ms) {
    if (!g->anim_active || anim_ms == 0) return (float)g->scroll_row;
    uint32_t el = now - g->anim_start_ms;
    if (el >= anim_ms) return (float)g->scroll_row;
    float t = jw_cf_ease_in_out_cubic((float)el / (float)anim_ms);
    return g->anim_from_row + ((float)g->scroll_row - g->anim_from_row) * t;
}

void jw_grid_step(jw_grid *g, cat_list_state *ls, int count, int dx, int dy,
                  uint32_t now, uint32_t anim_ms) {
    if (!g || !ls || count <= 0) return;
    int cols = g->cols, rows = g->rows;
    int idx  = ls->cursor;
    if (idx < 0 || idx >= count) idx = 0;

    if (dx != 0) {
        /* Linear: the grid is one list drawn in rows, wrapping at both ends. */
        idx = ((idx + dx) % count + count) % count;
    } else if (dy > 0) {
        int cand = idx + cols;
        if (cand < count) {
            idx = cand;
        } else if (idx / cols == (count - 1) / cols) {
            /* Last row: wrap to the same column in the first row. */
            int c = idx % cols;
            idx = (c < count) ? c : 0;
        } else {
            /* A row exists beneath but this column is empty in it. */
            idx = count - 1;
        }
    } else if (dy < 0) {
        int cand = idx - cols;
        if (cand >= 0) {
            idx = cand;
        } else {
            /* First row: wrap to the same column in the last row, else the last tile. */
            int last_start = ((count - 1) / cols) * cols;
            int cand2 = last_start + idx % cols;
            idx = (cand2 < count) ? cand2 : count - 1;
        }
    }
    if (idx != ls->cursor) {
        g->focus_prev = ls->cursor;
        g->focus_anim_start_ms = now;
    }
    ls->cursor = idx;

    int row        = idx / cols;
    int total_rows = (count + cols - 1) / cols;
    int max_scroll = jw__grid_max(0, total_rows - rows);
    int target     = g->scroll_row;
    if (row < target)              target = row;
    else if (row >= target + rows) target = row - rows + 1;
    if (target > max_scroll) target = max_scroll;
    if (target < 0) target = 0;

    if (target != g->scroll_row) {
        g->anim_from_row = jw__grid_scroll_now(g, now, anim_ms);
        g->scroll_row    = target;
        g->anim_start_ms = now;
        g->anim_active   = (anim_ms > 0);
    }
}

static bool jw__grid_ensure_scratch(jw_grid *g, int size) {
    if (g->scratch && g->scratch_size == size) return true;
    if (g->scratch) { SDL_DestroyTexture(g->scratch); g->scratch = NULL; }
    SDL_Renderer *r = cat_get_renderer();
    g->scratch = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                   SDL_TEXTUREACCESS_TARGET, size, size);
    if (!g->scratch) { g->scratch_size = 0; return false; }
    SDL_SetTextureBlendMode(g->scratch, SDL_BLENDMODE_BLEND);
    g->scratch_size = size;
    return true;
}

/* One tile: the border is a rounded rect of the border colour, and the content
   (plate + contain-fit art) is composited off screen then drawn inset by the
   border width with a matching smaller radius. Art stays plain; rounding and
   border are Leaf's, which is what makes them themeable. */
static void jw__grid_draw_tile(jw_grid *g, int x, int y, int size, SDL_Texture *art,
                               int aw, int ah, SDL_Texture *label,
                               int bw, cat_draw_color bc, int radius) {
    /* `size` may exceed the cell (focus scale); centre it on the cell. */
    int off = (size - g->tile) / 2;
    x -= off; y -= off;
    cat_draw_rounded_rect(x, y, size, size, radius, bc);

    int inner_draw = size - 2 * bw;
    if (inner_draw <= 0) return;
    /* Compose at the base tile size once; the draw below scales the result, so
       focus animation never rebuilds the scratch texture. */
    int inner = g->tile;
    SDL_Renderer *r = cat_get_renderer();
    if (!jw__grid_ensure_scratch(g, inner)) return;

    SDL_Texture *prev = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, g->scratch);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    /* Subtle plate so a transparent or undersized icon still reads as a tile. */
    SDL_SetRenderDrawColor(r, 255, 255, 255, 16);
    SDL_Rect plate = { 0, 0, inner, inner };
    SDL_RenderFillRect(r, &plate);
    if (art && aw > 0 && ah > 0) {
        /* Contain-fit, centred: never distorted; wrong-shaped art looks obviously
           wrong to its author rather than subtly wrong. */
        int pad = inner / 12;
        int box = inner - 2 * pad;
        float sc = (float)box / (float)(aw > ah ? aw : ah);
        int dw = (int)(aw * sc), dh = (int)(ah * sc);
        SDL_Rect dst = { (inner - dw) / 2, (inner - dh) / 2, dw, dh };
        SDL_RenderCopy(r, art, NULL, &dst);
    }
    if (label) {
        /* Full-tile overlay: the author positioned the wordmark within a tile-
           sized canvas, so it maps onto the interior edge to edge. */
        SDL_Rect full = { 0, 0, inner, inner };
        SDL_RenderCopy(r, label, NULL, &full);
    }
    SDL_SetRenderTarget(r, prev);

    int ir = jw__grid_max(0, radius - bw);
    cat_draw_image_rounded_ex(g->scratch, x + bw, y + bw, inner_draw, inner_draw, ir, CAT_CORNER_ALL);
}

bool jw_grid_draw(jw_grid *g, const cat_stylesheet_launcher *l,
                  const cat_list_state *ls, int count,
                  jw_grid_icon_fn icon_fn, jw_grid_icon_fn label_fn, void *ctx,
                  uint32_t now, uint32_t anim_ms) {
    if (!g || !l || !ls || count <= 0) return false;
    int cols = g->cols, rows = g->rows, pitch = g->tile + g->gutter;
    float scroll = jw__grid_scroll_now(g, now, anim_ms);
    bool animating = g->anim_active && (now - g->anim_start_ms) < anim_ms;
    if (!animating) g->anim_active = false;

    cat_draw_color border = cat_color_to_sdl(l->grid_border_color);
    cat_draw_color focus  = (CAT_COLOR_A(l->grid_focus_border_color) == 0)
                                ? cat_get_theme()->accent
                                : cat_color_to_sdl(l->grid_focus_border_color);
    int bw_norm  = cat_scale(l->grid_border_w);
    int bw_focus = cat_scale(l->grid_focus_border_w);

    /* Tiles never draw into the status band, even mid-scroll. */
    SDL_Renderer *r = cat_get_renderer();
    SDL_Rect prev_clip; SDL_RenderGetClipRect(r, &prev_clip);
    SDL_Rect clip = { 0, g->status_h, cat_get_screen_width(), cat_get_screen_height() - g->status_h };
    SDL_RenderSetClipRect(r, &clip);

    /* Focus scale tween, shared duration with the row scroll. */
    float grow = (float)jw__grid_max(100, l->grid_focus_scale_pct) / 100.0f - 1.0f;
    float ft = 1.0f;
    if (anim_ms > 0 && g->focus_prev >= 0) {
        uint32_t el = now - g->focus_anim_start_ms;
        ft = el >= anim_ms ? 1.0f : jw_cf_ease_in_out_cubic((float)el / (float)anim_ms);
    }
    if (ft >= 1.0f) g->focus_prev = -1;
    else animating = true;
    int focus_idx = ls->cursor;

    int total_rows = (count + cols - 1) / cols;
    int first = jw__grid_max(0, (int)scroll - 1);
    int last  = jw__grid_min(total_rows - 1, (int)scroll + rows + 1);
    /* Pass 1: every tile but the focused one, so the focused tile can overlap
       its neighbours when it grows. The previously focused tile eases back. */
    for (int pass = 0; pass < 2; pass++) {
        for (int row = first; row <= last; row++) {
            int y = g->y0 + (int)(((float)row - scroll) * (float)pitch);
            for (int c = 0; c < cols; c++) {
                int idx = row * cols + c;
                if (idx >= count) break;
                bool focused = (idx == focus_idx);
                if ((pass == 0) == focused) continue;
                int x = g->x0 + c * pitch;
                int aw = 0, ah = 0;
                SDL_Texture *art = icon_fn ? icon_fn(ctx, idx, &aw, &ah) : NULL;
                int lw = 0, lh = 0;
                SDL_Texture *label = label_fn ? label_fn(ctx, idx, &lw, &lh) : NULL;
                float sc = 1.0f;
                if (focused)                     sc = 1.0f + grow * ft;
                else if (idx == g->focus_prev)   sc = 1.0f + grow * (1.0f - ft);
                int size = (int)((float)g->tile * sc);
                int radius = (int)((float)g->radius * sc);
                jw__grid_draw_tile(g, x, y, size, art, aw, ah, label,
                                   focused ? bw_focus : bw_norm,
                                   focused ? focus : border, radius);
            }
        }
    }

    if (prev_clip.w == 0 && prev_clip.h == 0) SDL_RenderSetClipRect(r, NULL);
    else                                      SDL_RenderSetClipRect(r, &prev_clip);
    return animating;
}
