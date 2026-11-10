/*
 * Minimal text console for the bootloader splash framebuffer. With the MMU
 * off every access is Device memory: stay naturally aligned (-mstrict-align).
 * Glyphs: 8x16 font drawn at 2x (16x32 pixels per cell).
 */
#include "console.h"
#include "font.h"

#define MAXT	4
#define SCALE	2
#define CELL_W	(8 * SCALE)
#define CELL_H	(16 * SCALE)
#define MARGIN_X 16
#define MARGIN_Y 24

struct target {
	volatile uint8_t *base;
	uint32_t w, h, stride, bpp;
};

static struct target tg[MAXT];
static int ntg;
static char lines[CON_MAXROWS][CON_MAXCOLS + 1];
static uint32_t lcol[CON_MAXROWS];
static int nlines;

static inline volatile uint8_t *map(uintptr_t phys, uint32_t bytes)
{
#ifdef HOST
	return host_fb_map(phys, bytes);
#else
	(void)bytes;
	return (volatile uint8_t *)phys;
#endif
}

static void put_px(const struct target *t, uint32_t x, uint32_t y, uint32_t c)
{
	volatile uint8_t *p = t->base + (uintptr_t)y * t->stride + (uintptr_t)x * t->bpp;
	if (t->bpp == 4 && !(((uintptr_t)p) & 3)) {
		*(volatile uint32_t *)p = c;
	} else if (t->bpp == 2 && !(((uintptr_t)p) & 1)) {
		uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
		*(volatile uint16_t *)p = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
	} else {
		p[0] = (uint8_t)c;
		p[1] = (uint8_t)(c >> 8);
		p[2] = (uint8_t)(c >> 16);
		if (t->bpp >= 4)
			p[3] = 0xff;
	}
}

static void clear_target(const struct target *t)
{
	for (uint32_t y = 0; y < t->h; y++)
		for (uint32_t x = 0; x < t->w; x++)
			put_px(t, x, y, 0);
}

static void draw_cell(const struct target *t, int col, int row, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t x0 = MARGIN_X + (uint32_t)col * CELL_W, y0 = MARGIN_Y + (uint32_t)row * CELL_H;
	if (x0 + CELL_W > t->w || y0 + CELL_H > t->h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : 0;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(t, x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}

static void draw_row(const struct target *t, int row)
{
	const char *s = lines[row];
	int end = 0;
	while (s[end])
		end++;
	for (int col = 0; col < CON_MAXCOLS; col++)
		draw_cell(t, col, row, col < end ? s[col] : ' ', lcol[row]);
}

void con_init(void)
{
	ntg = 0;
	nlines = 0;
}

int con_targets(void)
{
	return ntg;
}

int con_add_target(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride, uint32_t bpp)
{
	if (ntg >= MAXT || !base || !w || !h || !stride || bpp < 2 || bpp > 4)
		return -1;
	struct target *t = &tg[ntg];
	t->base = map(base, stride * h);
	t->w = w;
	t->h = h;
	t->stride = stride;
	t->bpp = bpp;
	ntg++;
	clear_target(t);
	for (int r = 0; r < nlines; r++)
		draw_row(t, r);
	return 0;
}

/* Columns that fit on the first target (a 720 pixel panel shows 43 of the 64); a longer line is wrapped. */
static int vis_cols(void)
{
	int c = CON_MAXCOLS;
	if (ntg > 0 && tg[0].w > 2 * MARGIN_X + CELL_W) {
		int v = (int)((tg[0].w - 2 * MARGIN_X) / CELL_W);
		if (v < c)
			c = v;
	}
	return c;
}

static void set_n(int idx, const char *s, int max, uint32_t color)
{
	if (idx < 0 || idx >= CON_MAXROWS)
		return;
	int i = 0;
	while (s[i] && i < max && i < CON_MAXCOLS) {
		lines[idx][i] = s[i];
		i++;
	}
	lines[idx][i] = 0;
	lcol[idx] = color;
	if (idx >= nlines)
		nlines = idx + 1;
	for (int k = 0; k < ntg; k++)
		draw_row(&tg[k], idx);
}

int con_line(const char *s, uint32_t color)
{
	int cols = vis_cols(), idx;
	do {
		int n = 0;
		while (s[n] && n < cols)
			n++;
		if (nlines < CON_MAXROWS) {
			idx = nlines++;
		} else {
			idx = CON_MAXROWS - 1;	/* log full: keep overwriting the last row */
		}
		set_n(idx, s, n, color);
		s += n;
	} while (*s);
	return idx;
}

void con_set(int idx, const char *s, uint32_t color)
{
	set_n(idx, s, CON_MAXCOLS, color);
}
