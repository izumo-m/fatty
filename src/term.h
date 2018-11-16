#ifndef TERM_H
#define TERM_H

#include "minibidi.h"
#include "config.h"

// Colour numbers

struct term;

typedef enum {
  // ANSI colours
  BLACK_I   = 0,
  RED_I     = 1,
  GREEN_I   = 2,
  YELLOW_I  = 3,
  BLUE_I    = 4,
  MAGENTA_I = 5,
  CYAN_I    = 6,
  WHITE_I   = 7,

  // Bright/Bold ANSI colours
  BOLD_BLACK_I   = 8,
  BOLD_RED_I     = 9,
  BOLD_GREEN_I   = 10,
  BOLD_YELLOW_I  = 11,
  BOLD_BLUE_I    = 12,
  BOLD_MAGENTA_I = 13,
  BOLD_CYAN_I    = 14,
  BOLD_WHITE_I   = 15,

  // Colour numbers 16 through 231 are occupied by a 6x6x6 colour cube,
  // with R at most significant and B at least. (36*R + 6*G + B + 16)

  // Colour numbers 232 through 255 are occupied by a uniform series of
  // gray shades running between black and white but not including either
  // on grounds of redundancy.

  // Colour numbers 256 through 271 are copies of ANSI colours 0 through 15
  // supporting distinct handling of ANSI colours SGR 30..37/40../90../100.. 
  // and palette colours SGR 38/48;5;0..15
  // For distinct bold handling alone, they could also be mapped to 0..15
  // but duplicating them would also facilitate distinct colour values if desired
  ANSI0            = 256,

  // Default foreground
  FG_COLOUR_I      = 272,
  BOLD_FG_COLOUR_I = 273,

  // Default background
  BG_COLOUR_I      = 274,
  BOLD_BG_COLOUR_I = 275,

  // Cursor colours
  CURSOR_TEXT_COLOUR_I = 276,
  CURSOR_COLOUR_I      = 277,
  IME_CURSOR_COLOUR_I  = 278,

  // Selection highlight colours
  SEL_COLOUR_I         = 279,
  SEL_TEXT_COLOUR_I    = 280,

  // configured Bold colour
  BOLD_COLOUR_I = 281,

  // Number of colours
  COLOUR_NUM = 282,

  // True Colour indicator
  // assert (TRUE_COLOUR % 4) == 0 so that checking x >= TRUE_COLOUR
  // is resistant to previous |= 1 or ^= 2 (win_text)
  TRUE_COLOUR = 0x180
} colour_i;

// colour classes
#define CCL_ANSI8(i) ((i) >= ANSI0 && (i) < ANSI0 + 8)
#define CCL_DEFAULT(i) ((i) >= FG_COLOUR_I && (i) <= BOLD_BG_COLOUR_I)
#define CCL_TRUEC(i) ((i) >= TRUE_COLOUR)

/* Special Characters:
 * UCSWIDE is a special value used in the terminal data to signify
 * the character cell containing the right-hand half of a CJK wide
 * character.
 * SIXELCH is a special character which represents a part of SIXEL graphics.
 */
enum { UCSWIDE = 0,
       SIXELCH = 0xFFFC
};

/* Three character attribute types:
 * The ATTRs (normal attributes) are stored with the characters in
 * the main display arrays
 *
 * The TATTRs (temporary attributes) are generated on the fly, they
 * can overlap with characters but not with normal attributes.
 *
 * The DATTRs (display attributes) are internal to term.c/termline.c
 * (but defined here because their values have to match the others here);
 * they reuse the TATTR_* space but are always masked off
 * before sending to the front end.
 *
 * ATTR_INVALID is an illegal colour combination.
 */
enum {
  ATTR_FGSHIFT    = 0,
  ATTR_BGSHIFT    = 9,
  ATTR_FGMASK     = 0x000001FFu,
  ATTR_BGMASK     = 0x0003FE00u,
  ATTR_INVALID    = 0x0003FFFFu,
  ATTR_BOLD       = 0x00040000u,
  ATTR_DIM        = 0x00080000u,
  ATTR_REVERSE    = 0x00100000u,
  ATTR_UNDER      = 0x00200000u,
  ATTR_BLINK      = 0x00400000u,

