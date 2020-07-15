extern "C" {

#include <cygwin/version.h>

#include "tek.h"

#include <windows.h>
#include "winpriv.h"
#include "child.h"

enum tekmode tek_mode = TEKMODE_OFF;
bool tek_bypass = false;
static uchar intensity = 0x7F; // for point modes
static uchar style = 0;        // for vector modes
static uchar font = 0;
static short margin = 0;
static bool beam_defocused = false;
static bool beam_writethru = false;
static bool plotpen = false;
static bool apl_mode = false;

static short tek_y, tek_x;
static short gin_y, gin_x;
static uchar lastfont = 0;
static int lastwidth = -1;

static int beam_glow = 1;
static int thru_glow = 5;

static bool flash = false;

static wchar * APL = const_cast<wchar *>(W(" ¨)<≤=>]∨∧≠÷,+./0123456789([;×:\\¯⍺⊥∩⌊∊_∇∆⍳∘'⎕∣⊤○⋆?⍴⌈∼↓∪ω⊃↑⊂←⊢→≥-⋄ABCDEFGHIJKLMNOPQRSTUVWXYZ{⊣}$ "));

struct tekfont {
  void * f;
  short rows, cols;
  short hei, wid;
} tekfonts[] = {
  {0, 35, 74, 87, 55},
  {0, 38, 81, 80, 50},
  {0, 58, 121, 52, 33},
  {0, 64, 133, 48, 30}
};

struct tekchar {
  char type;
  uchar recent;
  bool defocused;
  bool writethru;
#if CYGWIN_VERSION_API_MINOR >= 74
  union {
    struct {
      wchar c;
      short w;
      uchar font;
    };
    struct {
      short y, x;
      uchar intensity;
      uchar style;
    };
  };
#else
  wchar c;
  short w;
  uchar font;
  short y, x;
  short intensity;
  uchar style;
#endif
};
static struct tekchar * tek_buf = 0;
static int tek_buf_len = 0;
static int tek_buf_size = 0;

static void
tek_buf_append(struct tekchar * tc)
{
  if (tek_buf_len == tek_buf_size) {
    int new_size = tek_buf_size + 1000;
    struct tekchar * new_buf = renewn(tek_buf, new_size);
    if (!new_buf)
      return;
    tek_buf = new_buf;
    tek_buf_size = new_size;
  }
  tek_buf[tek_buf_len ++] = * tc;
}

static void
tek_buf_clear(void)
{
  if (tek_buf)
    free(tek_buf);
  tek_buf = 0;
  tek_buf_len = 0;
  tek_buf_size = 0;
}


static void
tek_home(void)
{
  tek_x = 0;
  tek_y = 3120 - tekfonts[font].hei;
  margin = 0;
}

void
(tek_clear)(struct term* term_p)
{
  TERM_VAR_REF(true)

  tek_buf_clear();
  flash = true;
  tek_paint();
  flash = false;
  usleep(30000);
  tek_home();
}

void
tek_gin(void)
{
  //gin_y = tek_y;
  //gin_x = tek_x;
  tek_move_by(0, 0);
}

/* PAGE
   Erases the display, resets to Alpha Mode and home position;
   resets to Margin 1 and cancels Bypass condition.
*/
void
(tek_page)(struct term* term_p)
{
  TERM_VAR_REF(true)

  tek_clear();
  tek_mode = TEKMODE_ALPHA;
  margin = 0;
  tek_bypass = false;
}

/* RESET (xterm)
   Unlike the similarly-named Tektronix “RESET” button, this
   does everything that PAGE does as well as resetting the
   line-type and font-size to their default values.
*/
void
(tek_reset)(struct term* term_p)
{
  TERM_VAR_REF(true)

  // line type
  style = 0;
  beam_defocused = false;
  beam_writethru = false;
  // font
  font = 0;
  apl_mode = false;
  // clear etc
  tek_page();
  // let's also do this
  intensity = 0x7F;
}

void
tek_font(short f)
{
  font = f & 3;
}

void
tek_write(wchar c, int width)
{
  if (tek_bypass)
    return;

  if (apl_mode && c >= ' ' && c < 0x80) {
    c = APL[c - ' '];
    width = 1;
  }

  struct tekchar tekchar_tmp;
  tekchar_tmp.type = 0;
  tekchar_tmp.recent = (uchar)(beam_writethru ? thru_glow : beam_glow);
  tekchar_tmp.defocused = beam_defocused;
  tekchar_tmp.writethru = beam_writethru;
  tekchar_tmp.c = c;
  tekchar_tmp.w = (short)width;
  tekchar_tmp.font = font;
  tek_buf_append(&tekchar_tmp);
  if (width > 0) {
    tek_x += width * tekfonts[font].wid;
  }
}

void
tek_alt(bool alt_chars)
{
  apl_mode = alt_chars;
}


void
tek_copy(void)
{
}


/* vector_style
   0 solid
   1 dotted
   2 dot-dashed
   3 short dashed
   4 long dashed
*/
void
tek_beam(bool defocused, bool write_through, char vector_style)
{
  beam_defocused = defocused;
  beam_writethru = write_through;
  if (vector_style > 4)
    style = 0;
  else
    style = vector_style;
}

void
tek_intensity(bool defocused, int i)
{
  beam_defocused = defocused;
  intensity = i;
}

#define dont_debug_graph

void
tek_address(char * code)
{
#ifdef debug_graph
  printf("tek_address %d <%s>", tek_mode, code);
#endif
  /* https://vt100.net/docs/vt3xx-gp/chapter13.html#S13.14.3
	tag bits
	https://vt100.net/docs/vt3xx-gp/chapter13.html#T13-4
	01 11 11 01 10	12 Address Bits
			01	High Y
			11	Extra
			11	Low Y
			01	High X
			10	Low X
	01 11 01 10	10 Address Bits
	https://vt100.net/docs/vt3xx-gp/chapter13.html#T13-5
	01 10		High Y	Low X
	11 10		Low Y	Low X
	11 01 10	Low Y	High X	Low X
	10		Low X
	11 11 10	Extra	Low Y	Low X
	more seen
	01 11 10	High Y	Low Y	Low X
	11 11 01 10	Extra	Low Y	High X	Low X
	01 11 11 10	High Y	Extra	Low Y	Low X
	more unseen
	01 01 10	High Y	High X	Low X
  */
  // accumulate tags for switching; clear tags from input
  short tag = 0;
  char * tc = code;
  while (* tc) {
    tag = (tag << 2) | (* tc >> 5);
#ifdef debug_graph
    printf(" %d%d", *tc >> 6, (*tc >> 5) & 1);
#endif
    * tc &= 0x1F;
    tc ++;
  }
#ifdef debug_graph
  printf("\n");
#endif

  // switch by accumulated tag sequence
  if (tag == 0x1F6) {      // 12 Bit Address
    tek_y = code[0] << 7 | code[2] << 2 | code[1] >> 2;
    tek_x = code[3] << 7 | code[4] << 2 | (code[1] & 3);
  }
  else if (tag == 0x76) {  // 10 Bit Address
    tek_y = code[0] << 7 | code[1] << 2;
    tek_x = code[2] << 7 | code[3] << 2;
  }
  else if (tag == 0x06) {  // Short Address, Byte Changed: High Y
    //	01 10		High Y	Low X
    tek_y = (tek_y & 0x7F) | code[0] << 7;
    tek_x = (tek_x & 0xF83) | code[1] << 2;
  }
  else if (tag == 0x0E) {  // Short Address, Byte Changed: Low Y
    //	11 10		Low Y	Low X
    tek_y = (tek_y & 0xF83) | code[0] << 2;
    tek_x = (tek_x & 0xF83) | code[1] << 2;
  }
  else if (tag == 0x36) {  // Short Address, Byte Changed: High X
    //	11 01 10	Low Y	High X	Low X
    tek_y = (tek_y & 0xF83) | code[0] << 2;
    tek_x = (tek_x & 0x3) | code[1] << 7 | code[2] << 2;
  }
  else if (tag == 0x02) {  // Short Address, Byte Changed: Low X
    //	10		Low X
    tek_x = (tek_x & 0xF83) | code[0] << 2;
  }
  else if (tag == 0x3E) {  // Short Address, Byte Changed: Extra
    //	11 11 10	Extra	Low Y	Low X
    tek_y = (tek_y & 0xF80) | code[1] << 2 | code[0] >> 2;
    tek_x = (tek_x & 0xF80) | code[2] << 2 | (code[0] & 3);
  }
  else if (tag == 0x1E) {
    //	01 11 10	High Y	Low Y	Low X
    tek_y = (tek_y & 0x3) | code[0] << 7 | code[1] << 2;
    tek_x = (tek_x & 0xF83) | code[2] << 2;
  }
  else if (tag == 0xF6) {
    //	11 11 01 10	Extra	Low Y	High X	Low X
    tek_y = (tek_y & 0xF80) | code[1] << 2 | code[0] >> 2;
    tek_x = code[2] << 7 | code[3] << 2 | (code[0] & 3);
  }
  else if (tag == 0x7E) {
    //	01 11 11 10	High Y	Extra	Low Y	Low X
    tek_y = code[0] << 7 | code[2] << 2 | code[1] >> 2;
    tek_x = (tek_x & 0xF80) | code[3] << 2 | (code[1] & 3);
  }
  else if (tag == 0x16) {
    //	01 01 10	High Y	High X	Low X
    tek_y = (tek_y & 0x7F) | code[0] << 7;
    tek_x = (tek_x & 0x3) | code[1] << 7 | code[2] << 2;
  }
  else {  // error
#ifdef debug_graph
  printf(" -> err\n");
#endif
    return;
  }

#ifdef debug_graph
  printf(" -> (%d) -> %d:%d\n", tek_mode, tek_y, tek_x);
#endif
  struct tekchar tekchar_tmp;
  tekchar_tmp.type = tek_mode;
  tekchar_tmp.recent = (uchar)(beam_writethru ? thru_glow : beam_glow);
  tekchar_tmp.defocused = beam_defocused;
  tekchar_tmp.writethru = beam_writethru;
  tekchar_tmp.y = tek_y;
  tekchar_tmp.x = tek_x;
  tekchar_tmp.style = style;
  tekchar_tmp.intensity = intensity;
  tek_buf_append(&tekchar_tmp);
}

/*	DEAIHJBF
	0100	up	DEF
	0001	right	EAI
	1000	down	IHJ
	0010	left	JBF
 */
void
tek_step(char c)
{
  if (c & 8)
    tek_y -= 1;
  if (c & 4)
    tek_y += 1;
  if (c & 2)
    tek_x -= 1;
  if (c & 1)
    tek_x += 1;

  struct tekchar tekchar_tmp;
  if (plotpen) {
    tekchar_tmp.type = TEKMODE_POINT_PLOT;
    tekchar_tmp.recent = (uchar)(beam_writethru ? thru_glow : beam_glow);
    tekchar_tmp.defocused = beam_defocused;
    tekchar_tmp.writethru = beam_writethru;
    tekchar_tmp.y = tek_y;
    tekchar_tmp.x = tek_x;
    tekchar_tmp.intensity = intensity;
  }
  else {
    tekchar_tmp.type = TEKMODE_GRAPH0;
    tekchar_tmp.recent = (uchar)(beam_writethru ? thru_glow : beam_glow);
    tekchar_tmp.defocused = beam_defocused;
    tekchar_tmp.writethru = beam_writethru;
    tekchar_tmp.y = tek_y;
    tekchar_tmp.x = tek_x;
    tekchar_tmp.intensity = 0;
  }
    tek_buf_append(&tekchar_tmp);
}

void
tek_pen(bool on)
{
  plotpen = on;
  if (on)
    tek_step(0);
}


static void
trace_tek(void)
{
#ifdef debug_tek_output
static bool ptd = false;
  if (ptd) return;

  for (int i = 0; i < tek_buf_len; i++) {
    struct tekchar * tc = &tek_buf[i];

    if (tc->type == TEKMODE_GRAPH0)
      printf("move %4d %4d\n", tc->y, tc->x);
    else if (tc->type == TEKMODE_GRAPH)
      printf("line %4d %4d\n", tc->y, tc->x);
    else if (tc->type == TEKMODE_POINT_PLOT || tc->type == TEKMODE_SPECIAL_PLOT)
      printf("plot %4d %4d\n", tc->y, tc->x);
    else
      printf("text %04X:%d\n", tc->c, tc->w);
  }

  ptd = true;
#endif
}


static void
fix_gin()
{
  if (gin_y < 0)
    gin_y = 0;
  if (gin_y > 3119)
    gin_y = 3119;
  if (gin_x < 0)
    gin_x = 0;
  if (gin_x > 4095)
    gin_x = 4095;
}

void
tek_move_by(int dy, int dx)
{
  //printf("tek_move_by %d:%d\n", dy, dx);
  if (dy || dx) {
    gin_y += dy;
    gin_x += dx;
    fix_gin();
  }
  else {
    gin_y = 1560;
    gin_x = 2048;
  }
}

void
(tek_move_to)(struct term* term_p, int y, int x)
{
  TERM_VAR_REF(true)

  //printf("tek_move_to %d:%d\n", y, x);
  int height, width;
  win_get_pixels(&height, &width, false);

  int pad_l = 0, pad_t = 0;
  if (width > height * 4096 / 3120) {
    // width factor > height factor; reduce width
    int w = height * 4096 / 3120;
    pad_l = (width - w) / 2;
    width = w;
  }
  else if (height > width * 3120 / 4096) {
    // height factor > width factor; reduce height
    int h = width * 3120 / 4096;
    pad_t = (height - h) / 2;
    height = h;
  }

  gin_y = 3119 - (y - pad_t) * 3120 / height;
  gin_x = (x - pad_l) * 4096 / width;
  fix_gin();
}

void
(tek_send_address)(struct child* child_p)
{
  CHILD_VAR_REF(true)

  child_printf("%c%c%c%c",
               0x20 | (tek_y >> 7), 0x60 | ((tek_y >> 2) & 0x1F),
               0x20 | (tek_x >> 7), 0x40 | ((tek_x >> 2) & 0x1F));
  tek_mode = TEKMODE_ALPHA;
}

void
(tek_enq)(struct child* child_p)
{
  child_write("4", 1);
  tek_send_address();
}

colour fg;

static uint
get_font_quality(void)
{
  uchar tmp[4];
  tmp[FS_DEFAULT] = DEFAULT_QUALITY;
  tmp[FS_NONE] = NONANTIALIASED_QUALITY;
  tmp[FS_PARTIAL] = ANTIALIASED_QUALITY;
  tmp[FS_FULL] = CLEARTYPE_QUALITY;
  return tmp[(int)cfg.font_smoothing];
}

static void
init_font(short f)
{
  if (tekfonts[f].f)
    DeleteObject(tekfonts[f].f);

  wstring fn = *cfg.tek_font ? cfg.tek_font : cfg.font.name;
  tekfonts[f].f = CreateFontW(
                  - tekfonts[f].hei, - tekfonts[f].wid, 
                  0, 0, FW_NORMAL, 0, 0, 0,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  get_font_quality(), FIXED_PITCH | FF_DONTCARE,
                  fn);
}

static void
out_text(HDC dc, short x, short y, wchar * s, uchar f)
{
  SelectObject(dc, tekfonts[f].f);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, fg);

  int len = wcslen(s);
  int dxs[len];
  for (int i = 0; i < len; i++)
    dxs[i] = tekfonts[f].wid * lastwidth;
  ExtTextOutW(dc, x, y, 0, 0, s, len, dxs);
}

