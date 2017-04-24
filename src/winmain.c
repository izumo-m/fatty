// winmain.c (part of FaTTY)
// Copyright 2015 Juho Peltonen
// Based on code from mintty by Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#define dont_debuglog
#ifdef debuglog
  FILE * mtlog = 0;
#endif

#define dont_debug_resize

#include "term.h"
#include "winpriv.h"
#include "winsearch.h"
#include "winimg.h"

#include "term.h"
#include "appinfo.h"
#include "child.h"
#include "charset.h"

#include <CommCtrl.h>
#include <Windows.h>
#include <locale.h>
#include <getopt.h>
#include <pwd.h>

#include <mmsystem.h>  // PlaySound for MSys
#include <shellapi.h>

#include <sys/cygwin.h>

#if CYGWIN_VERSION_DLL_MAJOR >= 1007
#include <propsys.h>
#include <propkey.h>
#endif


bool icon_is_from_shortcut = false;

HINSTANCE inst;
HWND wnd, tab_wnd;
HIMC imc;

static char **main_argv;
static int main_argc;
static ATOM class_atom;
static bool invoked_from_shortcut = false;
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
static bool invoked_with_appid = false;
#endif


//filled by win_adjust_borders:
static LONG window_style;
static int term_width, term_height;
static int width, height;
static int extra_width, extra_height, norm_extra_width, norm_extra_height;

// State
bool win_is_fullscreen;
static bool go_fullscr_on_max;
static bool resizing;
static int zoom_token = 0;  // for heuristic handling of Shift zoom (#467, #476)
static bool default_size_token = false;

// Options
static string border_style = 0;
static int monitor = 0;
static bool center = false;
static bool right = false;
static bool bottom = false;
static bool left = false;
static bool top = false;
static bool maxwidth = false;
static bool maxheight = false;
static bool store_taskbar_properties = false;
static bool prevent_pinning = false;
bool disable_bidi = false;
bool support_wsl = false;


static HBITMAP caretbm;

static int term_initialized;

#if WINVER < 0x600

typedef struct {
  int cxLeftWidth;
  int cxRightWidth;
  int cyTopHeight;
  int cyBottomHeight;
} MARGINS;

#else

#include <uxtheme.h>

#endif

static HRESULT (WINAPI * pDwmIsCompositionEnabled)(BOOL *) = 0;
static HRESULT (WINAPI * pDwmExtendFrameIntoClientArea)(HWND, const MARGINS *) = 0;
static HRESULT (WINAPI * pDwmEnableBlurBehindWindow)(HWND, void *) = 0;

// Helper for loading a system library. Using LoadLibrary() directly is insecure
// because Windows might be searching the current working directory first.
static HMODULE
load_sys_library(string name)
{
  char path[MAX_PATH];
  uint len = GetSystemDirectory(path, MAX_PATH);
  if (len && len + strlen(name) + 1 < MAX_PATH) {
    path[len] = '\\';
    strcpy(&path[len + 1], name);
    return LoadLibrary(path);
  }
  else
    return 0;
}

static void
load_dwm_funcs(void)
{
  HMODULE dwm = load_sys_library("dwmapi.dll");
  if (dwm) {
    pDwmIsCompositionEnabled =
      (void *)GetProcAddress(dwm, "DwmIsCompositionEnabled");
    pDwmExtendFrameIntoClientArea =
      (void *)GetProcAddress(dwm, "DwmExtendFrameIntoClientArea");
    pDwmEnableBlurBehindWindow =
      (void *)GetProcAddress(dwm, "DwmEnableBlurBehindWindow");
  }
}

#define dont_debug_dpi

bool per_monitor_dpi_aware = false;
uint dpi = 96;

#define WM_DPICHANGED 0x02E0
const int Process_System_DPI_Aware = 1;
const int Process_Per_Monitor_DPI_Aware = 2;
static HRESULT (WINAPI * pGetProcessDpiAwareness)(HANDLE hprocess, int * value) = 0;
static HRESULT (WINAPI * pSetProcessDpiAwareness)(int value) = 0;
static HRESULT (WINAPI * pGetDpiForMonitor)(HMONITOR mon, int type, uint * x, uint * y) = 0;

DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#define DPI_AWARENESS_CONTEXT_UNAWARE           ((DPI_AWARENESS_CONTEXT)-1)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE      ((DPI_AWARENESS_CONTEXT)-2)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)
static DPI_AWARENESS_CONTEXT (WINAPI * pSetThreadDpiAwarenessContext)(DPI_AWARENESS_CONTEXT dpic) = 0;
static HRESULT (WINAPI * pEnableNonClientDpiScaling)(HWND win) = 0;
static BOOL (WINAPI * pAdjustWindowRectExForDpi)(LPRECT lpRect, DWORD dwStyle, BOOL bMenu, DWORD dwExStyle, UINT dpi) = 0;
static INT (WINAPI * pGetSystemMetricsForDpi)(INT index, UINT dpi) = 0;

static void
load_dpi_funcs(void)
{
  HMODULE shc = load_sys_library("shcore.dll");
  HMODULE user = load_sys_library("user32.dll");
#ifdef debug_dpi
  printf("load_dpi_funcs shcore %d user32 %d\n", !!shc, !!user);
#endif
  if (shc) {
    pGetProcessDpiAwareness =
      (void *)GetProcAddress(shc, "GetProcessDpiAwareness");
    pSetProcessDpiAwareness =
      (void *)GetProcAddress(shc, "SetProcessDpiAwareness");
    pGetDpiForMonitor =
      (void *)GetProcAddress(shc, "GetDpiForMonitor");
  }
  if (user) {
    pSetThreadDpiAwarenessContext =
      (void *)GetProcAddress(user, "SetThreadDpiAwarenessContext");
    pEnableNonClientDpiScaling =
      (void *)GetProcAddress(user, "EnableNonClientDpiScaling");
    pAdjustWindowRectExForDpi =
      (void *)GetProcAddress(user, "AdjustWindowRectExForDpi");
    pGetSystemMetricsForDpi =
      (void *)GetProcAddress(user, "GetSystemMetricsForDpi");
  }
#ifdef debug_dpi
  printf("SetProcessDpiAwareness %d GetProcessDpiAwareness %d GetDpiForMonitor %d SetThreadDpiAwarenessContext %d EnableNonClientDpiScaling %d AdjustWindowRectExForDpi %d GetSystemMetricsForDpi %d\n", !!pSetProcessDpiAwareness, !!pGetProcessDpiAwareness, !!pGetDpiForMonitor, !!pSetThreadDpiAwarenessContext, !!pEnableNonClientDpiScaling, !!pAdjustWindowRectExForDpi, !!pGetSystemMetricsForDpi);
#endif
}

void
set_dpi_auto_scaling(bool on)
{
  (void)on;
#if 0
 /* this was an attempt to get the Options menu to scale with DPI by
    disabling DPI awareness while constructing the menu in win_open_config;
    but then (if DPI zooming > 100% in Windows 10)
    any font change would resize the terminal by the zoom factor;
    also in a later Windows 10 update, it works without this
 */
#warning failed DPI tweak
  if (pSetThreadDpiAwarenessContext) {
    if (on)
      pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);
    else
      pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
  }
#endif
}

static bool
set_per_monitor_dpi_aware(void)
{
#if 0
 /* this was added under the assumption it might be needed 
    for EnableNonClientDpiScaling to work (as described) 
    but it's not needed, so we'll leave it
 */
  if (pSetThreadDpiAwarenessContext) {
    if (pSetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
      return true;
  }
#endif
  if (pSetProcessDpiAwareness && pGetProcessDpiAwareness) {
    HRESULT hr = pSetProcessDpiAwareness(Process_Per_Monitor_DPI_Aware);
    // E_ACCESSDENIED:
    // The DPI awareness is already set, either by calling this API previously
    // or through the application (.exe) manifest.
    if (hr != E_ACCESSDENIED && !SUCCEEDED(hr))
      pSetProcessDpiAwareness(Process_System_DPI_Aware);

    int awareness = 0;
    return SUCCEEDED(pGetProcessDpiAwareness(NULL, &awareness)) &&
      awareness == Process_Per_Monitor_DPI_Aware;
  }
  return false;
}

void
win_set_title(struct term* term, char *title)
{
  wchar wtitle[strlen(title) + 1];
  if (cs_mbstowcs(wtitle, title, lengthof(wtitle)) >= 0) {
    if (term == win_active_terminal() && cfg.title_settable)
      SetWindowTextW(wnd, wtitle);
    win_tab_set_title(term, wtitle);
  }
}

void
win_copy_title(void)
{
  int len = GetWindowTextLengthW(wnd);
  wchar title[len + 1];
  len = GetWindowTextW(wnd, title, len + 1);
  win_copy(title, 0, len + 1);
}

void win_copy_text(const char *s)
{
  unsigned int size;
  wchar *text = cs__mbstowcs(s);

  if (text == NULL) {
    return;
  }
  size = wcslen(text);
  if (size > 0) {
    win_copy(text, 0, size + 1);
  }
  free(text);
}

/*
 * Title stack (implemented as fixed-size circular buffer)
 */
void
win_save_title(void)
{
  win_active_tab_title_push();
}

void
win_restore_title(void)
{
  SetWindowTextW(wnd, win_active_tab_title_pop());
}

/*
 *  Switch to next or previous application window in z-order
 */

static HWND first_wnd, last_wnd;

static BOOL CALLBACK
wnd_enum_proc(HWND curr_wnd, LPARAM unused(lp)) {
  if (curr_wnd != wnd && !IsIconic(curr_wnd)) {
    WINDOWINFO curr_wnd_info;
    curr_wnd_info.cbSize = sizeof(WINDOWINFO);
    GetWindowInfo(curr_wnd, &curr_wnd_info);
    if (class_atom == curr_wnd_info.atomWindowType) {
      first_wnd = first_wnd ?: curr_wnd;
      last_wnd = curr_wnd;
    }
  }
  return true;
}

void
win_switch(bool back, bool alternate)
{
  first_wnd = 0, last_wnd = 0;
  EnumWindows(wnd_enum_proc, 0);
  if (first_wnd) {
    if (back)
      first_wnd = last_wnd;
    else
      SetWindowPos(wnd, last_wnd, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE
                       | (alternate ? SWP_NOZORDER : SWP_NOREPOSITION));
    BringWindowToTop(first_wnd);
  }
}

static void
get_my_monitor_info(MONITORINFO *mip)
{
  HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
  mip->cbSize = sizeof(MONITORINFO);
  GetMonitorInfo(mon, mip);
}

static void
get_monitor_info(int moni, MONITORINFO *mip)
{
  mip->cbSize = sizeof(MONITORINFO);

  BOOL CALLBACK
  monitor_enum (HMONITOR hMonitor, HDC hdcMonitor, LPRECT monp, LPARAM dwData)
  {
    (void)hdcMonitor, (void)monp, (void)dwData;

    GetMonitorInfo(hMonitor, mip);

    return --moni > 0;
  }

  EnumDisplayMonitors(0, 0, monitor_enum, 0);
}

#define dont_debug_display_monitors_mockup
#define dont_debug_display_monitors

#ifdef debug_display_monitors_mockup
# define debug_display_monitors
static const RECT monitors[] = {
  //(RECT){.left = 0, .top = 0, .right = 1920, .bottom = 1200},
    //    44
    // 3  11  2
    //     5   6
  {0, 0, 1920, 1200},
  {1920, 0, 3000, 1080},
  {-800, 200, 0, 600},
  {0, -1080, 1920, 0},
  {1300, 1200, 2100, 1800},
  {2100, 1320, 2740, 1800},
};
static long primary_monitor = 2 - 1;
static long current_monitor = 1 - 1;  // assumption for MonitorFromWindow
#endif

/*
   search_monitors(&x, &y, 0, false, &moninfo)
     returns number of monitors;
       stores smallest width/height of all monitors
       stores info of current monitor
   search_monitors(&x, &y, 0, true, &moninfo)
     returns number of monitors;
       stores smallest width/height of all monitors
       stores info of primary monitor
   search_monitors(&x, &y, mon, false/true, 0)
     returns index of given monitor (0/primary if not found)
   search_monitors(&x, &y, 0, false/true, 0)
     prints information about all monitors
 */
int
search_monitors(int * minx, int * miny, HMONITOR lookup_mon, bool get_primary, MONITORINFO *mip)
{
#ifdef debug_display_monitors_mockup
  BOOL
  EnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum, LPARAM dwData)
  {
    (void)lprcClip;
    for (unsigned long moni = 0; moni < lengthof(monitors); moni++) {
      RECT monrect = monitors[moni];
      HMONITOR hMonitor = (HMONITOR)(moni + 1);
      HDC hdcMonitor = hdc;
      //if (hdc) hdcMonitor = (HDC)...;
      //if (hdc) monrect = intersect(hdc.rect, monrect);
      //if (hdc) hdcMonitor.rect = intersection(hdc.rect, lprcClip, monrect);
      if (lpfnEnum(hMonitor, hdcMonitor, &monrect, dwData) == FALSE)
        return TRUE;
    }
    return TRUE;
  }

  BOOL GetMonitorInfo(HMONITOR hMonitor, LPMONITORINFO lpmi)
  {
    long moni = (long)hMonitor - 1;
    lpmi->rcMonitor = monitors[moni];
    lpmi->rcWork = monitors[moni];
    lpmi->dwFlags = 0;
    if (moni == primary_monitor)
      lpmi->dwFlags = MONITORINFOF_PRIMARY;
    return TRUE;
  }

  HMONITOR MonitorFromWindow(HWND hwnd, DWORD dwFlags)
  {
    (void)hwnd, (void)dwFlags;
    return (HMONITOR)current_monitor + 1;
  }
#endif

  int moni = 0;
  int moni_found = 0;
  * minx = 0;
  * miny = 0;
  HMONITOR refmon = 0;
  HMONITOR curmon = lookup_mon ? 0 : MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
  bool print_monitors = !lookup_mon && !mip;
#ifdef debug_display_monitors
  print_monitors = !lookup_mon;
#endif

  BOOL CALLBACK
  monitor_enum(HMONITOR hMonitor, HDC hdcMonitor, LPRECT monp, LPARAM dwData)
  {
    (void)hdcMonitor, (void)monp, (void)dwData;

    moni ++;
    if (hMonitor == lookup_mon) {
      // looking for index of specific monitor
      moni_found = moni;
      return FALSE;
    }

    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfo(hMonitor, &mi);

    if (get_primary && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
      moni_found = moni;  // fallback to be overridden by monitor found later
      refmon = hMonitor;
    }

    // determining smallest monitor width and height
    RECT fr = mi.rcMonitor;
    if (*minx == 0 || *minx > fr.right - fr.left)
      *minx = fr.right - fr.left;
    if (*miny == 0 || *miny > fr.bottom - fr.top)
      *miny = fr.bottom - fr.top;

    if (print_monitors) {
      printf("Monitor %d %s %s width,height %4d,%4d (%4d,%4d...%4d,%4d)\n", 
             moni,
             hMonitor == curmon ? "current" : "       ",
             mi.dwFlags & MONITORINFOF_PRIMARY ? "primary" : "       ",
             (int)(fr.right - fr.left), (int)(fr.bottom - fr.top),
             (int)fr.left, (int)fr.top, (int)fr.right, (int)fr.bottom);
    }

    return TRUE;
  }

  EnumDisplayMonitors(0, 0, monitor_enum, 0);
  if (lookup_mon) {
    return moni_found;
  }
  else if (mip) {
    if (!refmon)  // not detected primary monitor as requested?
      // determine current monitor
      refmon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    mip->cbSize = sizeof(MONITORINFO);
    GetMonitorInfo(refmon, mip);
    return moni;  // number of monitors
  }
  else
    return moni;  // number of monitors printed
}