  ATTR_ITALIC     = 0x00800000u,
  ATTR_INVISIBLE  = 0x01000000u,
  ATTR_BLINK2     = 0x02000000u,
  ATTR_STRIKEOUT  = 0x04000000u,
  ATTR_DOUBLYUND  = 0x08000000u,
  ATTR_OVERL      = 0x10000000u,
  ATTR_BROKENUND  = 0x0000000800000000u,
  ATTR_ULCOLOUR   = 0x0020000000000000u,

  ATTR_PROTECTED  = 0x20000000u,
  ATTR_WIDE       = 0x40000000u,
  ATTR_NARROW     = 0x80000000u,
  ATTR_EXPAND     = 0x0000000100000000u,
  ATTR_FRAMED     = 0x0010000000000000u,

  TATTR_EMOJI     = 0x1000000000000000u,

  GRAPH_MASK      = 0x0000FF0000000000u,
  ATTR_GRAPH_SHIFT = 40,

  FONTFAM_MASK    = 0x000F000000000000u,
  ATTR_FONTFAM_SHIFT = 48,

  ATTR_CURLYUND   = ATTR_UNDER | ATTR_DOUBLYUND,
  UNDER_MASK      = ATTR_UNDER | ATTR_DOUBLYUND | ATTR_BROKENUND,

  TATTR_COMBINING = 0x0000000200000000u, /* combining characters */
  TATTR_COMBDOUBL = 0x0000000400000000u, /* combining double characters */
  TATTR_ZOOMFULL  = 0x0000001000000000u, /* to be zoomed to full cell size */

  TATTR_RIGHTCURS = 0x0000002000000000u, /* cursor-on-RHS */
  TATTR_PASCURS   = 0x0000004000000000u, /* passive cursor (box) */
  TATTR_ACTCURS   = 0x0000008000000000u, /* active cursor (block) */

  TATTR_RESULT    = 0x0100000000000000u, /* search result */
  TATTR_CURRESULT = 0x0200000000000000u, /* current search result */
  TATTR_MARKED    = 0x0400000000000000u, /* scroll marker */
  TATTR_CURMARKED = 0x0800000000000000u, /* current scroll marker */

  TATTR_SELECTED  = 0x2000000000000000u, /* highlighted */

  DATTR_STARTRUN  = 0x8000000000000000u, /* start of redraw run */
  DATTR_MASK      = TATTR_RIGHTCURS | TATTR_PASCURS | TATTR_ACTCURS
                    | DATTR_STARTRUN
  // unassigned bits:
  //                0x0040000000000000u
  //                0x0080000000000000u
  //                0x4000000000000000u
};

/* Line attributes.
 */
enum {
  LATTR_NORM      = 0x0000u, /* DEC single-width line (DECSWL) */
  LATTR_WIDE      = 0x0001u, /* DEC double-width line (DECDWL) */
  LATTR_TOP       = 0x0002u, /* DEC double-height line (DECDHL), top half */
  LATTR_BOT       = 0x0003u, /* DEC double-height line (DECDHL), bottom half */
  LATTR_MODE      = 0x0003u, /* mask for double-width/height attributes */
  LATTR_WRAPPED   = 0x0010u, /* this line wraps to next */
  LATTR_WRAPPED2  = 0x0020u, /* with WRAPPED: CJK wide character
                                  * wrapped to next line, so last
                                  * single-width cell is empty */
  LATTR_CLEARPAD  = 0x0040u, /* flag to clear padding from overhang */
  LATTR_MARKED    = 0x0100u, /* scroll marker */
  LATTR_UNMARKED  = 0x0200u, /* secondary scroll marker */
  LATTR_NOBIDI    = 0x4000u, /* disable bidi on this line */
  // overlay line display (italic right-to-left overhang handling):
  LATTR_DISP1     = 0x1000u,
  LATTR_DISP2     = 0x2000u,
};

enum {
  ATTR_DEFFG = FG_COLOUR_I << ATTR_FGSHIFT,
  ATTR_DEFBG = BG_COLOUR_I << ATTR_BGSHIFT,
  ATTR_DEFAULT = ATTR_DEFFG | ATTR_DEFBG,
};

typedef unsigned long long cattrflags;

typedef struct {
  cattrflags attr;
  uint truefg;
  uint truebg;
  colour ulcolr;
} cattr;

