/* A 16-bit texture shuffle on a 32-bit target at another buffer width.
 *
 * GSTextureCache::LookupTarget moves a target's pages to where a new
 * buffer width puts them (RelayoutTarget) before a draw at that width.
 * A 16-bit shuffle on a 32-bit target gets the same move, except where
 * the move is unnecessary or unsafe:
 *
 *  - every page the shuffle draws sits at the same place at both widths
 *    (one 16-bit page row, within the narrower width);
 *  - it comes at exactly twice the width and stays within the target's
 *    height, which the shuffle conversion maps onto the layout there is;
 *  - its width is 1, a single page column split shuffles pass through;
 *  - the draw's texture comes from the target: the move drops every
 *    source made from it, and the renderer still holds that source.
 *
 * Three parts:
 *
 *  1. A page model. A target is a grid of page slots laid out at its
 *     width; a page n belongs at (n % width, n / width). A draw at width
 *     w writes page n into the slot its own width names; a relayout moves
 *     the pages the target's extent covers (its end block, set when the
 *     extent was last laid out, not grown by draws) and loses the rest.
 *     A 4-wide 256x224 target, shuffled 8 wide over 512x896 16-bit, then
 *     drawn 32-bit at width 8: with the move before the shuffle every one
 *     of the 112 pages is where width 8 reads it. Negative control: no
 *     move before the shuffle leaves only the pages the old extent held.
 *
 *  2. The decision, transcribed, over the shapes above, plus a source the
 *     draw holds: a move of the target it comes from frees it. Negative
 *     control: the decision without the in-use guard frees it.
 *
 *  3. The shape of the real code: both relayout branches in LookupTarget
 *     skip the in-use target, the shuffle branch carries the width-1,
 *     same-page and double-width exceptions, and GSRendererHW::Draw hands
 *     the source's target to the render-target and depth lookups.
 *
 * Usage: shuffle_relayout <GSRendererHW.cpp> <GSTextureCache.cpp> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 16
#define MAXH 32
#define EMPTY (-1)

/* ---- 1. page model ------------------------------------------------------ */

struct target
{
	int slot[MAXH][MAXW]; /* page stamp held in each slot of the texture */
	int width;            /* buffer width in pages */
	int rows;             /* texture height in page rows */
	int end_pages;        /* pages the end block covers */
};

static void target_init(struct target* t, int width, int rows)
{
	int x, y;
	for (y = 0; y < MAXH; y++)
		for (x = 0; x < MAXW; x++)
			t->slot[y][x] = EMPTY;
	t->width = width;
	t->rows = rows;
	t->end_pages = width * rows;
	for (y = 0; y < rows; y++)
		for (x = 0; x < width; x++)
			t->slot[y][x] = 1000 + y * width + x; /* scratch contents */
}

/* RelayoutTarget: pages up to the end block, within the old extent. */
static void relayout(struct target* t, int width)
{
	int moved[MAXH][MAXW];
	int pages, n, x, y, rows;

	pages = t->end_pages;
	if (pages > t->width * t->rows)
		pages = t->width * t->rows;
	for (y = 0; y < MAXH; y++)
		for (x = 0; x < MAXW; x++)
			moved[y][x] = EMPTY;
	for (n = 0; n < pages; n++)
		moved[n / width][n % width] = t->slot[n / t->width][n % t->width];
	rows = (pages + width - 1) / width;
	memcpy(t->slot, moved, sizeof(moved));
	t->width = width;
	t->rows = rows;
	t->end_pages = pages;
}

/* A draw at width w over cols x rows pages: page n goes into the slot its
 * own width names; the texture grows, the end block does not. */
static void draw(struct target* t, int w, int cols, int rows, int stamp)
{
	int px, py;
	for (py = 0; py < rows; py++)
		for (px = 0; px < cols; px++)
			t->slot[py][px] = stamp + py * w + px;
	if (rows > t->rows)
		t->rows = rows;
}

/* Pages of the shuffle that a read at width w finds where they belong. */
static int pages_in_place(const struct target* t, int w, int pages, int stamp)
{
	int n, ok = 0;
	for (n = 0; n < pages; n++)
		if (t->slot[n / w][n % w] == stamp + n)
			ok++;
	return ok;
}

static int run_model(int move_before_shuffle)
{
	struct target t;
	target_init(&t, 4, 7);              /* 256x224, 32-bit, width 4 */
	if (move_before_shuffle)
		relayout(&t, 8);
	draw(&t, 8, 8, 14, 0);              /* shuffle: 512x896 16-bit, width 8 */
	if (t.width != 8)
		relayout(&t, 8);                /* the next 32-bit draw at width 8 */
	return pages_in_place(&t, 8, 112, 0);
}

/* ---- 2. the decision ---------------------------------------------------- */

struct shape
{
	const char* what;
	int bpp;          /* draw format */
	int dst_bpp;      /* target format */
	int tbw;          /* draw width */
	int dst_tbw;      /* target width */
	int rect_z, rect_w; /* draw rect, 16-bit coordinates */
	int dst_height;   /* target unscaled height */
	int from_target;  /* the draw's texture comes from this target */
	int expect;       /* the pages move */
};