/*
 * Minimise or restore the window in response to a server-side request.
 */
void
win_set_iconic(bool iconic)
{
  if (iconic ^ IsIconic(wnd))
    ShowWindow(wnd, iconic ? SW_MINIMIZE : SW_RESTORE);
}

/*
 * Move the window in response to a server-side request.
 */
void
win_set_pos(int x, int y)
{
  trace_resize(("--- win_set_pos %d %d\n", x, y));
  if (!IsZoomed(wnd))
    SetWindowPos(wnd, null, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
void
win_set_zorder(bool top)
{
  SetWindowPos(wnd, top ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE);
}

bool
win_is_iconic(void)
{
  return IsIconic(wnd);
}

void
win_get_pos(int *xp, int *yp)
{
  RECT r;
  GetWindowRect(wnd, &r);
  *xp = r.left;
  *yp = r.top;
}

void
win_get_pixels(int *height_p, int *width_p)
{
  RECT r;
  GetWindowRect(wnd, &r);
  // report inner pixel size, without padding, like xterm:
  int sy = win_search_visible() ? SEARCHBAR_HEIGHT : 0;
  *height_p = r.bottom - r.top - extra_height - 2 * PADDING - sy;
  *width_p = r.right - r.left - extra_width - 2 * PADDING;
}

void
win_get_screen_chars(int *rows_p, int *cols_p)
{
  MONITORINFO mi;
  get_my_monitor_info(&mi);
  RECT fr = mi.rcMonitor;
  *rows_p = (fr.bottom - fr.top - 2 * PADDING) / cell_height;
  *cols_p = (fr.right - fr.left - 2 * PADDING) / cell_width;
}

void
win_set_pixels(int height, int width)
{
  trace_resize(("--- win_set_pixels %d %d\n", height, width));
  int sy = win_search_visible() ? SEARCHBAR_HEIGHT : 0;
  SetWindowPos(wnd, null, 0, 0,
               width + extra_width + 2 * PADDING,
               height + extra_height + 2 * PADDING + sy,
               SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOZORDER);
}

bool
win_is_glass_available(void)
{
  BOOL result = false;
  if (pDwmIsCompositionEnabled)
    pDwmIsCompositionEnabled(&result);
  return result;
}

static void
update_blur(void)
{
// This feature is disabled in config.c as it does not seem to work,
// see https://github.com/mintty/mintty/issues/501
  if (pDwmEnableBlurBehindWindow) {
    bool blur =
      cfg.transparency && cfg.blurred && !win_is_fullscreen &&
      !(cfg.opaque_when_focused && win_active_terminal()->has_focus);
#define dont_use_dwmapi_h
#ifdef use_dwmapi_h
#warning dwmapi_include_shown_for_documentation
#include <dwmapi.h>
    DWM_BLURBEHIND bb;
#else
    struct {
      DWORD dwFlags;
      BOOL  fEnable;
      HRGN  hRgnBlur;
      BOOL  fTransitionOnMaximized;
    } bb;
#define DWM_BB_ENABLE 1
#endif
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = blur;
    bb.hRgnBlur = NULL;
    bb.fTransitionOnMaximized = FALSE;

    pDwmEnableBlurBehindWindow(wnd, &bb);
  }
}

static void
update_glass(void)
{
  if (pDwmExtendFrameIntoClientArea) {
    bool enabled =
      cfg.transparency == TR_GLASS && !win_is_fullscreen &&
      !(cfg.opaque_when_focused && win_active_terminal()->has_focus);
    pDwmExtendFrameIntoClientArea(wnd, &(MARGINS){enabled ? -1 : 0, 0, 0, 0});
  }
}

/*
 * Go full-screen. This should only be called when we are already maximised.
 */
static void
make_fullscreen(void)
{
  win_is_fullscreen = true;

 /* Remove the window furniture. */
  LONG style = GetWindowLong(wnd, GWL_STYLE);
  style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
  SetWindowLong(wnd, GWL_STYLE, style);

 /* The glass effect doesn't work for fullscreen windows */
  update_glass();

 /* Resize ourselves to exactly cover the nearest monitor. */
  MONITORINFO mi;
  get_my_monitor_info(&mi);
  RECT fr = mi.rcMonitor;
  SetWindowPos(wnd, HWND_TOP, fr.left, fr.top,
               fr.right - fr.left, fr.bottom - fr.top, SWP_FRAMECHANGED);
}

/*
 * Clear the full-screen attributes.
 */
static void
clear_fullscreen(void)
{
  win_is_fullscreen = false;
  update_glass();

 /* Reinstate the window furniture. */
  LONG style = GetWindowLong(wnd, GWL_STYLE);
  if (border_style) {
    if (strcmp(border_style, "void") != 0) {
      style |= WS_THICKFRAME;
    }
  }
  else {
    style |= WS_CAPTION | WS_BORDER | WS_THICKFRAME;
  }
  SetWindowLong(wnd, GWL_STYLE, style);
  SetWindowPos(wnd, null, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void
win_set_geom(int y, int x, int height, int width)
{
  trace_resize(("--- win_set_geom %d %d %d %d\n", y, x, height, width));

  if (win_is_fullscreen)
    clear_fullscreen();

  MONITORINFO mi;
  get_my_monitor_info(&mi);
  RECT ar = mi.rcWork;

  int scr_height = ar.bottom - ar.top, scr_width = ar.right - ar.left;
  int term_height, term_width;
  int term_x, term_y;
  win_get_pixels(&term_height, &term_width);
  win_get_pos(&term_x, &term_y);

  if (x >= 0)
    term_x = x;
  if (y >= 0)
    term_y = y;
  if (width == 0)
    term_width = scr_width;
  else if (width > 0)
    term_width = width;
  if (height == 0)
    term_height = scr_height;
  else if (height > 0)
    term_height = height;

  SetWindowPos(wnd, null, term_x, term_y,
               term_width + 2 * PADDING, term_height + 2 * PADDING,
               SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOZORDER);
}

static void
win_fix_position(void)
{
  RECT wr;
  GetWindowRect(wnd, &wr);
  MONITORINFO mi;
  get_my_monitor_info(&mi);
  RECT ar = mi.rcWork;

  // Correct edges. Top and left win if the window is too big.
  wr.left -= max(0, wr.right - ar.right);
  wr.top -= max(0, wr.bottom - ar.bottom);
  wr.left = max(wr.left, ar.left);
  wr.top = max(wr.top, ar.top);

  SetWindowPos(wnd, 0, wr.left, wr.top, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void
win_set_chars(int rows, int cols)
{
  trace_resize(("--- win_set_chars %d×%d\n", rows, cols));
  win_set_pixels(rows * cell_height + win_tab_height(), cols * cell_width);
  win_fix_position();
}


// Clockwork
int get_tick_count(void) { return GetTickCount(); }
int cursor_blink_ticks(void) { return GetCaretBlinkTime(); }

static void
flash_taskbar(bool enable)
{
  static bool enabled;
  if (enable != enabled) {
    FlashWindowEx(&(FLASHWINFO){
      .cbSize = sizeof(FLASHWINFO),
      .hwnd = wnd,
      .dwFlags = enable ? FLASHW_TRAY | FLASHW_TIMER : FLASHW_STOP,
      .uCount = 1,
      .dwTimeout = 0
    });
    enabled = enable;
  }
}

/*
 * Bell.
 */
void
win_bell(struct term* term, config * conf)
{
  if (conf->bell_sound || conf->bell_type) {
    wchar * bell_name = (wchar *)conf->bell_file;
    bool free_bell_name = false;
    if (*bell_name) {
      if (wcschr(bell_name, L'/') || wcschr(bell_name, L'\\')) {
        if (bell_name[1] != ':') {
          char * bf = path_win_w_to_posix(bell_name);
          bell_name = path_posix_to_win_w(bf);
          free(bf);
          free_bell_name = true;
        }
      }
      else {
        wchar * bell_file = bell_name;
        char * bf;
        if (!wcschr(bell_name, '.')) {
          int len = wcslen(bell_name);
          bell_file = newn(wchar, len + 5);
          wcscpy(bell_file, bell_name);
          wcscpy(&bell_file[len], W(".wav"));
          bf = get_resource_file(W("sounds"), bell_file, false);
          free(bell_file);
        }
        else
          bf = get_resource_file(W("sounds"), bell_name, false);
        if (bf) {
          bell_name = path_posix_to_win_w(bf);
          free(bf);
          free_bell_name = true;
        }
        else
          bell_name = null;
      }
    }

    if (bell_name && *bell_name && PlaySoundW(bell_name, NULL, SND_ASYNC | SND_FILENAME)) {
      // played
    }
    else if (conf->bell_freq)
      Beep(conf->bell_freq, conf->bell_len);
    else if (conf->bell_type > 0) {
      //  1 -> 0x00000000 MB_OK              Default Beep
      //  2 -> 0x00000010 MB_ICONSTOP        Critical Stop
      //  3 -> 0x00000020 MB_ICONQUESTION    Question
      //  4 -> 0x00000030 MB_ICONEXCLAMATION Exclamation
      //  5 -> 0x00000040 MB_ICONASTERISK    Asterisk
      // -1 -> 0xFFFFFFFF                    Simple Beep
      MessageBeep((conf->bell_type - 1) * 16);
    } else if (conf->bell_type < 0)
      MessageBeep(0xFFFFFFFF);

    if (free_bell_name)
      free(bell_name);
  }

  if (conf->bell_taskbar && !win_active_terminal()->has_focus)
    flash_taskbar(true);
  if (!term->has_focus)
    win_tab_attention(term);
}

void
win_invalidate_all(void)
{
  InvalidateRect(wnd, null, true);
  win_for_each_term(term_paint);
}


#ifdef debug_dpi
static void
print_system_metrics(int dpi, string tag)
{
# ifndef SM_CXPADDEDBORDER
# define SM_CXPADDEDBORDER 92
# endif
  printf("metrics /%d [%s]\n"
         "        border %d/%d %d/%d edge %d/%d %d/%d\n"
         "        frame  %d/%d %d/%d size %d/%d %d/%d\n"
         "        padded %d/%d\n"
         "        caption %d/%d\n"
         "        scrollbar %d/%d\n",
         dpi, tag,
         GetSystemMetrics(SM_CXBORDER), pGetSystemMetricsForDpi(SM_CXBORDER, dpi),
         GetSystemMetrics(SM_CYBORDER), pGetSystemMetricsForDpi(SM_CYBORDER, dpi),
         GetSystemMetrics(SM_CXEDGE), pGetSystemMetricsForDpi(SM_CXEDGE, dpi),
         GetSystemMetrics(SM_CYEDGE), pGetSystemMetricsForDpi(SM_CYEDGE, dpi),
         GetSystemMetrics(SM_CXFIXEDFRAME), pGetSystemMetricsForDpi(SM_CXFIXEDFRAME, dpi),
         GetSystemMetrics(SM_CYFIXEDFRAME), pGetSystemMetricsForDpi(SM_CYFIXEDFRAME, dpi),
         GetSystemMetrics(SM_CXSIZEFRAME), pGetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi),
         GetSystemMetrics(SM_CYSIZEFRAME), pGetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi),
         GetSystemMetrics(SM_CXPADDEDBORDER), pGetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi),
         GetSystemMetrics(SM_CYCAPTION), pGetSystemMetricsForDpi(SM_CYCAPTION, dpi),
         GetSystemMetrics(SM_CXVSCROLL), pGetSystemMetricsForDpi(SM_CXVSCROLL, dpi)
         );
}
#endif

static void
win_adjust_borders(int t_width, int t_height)
{
  term_width = t_width;
  term_height = t_height;
 
  RECT cr = {0, 0, term_width + 2 * PADDING, term_height + 2 * PADDING + win_tab_height()};
  RECT wr = cr;
  window_style = WS_OVERLAPPEDWINDOW;
  if (border_style) {
    if (strcmp(border_style, "void") == 0)
      window_style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    else
      window_style &= ~(WS_CAPTION | WS_BORDER);
  }

  if (pGetDpiForMonitor && pAdjustWindowRectExForDpi) {
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    uint x, dpi;
    pGetDpiForMonitor(mon, 0, &x, &dpi);  // MDT_EFFECTIVE_DPI
    pAdjustWindowRectExForDpi(&wr, window_style, false, 0, dpi);
#ifdef debug_dpi
    RECT wr0 = cr;
    AdjustWindowRect(&wr0, window_style, false);
    printf("adjust borders dpi %3d: %d %d\n", dpi, (int)(wr.right - wr.left), (int)(wr.bottom - wr.top));
    printf("                      : %d %d\n", (int)(wr0.right - wr0.left), (int)(wr0.bottom - wr0.top));
    print_system_metrics(dpi, "win_adjust_borders");
#endif
  }
  else
    AdjustWindowRect(&wr, window_style, false);

  width = wr.right - wr.left;
  height = wr.bottom - wr.top;

  if (cfg.scrollbar)
    width += GetSystemMetrics(SM_CXVSCROLL);

  extra_width = width - (cr.right - cr.left);
  extra_height = height - (cr.bottom - cr.top);
  norm_extra_width = extra_width;
  norm_extra_height = extra_height;
}

void
win_adapt_term_size(bool sync_size_with_font, bool scale_font_with_size)
{
  trace_resize(("--- win_adapt_term_size sync_size %d scale_font %d (full %d Zoomed %d)\n", sync_size_with_font, scale_font_with_size, win_is_fullscreen, IsZoomed(wnd)));
  if (IsIconic(wnd))
    return;

#ifdef debug_dpi
  HDC dc = GetDC(wnd);
  printf("monitor size %dmm*%dmm res %d*%d dpi/dev %d",
         GetDeviceCaps(dc, HORZSIZE), GetDeviceCaps(dc, VERTSIZE), 
         GetDeviceCaps(dc, HORZRES), GetDeviceCaps(dc, VERTRES),
         GetDeviceCaps(dc, LOGPIXELSY));
  //googled this:
  //int physical_width = GetDeviceCaps(dc, DESKTOPHORZRES);
  //int virtual_width = GetDeviceCaps(dc, HORZRES);
  //int dpi = (int)(96f * physical_width / virtual_width);
  //but as observed here, physical_width and virtual_width are always equal
  ReleaseDC(wnd, dc);
  if (pGetDpiForMonitor) {
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    uint x, y;
    pGetDpiForMonitor(mon, 0, &x, &y);  // MDT_EFFECTIVE_DPI
    // we might think about scaling the font size by this factor,
    // but this is handled elsewhere; (used to be via WM_DPICHANGED, 
    // now via WM_WINDOWPOSCHANGED and initially)
    printf(" eff %d", y);
  }
  printf("\n");
#endif

  struct term* term = win_active_terminal();
  if (sync_size_with_font && !win_is_fullscreen) {
    win_set_chars(term->rows, term->cols);
    //win_fix_position();
    win_invalidate_all();
    return;
  }

 /* Current window sizes ... */
  RECT cr, wr;
  GetClientRect(wnd, &cr);
  GetWindowRect(wnd, &wr);
  int client_width = cr.right - cr.left;
  int client_height = cr.bottom - cr.top;
  extra_width = wr.right - wr.left - client_width;
  extra_height = wr.bottom - wr.top - client_height;
  if (!win_is_fullscreen) {
    norm_extra_width = extra_width;
    norm_extra_height = extra_height;
  }
  int term_width = client_width - 2 * PADDING;
  int term_height = client_height - 2 * PADDING - g_render_tab_height;

  if (!sync_size_with_font && win_search_visible()) {
    term_height -= SEARCHBAR_HEIGHT;
  }

  if (scale_font_with_size && term->cols != 0 && term->rows != 0) {
    // calc preliminary size (without font scaling), as below
    // should use term_height rather than rows; calc and store in term_resize
    int cols0 = max(1, term_width / cell_width);
    int rows0 = max(1, term_height / cell_height);

    // rows0/term.rows gives a rough scaling factor for cell_height
    // cols0/term.cols gives a rough scaling factor for cell_width
    // cell_height, cell_width give a rough scaling indication for font_size
    // height or width could be considered more according to preference
    bool bigger = rows0 * cols0 > term->rows * term->cols;
    int font_size1 =
      // heuristic best approach taken...
      // bigger
      //   ? max(font_size * rows0 / term.rows, font_size * cols0 / term.cols)
      //   : min(font_size * rows0 / term.rows, font_size * cols0 / term.cols);
      // bigger
      //   ? font_size * rows0 / term.rows + 2
      //   : font_size * rows0 / term.rows;
      bigger
        ? (font_size * rows0 / term->rows + font_size * cols0 / term->cols) / 2 + 1
        : (font_size * rows0 / term->rows + font_size * cols0 / term->cols) / 2;
      // bigger
      //   ? font_size * rows0 * cols0 / (term.rows * term.cols)
      //   : font_size * rows0 * cols0 / (term.rows * term.cols);
      trace_resize(("term size %d %d -> %d %d\n", term->rows, term->cols, rows0, cols0));
      trace_resize(("font size %d -> %d\n", font_size, font_size1));

    // heuristic attempt to stabilize font size roundtrips, esp. after fullscreen
    if (!bigger) font_size1 = font_size1 * 20 / 19;

    if (font_size1 != font_size)
      win_set_font_size(font_size1, false);
  }

  int cols = max(1, term_width / cell_width);
  int rows = max(1, term_height / cell_height);
  if (rows != term->rows || cols != term->cols) {
    term_resize(term, rows, cols);
    struct winsize ws = {rows, cols, cols * cell_width, rows * cell_height};
    child_resize(term->child, &ws);
  }
  win_invalidate_all();

  win_update_search();
  term_schedule_search_update(term);
  win_schedule_update();
}

/*
 * Maximise or restore the window in response to a server-side request.
 * Argument value of 2 means go fullscreen.
 */
void
win_maximise(int max)
{
  if (max == -2) // toggle full screen
    max = win_is_fullscreen ? 0 : 2;
  if (IsZoomed(wnd)) {
    if (!max)
      ShowWindow(wnd, SW_RESTORE);
    else if (max == 2 && !win_is_fullscreen)
      make_fullscreen();
  }
  else if (max) {
    if (max == 2)
      go_fullscr_on_max = true;
    ShowWindow(wnd, SW_MAXIMIZE);
  }
}

/*
 * Go back to configured window size.
 */
static void
default_size(void)
{
  if (IsZoomed(wnd))
    ShowWindow(wnd, SW_RESTORE);
  win_set_chars(cfg.rows, cfg.cols);
}

static void
update_transparency(void)
{
  int trans = cfg.transparency;
  if (trans == TR_GLASS)
    trans = 0;
  LONG style = GetWindowLong(wnd, GWL_EXSTYLE);
  style = trans ? style | WS_EX_LAYERED : style & ~WS_EX_LAYERED;
  SetWindowLong(wnd, GWL_EXSTYLE, style);
  if (trans) {
    if (cfg.opaque_when_focused && win_active_terminal()->has_focus)
      trans = 0;
    SetLayeredWindowAttributes(wnd, 0, 255 - (uchar)trans, LWA_ALPHA);
  }

  update_blur();
  update_glass();
}

void
win_update_scrollbar(void)
{
  int scrollbar = win_active_terminal()->show_scrollbar ? cfg.scrollbar : 0;
  LONG style = GetWindowLong(wnd, GWL_STYLE);
  SetWindowLong(wnd, GWL_STYLE,
                scrollbar ? style | WS_VSCROLL : style & ~WS_VSCROLL);
  LONG exstyle = GetWindowLong(wnd, GWL_EXSTYLE);
  SetWindowLong(wnd, GWL_EXSTYLE,
                scrollbar < 0 ? exstyle | WS_EX_LEFTSCROLLBAR
                              : exstyle & ~WS_EX_LEFTSCROLLBAR);
  SetWindowPos(wnd, null, 0, 0, 0, 0,
               SWP_NOACTIVATE | SWP_NOMOVE |
               SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

static void _reconfig(struct term* term, bool font_changed) {
  if (term->report_font_changed && font_changed)
    if (term->report_ambig_width)
      child_write(term->child, cs_ambig_wide ? "\e[2W" : "\e[1W", 4);
    else
      child_write(term->child, "\e[0W", 4);
  else if (term->report_ambig_width && old_ambig_wide != cs_ambig_wide)
    child_write(term->child, cs_ambig_wide ? "\e[2W" : "\e[1W", 4);
}

static void font_cs_reconfig(bool font_changed) {
  if (font_changed) {
    win_init_fonts(cfg.font.size);
    trace_resize((" (font_cs_reconfig -> win_adapt_term_size)\n"));
    win_adapt_term_size(true, false);
  }
  win_update_scrollbar();
  update_transparency();
  win_update_mouse();

  old_ambig_wide = cs_ambig_wide;
  cs_reconfig();
  win_for_each_term_bool(_reconfig, font_changed);
}
  
void
win_reconfig(void)
{
  trace_resize(("--- win_reconfig\n"));
 /* Pass new config data to the terminal */
  win_for_each_term(term_reconfig);

  bool font_changed =
    wcscmp(new_cfg.font.name, cfg.font.name) ||
    new_cfg.font.size != cfg.font.size ||
    new_cfg.font.weight != cfg.font.weight ||
    new_cfg.font.isbold != cfg.font.isbold ||
    new_cfg.bold_as_font != cfg.bold_as_font ||
    new_cfg.bold_as_colour != cfg.bold_as_colour ||
    new_cfg.font_smoothing != cfg.font_smoothing;

  if (new_cfg.fg_colour != cfg.fg_colour)
    win_set_colour(FG_COLOUR_I, new_cfg.fg_colour);

  if (new_cfg.bg_colour != cfg.bg_colour)
    win_set_colour(BG_COLOUR_I, new_cfg.bg_colour);

  if (new_cfg.cursor_colour != cfg.cursor_colour)
    win_set_colour(CURSOR_COLOUR_I, new_cfg.cursor_colour);

  /* Copy the new config and refresh everything */
  copy_config("win_reconfig", &cfg, &new_cfg);

  font_cs_reconfig(font_changed);
}

static bool
confirm_exit(void)
{
  if (!child_is_any_parent())
    return true;

  int ret =
    MessageBox(
      wnd,
      "Processes are running in session.\n"
      "Close anyway?",
      APPNAME, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2
    );

  // Treat failure to show the dialog as confirmation.
  return !ret || ret == IDOK;
}

static bool
confirm_tab_exit(void)
{
  if (!child_is_parent(win_active_terminal()->child))
    return true;

  int ret =
    MessageBox(
      wnd,
      "Processes are running in active tab.\n"
      "Close anyway?",
      APPNAME, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2
    );

  // Treat failure to show the dialog as confirmation.
  return !ret || ret == IDOK;
}

static int
confirm_multi_tab(void)
{
  return MessageBox(
           wnd,
           "Mutiple tab opened.\n"
           "Close all the tabs?",
           APPNAME, MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON3
         );
}

#define dont_debug_messages
#define dont_debug_only_sizepos_messages
#define dont_debug_mouse_messages

static LRESULT CALLBACK
win_proc(HWND wnd, UINT message, WPARAM wp, LPARAM lp)
{
#ifdef debug_messages
static struct {
  uint wm_;
  char * wm_name;
} wm_names[] = {
#include "_wm.t"
};
  char * wm_name = "WM_?";
  for (uint i = 0; i < lengthof(wm_names); i++)
    if (message == wm_names[i].wm_) {
      wm_name = wm_names[i].wm_name;
      break;
    }
  if ((message != WM_KEYDOWN || !(lp & 0x40000000))
      && message != WM_TIMER && message != WM_NCHITTEST
# ifndef debug_mouse_messages
      && message != WM_SETCURSOR
      && message != WM_MOUSEMOVE && message != WM_NCMOUSEMOVE
# endif
     )
#ifdef debug_only_sizepos_messages
    if (strstr(wm_name, "POSCH") || strstr(wm_name, "SIZ"))
#endif
    printf("[%d] win_proc %04X %s (%04X %08X)\n", (int)time(0), message, wm_name, (unsigned)wp, (unsigned)lp);
#endif

  struct term* term = 0;
  if (term_initialized) term = win_active_terminal();
  switch (message) {
    when WM_NCCREATE:
      if (cfg.handle_dpichanged && pEnableNonClientDpiScaling) {
        //CREATESTRUCT * csp = (CREATESTRUCT *)lp;
        resizing = true;
        BOOL res = pEnableNonClientDpiScaling(wnd);
        resizing = false;
        (void)res;
#ifdef debug_dpi
        uint err = GetLastError();
        int wmlen = 1024;  // size of heap-allocated array
        wchar winmsg[wmlen];  // constant and < 1273 or 1705 => issue #530
        FormatMessageW(
              FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
              0, err, 0, winmsg, wmlen, 0
        );
        printf("NC:EnableNonClientDpiScaling: %d %ls\n", !!res, winmsg);
#endif
        return 1;
      }

    when WM_TIMER: {
      win_process_timer_message(wp);
      return 0;
    }

    when WM_CLOSE:
      if (win_tab_count() > 1) {
        switch (confirm_multi_tab()) {
          when IDNO:
            if (!cfg.confirm_exit || confirm_tab_exit()) {
              child_terminate(term->child);
            }
            return 0;
          when IDCANCEL:
            return 0;
        }
      }
      if (!cfg.confirm_exit || confirm_exit())
        child_kill();
      return 0;

    when WM_COMMAND or WM_SYSCOMMAND: {
# ifdef debug_messages
      static struct {
        uint idm_;
        char * idm_name;
      } idm_names[] = {
# include "_winidm.t"
      };
      char * idm_name = "IDM_?";
      for (uint i = 0; i < lengthof(idm_names); i++)
        if ((wp & ~0xF) == idm_names[i].idm_) {
          idm_name = idm_names[i].idm_name;
          break;
        }
      printf("                           %04X %s\n", (int)wp, idm_name);
# endif
      switch (wp & ~0xF) {  /* low 4 bits reserved to Windows */
        when IDM_OPEN: term_open(term);
        when IDM_COPY: term_copy(term);
        when IDM_PASTE: win_paste();
        when IDM_SELALL: term_select_all(term); win_update();
        when IDM_RESET: winimgs_clear(term); term_reset(term); win_update();
        when IDM_DEFSIZE:
          default_size();
        when IDM_DEFSIZE_ZOOM:
          if (GetKeyState(VK_SHIFT) & 0x80) {
            // Shift+Alt+F10 should restore both window size and font size

            // restore default font size first:
            win_zoom_font(0, false);

            // restore window size:
            default_size_token = true;
            default_size();  // or defer to WM_PAINT
          }
          else {
            default_size();
          }
        when IDM_FULLSCREEN or IDM_FULLSCREEN_ZOOM:
          if ((wp & ~0xF) == IDM_FULLSCREEN_ZOOM)
            zoom_token = 4;  // override cfg.zoom_font_with_window == 0
          else
            zoom_token = -4;
          win_maximise(win_is_fullscreen ? 0 : 2);

          term_schedule_search_update(term);
          win_update_search();
        when IDM_SEARCH: win_open_search();
        when IDM_FLIPSCREEN: term_flip_screen(term);
        when IDM_OPTIONS: win_open_config();
        when IDM_NEW: child_fork(term->child, main_argc, main_argv, 0);
        when IDM_NEW_MONI: child_fork(term->child, main_argc, main_argv, (int)lp - ' ');
        when IDM_COPYTITLE: win_copy_title();
        when IDM_NEWTAB: win_tab_create();
        when IDM_KILLTAB: child_terminate(term->child);
        when IDM_PREVTAB: win_tab_change(-1);
        when IDM_NEXTTAB: win_tab_change(+1);
        when IDM_MOVELEFT: win_tab_move(-1);
        when IDM_MOVERIGHT: win_tab_move(+1);
      }
    }

    when WM_VSCROLL:
      switch (LOWORD(wp)) {
        when SB_BOTTOM:   term_scroll(term, -1, 0);
        when SB_TOP:      term_scroll(term, +1, 0);
        when SB_LINEDOWN: term_scroll(term, 0, +1);
        when SB_LINEUP:   term_scroll(term, 0, -1);
        when SB_PAGEDOWN: term_scroll(term, 0, +max(1, term->rows - 1));
        when SB_PAGEUP:   term_scroll(term, 0, -max(1, term->rows - 1));
        when SB_THUMBPOSITION or SB_THUMBTRACK: {
          SCROLLINFO info;
          info.cbSize = sizeof(SCROLLINFO);
          info.fMask = SIF_TRACKPOS;
          GetScrollInfo(wnd, SB_VERT, &info);
          term_scroll(term, 1, info.nTrackPos);
        }
      }

    when WM_MOUSEMOVE: win_mouse_move(false, lp);
    when WM_NCMOUSEMOVE: win_mouse_move(true, lp);
    when WM_MOUSEWHEEL: win_mouse_wheel(wp, lp);
    when WM_LBUTTONDOWN: win_mouse_click(MBT_LEFT, lp);
    when WM_RBUTTONDOWN: win_mouse_click(MBT_RIGHT, lp);
    when WM_MBUTTONDOWN: win_mouse_click(MBT_MIDDLE, lp);
    when WM_LBUTTONUP: win_mouse_release(MBT_LEFT, lp);
    when WM_RBUTTONUP: win_mouse_release(MBT_RIGHT, lp);
    when WM_MBUTTONUP: win_mouse_release(MBT_MIDDLE, lp);

    when WM_KEYDOWN or WM_SYSKEYDOWN:
      if (win_key_down(wp, lp))
        return 0;

    when WM_KEYUP or WM_SYSKEYUP:
      if (win_key_up(wp, lp))
        return 0;

    when WM_CHAR or WM_SYSCHAR:
      child_sendw(term->child, &(wchar){wp}, 1);
      return 0;

    when WM_INPUTLANGCHANGEREQUEST:  // catch Shift-Control-0
      if ((GetKeyState(VK_SHIFT) & 0x80) && (GetKeyState(VK_CONTROL) & 0x80))
        if (win_key_down('0', 0x000B0001))
          return 0;

    when WM_INPUTLANGCHANGE:
      win_set_ime_open(ImmIsIME(GetKeyboardLayout(0)) && ImmGetOpenStatus(imc));

    when WM_IME_NOTIFY:
      if (wp == IMN_SETOPENSTATUS)
        win_set_ime_open(ImmGetOpenStatus(imc));

    when WM_IME_STARTCOMPOSITION:
      ImmSetCompositionFont(imc, &lfont);

    when WM_IME_COMPOSITION:
      if (lp & GCS_RESULTSTR) {
        LONG len = ImmGetCompositionStringW(imc, GCS_RESULTSTR, null, 0);
        if (len > 0) {
          char buf[len];
          ImmGetCompositionStringW(imc, GCS_RESULTSTR, buf, len);
          child_sendw(term->child, (wchar *)buf, len / 2);
        }
        return 1;
      }

    when WM_THEMECHANGED or WM_WININICHANGE or WM_SYSCOLORCHANGE:
      // Size of window border (border, title bar, scrollbar) changed by:
      //   Personalization of window geometry (e.g. Title Bar Size)
      //     -> Windows sends WM_SYSCOLORCHANGE
      //   Performance Option "Use visual styles on windows and borders"
      //     -> Windows sends WM_THEMECHANGED and WM_SYSCOLORCHANGE
      // and in both case a couple of WM_WININICHANGE

      win_adjust_borders(cell_width * cfg.cols, cell_height * cfg.rows);
      RedrawWindow(wnd, null, null, 
                   RDW_FRAME | RDW_INVALIDATE |
                   RDW_UPDATENOW | RDW_ALLCHILDREN);
      win_update_search();

    when WM_FONTCHANGE:
      font_cs_reconfig(true);

    when WM_PAINT:
      win_paint();

#ifdef handle_default_size_asynchronously
      if (default_size_token) {
        default_size();
        default_size_token = false;
      }
#endif

      return 0;

    when WM_ACTIVATE:
      if ((wp & 0xF) != WA_INACTIVE) {
        flash_taskbar(false);  /* stop */
        term_set_focus(term, true, true);
      } else {
        term_set_focus(term, false, true);
      }
      update_transparency();
      win_key_reset();

    when WM_SETFOCUS:
      trace_resize(("# WM_SETFOCUS VK_SHIFT %02X\n", (uchar)GetKeyState(VK_SHIFT)));
      term_set_focus(term, true, false);
      CreateCaret(wnd, caretbm, 0, 0);
      //flash_taskbar(false);  /* stop; not needed when leaving search bar */
      win_update();
      ShowCaret(wnd);
      zoom_token = -4;

    when WM_KILLFOCUS:
      win_show_mouse();
      term_set_focus(term, false, false);
      DestroyCaret();
      win_update();

    when WM_INITMENU:
      win_update_menus();
      return 0;

    when WM_MOVING:
      trace_resize(("# WM_MOVING VK_SHIFT %02X\n", (uchar)GetKeyState(VK_SHIFT)));
      zoom_token = -4;

    when WM_ENTERSIZEMOVE:
      trace_resize(("# WM_ENTERSIZEMOVE VK_SHIFT %02X\n", (uchar)GetKeyState(VK_SHIFT)));
      resizing = true;

    when WM_SIZING: {  // mouse-drag window resizing
      trace_resize(("# WM_SIZING (resizing %d) VK_SHIFT %02X\n", resizing, (uchar)GetKeyState(VK_SHIFT)));
      zoom_token = 2;
     /*
      * This does two jobs:
      * 1) Keep the tip uptodate
      * 2) Make sure the window size is _stepped_ in units of the font size.
      */
      LPRECT r = (LPRECT) lp;
      int width = r->right - r->left - extra_width - 2 * PADDING;
      int height = r->bottom - r->top - extra_height - 2 * PADDING - g_render_tab_height;
      int cols = max(1, (float)width / cell_width + 0.5);
      int rows = max(1, (float)height / cell_height + 0.5);

      int ew = width - cols * cell_width;
      int eh = height - rows * cell_height;

      if (wp >= WMSZ_BOTTOM) {
        wp -= WMSZ_BOTTOM;
        r->bottom -= eh;
      }
      else if (wp >= WMSZ_TOP) {
        wp -= WMSZ_TOP;
        r->top += eh;
      }

      if (wp == WMSZ_RIGHT)
        r->right -= ew;
      else if (wp == WMSZ_LEFT)
        r->left += ew;

      win_show_tip(r->left + extra_width, r->top + extra_height, cols, rows);

      return ew || eh;
    }

    when WM_SIZE: {
      trace_resize(("# WM_SIZE (resizing %d) VK_SHIFT %02X\n", resizing, (uchar)GetKeyState(VK_SHIFT)));
      if (wp == SIZE_RESTORED && win_is_fullscreen)
        clear_fullscreen();
      else if (wp == SIZE_MAXIMIZED && go_fullscr_on_max) {
        go_fullscr_on_max = false;
        make_fullscreen();
      }

      if (!resizing) {
        trace_resize((" (win_proc (WM_SIZE) -> win_adapt_term_size)\n"));
        // enable font zooming on Shift unless
#ifdef does_not_enable_shift_maximize_initially
        // - triggered by Windows shortcut (with Windows key)
        // - triggered by Ctrl+Shift+F (zoom_token < 0)
        if ((zoom_token >= 0) && !(GetKeyState(VK_LWIN) & 0x80))
          if (zoom_token < 1)  // accept overriding zoom_token 4
            zoom_token = 1;
#else
        // - triggered by Windows shortcut (with Windows key)
        if (!(GetKeyState(VK_LWIN) & 0x80))
          if (zoom_token < 1)  // accept overriding zoom_token 4
            zoom_token = 1;
#endif
        bool scale_font = (cfg.zoom_font_with_window || zoom_token > 2)
                       && (zoom_token > 0) && (GetKeyState(VK_SHIFT) & 0x80)
                       && !default_size_token;
        win_adapt_term_size(false, scale_font);
        if (zoom_token > 0)
          zoom_token = zoom_token >> 1;
        default_size_token = false;
      }

      return 0;
    }

    when WM_NOTIFY:
      switch (((LPNMHDR)lp)->code) {
        when TCN_SELCHANGE:
          win_tab_mouse_click(TabCtrl_GetCurSel(tab_wnd));
      }

    when WM_DRAWITEM:
      win_paint_tabs(lp, 0);

    when WM_EXITSIZEMOVE or WM_CAPTURECHANGED: { // after mouse-drag resizing
      trace_resize(("# WM_EXITSIZEMOVE (resizing %d) VK_SHIFT %02X\n", resizing, (uchar)GetKeyState(VK_SHIFT)));
      bool shift = GetKeyState(VK_SHIFT) & 0x80;

      if (resizing) {
        resizing = false;
        win_destroy_tip();
        trace_resize((" (win_proc (WM_EXITSIZEMOVE) -> win_adapt_term_size)\n"));
        win_adapt_term_size(shift, false);
      }
    }

    when WM_WINDOWPOSCHANGED: {
#     define WP ((WINDOWPOS *) lp)
      trace_resize(("# WM_WINDOWPOSCHANGED (resizing %d) %d %d @ %d %d\n", resizing, WP->cy, WP->cx, WP->y, WP->x));
      bool dpi_changed = true;
      if (per_monitor_dpi_aware && cfg.handle_dpichanged && pGetDpiForMonitor) {
        HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
        uint x, y;
        pGetDpiForMonitor(mon, 0, &x, &y);  // MDT_EFFECTIVE_DPI
#ifdef debug_dpi
        printf("WM_WINDOWPOSCHANGED %d -> %d (aware %d handle %d)\n", dpi, y, per_monitor_dpi_aware, cfg.handle_dpichanged);
#endif
        if (y != dpi) {
          dpi = y;
        }
        else
          dpi_changed = false;
      }

      if (dpi_changed && per_monitor_dpi_aware && cfg.handle_dpichanged) {
        // remaining glitch:
        // start mintty -p @1; move it to other monitor;
        // columns will be less
        //win_init_fonts(cfg.font.size);
        font_cs_reconfig(true);
        win_adapt_term_size(true, false);
      }
    }

    when WM_DPICHANGED: {
#ifdef handle_dpi_on_dpichanged
      bool dpi_changed = true;
      if (per_monitor_dpi_aware && cfg.handle_dpichanged && pGetDpiForMonitor) {
        HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
        uint x, y;
        pGetDpiForMonitor(mon, 0, &x, &y);  // MDT_EFFECTIVE_DPI
#ifdef debug_dpi
        printf("WM_DPICHANGED %d -> %d (aware %d handle %d)\n", dpi, y, per_monitor_dpi_aware, cfg.handle_dpichanged);
#endif
        if (y != dpi) {
          dpi = y;
        }
        else
          dpi_changed = false;
      }
#ifdef debug_dpi
      else
        printf("WM_DPICHANGED (aware %d handle %d)\n", per_monitor_dpi_aware, cfg.handle_dpichanged);
#endif

      if (dpi_changed && per_monitor_dpi_aware && cfg.handle_dpichanged) {
        // this RECT is adjusted with respect to the monitor dpi already,
        // so we don't need to consider GetDpiForMonitor
        LPRECT r = (LPRECT) lp;
        // try to stabilize font size roundtrip; 
        // heuristic tweak of window size to compensate for 
        // font scaling rounding errors that would continuously 
        // decrease the window size if moving between monitors repeatedly
        long width = (r->right - r->left) * 20 / 19;
        long height = (r->bottom - r->top) * 20 / 19;
        SetWindowPos(wnd, 0, r->left, r->top, width, height,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        int y = term->rows, x = term->cols;
        win_adapt_term_size(false, true);
        //?win_init_fonts(cfg.font.size);
        // try to stabilize terminal size roundtrip
        if (term->rows != y || term->cols != x) {
          // win_fix_position also clips the window to desktop size
          win_set_chars(y, x);
        }
#ifdef debug_dpi
        printf("SM_CXVSCROLL %d\n", GetSystemMetrics(SM_CXVSCROLL));
#endif
        return 0;
      }
      break;
#endif
    }
  }
 /*
  * Any messages we don't process completely above are passed through to
  * DefWindowProc() for default processing.
  */
  return DefWindowProcW(wnd, message, wp, lp);
}


void
show_message(char * msg, UINT type)
{
  FILE * out = (type & (MB_ICONWARNING | MB_ICONSTOP)) ? stderr : stdout;
  char * outmsg = cs__utftombs(msg);
  if (fputs(outmsg, out) < 0 || fputs("\n", out) < 0 || fflush(out) < 0) {
    wchar * wmsg = cs__utftowcs(msg);
    MessageBoxW(0, wmsg, W(APPNAME), type);
    delete(wmsg);
  }
  delete(outmsg);
}

static void
show_info(char * msg)
{
  show_message(msg, MB_OK);
}

static char *
opterror_msg(string msg, bool utf8params, string p1, string p2)
{
  // Note: msg is in UTF-8,
  // parameters are in current encoding unless utf8params is true
  if (!utf8params) {
    if (p1) {
      wchar * w = cs__mbstowcs(p1);
      p1 = cs__wcstoutf(w);
      free(w);
    }
    if (p2) {
      wchar * w = cs__mbstowcs(p2);
      p2 = cs__wcstoutf(w);
      free(w);
    }
  }

  char * fullmsg;
  int len = asprintf(&fullmsg, msg, p1, p2);
  if (!utf8params) {
    if (p1)
      free((char *)p1);
    if (p2)
      free((char *)p2);
  }

  if (len > 0)
    return fullmsg;
  else
    return null;
}

bool
print_opterror(FILE * stream, string msg, bool utf8params, string p1, string p2)
{
  char * fullmsg = opterror_msg(msg, utf8params, p1, p2);
  bool ok = false;
  if (fullmsg) {
    char * outmsg = cs__utftombs(fullmsg);
    delete(fullmsg);
    ok = fprintf(stream, "%s.\n", outmsg);
    if (ok)
      ok = fflush(stream);
    delete(outmsg);
  }
  return ok;
}

static void
print_error(string msg)
{
  print_opterror(stderr, msg, true, "", "");
}

static void
option_error(char * msg, char * option)
{
  // msg is in UTF-8, option is in current encoding
  char * optmsg = opterror_msg(msg, false, option, null);
  char * fullmsg = asform("%s\n%s", optmsg, _("Try '--help' for more information"));
  show_message(fullmsg, MB_ICONWARNING);
  exit(1);
}

static void
show_iconwarn(wchar * winmsg)
{
  char * msg = _("Could not load icon");
  char * in = cs__wcstoutf(cfg.icon);

  char * fullmsg;
  int len;
  if (winmsg) {
    char * wmsg = cs__wcstoutf(winmsg);
    len = asprintf(&fullmsg, "%s '%s':\n%s", msg, in, wmsg);
    free(wmsg);
  }
  else
    len = asprintf(&fullmsg, "%s '%s'", msg, in);
  free(in);
  if (len > 0) {
    show_message(fullmsg, MB_ICONWARNING);
    free(fullmsg);
  }
  else
    show_message(msg, MB_ICONWARNING);
}

#if CYGWIN_VERSION_DLL_MAJOR >= 1005

#include <shlobj.h>

static wchar *
get_shortcut_icon_location(wchar * iconfile)
{
  IShellLinkW * shell_link;
  IPersistFile * persist_file;
  HRESULT hres = OleInitialize(NULL);
  if (hres != S_FALSE && hres != S_OK)
    return 0;

  hres = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **) &shell_link);
  if (!SUCCEEDED(hres))
    return 0;

  hres = shell_link->lpVtbl->QueryInterface(shell_link, &IID_IPersistFile,
                                            (void **) &persist_file);
  if (!SUCCEEDED(hres)) {
    shell_link->lpVtbl->Release(shell_link);
    return 0;
  }

  /* Load the shortcut.  */
  hres = persist_file->lpVtbl->Load(persist_file, iconfile, STGM_READ);

  wchar * result = 0;

  if (SUCCEEDED(hres)) {
    WCHAR wil[MAX_PATH + 1];
    * wil = 0;
    int index;
    hres = shell_link->lpVtbl->GetIconLocation(shell_link, wil, MAX_PATH, &index);
    if (!SUCCEEDED(hres) || !*wil)
      goto iconex;

    wchar * wicon = wil;

    /* Append ,icon-index if non-zero.  */
    wchar * widx = W("");
    if (index) {
      char idx[22];
      sprintf(idx, ",%d", index);
      widx = cs__mbstowcs(idx);
    }

    /* Resolve leading Windows environment variable component.  */
    wchar * wenv = W("");
    wchar * fin;
    if (wil[0] == '%' && wil[1] && wil[1] != '%' && (fin = wcschr(&wil[2], '%'))) {
      char var[fin - wil];
      char * cop = var;
      wchar * v;
      for (v = &wil[1]; *v != '%'; v++) {
        if (*v >= 'a' && *v <= 'z')
          *cop = *v - 'a' + 'A';
        else
          *cop = *v;
        cop++;
      }
      *cop = '\0';
      v ++;
      wicon = v;

      char * val = getenv(var);
      if (val) {
        wenv = cs__mbstowcs(val);
      }
    }

    result = newn(wchar, wcslen(wenv) + wcslen(wicon) + wcslen(widx) + 1);
    wcscpy(result, wenv);
    wcscpy(&result[wcslen(result)], wicon);
    wcscpy(&result[wcslen(result)], widx);
    if (* widx)
      free(widx);
    if (* wenv)
      free(wenv);
  }
  iconex:

  /* Release the pointer to the IPersistFile interface. */
  persist_file->lpVtbl->Release(persist_file);

  /* Release the pointer to the IShellLink interface. */
  shell_link->lpVtbl->Release(shell_link);

  return result;
}

#endif

static void
configure_taskbar(void)
{
#define no_patch_jumplist
#ifdef patch_jumplist
#include "jumplist.h"
  // test data
  wchar * jump_list_title[] = {
    W("title1"), W(""), W(""), W("mä€"), W(""), W(""), W(""), W(""), W(""), W(""), 
  };
  wchar * jump_list_cmd[] = {
    W("-o Rows=15"), W("-o Rows=20"), W(""), W("-t mö€"), W(""), W(""), W(""), W(""), W(""), W(""), 
  };
  // the patch offered in issue #290 does not seem to work
  setup_jumplist(jump_list_title, jump_list_cmd);
#endif

#if CYGWIN_VERSION_DLL_MAJOR >= 1007
  // initial patch (issue #471) contributed by Johannes Schindelin
  wchar * app_id = (wchar *) cfg.app_id;
  wchar * relaunch_icon = (wchar *) cfg.icon;
  wchar * relaunch_display_name = (wchar *) cfg.app_name;
  wchar * relaunch_command = (wchar *) cfg.app_launch_cmd;

#define dont_debug_properties

#ifdef two_witty_ideas_with_bad_side_effects
#warning automatic derivation of an AppId is likely not a good idea
  // If an icon is configured but no app_id, we can derive one from the 
  // icon in order to enable proper taskbar grouping by common icon.
  // However, this has an undesirable side-effect if a shortcut is 
  // pinned (presumably getting some implicit AppID from Windows) and 
  // instances are started from there (with a different AppID...).
  // Disabled.
  if (relaunch_icon && *relaunch_icon && (!app_id || !*app_id)) {
    const char * iconbasename = strrchr(cfg.icon, '/');
    if (iconbasename)
      iconbasename ++;
    else {
      iconbasename = strrchr(cfg.icon, '\\');
      if (iconbasename)
        iconbasename ++;
      else
        iconbasename = cfg.icon;
    }
    char * derived_app_id = malloc(strlen(iconbasename) + 7 + 1);
    strcpy(derived_app_id, "Fatty.");
    strcat(derived_app_id, iconbasename);
    app_id = derived_app_id;
  }
  // If app_name is configured but no app_launch_cmd, we need an app_id 
  // to make app_name effective as taskbar title, so invent one.
  if (relaunch_display_name && *relaunch_display_name && 
      (!app_id || !*app_id)) {
    app_id = "Fatty.AppID";
  }
#endif

  // Set the app ID explicitly, as well as the relaunch command and display name
  if (prevent_pinning || (app_id && *app_id)) {
    HMODULE shell = load_sys_library("shell32.dll");
    HRESULT (WINAPI *pGetPropertyStore)(HWND hwnd, REFIID riid, void **ppv) =
      (void *)GetProcAddress(shell, "SHGetPropertyStoreForWindow");
#ifdef debug_properties
      printf("SHGetPropertyStoreForWindow linked %d\n", !!pGetPropertyStore);
#endif
    if (pGetPropertyStore) {
      IPropertyStore *pps;
      HRESULT hr;
      PROPVARIANT var;

      hr = pGetPropertyStore(wnd, &IID_IPropertyStore, (void **) &pps);
#ifdef debug_properties
      printf("IPropertyStore found %d\n", SUCCEEDED(hr));
#endif
      if (SUCCEEDED(hr)) {
        // doc: https://msdn.microsoft.com/en-us/library/windows/desktop/dd378459%28v=vs.85%29.aspx
        // def: typedef struct tagPROPVARIANT PROPVARIANT: propidl.h
        // def: enum VARENUM (VT_*): wtypes.h
        // def: PKEY_*: propkey.h
        if (relaunch_command && *relaunch_command && store_taskbar_properties) {
#ifdef debug_properties
          printf("AppUserModel_RelaunchCommand=%ls\n", relaunch_command);
#endif
          var.pwszVal = relaunch_command;
          var.vt = VT_LPWSTR;
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_RelaunchCommand, &var);
        }
        if (relaunch_display_name && *relaunch_display_name) {
#ifdef debug_properties
          printf("AppUserModel_RelaunchDisplayNameResource=%ls\n", relaunch_display_name);
#endif
          var.pwszVal = relaunch_display_name;
          var.vt = VT_LPWSTR;
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_RelaunchDisplayNameResource, &var);
        }
        if (relaunch_icon && *relaunch_icon) {
#ifdef debug_properties
          printf("AppUserModel_RelaunchIconResource=%ls\n", relaunch_icon);
#endif
          var.pwszVal = relaunch_icon;
          var.vt = VT_LPWSTR;
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_RelaunchIconResource, &var);
        }
        if (prevent_pinning) {
          var.boolVal = VARIANT_TRUE;
#ifdef debug_properties
          printf("AppUserModel_PreventPinning=%d\n", var.boolVal);
#endif
          var.vt = VT_BOOL;
          // PreventPinning must be set before setting ID
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_PreventPinning, &var);
        }
#ifdef set_userpinned
DEFINE_PROPERTYKEY(PKEY_AppUserModel_StartPinOption, 0x9f4c2855,0x9f79,0x4B39,0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3,12);
#define APPUSERMODEL_STARTPINOPTION_USERPINNED 2
#warning needs Windows 8/10 to build...
        {
          var.uintVal = APPUSERMODEL_STARTPINOPTION_USERPINNED;
#ifdef debug_properties
          printf("AppUserModel_StartPinOption=%d\n", var.uintVal);
#endif
          var.vt = VT_UINT;
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_StartPinOption, &var);
        }
#endif
        if (app_id && *app_id) {
#ifdef debug_properties
          printf("AppUserModel_ID=%ls\n", app_id);
#endif
          var.pwszVal = app_id;
          var.vt = VT_LPWSTR;  // VT_EMPTY should remove but has no effect
          pps->lpVtbl->SetValue(pps,
              &PKEY_AppUserModel_ID, &var);
        }

        pps->lpVtbl->Commit(pps);
        pps->lpVtbl->Release(pps);
      }
    }
  }