extern const cattr CATTR_DEFAULT;

typedef struct {
 /*
  * The cc_next field is used to link multiple termchars
  * together into a list, so as to fit more than one character
  * into a character cell (Unicode combining characters).
  *
  * cc_next is a relative offset into the current array of
  * termchars. I.e. to advance to the next character in a list,
  * one does `tc += tc->next'.
  *
  * Zero means end of list.
  */
  short cc_next;

 /*
  * Any code in terminal.c which definitely needs to be changed
  * when extra fields are added here is labelled with a comment
  * saying FULL-TERMCHAR.
  */
  wchar chr;
  cattr attr;
} termchar;

/*const*/ extern termchar basic_erase_char;

typedef struct {
  ushort lattr;
  ushort cols;    /* number of real columns on the line */
  ushort size;    /* number of allocated termchars
                     (cc-lists may make this > cols) */
  bool temporary; /* true if decompressed from scrollback */
  short cc_free;  /* offset to first cc in free list */
  termchar *chars;
} termline;

typedef termline * termlines;

extern termline *newline(int cols, termchar erase_char);
extern void freeline(termline *);
extern void clearline(termline *, termchar erase_char);
extern void resizeline(termline *, int);

extern int sblines(struct term* term);
extern termline *fetch_line(struct term* term, int y);
extern void release_line(termline *);

typedef struct {
  int width;
  termchar *chars;
  int *forward, *backward;      /* the permutations of line positions */
} bidi_cache_entry;

/* Traditional terminal character sets */
typedef enum {
  CSET_ASCII = 'B',   /* Normal ASCII charset */
  CSET_GBCHR = 'A',   /* UK variant */
  CSET_LINEDRW = '0', /* Line drawing charset */
  CSET_TECH = '>',    /* DEC Technical */
  CSET_OEM = 'U',     /* OEM Codepage 437 */
  // definitions for DEC Supplemental support:
  CSET_DECSUPP = '<', // <      DEC Supplementary (VT200)
  CSET_DECSPGR = '%', // % 5    DEC Supplementary Graphics (VT300)
  // definitions for NRC support:
  CSET_NL = '4', // 4           Dutch
  CSET_FI = '5', // C or 5      Finnish
  CSET_FR = 'R', // R or f      French
  CSET_CA = 'Q', // Q or 9      French Canadian (VT200, VT300)
  CSET_DE = 'K', // K           German
  CSET_IT = 'Y', // Y           Italian
  CSET_NO = '`', // ` or E or 6 Norwegian/Danish
  CSET_PT = '6', // % 6         Portuguese (VT300)
  CSET_ES = 'Z', // Z           Spanish
  CSET_SE = '7', // H or 7      Swedish
  CSET_CH = '=', // =           Swiss
} term_cset;

typedef struct {
  int y, x;
} pos;

typedef enum {
  MBT_LEFT = 1, MBT_MIDDLE = 2, MBT_RIGHT = 3, MBT_4 = 4, MBT_5 = 5
} mouse_button;

enum {
  NO_UPDATE = 0,
  PARTIAL_UPDATE = 1,
  FULL_UPDATE = 2,
  DISABLE_UPDATE = 3
};
typedef struct {
  termline ** buf;
  int start;
  int length;
  int capacity;
} circbuf;

typedef struct {
  int x;
  int y;
  int len;
} result;

typedef struct {
  result * results;
  wchar * query;
  uint * xquery;
  int xquery_length;
  int capacity;
  int current;
  int length;
  int update_type;
} termresults;


typedef struct {
  void *fp;
  uint ref_counter;
  uint amount;
} tempfile_t;

typedef struct {
  tempfile_t *tempfile;
  size_t position;
} temp_strage_t;

typedef struct imglist {
  unsigned char *pixels;
  void *hdc;
  void *hbmp;
  temp_strage_t *strage;
  int top;
  int left;
  int width;
  int height;
  int pixelwidth;
  int pixelheight;
  struct imglist *next;
} imglist;

typedef struct {
  void *parser_state;
  imglist *first;
  imglist *last;
  imglist *altfirst;
  imglist *altlast;
} termimgs;

struct mode_entry {
  int mode, val;
};

