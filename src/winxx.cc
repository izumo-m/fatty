#include <windows.h>
#include <set>
#include <tuple>
#include <vector>
#include <algorithm>
#include <climits>
#include <string>
#include <sstream>

#include <unistd.h>
#include <stdlib.h>

#include "win.hh"

#include <d2d1.h>

#include <CommCtrl.h>

extern "C" {
  #include "winpriv.h"
  #include "winsearch.h"
  
  extern wchar * cs__mbstowcs(const char * s);
}

#define lengthof(array) (sizeof(array) / sizeof(*(array)))

using std::tuple;
using std::get;

typedef void (*CallbackFn)(void*);
typedef tuple<CallbackFn, void*> Callback;
typedef std::set<Callback> CallbackSet;

static CallbackSet callbacks;
static std::vector<Tab> tabs;
static unsigned int active_tab = 0;

static float g_xscale, g_yscale;

static void init_scale_factors() {
    static ID2D1Factory* d2d_factory = nullptr;
    if (d2d_factory == nullptr) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory);
    }
    float xdpi, ydpi;
    d2d_factory->ReloadSystemMetrics();
    d2d_factory->GetDesktopDpi(&xdpi, &ydpi);
    g_xscale = xdpi / 96.0f;
    g_yscale = ydpi / 96.0f;
}

Tab::Tab() : terminal(new term), chld(new child) {
    memset(terminal.get(), 0, sizeof(struct term));
    memset(chld.get(), 0, sizeof(struct child));
    info.attention = false;
    info.titles_i = 0;
    info.idx = 0;
}

Tab::~Tab() {
    if (terminal)
        term_free(terminal.get());
    if (chld)
        child_free(chld.get());
}

Tab::Tab(Tab&& t) {
    info = t.info;
    terminal = std::move(t.terminal);
    chld = std::move(t.chld);
}

Tab& Tab::operator=(Tab&& t) {
    std::swap(terminal, t.terminal);
    std::swap(chld, t.chld);
    std::swap(info, t.info);
    return *this;
}