static short txt_y, txt_x;
static short out_y, out_x;
static wchar * txt = 0;
static int txt_len = 0;
static int txt_wid = 0;

static void
out_flush(HDC dc)
{
  if (txt) {
    if (!lastwidth) {
      // fix position of combining character
      short pw = tekfonts[lastfont].wid;
      txt_x -= pw;
    }
    //printf("%d <%ls> [%d]\n", txt_len, txt, lastwidth);
    out_text(dc, txt_x, 3120 - txt_y - tekfonts[font].hei, txt, lastfont & 3);
    free(txt);
    txt = 0;
    txt_len = 0;
    txt_wid = 0;
  }
}

static void
out_cr(void)
{
  out_x = margin;
}

static void
out_lf(void)
{
  short ph = tekfonts[lastfont & 3].hei;
  out_y -= ph;
  if (out_y < 0) {
    out_y = 3120 - ph;
    margin = 2048 - margin;
    out_x = (out_x + 2048) % 4096;
  }
}

static void
out_up(void)
{
  short ph = tekfonts[lastfont & 3].hei;
  out_y += ph;
  if (out_y + ph >= 3120) {
    out_y = 0;
    margin = 2048 - margin;
    out_x = (out_x + 2048) % 4096;
  }
}

static void
out_char(HDC dc, struct tekchar * tc)
{
  if (tc->c < ' ') {
    out_flush(dc);
    short pw = tekfonts[tc->font].wid;
    switch(tc->c) {
      when '\b':  /* BS: left */
        out_x -= pw;
        if (out_x < margin) {
          out_up();
          out_x = 4096 - pw;
        }
      when '\t':  /* HT: right */
        out_x += pw;
        if (out_x + pw > 4096) {
          out_cr();
          out_lf();
        }
      when '\v':  /* VT: up */
        out_up();
      when '\n':  /* LF: down */
        out_lf();
      when '\r':  /* CR: carriage return */
        out_cr();
    }
  }
  else {
    if (tc->w != lastwidth || !tc->w) {
      out_flush(dc);
    }
    lastwidth = tc->w;

    if (!txt) {
      txt_y = out_y;
      txt_x = out_x;
    }

    short pw = tc->w * tekfonts[tc->font].wid;
    out_x += pw;
    if (out_x + pw > 4096) {
      out_flush(dc);
      out_cr();
      out_lf();
    }

    txt = renewn(txt, (txt ? wcslen(txt) : 0) + 2);
    txt[txt_len ++] = tc->c;
    txt[txt_len] = 0;
    txt_wid += pw;
  }
  lastfont = tc->font;
}