#endif
}

#define usage __("Usage:")
#define synopsis __("[OPTION]... [ PROGRAM [ARG]... | - ]")
static char help[] =
  //_ help text (output of -H / --help), after initial line ("synopsis")
  __("Start a new terminal session running the specified program or the user's shell.\n"
  "If a dash is given instead of a program, invoke the shell as a login shell.\n"
  "\n"
  "Options:\n"
///12345678901234567890123456789012345678901234567890123456789012345678901234567890
  "  -b, --tab COMMAND     Spawn a new tab and execute the command\n"
  "  -c, --config FILE     Load specified config file (cf. -C or -o ThemeFile)\n"
  "  -e, --exec ...        Treat remaining arguments as the command to execute\n"
  "  -h, --hold never|start|error|always  Keep window open after command finishes\n"
  "  -p, --position X,Y    Open window at specified coordinates\n"
  "  -p, --position center|left|right|top|bottom  Open window at special position\n"
  "  -p, --position @N     Open window on monitor N\n"
  "  -s, --size COLS,ROWS  Set screen size in characters (also COLSxROWS)\n"
  "  -s, --size maxwidth|maxheight  Set max screen size in given dimension\n"
  "  -T, --Title TITLE     Set window title (default: the invoked command)\n"
  "  -t, --title TITLE     Set window title (default: the invoked command) (cf. -T)\n"
  "                        Must be set before -b/--tab option\n"
  "  -u, --utmp            Create a utmp entry\n"
  "  -w, --window normal|min|max|full|hide  Set initial window state\n"
  "  -i, --icon FILE[,IX]  Load window icon from file, optionally with index\n"
  "  -l, --log FILE|-      Log output to file or stdout\n"
  "      --nobidi|--nortl  Disable bidi (right-to-left support)\n"
  "  -o, --option OPT=VAL  Set/Override config file option with given value\n"
  "  -B, --Border frame|void  Use thin/no window border\n"
  "      --nopin           Make this instance not pinnable to taskbar\n"
  "  -D, --daemon          Start new instance with Windows shortcut key\n"
  "      --class CLASS     Set window class name (default: " APPNAME ")\n"
  "  -H, --help            Display help and exit\n"
  "  -V, --version         Print version information and exit\n"
  "See manual page for further command line options and configuration.\n"
);