static int decide(const struct shape* s, int guard_in_use)
{
	int narrow, same_pages, double_width;
	if (guard_in_use && s->from_target)
		return 0;
	if (s->bpp != 16 || s->dst_bpp != 32 || s->tbw <= 1 || s->dst_tbw <= 0 || s->tbw == s->dst_tbw)
		return 0;
	narrow = (s->tbw < s->dst_tbw ? s->tbw : s->dst_tbw) * 64;
	same_pages = s->rect_w <= 64 && s->rect_z <= narrow;
	double_width = s->tbw == s->dst_tbw * 2 && s->rect_w <= s->dst_height;
	return !same_pages && !double_width;
}

static const struct shape s_shapes[] = {
	{ "taller than the target at twice the width", 16, 32, 8, 4, 512, 896, 224, 0, 1 },
	{ "twice the width within the target's height", 16, 32, 8, 4, 512, 224, 224, 0, 0 },
	{ "three times the width",                      16, 32, 12, 4, 768, 224, 224, 0, 1 },
	{ "narrower, several page rows",                16, 32, 4, 10, 256, 448, 448, 0, 1 },
	{ "one page row within the narrower width",     16, 32, 8, 10, 384, 64, 448, 0, 0 },
	{ "one page row past the narrower width",       16, 32, 10, 8, 640, 64, 448, 0, 1 },
	{ "width 1",                                    16, 32, 1, 10, 64, 448, 448, 0, 0 },
	{ "same width",                                 16, 32, 10, 10, 640, 896, 448, 0, 0 },
	{ "32-bit draw (the other branch)",             32, 32, 8, 4, 512, 448, 224, 0, 0 },
	{ "texture from the target itself",             16, 32, 8, 4, 512, 896, 224, 1, 0 },
};

/* A relayout drops every source made from the target; the draw's source
 * survives only if it was not one. */
static int held_source_survives(const struct shape* s, int guard_in_use)
{
	return !(decide(s, guard_in_use) && s->from_target);
}

/* ---- 3. shape of the real code ------------------------------------------ */

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

/* The body of the function whose definition starts with head. */
static const char* body(const char* src, const char* head, const char** end)
{
	const char* s = strstr(src, head);
	*end = s ? strstr(s, "\n}\n") : NULL;
	return *end ? s : NULL;
}

static int count_in(const char* s, const char* end, const char* needle)
{
	int n = 0;
	const size_t len = strlen(needle);
	while (s && (s = strstr(s, needle)) != NULL && s < end)
	{
		n++;
		s += len;
	}
	return n;
}

static int check(const char* what, int ok)
{
	printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	char* hw;
	char* tc;
	const char* lt;
	const char* lt_end;
	const char* dr;
	const char* dr_end;
	int fail = 0;
	int ok, i;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <GSRendererHW.cpp> <GSTextureCache.cpp>\n", argv[0]);
		return 2;
	}

	printf("page model, 4-wide 256x224 target shuffled 8 wide:\n");
	ok = run_model(1);
	printf("  moved before the shuffle: %d of 112 pages in place\n", ok);
	fail += check("every page in place", ok == 112);
	ok = run_model(0);
	printf("  negative control, not moved: %d of 112 pages in place\n", ok);
	fail += check("negative control loses pages", ok < 112);

	printf("decision:\n");
	for (i = 0; i < (int)(sizeof(s_shapes) / sizeof(s_shapes[0])); i++)
		fail += check(s_shapes[i].what, decide(&s_shapes[i], 1) == s_shapes[i].expect);
	fail += check("the draw's own source survives",
		held_source_survives(&s_shapes[9], 1));
	fail += check("negative control, no in-use guard: the source is freed",
		!held_source_survives(&s_shapes[9], 0));

	hw = read_file(argv[1]);
	tc = read_file(argv[2]);
	if (!hw || !tc)
	{
		fprintf(stderr, "cannot read the sources\n");
		return 2;
	}
	printf("real code:\n");
	lt = body(tc, "GSTextureCache::Target* GSTextureCache::LookupTarget(", &lt_end);
	dr = body(hw, "void GSRendererHW::Draw()", &dr_end);
	fail += check("LookupTarget found", lt != NULL);
	fail += check("Draw found", dr != NULL);
	if (lt)
	{
		fail += check("LookupTarget takes in_use", count_in(lt, lt_end, "const Target* in_use)") == 1);
		fail += check("both relayout branches skip the in-use target",
			count_in(lt, lt_end, "dst != in_use &&") == 2);
		fail += check("the shuffle branch skips width 1", count_in(lt, lt_end, "TEX0.TBW > 1 && dst->m_TEX0.TBW > 0 &&") == 1);
		fail += check("the shuffle branch skips pages that stay put",
			count_in(lt, lt_end, "draw_rect.w <= psm_s.pgs.y && draw_rect.z <= narrow") == 1);
		fail += check("the shuffle branch skips the double-width shuffle",
			count_in(lt, lt_end, "TEX0.TBW == dst->m_TEX0.TBW * 2u && draw_rect.w <= dst->m_unscaled_size.y") == 1);
		fail += check("the shuffle branch moves when neither holds",
			count_in(lt, lt_end, "if (!same_pages && !double_width)") == 1);
	}
	if (dr)
		fail += check("Draw hands the source's target to the rt and ds lookups",
			count_in(dr, dr_end, "src ? src->m_from_target : nullptr)") == 2);

	free(hw);
	free(tc);
	printf("shuffle_relayout: %s\n", fail ? "FAIL" : "ok");
	return fail ? 1 : 0;
}