typedef struct {
  short x, y;
  cattr attr;
  bool origin;
  bool autowrap;  // switchable (xterm Wraparound Mode (DECAWM Auto Wrap))
  bool wrapnext;
  bool rev_wrap;  // switchable (xterm Reverse-wraparound Mode)
  short gl, gr;
  term_cset csets[4];
  term_cset cset_single;
  uchar oem_acs;
  bool utf;
  bool decnrc_enabled;    /* DECNRCM sequence to enable NRC? */
} term_cursor;

struct term {
  bool on_alt_screen;     /* On alternate screen? */
  bool show_other_screen;

  termlines *lines, *other_lines;
  term_cursor curs, saved_cursors[2];

  uchar **scrollback;     /* lines scrolled off top of screen */
  int disptop;            /* distance scrolled back (0 or -ve) */
  int sblen;              /* length of scrollback buffer */
  int sblines;            /* number of lines of scrollback */
  int sbpos;              /* index of next scrollback position to be filled */
  int tempsblines;        /* number of lines of .scrollback that
                           * can be retrieved onto the terminal
                           * ("temporary scrollback") */
  long long int virtuallines;
  long long int altvirtuallines;

  termlines *displines;   /* buffer of text on real screen */

  termchar erase_char;

  char *inbuf;            /* terminal input buffer */
  uint inbuf_size, inbuf_pos;

  bool rvideo;            /* global reverse video flag */
  bool cursor_on;         /* cursor enabled flag */
  bool deccolm_allowed;   /* DECCOLM sequence for 80/132 cols allowed? */
  bool reset_132;         /* Flag ESC c resets to 80 cols */
  bool cblinker;          /* When blinking is the cursor on ? */
  bool tblinker;          /* When the blinking text is on */
  bool tblinker2;         /* When fast blinking is on */
  bool blink_is_real;     /* Actually blink blinking text */
  bool echoing;           /* Does terminal want local echo? */
  bool insert;            /* Insert mode */
  int marg_top, marg_bot; /* scroll margins */
  bool printing, only_printing;  /* Are we doing ANSI printing? */
  int  print_state;       /* state of print-end-sequence scan */
  char *printbuf;         /* buffered data for printer */
  uint printbuf_size, printbuf_pos;

  int  rows, cols;
  bool has_focus;
  bool focus_reported;
  bool in_vbell;

  bool vt220_keys;
  bool shortcut_override;
  bool backspace_sends_bs;
  bool delete_sends_del;
  bool escape_sends_fs;
  bool app_escape_key;
  unsigned int app_control;
  bool app_cursor_keys;
  bool app_keypad;
  bool app_wheel;
  bool bell_taskbar; // xterm: bellIsUrgent; switchable with CSI ? 1042 h/l
  bool bell_popup;   // xterm: popOnBell;    switchable with CSI ? 1043 h/l
  bool wheel_reporting;
  int  modify_other_keys;
  bool newline_mode;
  bool report_focus;
  bool report_font_changed;
  bool report_ambig_width;
  bool bracketed_paste;
  bool show_scrollbar;
  bool wide_indic;
  bool wide_extra;
  bool disable_bidi;
  bool enable_bold_colour;

  bool sixel_display;        // true if sixel scrolling mode is off
  bool sixel_scrolls_right;  // on: sixel scrolling leaves cursor to right of graphic
                             // off(default): the position after sixel depends on sixel_scrolls_left
  bool sixel_scrolls_left;   // on: sixel scrolling moves cursor to beginning of the line
                             // off(default): sixel scrolling moves cursor to left of graphics
  bool private_color_registers;
  int  cursor_type;
  int  cursor_blinks;
  bool cursor_invalid;

  uchar esc_mod;  // Modifier character in escape sequences

  uint csi_argc;
  uint csi_argv[32];
  uint csi_argv_defined[32];

  int  cmd_num;        // OSC command number, or -1 for DCS
  char *cmd_buf;       // OSC or DCS string buffer and length
  uint cmd_buf_cap;
  uint cmd_len;
  int dcs_cmd;

  uchar *tabs;

  enum {
    NORMAL, ESCAPE, CSI_ARGS,
    IGNORE_STRING, CMD_STRING, CMD_ESCAPE,
    OSC_START,
    OSC_NUM,
    OSC_PALETTE,
    DCS_START,
    DCS_PARAM,
    DCS_INTERMEDIATE,
    DCS_PASSTHROUGH,
    DCS_IGNORE,
    DCS_ESCAPE
  } state;