static const char short_opts[] = "+:b:c:C:eh:i:l:o:p:s:t:T:B:R:uw:HVdD";

static const struct option
opts[] = {
  {"config",     required_argument, 0, 'c'},
  {"loadconfig", required_argument, 0, 'C'},
  {"exec",       no_argument,       0, 'e'},
  {"hold",       required_argument, 0, 'h'},
  {"icon",       required_argument, 0, 'i'},
  {"log",        required_argument, 0, 'l'},
  {"utmp",       no_argument,       0, 'u'},
  {"option",     required_argument, 0, 'o'},
  {"position",   required_argument, 0, 'p'},
  {"size",       required_argument, 0, 's'},
  {"title",      required_argument, 0, 't'},
  {"Title",      required_argument, 0, 'T'},
  {"Border",     required_argument, 0, 'B'},
  {"window",     required_argument, 0, 'w'},
  {"class",      required_argument, 0, ''},  // short option not enabled
  {"dir",        required_argument, 0, ''},  // short option not enabled
  {"nobidi",     no_argument,       0, ''},  // short option not enabled
  {"nortl",      no_argument,       0, ''},  // short option not enabled
  {"wsl",        no_argument,       0, ''},  // short option not enabled
  {"help",       no_argument,       0, 'H'},
  {"version",    no_argument,       0, 'V'},
  {"nodaemon",   no_argument,       0, 'd'},
  {"daemon",     no_argument,       0, 'D'},
  {"nopin",      no_argument,       0, ''},  // short option not enabled
  {"store-taskbar-properties", no_argument, 0, ''},  // no short option
  {0, 0, 0, 0}
};