extern "C" {

  void win_set_timer(CallbackFn cb, void* data, uint ticks) {
      auto result = callbacks.insert(std::make_tuple(cb, data));
      CallbackSet::iterator iter = result.first;
      SetTimer(wnd, reinterpret_cast<UINT_PTR>(&*iter), ticks, NULL);
  }
  
  void win_process_timer_message(WPARAM message) {
      void* pointer = reinterpret_cast<void*>(message);
      auto callback = *reinterpret_cast<Callback*>(pointer);
      callbacks.erase(callback);
      KillTimer(wnd, message);
  
      // call the callback
      get<0>(callback)( get<1>(callback) );
  }
  
  static void invalidate_tabs() {
      win_invalidate_all(false);
  }
  
  term* win_active_terminal() {
      if (active_tab >= tabs.size()) {
        if (tabs.size() == 0) {
          return NULL;
        }
        active_tab = tabs.size() - 1;
      }
      return tabs.at(active_tab).terminal.get();
  }

  int win_tab_count() {
    return tabs.size();
  }

  int win_active_tab() {
    return active_tab;
  }
  
  static void update_window_state() {
      win_update_menus(false /*should this be true?*/);
      if (cfg.title_settable)
        SetWindowTextW(wnd, win_tab_get_title(active_tab));
      win_adapt_term_size(false, false);
  }
  
  static void set_active_tab(unsigned int index) {
      if (index >= tabs.size()) {
        active_tab = tabs.size() - 1;
      } else {
        active_tab = index;
      }

      SendMessage(tab_wnd, TCM_SETCURSEL, index, 0);
      Tab* active = &tabs.at(active_tab);
      for (Tab& tab : tabs) {
          term_set_focus(tab.terminal.get(), &tab == active, false);
      }
      active->info.attention = false;
      SetFocus(wnd);
      
      struct term *term = active->terminal.get();
      term->results.update_type = DISABLE_UPDATE;
      if (IsWindowVisible(win_get_search_wnd()) != term->search_window_visible) {
        ShowWindow(win_get_search_wnd(), term->search_window_visible ? SW_SHOW : SW_HIDE);
      }
      if (term->search_window_visible) {
        if (term->results.query) {
          SetWindowTextW(win_get_search_edit_wnd(), term->results.query);
        }
        else {
          SetWindowTextW(win_get_search_edit_wnd(), L"");
        }
      }
  
      update_window_state();
      win_invalidate_all(false);
      term->results.update_type = NO_UPDATE;
  }
  
  static unsigned int rel_index(int change) {
      return (int(active_tab) + change + tabs.size()) % tabs.size();
  }
  
  void win_tab_change(int change) {
      set_active_tab(rel_index(change));
  }
  
  void win_tab_move(int amount) {
      unsigned int new_idx = rel_index(amount);
      std::swap(tabs[active_tab], tabs[new_idx]);
      TCITEMW tie; 
      tie.mask = TCIF_TEXT; 
      tie.pszText = win_tab_get_title(active_tab);
      SendMessageW(tab_wnd, TCM_SETITEMW, active_tab, (LPARAM)&tie);
      tie.mask = TCIF_TEXT; 
      tie.pszText = win_tab_get_title(new_idx);
      SendMessageW(tab_wnd, TCM_SETITEMW, new_idx, (LPARAM)&tie);
      set_active_tab(new_idx);
  }
  
  static std::vector<Tab>::iterator tab_by_term(struct term* term) {
      std::vector<Tab>::iterator match = find_if(tabs.begin(), tabs.end(), [=](Tab& tab) {
              return tab.terminal.get() == term; });
      return match;
  }
  
  static char* g_home;
  static char* g_cmd;
  static char** g_argv;
  
  static void newtab(
          unsigned short rows, unsigned short cols,
          unsigned short width, unsigned short height, const char* cwd, char* title) {
      LRESULT res;      
      TCITEM tie; 
      tie.mask = TCIF_TEXT; 
      if (title) {
        tie.pszText = title;
      } else {
        tie.pszText = g_cmd;
      }
      res = SendMessage(tab_wnd, TCM_INSERTITEM, tabs.size(), (LPARAM)&tie);
      if (res == -1) return;
      tabs.push_back(Tab());
      Tab& tab = tabs.back();
      tab.terminal->child = tab.chld.get();
      term_reset(tab.terminal.get(), true);
      tab.terminal.get()->show_scrollbar = !!cfg.scrollbar;  // hotfix #597
      term_resize(tab.terminal.get(), rows, cols);
      tab.chld->cmd = g_cmd;
      tab.chld->home = g_home;
      struct winsize wsz{rows, cols, width, height};
      child_create(tab.chld.get(), tab.terminal.get(), g_argv, &wsz, cwd);
      tab.info.idx = tabs.size()-1;
      wchar *ws = cs__mbstowcs(tie.pszText);
      win_tab_set_title(tab.terminal.get(), ws);
      free(ws);
  }
  
  static void set_tab_bar_visibility(bool b);
  
  void win_tab_set_argv(char** argv) {
      g_argv = argv;
  }
  
  void win_tab_init(char* home, char* cmd, char** argv, int width, int height, char* title) {
      g_home = home;
      g_cmd = cmd;
      g_argv = argv;
      newtab(cfg.rows, cfg.cols, width, height, nullptr, title);
      set_tab_bar_visibility(tabs.size() > 1);
  }
  
  void win_tab_create() {
      struct term t = *tabs[active_tab].terminal;
      std::stringstream cwd_path;
      cwd_path << "/proc/" << t.child->pid << "/cwd";
      char* cwd = realpath(cwd_path.str().c_str(), 0);
      newtab(t.rows, t.cols, t.cols * cell_width, t.rows * cell_height, cwd, nullptr);
      free(cwd);
      set_active_tab(tabs.size() - 1);
      set_tab_bar_visibility(tabs.size() > 1);
  }
  
  void win_tab_delete(struct term* term, bool point_blank) {
      std::vector<Tab>::iterator tab_it = tab_by_term(term);
      if (tab_it == tabs.end()) return;
      Tab& tab = *tab_it;
      struct term *terminal = tab.terminal.get();
      if (!terminal) return;
      struct child *child = tab.chld.get();
      if (!child) return;
      pid_t pid = child->pid;
      if (!(pid)) return;
      auto cb = std::find_if(callbacks.begin(), callbacks.end(), [terminal](Callback x) {
        return ((struct term *)(get<1>(x)) == terminal); });
      if (cb != callbacks.end()) {
        KillTimer(wnd, reinterpret_cast<UINT_PTR>(&*cb));
        callbacks.erase(cb);
      }
      unsigned int old_active_tab = active_tab;
      SendMessage(tab_wnd, TCM_SETCURSEL, 0, 0);
      if (old_active_tab + 1 >= tabs.size()) {
        if (old_active_tab > 0) {
          SendMessage(tab_wnd, TCM_SETCURSEL, old_active_tab - 1, 0);
        }
      } else {
        SendMessage(tab_wnd, TCM_SETCURSEL, old_active_tab + 1, 0);
      }
      SendMessage(tab_wnd, TCM_DELETEITEM, tab.info.idx, 0);
      child_terminate(child, point_blank);
      for (unsigned int i = old_active_tab; i < tabs.size(); i++) {
        tabs.at(i).info.idx--;
      }
      tabs.erase(tab_it);
      set_active_tab(active_tab > old_active_tab ? old_active_tab : active_tab);
      if (tabs.size() > 0) {
          set_tab_bar_visibility(tabs.size() > 1);
          win_invalidate_all(false);
      }
  }
  
  void win_tab_clean() {
      bool invalidate = false;
      for (;;) {
          std::vector<Tab>::iterator it = std::find_if(tabs.begin(), tabs.end(), [](Tab& x) {
                  return x.chld->pid == 0; });
          if (it == tabs.end()) break;
          invalidate = true;
          for (;;) {
            auto cb = std::find_if(callbacks.begin(), callbacks.end(), [&it](Callback x) {
              return ((struct term *)(get<1>(x)) == (*it).terminal.get()); });
            if (cb == callbacks.end()) break;
            KillTimer(wnd, reinterpret_cast<UINT_PTR>(&*cb));
            callbacks.erase(cb);
          }
          unsigned int old_active_tab = active_tab;
          SendMessage(tab_wnd, TCM_SETCURSEL, 0, 0);
          if (old_active_tab + 1 >= tabs.size()) {
            if (old_active_tab > 0) {
              SendMessage(tab_wnd, TCM_SETCURSEL, old_active_tab - 1, 0);
            }
          } else {
            SendMessage(tab_wnd, TCM_SETCURSEL, old_active_tab + 1, 0);
          }
          SendMessage(tab_wnd, TCM_DELETEITEM, (*it).info.idx, 0);
          for (unsigned int i = active_tab; i < tabs.size(); i++) {
            tabs.at(i).info.idx--;
          }
          tabs.erase(it);
          active_tab = active_tab > old_active_tab ? old_active_tab : active_tab;
      }
      if (invalidate && tabs.size() > 0) {
          set_active_tab(active_tab);
          set_tab_bar_visibility(tabs.size() > 1);
          win_invalidate_all(false);
      }
  }
  
  void win_tab_attention(struct term* term) {
      std::vector<Tab>::iterator tab_it = tab_by_term(term);
      if (tab_it == tabs.end()) return;
      Tab& tab = *tab_it;
      tab.info.attention = true;
      invalidate_tabs();
  }
  
  void win_tab_set_title(struct term* term, wchar_t* title) {
      std::vector<Tab>::iterator tab_it = tab_by_term(term);
      if (tab_it == tabs.end()) return;
      Tab& tab = *tab_it;
      if (tab.info.titles[tab.info.titles_i] != title) {
          tab.info.titles[tab.info.titles_i] = title;
          invalidate_tabs();
      }
      TCITEMW tie; 
      tie.mask = TCIF_TEXT; 
      tie.pszText = (wchar *)tab.info.titles[tab.info.titles_i].data();
      SendMessageW(tab_wnd, TCM_SETITEMW, tab.info.idx, (LPARAM)&tie);
      if (term == win_active_terminal()) {
        win_set_title((wchar *)tab.info.titles[tab.info.titles_i].data());
      }
  }
  
  wchar_t* win_tab_get_title(unsigned int idx) {
      return (wchar_t *)tabs[idx].info.titles[tabs[idx].info.titles_i].c_str();
  }
  
  void win_tab_title_push(struct term* term) {
    std::vector<Tab>::iterator tab_it = tab_by_term(term);
    if (tab_it == tabs.end()) return;
    Tab& tab = *tab_it;
    if (tab.info.titles_i == lengthof(tab.info.titles))
      tab.info.titles_i = 0;
    else
      tab.info.titles_i++;
  }
    
  wchar_t* win_tab_title_pop(struct term* term) {
    std::vector<Tab>::iterator tab_it = tab_by_term(term);
    if (tab_it == tabs.end()) return wcsdup(L"");
    Tab& tab = *tab_it;
    if (!tab.info.titles_i)
      tab.info.titles_i = lengthof(tab.info.titles);
    else
      tab.info.titles_i--;
    return win_tab_get_title(active_tab);
  }
  
  /*
   * Title stack (implemented as fixed-size circular buffer)
   */
  void
  win_tab_save_title(struct term* term)
  {
    win_tab_title_push(term);
  }
  
  void
  win_tab_restore_title(struct term* term)
  {
    wchar_t *title = win_tab_title_pop(term);
    win_tab_set_title(term, title);
    free(title);
  }
  
  bool win_should_die() {
      return tabs.size() == 0;
  }
  
  static int tabheight() {
      init_scale_factors();
      RECT tr;
      SendMessage(tab_wnd, TCM_GETITEMRECT, 0, (LPARAM)&tr);
      return tr.bottom;
  }
  
  static bool tab_bar_visible = false;
  
  static void set_tab_bar_visibility(bool b) {
      if (b == tab_bar_visible) return;
  
      tab_bar_visible = b;
      g_render_tab_height = win_tab_height();
      win_adapt_term_size(false, false);
      win_invalidate_all(false);
  }
  
  int win_tab_height() {
    return tab_bar_visible ? tabheight() : 0;
  }
  
  static int tab_font_size() {
      return 14 * g_yscale;
  }
  
  static HGDIOBJ new_tab_font() {
      return CreateFont(tab_font_size(),0,0,0,FW_NORMAL,0,0,0,1,0,0,CLEARTYPE_QUALITY,0,0);
  }
  
  static HGDIOBJ new_active_tab_font() {
      return CreateFont(tab_font_size(),0,0,0,FW_BOLD,0,0,0,1,0,0,CLEARTYPE_QUALITY,0,0);
  }
  
  // Wrap GDI object for automatic release
  struct SelectWObj {
      HDC tdc;
      HGDIOBJ old;
      SelectWObj(HDC dc, HGDIOBJ obj) { tdc = dc; old = SelectObject(dc, obj); }
      ~SelectWObj() { DeleteObject(SelectObject(tdc, old)); }
  };
  
  void win_paint_tabs(LPARAM lp, int width) {
      RECT loc_tabrect;
      GetWindowRect(tab_wnd, &loc_tabrect);
      if (width) 
        loc_tabrect.right = loc_tabrect.left + width;
      else
        width = loc_tabrect.right - loc_tabrect.left;
      SetWindowPos(tab_wnd, 0, 0, 0, width, win_tab_height(), tab_bar_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
  
      if (!tab_bar_visible) return;
  
      HDC dc = GetDC(wnd);
      SetRect(&loc_tabrect, 0, win_tab_height(), width, win_tab_height() + PADDING);
      const auto brush = CreateSolidBrush(cfg.bg_colour);
      FillRect(dc, &loc_tabrect, brush);
      DeleteObject(brush);
      ReleaseDC(wnd, dc);
      
      if (lp == 0) return;
  
      LPDRAWITEMSTRUCT lpdis = (DRAWITEMSTRUCT *)lp;
        
      // index of tab being drawn
      unsigned int Index = (unsigned int)lpdis->itemID;
      if (Index >= tabs.size()) return;
  
      // device context to draw on
      dc = lpdis->hDC;
      if (dc == NULL) return;
  
      // bounding rectangle of current tab
      RECT tabrect = lpdis->rcItem;
  
      const auto bg = cfg.tab_bg_colour;
      const auto fg = cfg.tab_fg_colour;
      const auto active_bg = cfg.tab_active_bg_colour;
      const auto attention_bg = cfg.tab_attention_bg_colour;
  
      const int tabwidth = tabrect.right - tabrect.left;
      const int tabheight = tabrect.bottom - tabrect.top;
  
      SetRect(&loc_tabrect, 0, 0, tabwidth, tabheight);
      HDC bufdc = CreateCompatibleDC(dc);
      SetBkMode(bufdc, TRANSPARENT);
      SetTextColor(bufdc, fg);
      SetTextAlign(bufdc, TA_CENTER);
      {
          auto brush = CreateSolidBrush(bg);
          auto obrush = SelectWObj(bufdc, brush);
          auto open = SelectWObj(bufdc, CreatePen(PS_SOLID, 0, fg));
          auto obuf = SelectWObj(bufdc,
                  CreateCompatibleBitmap(dc, tabwidth, tabheight));
  
          auto ofont = SelectWObj(bufdc, new_tab_font());
  
          if (Index == active_tab) {
              auto activebrush = CreateSolidBrush(active_bg);
              FillRect(bufdc, &loc_tabrect, activebrush);
              DeleteObject(activebrush);
          } else if (tabs[Index].info.attention) {
              auto activebrush = CreateSolidBrush(attention_bg);
              FillRect(bufdc, &loc_tabrect, activebrush);
              DeleteObject(activebrush);
          } else {
              FillRect(bufdc, &loc_tabrect, brush);
          }
  
          if (Index == active_tab) {
              auto _f = SelectWObj(bufdc, new_active_tab_font());
              TextOutW(bufdc, tabwidth/2, (tabheight - tab_font_size()) / 2, win_tab_get_title(Index), wcslen(win_tab_get_title(Index)));
          } else {
              TextOutW(bufdc, tabwidth/2, (tabheight - tab_font_size()) / 2, win_tab_get_title(Index), wcslen(win_tab_get_title(Index)));
          }
  
          BitBlt(dc, tabrect.left, tabrect.top, tabwidth, tabheight,
                  bufdc, 0, 0, SRCCOPY);
      }
      DeleteDC(bufdc);
  }
  
  void win_for_each_term(void (*cb)(struct term* term)) {
      for (Tab& tab : tabs)
          cb(tab.terminal.get());
  }
  
  void win_for_each_term_bool(void (*cb)(struct term* term, bool param), bool param) {
      for (Tab& tab : tabs)
          cb(tab.terminal.get(), param);
  }
  
  void win_tab_mouse_click(int x) {
      set_active_tab(x);
  }
  
} /* extern C */

std::vector<Tab>& win_tabs() {
    return tabs;
}

static void lambda_callback(void* data) {
    auto callback = static_cast<std::function<void()>*>(data);
    (*callback)();
    delete callback;
}
void win_callback(unsigned int ticks, std::function<void()> callback) {
    win_set_timer(lambda_callback, new std::function<void()>(callback), ticks);
}