  // Mouse mode
  enum {
    MM_NONE,
    MM_X10,       // just clicks
    MM_VT200,     // click and release
    MM_BTN_EVENT, // click, release, and drag with button down
    MM_ANY_EVENT, // click, release, and any movement
    MM_LOCATOR,   // DEC locator events
  } mouse_mode;

  // Mouse encoding
  enum {
    ME_X10,        // CSI M followed by one byte each for event, X and Y
    ME_UTF8,       // Same as X10, but with UTF-8 encoded X and Y (ugly!)
    ME_URXVT_CSI,  // CSI event ; x ; y M
    ME_XTERM_CSI   // CSI > event ; x ; y M/m
  } mouse_enc;

  enum {
    // The state can be zero, one of the mouse buttons or one of the cases here.
    MS_SEL_CHAR = -1, MS_SEL_WORD = -2, MS_SEL_LINE = -3,
    MS_COPYING = -4, MS_PASTING = -5, MS_OPENING = -6
  } mouse_state;

  bool locator_1_enabled;
  bool locator_by_pixels;
  bool locator_report_up;
  bool locator_report_dn;
  bool locator_rectangle;
  int locator_top, locator_left, locator_bottom, locator_right;

  bool selected, sel_rect;
  pos sel_start, sel_end, sel_anchor;
  bool hovering;
  pos hover_start, hover_end;

 /* Scroll steps during selection when cursor out of window. */
  int sel_scroll;
  pos sel_pos;

  wchar *paste_buffer;
  int paste_len, paste_pos;

 /* True when we've seen part of a multibyte input char */
  bool in_mb_char;

 /* Non-zero when we've seen the first half of a surrogate pair */
  wchar high_surrogate;

 /*
  * These are buffers used by the bidi and Arabic shaping code.
  */
  termchar *ltemp;
  int ltemp_size;
  bidi_char *wcFrom, *wcTo;
  int wcFromTo_size;
  bidi_cache_entry *pre_bidi_cache, *post_bidi_cache;
  int bidi_cache_size;

  // Search results
  termresults results;

  termimgs imgs;

  bool search_window_visible;

  int markpos;
  bool markpos_valid;

  struct child* child;

  struct mode_entry * mode_stack;
  int mode_stack_len;
};

extern void term_resize(struct term* term, int, int);
extern void term_scroll(struct term* term, int, int);
extern void term_reset(struct term* term, bool full);
extern void term_free(struct term* term);
extern void term_clear_scrollback(struct term* term);
extern void term_mouse_click(struct term* term, mouse_button, mod_keys, pos, int count);
extern void term_mouse_release(struct term* term, mouse_button, mod_keys, pos);
extern void term_mouse_move(struct term* term, mod_keys, pos);
extern void term_mouse_wheel(struct term* term, int delta, int lines_per_notch, mod_keys, pos);
extern void term_select_all(struct term* term);
extern void term_paint(struct term* term);
extern void term_invalidate(struct term* term, int left, int top, int right, int bottom);
extern void term_open(struct term* term);
extern void term_copy(struct term* term);
extern void term_paste(struct term* term, wchar *, uint len);
extern void term_send_paste(struct term* term);
extern void term_cancel_paste(struct term* term);
extern void term_cmd(struct term* term, char * cmdpat);
extern void term_reconfig(struct term* term);
extern void term_flip_screen(struct term* term);
extern void term_reset_screen(struct term* term);
extern void term_write(struct term* term, const char *, uint len);
extern void term_flush(struct term* term);
extern void term_set_focus(struct term* term, bool has_focus, bool may_report);
extern int  term_cursor_type(struct term* term);
extern bool term_cursor_blinks(struct term* term);
extern void term_hide_cursor(struct term* term);

extern void term_set_search(struct term* term, wchar * needle);
extern void term_schedule_search_partial_update(struct term* term);
extern void term_schedule_search_update(struct term* term);
extern void term_update_search(struct term* term);
extern void term_clear_results(struct term* term);
extern void term_clear_search(struct term* term);

extern char * get_emoji_description(termchar *);

#endif