int
main(int argc, char *argv[])
{
  char* home;
  char* cmd;

  main_argv = argv;
  main_argc = argc;
#ifdef debuglog
  mtlog = fopen("/tmp/mtlog", "a");
  {
    char timbuf [22];
    struct timeval now;
    gettimeofday (& now, 0);
    strftime(timbuf, sizeof (timbuf), "%Y-%m-%d %H:%M:%S", localtime(& now.tv_sec));
    fprintf(mtlog, "[%s.%03d] %s\n", timbuf, (int)now.tv_usec / 1000, argv[0]);
    fflush(mtlog);
  }
#endif
  init_config();
  cs_init();

  // Determine home directory.
  home = getenv("HOME");
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  // Before Cygwin 1.5, the passwd structure is faked.
  struct passwd *pw = getpwuid(getuid());
#endif
  home = home ? strdup(home) :
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    (pw && pw->pw_dir && *pw->pw_dir) ? strdup(pw->pw_dir) :
#endif
    asform("/home/%s", getlogin());

  // Set size and position defaults.
  STARTUPINFOW sui;
  GetStartupInfoW(&sui);
  cfg.window = sui.dwFlags & STARTF_USESHOWWINDOW ? sui.wShowWindow : SW_SHOW;
  cfg.x = cfg.y = CW_USEDEFAULT;
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  invoked_from_shortcut = sui.dwFlags & STARTF_TITLEISLINKNAME;
  invoked_with_appid = sui.dwFlags & STARTF_TITLEISAPPID;
  // shortcut or AppId would be found in sui.lpTitle
# ifdef debuglog
  fprintf(mtlog, "shortcut %d %ls\n", invoked_from_shortcut, sui.lpTitle);
# endif
#endif

  // Load config files
  // try global config file
  load_config("/etc/fattyrc", true);
  // try Windows config location (#201)
  char * appdata = getenv("APPDATA");
  if (appdata && *appdata) {
    string rc_file = asform("%s/fatty/config", appdata);
    load_config(rc_file, true);
    delete(rc_file);
  }
  // try XDG config base directory default location (#525)
  string rc_file = asform("%s/.config/fatty/config", home);
  load_config(rc_file, true);
  delete(rc_file);
  // try home config file
  rc_file = asform("%s/.fattyrc", home);
  load_config(rc_file, true);
  delete(rc_file);

  char *tablist[32];
  char *tablist_title[32];
  int current_tab_size = 0;
  
  for (int i = 0; i < 32; i++)
    tablist_title[i] = NULL;

  if (getenv("FATTY_ICON")) {
    //cfg.icon = strdup(getenv("FATTY_ICON"));
    cfg.icon = cs__utftowcs(getenv("FATTY_ICON"));
    icon_is_from_shortcut = true;
    unsetenv("FATTY_ICON");
  }

#if CYGWIN_VERSION_DLL_MAJOR >= 1005
  if (invoked_from_shortcut) {
    wchar * icon = get_shortcut_icon_location(sui.lpTitle);
# ifdef debuglog
    fprintf(mtlog, "icon <%ls>\n", icon); fflush(mtlog);
# endif
    if (icon) {
      cfg.icon = icon;
      icon_is_from_shortcut = true;
    }
  }
#endif

  for (;;) {
    int opt = getopt_long(argc, argv, short_opts, opts, 0);
    if (opt == -1 || opt == 'e')
      break;
    char *longopt = argv[optind - 1], *shortopt = (char[]){'-', optopt, 0};
    switch (opt) {
      when 'c': load_config(optarg, true);
      when 'C': load_config(optarg, false);
      when 'h': set_arg_option("Hold", optarg);
      when 'i': set_arg_option("Icon", optarg);
      when 'l': set_arg_option("Log", optarg);
      when 'o': parse_arg_option(optarg);
      when 'p':
        if (strcmp(optarg, "center") == 0 || strcmp(optarg, "centre") == 0)
          center = true;
        else if (strcmp(optarg, "right") == 0)
          right = true;
        else if (strcmp(optarg, "bottom") == 0)
          bottom = true;
        else if (strcmp(optarg, "left") == 0)
          left = true;
        else if (strcmp(optarg, "top") == 0)
          top = true;
        else if (sscanf(optarg, "@%i%1s", &monitor, (char[2]){}) == 1)
          ;
        else if (sscanf(optarg, "%i,%i%1s", &cfg.x, &cfg.y, (char[2]){}) == 2)
          ;
        else
          option_error(_("Syntax error in position argument '%s'"), optarg);
      when 's':
        if (strcmp(optarg, "maxwidth") == 0)
          maxwidth = true;
        else if (strcmp(optarg, "maxheight") == 0)
          maxheight = true;
        else if (sscanf(optarg, "%u,%u%1s", &cfg.cols, &cfg.rows, (char[2]){}) == 2)
          ;
        else if (sscanf(optarg, "%ux%u%1s", &cfg.cols, &cfg.rows, (char[2]){}) == 2)
          ;
        else
          option_error(_("Syntax error in size argument '%s'"), optarg);
      when 't':
        tablist_title[current_tab_size] = optarg;
      when 'T':
        set_arg_option("Title", optarg);
        cfg.title_settable = false;
      when 'B':
        border_style = strdup(optarg);
      when 'u': cfg.create_utmp = true;
      when '':
        prevent_pinning = true;
        store_taskbar_properties = true;
      when '': store_taskbar_properties = true;
      when 'w': set_arg_option("Window", optarg);
      when 'b':
        tablist[current_tab_size] = optarg;
        current_tab_size++;
      when '': set_arg_option("Class", optarg);
      when '': disable_bidi = true;
      when '': support_wsl = true;
      when '':
        if (chdir(optarg) < 0) {
          if (*optarg == '"' || *optarg == '\'')
            if (optarg[strlen(optarg) - 1] == optarg[0]) {
              // strip off embedding quotes as provided when started 
              // from Windows context menu by registry entry
              char * dir = strdup(&optarg[1]);
              dir[strlen(dir) - 1] = '\0';
              chdir(dir);
              free(dir);
            }
        }
      when 'd':
        cfg.daemonize = false;
      when 'D':
        cfg.daemonize_always = true;
      when 'H': {
        char * helptext = asform("%s %s %s\n\n%s", _(usage), APPNAME, _(synopsis), _(help));
        show_info(helptext);
        free(helptext);
        return 0;
      }
      when 'V': {
        char * vertext =
          asform("%s\n%s\n%s\n%s\n", 
                 VERSION_TEXT, COPYRIGHT, LICENSE_TEXT, _(WARRANTY_TEXT));
        show_info(vertext);
        free(vertext);
        return 0;
      }
      when '?':
        option_error(_("Unknown option '%s'"), optopt ? shortopt : longopt);
      when ':':
        option_error(_("Option '%s' requires an argument"),
                     longopt[1] == '-' ? longopt : shortopt);
    }
  }
  copy_config("main after -o", &file_cfg, &cfg);
  if (*cfg.colour_scheme)
    load_scheme(cfg.colour_scheme);
  else if (*cfg.theme_file)
    load_theme(cfg.theme_file);

  finish_config();

  int term_rows = cfg.rows;
  int term_cols = cfg.cols;
  if (getenv("FATTY_ROWS")) {
    term_rows = atoi(getenv("FATTY_ROWS"));
    if (term_rows < 1)
      term_rows = cfg.rows;
    unsetenv("FATTY_ROWS");
  }
  if (getenv("FATTY_COLS")) {
    term_cols = atoi(getenv("FATTY_COLS"));
    if (term_cols < 1)
      term_cols = cfg.cols;
    unsetenv("FATTY_COLS");
  }
  if (getenv("FATTY_MONITOR")) {
    monitor = atoi(getenv("FATTY_MONITOR"));
    unsetenv("FATTY_MONITOR");
  }

  // if started from console, try to detach from caller's terminal (~daemonizing)
  // in order to not suppress signals
  // (indicated by isatty if linked with -mwindows as ttyname() is null)
  bool daemonize = cfg.daemonize && !isatty(0);
  // disable daemonizing if started from desktop
  if (invoked_from_shortcut)
    daemonize = false;
  // disable daemonizing if started from ConEmu
  if (getenv("ConEmuPID"))
    daemonize = false;
  if (cfg.daemonize_always)
    daemonize = true;
  if (daemonize) {  // detach from parent process and terminal
    pid_t pid = fork();
    if (pid < 0)
      print_error(_("Fatty could not detach from caller, starting anyway"));
    if (pid > 0)
      exit(0);  // exit parent process

    setsid();  // detach child process
  }

  load_dwm_funcs();  // must be called after the fork() above!

  load_dpi_funcs();
  per_monitor_dpi_aware = set_per_monitor_dpi_aware();
#ifdef debug_dpi
  printf("per_monitor_dpi_aware %d\n", per_monitor_dpi_aware);
#endif

  // Work out what to execute.
  argv += optind;
  if (*argv && (argv[1] || strcmp(*argv, "-")))
    cmd = *argv;
  else {
    // Look up the user's shell.
    cmd = getenv("SHELL");
    cmd = cmd ? strdup(cmd) :
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
      (pw && pw->pw_shell && *pw->pw_shell) ? strdup(pw->pw_shell) :
#endif
      "/bin/sh";

    // Determine the program name argument.
    char *slash = strrchr(cmd, '/');
    char *arg0 = slash ? slash + 1 : cmd;

    // Prepend '-' if a login shell was requested.
    if (*argv)
      arg0 = asform("-%s", arg0);

    // Create new argument array.
    argv = newn(char *, 2);
    *argv = arg0;
  }

  // Load icon if specified.
  HICON large_icon = 0, small_icon = 0;
  if (*cfg.icon) {
    //string icon_file = strdup(cfg.icon);
    // could use path_win_w_to_posix(cfg.icon) to avoid the locale trick below
    string icon_file = cs__wcstoutf(cfg.icon);
    uint icon_index = 0;
    char *comma = strrchr(icon_file, ',');
    if (comma) {
      char *start = comma + 1, *end;
      icon_index = strtoul(start, &end, 0);
      if (start != end && !*end)
        *comma = 0;
      else
        icon_index = 0;
    }
    SetLastError(0);
#if HAS_LOCALES
    char * valid_locale = setlocale(LC_CTYPE, 0);
    if (valid_locale) {
      valid_locale = strdup(valid_locale);
      setlocale(LC_CTYPE, "C.UTF-8");
# if CYGWIN_VERSION_API_MINOR >= 222
      cygwin_internal(CW_INT_SETLOCALE);  // fix internal locale
# endif
    }
#endif
    wchar *win_icon_file = path_posix_to_win_w(icon_file);
#if HAS_LOCALES
    if (valid_locale) {
      setlocale(LC_CTYPE, valid_locale);
# if CYGWIN_VERSION_API_MINOR >= 222
      cygwin_internal(CW_INT_SETLOCALE);  // fix internal locale
# endif
      free(valid_locale);
    }
#endif
    if (win_icon_file) {
      ExtractIconExW(win_icon_file, icon_index, &large_icon, &small_icon, 1);
      free(win_icon_file);
    }
    if (!large_icon) {
      small_icon = 0;
      uint err = GetLastError();
      if (err) {
        int wmlen = 1024;  // size of heap-allocated array
        wchar winmsg[wmlen];  // constant and < 1273 or 1705 => issue #530
        //wchar * winmsg = newn(wchar, wmlen);  // free below!
        FormatMessageW(
          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_MAX_WIDTH_MASK,
          0, err, 0, winmsg, wmlen, 0
        );
        show_iconwarn(winmsg);
      }
      else
        show_iconwarn(null);
    }
    delete(icon_file);
  }

  // Set the AppID if specified and the required function is available.
  if (*cfg.app_id) {
    HMODULE shell = load_sys_library("shell32.dll");
    HRESULT (WINAPI *pSetAppID)(PCWSTR) =
      (void *)GetProcAddress(shell, "SetCurrentProcessExplicitAppUserModelID");

    if (pSetAppID)
      pSetAppID(cfg.app_id);
  }

  inst = GetModuleHandle(NULL);

  // Window class name.
  wstring wclass = W(APPNAME);
  if (*cfg.classname)
    wclass = cfg.classname;

  // Put child command line into window title if we haven't got one already.
  wstring wtitle = cfg.title;
  if (!*wtitle) {
    size_t len;
    char *argz;
    argz_create(argv, &argz, &len);
    argz_stringify(argz, len, ' ');
    char * title = argz;
    size_t size = cs_mbstowcs(0, title, 0) + 1;
    if (size) {
      wchar *buf = newn(wchar, size);
      cs_mbstowcs(buf, title, size);
      wtitle = buf;
    }
    else {
      print_error(_("Using default title due to invalid characters in program name"));
      wtitle = W(APPNAME);
    }
  }

  // The window class.
  class_atom = RegisterClassExW(&(WNDCLASSEXW){
    .cbSize = sizeof(WNDCLASSEXW),
    .style = 0,
    .lpfnWndProc = win_proc,
    .cbClsExtra = 0,
    .cbWndExtra = 0,
    .hInstance = inst,
    .hIcon = large_icon ?: LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON)),
    .hIconSm = small_icon,
    .hCursor = LoadCursor(null, IDC_IBEAM),
    .hbrBackground = null,
    .lpszMenuName = null,
    .lpszClassName = wclass,
  });


  // Initialise the fonts, thus also determining their width and height.
  win_init_fonts(cfg.font.size);

  // Reconfigure the charset module now that arguments have been converted,
  // the locale/charset settings have been loaded, and the font width has
  // been determined.
  cs_reconfig();

  // Determine window sizes.