void
(tek_init)(struct term* term_p, int glow)
{
  TERM_VAR_REF(true)

  init_font(0);
  init_font(1);
  init_font(2);
  init_font(3);

  static bool init = false;
  if (!init) {
    tek_reset();
    beam_glow = glow;
    init = true;
  }
}

void
(tek_paint)(struct term* term_p)
{
  TERM_VAR_REF(true)

  //unsigned long now = mtime();
  trace_tek();

  /* scale mode, how to map Tek coordinates to window coordinates
     -1 pre-scale
     0 calculate; would need font scaling
     1 post-scale; would need proper calculation of pen width
  */
  short scale_mode = -1;

  // retrieve colour configuration
  colour fg0 = win_get_colour(TEK_FG_COLOUR_I);
  if (fg0 == (colour)-1)
    fg0 = win_get_colour(FG_COLOUR_I);
  colour bg = win_get_colour(TEK_BG_COLOUR_I);
  if (bg == (colour)-1)
    bg = win_get_colour(BG_COLOUR_I);
  colour cc = win_get_colour(TEK_CURSOR_COLOUR_I);
  if (cc == (colour)-1)
    cc = win_get_colour(CURSOR_COLOUR_I);
  // optionally use current text colour?
  //cattr attr = apply_attr_colour(term.curs.attr, ACM_SIMPLE); .truefg/bg

  // adjust colours
  // use full colour for glow or bold (defocused)
  colour glowfg = fg0;
  // derived dimmed default colour
  fg0 = ((fg0 & 0xFEFEFEFE) >> 1) + ((fg0 & 0xFCFCFCFC) >> 2)
                                  + ((bg & 0xFCFCFCFC) >> 2);
  // also dim cursor colour
  cc = ((cc & 0xFEFEFEFE) >> 1) + ((cc & 0xFCFCFCFC) >> 2)
                                + ((bg & 0xFCFCFCFC) >> 2);

  // retrieve terminal pixel size (without padding)
  int height, width;
  win_get_pixels(&height, &width, false);

  // align to aspect ratio
  int pad_l = 0, pad_t = 0;
  int pad_r = 0, pad_b = 0;
  if (width > height * 4096 / 3120) {
    // width factor > height factor; reduce width
    int w = height * 4096 / 3120;
    pad_l = (width - w) / 2;
    pad_r = width - w - pad_l;
    width = w;
  }
  else if (height > width * 3120 / 4096) {
    // height factor > width factor; reduce height
    int h = width * 3120 / 4096;
    pad_t = (height - h) / 2;
    pad_b = height - h - pad_t;
    height = h;
  }
  (void)pad_r; (void)pad_b;  // could be used to clear outer pane

  HDC dc = GetDC(wnd);
  HDC hdc = CreateCompatibleDC(dc);
  HBITMAP hbm = scale_mode == 1
                ? CreateCompatibleBitmap(dc, 4096, 4096)
                : CreateCompatibleBitmap(dc, width, height);
  (void)SelectObject(hdc, hbm);

  // fill background
  if (flash)
    bg = fg0;
  HBRUSH bgbr = CreateSolidBrush(bg);
  RECT tmp_rect;
  if (scale_mode == 1)
    tmp_rect = {0, 0, 4096, 4096};
  else
    tmp_rect = {0, 0, width, height};
  FillRect(hdc, &tmp_rect, bgbr);
  DeleteObject(bgbr);

  auto tx = [&](int x) -> int
  {
    if (scale_mode)
      return x;
    else
      return x * width / 4096;
  };
  auto ty = [&](int y) -> int
  {
    if (scale_mode)
      return 3119 - y;
    else
      return (3119 - y) * height / 4096;
  };

  XFORM oldxf;
  if (scale_mode == -1 && SetGraphicsMode(hdc, GM_ADVANCED)) {
    GetWorldTransform(hdc, &oldxf);
    XFORM xform = (XFORM){(float)width / (float)4096.0, 0.0, 0.0, 
                          (float)height / (float)3120.0, 0.0, 0.0};
    if (!ModifyWorldTransform(hdc, &xform, MWT_LEFTMULTIPLY))
      scale_mode = 0;
  }
  else
    scale_mode = 0;

  txt = 0;
  out_x = 0;
  out_y = 3120 - tekfonts[font].hei;
  margin = 0;
  lastfont = 4;
  int pen_width = scale_mode == 1 ? width / 204 + height / 156 : 0;
  //printf("tek_paint %d %p\n", tek_buf_len, tek_buf);
  for (int i = 0; i < tek_buf_len; i++) {
    struct tekchar * tc = &tek_buf[i];

    // write-thru mode and beam glow effect (bright drawing spot)
    if (tc->writethru) {
      fg = fg0;
      if (tc->recent) {
        // simulate Write-Thru by distinct colour?
        //fg = RGB(200, 100, 0);
        // fade out?
        if (tc->recent <= (thru_glow + 1) / 2)
          fg = ((fg0 & 0xFEFEFEFE) >> 1) + ((bg & 0xFEFEFEFE) >> 1);

        tc->recent--;
      }
      else {
        // simulate faded Write-Thru by distinct colour?
        //fg = RGB(200, 100, 0);
        fg = cfg.tek_write_thru_colour;
        if (fg == (colour)-1) {
          fg = ((fg0 & 0xFEFEFEFE) >> 1) + ((bg & 0xFEFEFEFE) >> 1);
          fg = RGB(green(fg), blue(fg), red(fg));
        }
        // fade away?
        //fg = bg;
      }
    }
    else if (tc->recent) {
      fg = glowfg;
      tc->recent --;
    }
    else
      fg = fg0;

    // defocused mode
    if (tc->defocused) {
      // simulate defocused by brighter display
      //fg = glowfg;
      // or by wider pen
      pen_width = width / 204 + height / 156;
      // or by shaded pen; not implemented
    }

    if (tc->type) {
      out_flush(hdc);
      out_x = tc->x;
      out_y = tc->y;
    }
    else {
      if (tc->font != lastfont)
        out_flush(hdc);
      out_char(hdc, &tek_buf[i]);
    }

    if (tc->type == TEKMODE_GRAPH0)
      MoveToEx(hdc, tx(tc->x), ty(tc->y), null);
    else if (tc->type == TEKMODE_GRAPH) {
      HPEN pen;
      auto create_pen = [&](DWORD style) -> HPEN
      {
#ifdef use_extpen
        LOGBRUSH brush = (LOGBRUSH){BS_HOLLOW, fg, 0};
        return ExtCreatePen(PS_GEOMETRIC | style, pen_width, &brush, 0, 0);
#else
        return CreatePen(style, pen_width, fg);
#endif
      };
      switch (tc->style) {
        // 1 dotted
        when 1: pen = create_pen(PS_DOT);
        // 2 dot-dashed
        when 2: pen = create_pen(PS_DASHDOT);
        // 3 short dashed
        when 3: pen = create_pen(PS_DASHDOTDOT);
        // 4 long dashed
        when 4: pen = create_pen(PS_DASH);
        // 0 solid
        break;
        default:
          if (pen_width)
            pen = create_pen(PS_SOLID);
          else
            pen = CreatePen(PS_SOLID, pen_width, fg);
      }
      HPEN oldpen = (HPEN)SelectObject(hdc, pen);
      SetBkMode(hdc, TRANSPARENT);  // stabilize broken vector styles
      LineTo(hdc, tx(tc->x), ty(tc->y));
      oldpen = (HPEN)SelectObject(hdc, oldpen);
      DeleteObject(oldpen);
      // add final point
      SetPixel(hdc, tx(tc->x), ty(tc->y), fg);
    }
    else if (tc->type == TEKMODE_POINT_PLOT || tc->type == TEKMODE_SPECIAL_PLOT) {
      if (tc->intensity == 0x7F)
        SetPixel(hdc, tx(tc->x), ty(tc->y), fg);
      else {
        static short intensify[64] =
          { 0,  1,  1,  1,   1,  1,  1,  2,    2,  2,  2,  2,   3,  3,  3,  3,
            4,  4,  4,  5,   5,  5,  6,  6,    7,  8,  9, 10,  11, 12, 12, 13,
           14, 16, 17, 19,  20, 22, 23, 25,   28, 31, 34, 38,  41, 44, 47, 50,
           56, 62, 69, 75,  81, 88, 94, 100,  56, 63, 69, 75,  81, 88, 96, 100
          };
        int r = red(fg);
        int g = green(fg);
        int b = blue(fg);
        int _r = red(bg);
        int _g = green(bg);
        int _b = blue(bg);
        r = (r - _r) * intensify[tc->intensity & 0x3F] / 100 + _r;
        g = (g - _g) * intensify[tc->intensity & 0x3F] / 100 + _g;
        b = (b - _b) * intensify[tc->intensity & 0x3F] / 100 + _b;
        colour fgpix = RGB(r, g, b);
        SetPixel(hdc, tx(tc->x), ty(tc->y), fgpix);
      }
    }
  }

  // cursor ▐ or fill rectangle; ❚ spiddly; █▒▓ do not work unclipped
  if (lastfont < 4) {
    if (cc != fg)
      out_flush(hdc);
    fg = cc;
    struct tekchar tekchar_tmp;
    tekchar_tmp.type = 0;
    tekchar_tmp.c = 0x2590;
    tekchar_tmp.w = 1;
    tekchar_tmp.font = lastfont;
    out_char(hdc, &tekchar_tmp);
  }
  out_flush(hdc);

  // GIN mode
  if (tek_mode == TEKMODE_GIN) {
    fg = ((fg0 & 0xFEFEFEFE) >> 1) + ((bg & 0xFEFEFEFE) >> 1);
    HPEN pen = CreatePen(PS_SOLID, pen_width, fg);
    HPEN oldpen = (HPEN)SelectObject(hdc, pen);
    SetBkMode(hdc, TRANSPARENT);  // stabilize broken vector styles

    //printf("GIN %d:%d\n", gin_y, gin_x);
    MoveToEx(hdc, tx(0), ty(gin_y), null);
    LineTo(hdc, tx(4096), ty(gin_y));
    MoveToEx(hdc, tx(gin_x), ty(0), null);
    LineTo(hdc, tx(gin_x), ty(3120));

    oldpen = (HPEN)SelectObject(hdc, oldpen);
    DeleteObject(oldpen);
  }

  if (scale_mode == -1)
    SetWorldTransform(hdc, &oldxf);

  if (scale_mode == 1)
    StretchBlt(dc,
               PADDING + pad_l, OFFSET + PADDING + pad_t,
               width, height,
               hdc,
               0, 0, 4096, 3120, SRCCOPY);
  else
    BitBlt(dc,
           PADDING + pad_l, OFFSET + PADDING + pad_t,
           width, height,
           hdc, 0, 0, SRCCOPY);

  DeleteObject(hbm);
  DeleteDC(hdc);
  ReleaseDC(wnd, dc);
  //printf("tek_painted %ld\n", mtime() - now);
}

}