#if 0
  if (per_monitor_dpi_aware && pGetDpiForMonitor) {
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    uint x;
    pGetDpiForMonitor(mon, 0, &x, &dpi);  // MDT_EFFECTIVE_DPI
  }
#endif
  win_adjust_borders(cell_width * term_cols, cell_height * term_rows);

  // Having x == CW_USEDEFAULT but not y still triggers default positioning,
  // whereas y == CW_USEDEFAULT but not x results in an invisible window,
  // so to avoid the latter,
  // require both x and y to be set for custom positioning.
  if (cfg.y == (int)CW_USEDEFAULT)
    cfg.x = CW_USEDEFAULT;

  int x = cfg.x;
  int y = cfg.y;

#define dont_debug_position
#ifdef debug_position
#define printpos(tag, x, y, mon)	printf("%s %d %d (%d %d %d %d)\n", tag, x, y, (int)mon.left, (int)mon.top, (int)mon.right, (int)mon.bottom);
#else
#define printpos(tag, x, y, mon)
#endif

  // Create initial window.
  wnd = CreateWindowExW(cfg.scrollbar < 0 ? WS_EX_LEFTSCROLLBAR : 0,
                        wclass, wtitle,
                        window_style | (cfg.scrollbar ? WS_VSCROLL : 0),
                        x, y, width, height,
                        null, null, inst, null);
  // Workaround for failing title parameter:
  if (pEnableNonClientDpiScaling)
    SetWindowTextW(wnd, wtitle);

  tab_wnd = CreateWindowW(WC_TABCONTROLW, 0, 
                          WS_CHILD | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_OWNERDRAWFIXED, 
                          0, 0, width, win_tab_height(), 
                          wnd, NULL, inst, NULL);
  TabCtrl_SetMinTabWidth(tab_wnd, 100);

  // Adapt window position (and maybe size) to special parameters
  // also select monitor if requested
  if (center || right || bottom || left || top || maxwidth || maxheight
      || monitor > 0
     ) {
    MONITORINFO mi;
    get_my_monitor_info(&mi);
    RECT ar = mi.rcWork;
    printpos("cre", x, y, ar);

    if (monitor > 0) {
      MONITORINFO monmi;
      get_monitor_info(monitor, &monmi);
      RECT monar = monmi.rcWork;

      if (x == (int)CW_USEDEFAULT) {
        // Shift and scale assigned default position to selected monitor.
        win_get_pos(&x, &y);
        printpos("def", x, y, ar);
        x = monar.left + (x - ar.left) * (monar.right - monar.left) / (ar.right - ar.left);
        y = monar.top + (y - ar.top) * (monar.bottom - monar.top) / (ar.bottom - ar.top);
      }
      else {
        // Shift selected position to selected monitor.
        x += monar.left - ar.left;
        y += monar.top - ar.top;
      }

      ar = monar;
      printpos("mon", x, y, ar);
    }

    if (cfg.x == (int)CW_USEDEFAULT) {
      if (monitor == 0)
        win_get_pos(&x, &y);
      if (left || right)
        cfg.x = 0;
      if (top || bottom)
        cfg.y = 0;
        printpos("fix", x, y, ar);
    }

    if (left)
      x = ar.left + cfg.x;
    else if (right)
      x = ar.right - cfg.x - width;
    else if (center)
      x = (ar.left + ar.right - width) / 2;
    if (top)
      y = ar.top + cfg.y;
    else if (bottom)
      y = ar.bottom - cfg.y - height;
    else if (center)
      y = (ar.top + ar.bottom - height) / 2;
      printpos("pos", x, y, ar);

    if (maxwidth) {
      x = ar.left;
      width = ar.right - ar.left;
    }
    if (maxheight) {
      y = ar.top;
      height = ar.bottom - ar.top;
    }
    printpos("fin", x, y, ar);

    SetWindowPos(wnd, NULL, x, y, width, height,
      SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
  }

  if (per_monitor_dpi_aware) {
    if (cfg.x != (int)CW_USEDEFAULT) {
      // The first SetWindowPos actually set x and y
      SetWindowPos(wnd, NULL, x, y, width, height,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
      // Then, we are placed the windows on the correct monitor and we can
      // now interpret width/height in correct DPI.
      SetWindowPos(wnd, NULL, x, y, width, height,
        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }
    // retrieve initial monitor DPI
    if (pGetDpiForMonitor) {
      HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
      uint x;
      pGetDpiForMonitor(mon, 0, &x, &dpi);  // MDT_EFFECTIVE_DPI
#ifdef debug_dpi
      uint ang, raw;
      pGetDpiForMonitor(mon, 1, &x, &ang);  // MDT_ANGULAR_DPI
      pGetDpiForMonitor(mon, 2, &x, &raw);  // MDT_RAW_DPI
      printf("initial dpi eff %d ang %d raw %d\n", dpi, ang, raw);
      print_system_metrics(dpi, "initial");
#endif
      // recalculate effective font size and adjust window
      if (dpi != 96) {
        font_cs_reconfig(true);
        win_set_chars(cfg.rows, cfg.cols);
      }
    }
  }

  if (border_style) {
    LONG style = GetWindowLong(wnd, GWL_STYLE);
    if (strcmp(border_style, "void") == 0) {
      style &= ~(WS_CAPTION | WS_BORDER | WS_THICKFRAME);
    }
    else {
      style &= ~(WS_CAPTION | WS_BORDER);
    }
    SetWindowLong(wnd, GWL_STYLE, style);
    SetWindowPos(wnd, null, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }

  configure_taskbar();

  // The input method context.
  imc = ImmGetContext(wnd);

  // Correct autoplacement, which likes to put part of the window under the
  // taskbar when the window size approaches the work area size.
  if (cfg.x == (int)CW_USEDEFAULT) {
    win_fix_position();
  }

  // Initialise the terminal.

  // TODO: should refactor win_tab_init to just initialize tabs-system and
  // create tabls with separate function (more general version of
  // win_tab_create. Would be cleaner and no need for win_tab_set_argv etc

  if (current_tab_size == 0) {
    win_tab_init(home, cmd, argv, term_width, term_height, tablist_title[0]);
  }
  else {
    for (int i = 0; i < current_tab_size; i++) {
      if (tablist[i] != NULL) {
        char *tabexec = tablist[i];
        char *tab_argv[4] = { cmd, "-c", tabexec, NULL };

        win_tab_init(home, cmd, tab_argv, term_width, term_height, tablist_title[i]);
      }
    }

    win_tab_set_argv(argv);
  }

  term_initialized = 1;

  setenv("CHERE_INVOKING", "1", false);
  
  child_init();

  // Initialise the scroll bar.
  SetScrollInfo(
    wnd, SB_VERT,
    &(SCROLLINFO){
      .cbSize = sizeof(SCROLLINFO),
      .fMask = SIF_ALL | SIF_DISABLENOSCROLL,
      .nMin = 0, .nMax = term_rows - 1,
      .nPage = term_rows, .nPos = 0,
    },
    false
  );

  // Set up an empty caret bitmap. We're painting the cursor manually.
  caretbm = CreateBitmap(1, cell_height, 1, 1, newn(short, cell_height));
  CreateCaret(wnd, caretbm, 0, 0);

  // Initialise various other stuff.
  win_init_drop_target();
  win_init_menus();
  update_transparency();

  // Finally show the window!
  go_fullscr_on_max = (cfg.window == -1);
  ShowWindow(wnd, go_fullscr_on_max ? SW_SHOWMAXIMIZED : cfg.window);
  SetFocus(wnd);

  // Message loop.
  do {
    MSG msg;
    while (PeekMessage(&msg, null, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT)
        return msg.wParam;
      if (!IsDialogMessage(config_wnd, &msg))
        DispatchMessage(&msg);
    }
    child_proc();
    win_tab_clean();
  } while (!win_should_die());
  win_tab_clean();
}
