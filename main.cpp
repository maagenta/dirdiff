/*
  DirDiff - directory comparer built on WDL/SWELL.

  Windows: native Win32 (common controls v6, Explorer theme).
  macOS:   SWELL on Cocoa (NSOutlineView); entry point in mac_main.mm.
  Linux:   generic SWELL on GDK (controls drawn with LICE).

  Usage:
    dirdiff [--content] [--hide-same] [--files] [--expand=N] [--select=relative/path] [dirA dirB]

  The UI is two SysTreeView32 controls with the same structure (the union of
  the two trees). Each directory shows how many files in its subtree
  differ: "photos (201)". Expanding, collapsing, selecting and scrolling in
  one tree is mirrored in the other.
*/

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shobjidl.h>
#include <shellapi.h>
#include "../WDL/win32_utf8.h"
#endif

#include "../WDL/swell/swell.h"
#include "../WDL/wingui/wndsize.h"
#include "../WDL/wdlstring.h"
#include "../WDL/wdlcstring.h"
#include "../WDL/ptrlist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "resource.h"
#include "diffengine.h"

#ifdef _WIN32
#define DD_SEP_STR "\\"
#define IS_TVN_ITEMEXPANDING(c) ((c) == TVN_ITEMEXPANDINGA || (c) == TVN_ITEMEXPANDINGW)
#define IS_TVN_SELCHANGED(c) ((c) == TVN_SELCHANGEDA || (c) == TVN_SELCHANGEDW)
#else
#define DD_SEP_STR "/"
#define IS_TVN_ITEMEXPANDING(c) ((c) == TVN_ITEMEXPANDING)
#define IS_TVN_SELCHANGED(c) ((c) == TVN_SELCHANGED)
#endif

#ifdef __APPLE__
void DirDiff_MacLinkScroll(HWND a, HWND b);   // mac_main.mm
void DirDiff_MacUnlinkScroll();
#endif

#define APP_TITLE "Directory Comparer"
#define IDT_SCAN 1
#define WM_DD_AFTEREXPAND (WM_USER + 100)
#define WM_DD_SYNCSCROLL (WM_USER + 101)

/* ----------------------------------------------------------------------- */
/* State                                                                    */
/* ----------------------------------------------------------------------- */

HINSTANCE g_hInst;
static HWND g_hwnd;
static HWND g_tree[2];
static WDL_WndSizer g_sizer;
static POINT g_minSize;

static DiffDir *g_root;            // displayed result (owned by the UI)
static WDL_FastString g_rootName[2];
static int g_resultGen;            // changes with every new result (validates deferred messages)
static double g_lastSeconds;
static int g_lastMethod = -1;

static DiffScanner *g_scan;        // scan in progress, or NULL
static HANDLE g_thread;
static DWORD g_scanStart;

static DiffNode *g_sel;            // selected node (the same in both trees)
static int g_sync;                 // >0 while an action is being mirrored in the other tree

static int g_method = CMP_SIZE_DATE;
static bool g_hideSame, g_showFiles;

// command line
static WDL_FastString g_cliPath[2], g_cliSelect;

// expansion and selection restored when comparing again
static WDL_PtrList<char> g_keepExpanded;
static WDL_FastString g_keepSelected;
static bool g_keepValid;
static int g_expandLevels = 1;
static bool g_autoCompare;

#ifndef _WIN32
const char **g_argv;
int g_argc;
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
static bool g_quit;
#endif

/* ----------------------------------------------------------------------- */
/* Formatting                                                               */
/* ----------------------------------------------------------------------- */

// 12408 -> "12,408"; four-digit numbers have no separator
static void FmtInt(WDL_INT64 v, char *buf, int bufsz)
{
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
  const int len = (int)strlen(tmp), neg = tmp[0] == '-';
  if (len - neg <= 4)
  {
    lstrcpyn_safe(buf, tmp, bufsz);
    return;
  }
  char out[48];
  int o = 0;
  for (int i = 0; i < len; i++)
  {
    if (i > neg && ((len - i) % 3) == 0) out[o++] = ',';
    out[o++] = tmp[i];
  }
  out[o] = 0;
  lstrcpyn_safe(buf, out, bufsz);
}

static void FmtSize(WDL_INT64 b, char *buf, int bufsz)
{
  if (b < 1024)
  {
    snprintf(buf, bufsz, "%d bytes", (int)b);
    return;
  }
  static const char *units[] = { "KB", "MB", "GB", "TB" };
  double v = (double)b / 1024.0;
  int u = 0;
  while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
  snprintf(buf, bufsz, "%.1f %s", v, units[u]);
}

static void FmtTime(WDL_INT64 t, char *buf, int bufsz)
{
  time_t tt = (time_t)t;
  struct tm tmv;
#ifdef _WIN32
  if (localtime_s(&tmv, &tt)) { buf[0] = 0; return; }
#else
  if (!localtime_r(&tt, &tmv)) { buf[0] = 0; return; }
#endif
  strftime(buf, bufsz, "%Y-%m-%d %H:%M", &tmv);
}

// path relative to the root ("photos/2024/march"), with the given separator
static void RelJoin(const DiffNode *n, const char *sep, WDL_FastString *out)
{
  out->Set("");
  WDL_PtrList<const char> parts;
  for (const DiffNode *p = n; p && p->parent; p = p->parent) parts.Add(p->name);
  for (int i = parts.GetSize() - 1; i >= 0; i--)
  {
    out->Append(parts.Get(i));
    if (i) out->Append(sep);
  }
}

// for the status bar: the root is shown by its name
static void RelPath(const DiffNode *n, WDL_FastString *out)
{
  if (n && !n->parent) out->Set(g_rootName[0].Get());
  else RelJoin(n, DD_SEP_STR, out);
}

// text of an item in tree "side" (SIDE_A or SIDE_B)
static void NodeLabel(const DiffNode *n, int side, WDL_FastString *out)
{
  if (n->isDir)
  {
    const DiffDir *d = (const DiffDir *)n;
    out->Set(d->parent ? d->name : g_rootName[side].Get());
    if (d->Diffs() > 0)
    {
      char num[32];
      FmtInt(d->Diffs(), num, sizeof(num));
      out->Append(" (");
      out->Append(num);
      out->Append(")");
    }
    if (d->presence == PRESENT_A) out->Append(side == SIDE_A ? " [only in A]" : " [missing]");
    else if (d->presence == PRESENT_B) out->Append(side == SIDE_B ? " [only in B]" : " [missing]");
    if (d->readError & (1 << side)) out->Append(" [read error]");
    return;
  }
  const DiffFile *f = (const DiffFile *)n;
  out->Set(f->name);
  switch (f->status)
  {
    case FS_MODIFIED: out->Append(" [modified]"); break;
    case FS_ONLY_A: out->Append(side == SIDE_A ? " [only in A]" : " [missing]"); break;
    case FS_ONLY_B: out->Append(side == SIDE_B ? " [only in B]" : " [missing]"); break;
    case FS_ERROR: out->Append(" [error]"); break;
  }
}

/* ----------------------------------------------------------------------- */
/* Trees                                                                    */
/* ----------------------------------------------------------------------- */

static bool NodeVisible(const DiffNode *n)
{
  if (!g_hideSame || !n->parent) return true;
  if (n->isDir) return ((const DiffDir *)n)->differs;
  return ((const DiffFile *)n)->status != FS_SAME;
}

static bool HasVisibleChildren(const DiffDir *d)
{
  if (g_hideSame) return d->nDiffSubdirs > 0 || (g_showFiles && d->nDirectDiffs > 0);
  return d->dirs.GetSize() > 0 || (g_showFiles && d->files.GetSize() > 0);
}

static HTREEITEM InsertNode(int side, HTREEITEM parent, DiffNode *n)
{
  WDL_FastString label;
  NodeLabel(n, side, &label);

  TV_INSERTSTRUCT tis;
  memset(&tis, 0, sizeof(tis));
  tis.hParent = parent;
  tis.hInsertAfter = TVI_LAST;
  tis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  tis.item.pszText = (char *)label.Get();
  tis.item.lParam = (LPARAM)n;
  // with cChildren=1 the expand button appears even though the children have
  // not been inserted yet: they are inserted on demand in TVN_ITEMEXPANDING
  tis.item.cChildren = (n->isDir && HasVisibleChildren((const DiffDir *)n)) ? 1 : 0;

  HTREEITEM h = TreeView_InsertItem(g_tree[side], &tis);
  n->ui[side] = (INT_PTR)h;
  return h;
}

static void PopulateChildren(DiffDir *d, int side)
{
  const unsigned char bit = (unsigned char)(1 << side);
  if (!d->ui[side] || (d->uiPopulated & bit)) return;
  d->uiPopulated |= bit;

  HTREEITEM parent = (HTREEITEM)d->ui[side];
  for (int i = 0; i < d->dirs.GetSize(); i++)
  {
    DiffDir *c = d->dirs.Get(i);
    if (NodeVisible(c)) InsertNode(side, parent, c);
  }
  if (g_showFiles)
  {
    for (int i = 0; i < d->files.GetSize(); i++)
    {
      DiffFile *f = d->files.Get(i);
      if (NodeVisible(f)) InsertNode(side, parent, f);
    }
  }
}

// forget the HTREEITEMs before rebuilding the trees
static void ResetUi(DiffDir *d)
{
  d->ui[0] = d->ui[1] = 0;
  const bool populated = d->uiPopulated != 0;
  d->uiPopulated = 0;
  if (!populated) return;
  for (int i = 0; i < d->files.GetSize(); i++)
  {
    DiffFile *f = d->files.Get(i);
    f->ui[0] = f->ui[1] = 0;
  }
  for (int i = 0; i < d->dirs.GetSize(); i++) ResetUi(d->dirs.Get(i));
}

static void ExpandSaved(DiffDir *d)
{
  if (!d->uiExpanded || !d->ui[0] || !d->ui[1] || !HasVisibleChildren(d)) return;
  for (int s = 0; s < 2; s++) PopulateChildren(d, s);
  for (int s = 0; s < 2; s++) TreeView_Expand(g_tree[s], (HTREEITEM)d->ui[s], TVE_EXPAND);
  for (int i = 0; i < d->dirs.GetSize(); i++)
  {
    DiffDir *c = d->dirs.Get(i);
    if (c->ui[0]) ExpandSaved(c);
  }
}

static bool IsDescendant(const DiffNode *n, const DiffDir *d)
{
  for (const DiffDir *p = n ? n->parent : NULL; p; p = p->parent)
    if (p == d) return true;
  return false;
}

static void UpdateSelectionStatus();

static void SelectNode(DiffNode *n)
{
  if (!n || !n->ui[0] || !n->ui[1]) return;
  g_sel = n;
  g_sync++;
  for (int s = 0; s < 2; s++)
  {
    TreeView_SelectItem(g_tree[s], (HTREEITEM)n->ui[s]);
    TreeView_EnsureVisible(g_tree[s], (HTREEITEM)n->ui[s]); // SWELL/Cocoa does not scroll on select
  }
  g_sync--;
  UpdateSelectionStatus();
}

// rebuilds both trees, preserving expansion and selection
static void BuildTrees()
{
  g_sync++;
  for (int s = 0; s < 2; s++) TreeView_DeleteAllItems(g_tree[s]);
  if (g_root)
  {
    ResetUi(g_root);
    for (int s = 0; s < 2; s++) InsertNode(s, TVI_ROOT, g_root);
    ExpandSaved(g_root);
  }
  g_sync--;

  DiffNode *sel = g_sel;
  while (sel && !sel->ui[0]) sel = sel->parent; // if it ended up hidden, its nearest visible ancestor
  if (!sel) sel = g_root;
  g_sel = NULL;
  if (sel) SelectNode(sel);
  else UpdateSelectionStatus();
}

/* ----------------------------------------------------------------------- */
/* Scroll synchronization                                                   */
/* ----------------------------------------------------------------------- */

#ifdef _WIN32
// aligns the first visible item of the other tree with this one's
static void SyncScrollFrom(int side)
{
  if (g_sync || !g_tree[0] || !g_tree[1]) return;
  HTREEITEM fv = TreeView_GetFirstVisible(g_tree[side]);
  if (!fv) return;
  TVITEM it;
  memset(&it, 0, sizeof(it));
  it.mask = TVIF_PARAM | TVIF_HANDLE;
  it.hItem = fv;
  if (!TreeView_GetItem(g_tree[side], &it)) return;
  const DiffNode *n = (const DiffNode *)it.lParam;
  const int other = !side;
  if (!n || !n->ui[other]) return;
  HTREEITEM target = (HTREEITEM)n->ui[other];
  if (TreeView_GetFirstVisible(g_tree[other]) == target) return;
  g_sync++;
  TreeView_Select(g_tree[other], target, TVGN_FIRSTVISIBLE);
  g_sync--;
}

static LRESULT CALLBACK TreeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR)
{
  const LRESULT r = DefSubclassProc(hwnd, msg, wParam, lParam);
  switch (msg)
  {
    case WM_VSCROLL:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_SIZE:
      SyncScrollFrom((int)id);
    break;
  }
  return r;
}
#elif !defined(__APPLE__)
// generic SWELL does not expose the treeview scroll position: the mouse
// wheel is forwarded to the other tree, which has the same rows
static WNDPROC g_treeOldProc[2];
static LRESULT WINAPI TreeWheelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  const int side = hwnd == g_tree[1] ? 1 : 0;
  const LRESULT r = CallWindowProc(g_treeOldProc[side], hwnd, msg, wParam, lParam);
  if (msg == WM_MOUSEWHEEL && !g_sync && g_tree[!side])
  {
    g_sync++;
    SendMessage(g_tree[!side], WM_MOUSEWHEEL, wParam, lParam);
    g_sync--;
  }
  return r;
}
#endif

/* ----------------------------------------------------------------------- */
/* Status bar                                                               */
/* ----------------------------------------------------------------------- */

static void UpdateSelectionStatus()
{
  if (!g_hwnd || g_scan) return;
  if (!g_root || !g_sel)
  {
    SetDlgItemText(g_hwnd, IDC_STATUS, "");
    return;
  }

  WDL_FastString s;
  RelPath(g_sel, &s);
  char a[64], b[64], c[64], d[64];

  if (g_sel->isDir)
  {
    const DiffDir *dd = (const DiffDir *)g_sel;
    const int n = dd->Diffs();
    if (!dd->differs) s.Append(" — no differences");
    else
    {
      FmtInt(n, a, sizeof(a));
      FmtInt(dd->nModified, b, sizeof(b));
      FmtInt(dd->nOnlyA, c, sizeof(c));
      FmtInt(dd->nOnlyB, d, sizeof(d));
      s.AppendFormatted(512, " — %s %s: %s %s, %s only in A, %s only in B",
                        a, "different", b, "modified", c, d);
      if (dd->nErrors)
      {
        FmtInt(dd->nErrors, a, sizeof(a));
        s.AppendFormatted(128, ", %s %s", a, dd->nErrors == 1 ? "error" : "errors");
      }
    }
    if (dd->presence == PRESENT_A) s.Append(" · the directory exists only in A");
    else if (dd->presence == PRESENT_B) s.Append(" · the directory exists only in B");
    if (dd->readError) s.Append(" · could not be read completely");
  }
  else
  {
    const DiffFile *f = (const DiffFile *)g_sel;
    switch (f->status)
    {
      case FS_SAME: s.Append(" — identical"); break;
      case FS_MODIFIED: s.Append(" — modified"); break;
      case FS_ONLY_A: s.Append(" — only in A"); break;
      case FS_ONLY_B: s.Append(" — only in B"); break;
      case FS_ERROR: s.Append(" — could not be read"); break;
    }
    for (int side = 0; side < 2; side++)
    {
      if (f->size[side] < 0) continue;
      FmtSize(f->size[side], a, sizeof(a));
      FmtTime(f->mtime[side], b, sizeof(b));
      if (f->status == FS_SAME) s.AppendFormatted(256, " · %s, %s", a, b);
      else s.AppendFormatted(256, " · %s: %s, %s", side ? "B" : "A", a, b);
      if (f->status == FS_SAME) break;
    }
    if (f->status == FS_MODIFIED && f->firstDiff >= 0)
    {
      FmtInt(f->firstDiff, a, sizeof(a));
      s.AppendFormatted(128, " · first difference at byte %s", a);
    }
  }
  SetDlgItemText(g_hwnd, IDC_STATUS, s.Get());
}

static void UpdateTotals()
{
  if (!g_root)
  {
    SetDlgItemText(g_hwnd, IDC_TOTALS, "");
    return;
  }
  char a[32], b[32], t[32], buf[160];
  FmtInt(g_root->nFiles, a, sizeof(a));
  FmtInt(g_root->Diffs(), b, sizeof(b));
  snprintf(t, sizeof(t), "%.1f", g_lastSeconds);
  snprintf(buf, sizeof(buf), "%s files · %s different · %s s", a, b, t);
  SetDlgItemText(g_hwnd, IDC_TOTALS, buf);
}

static void UpdateScanStatus()
{
  if (!g_scan) return;
  DiffScanner::Progress p;
  g_scan->GetProgress(&p);
  char a[32], b[32];
  FmtInt(p.files, a, sizeof(a));
  FmtInt(p.diffs, b, sizeof(b));
  WDL_FastString s;
  s.SetFormatted(256, "%s… %s files · %s different", g_scan->WasCancelled() ? "Cancelling" : "Scanning", a, b);
  if (g_scan->GetMethod() == CMP_CONTENT && p.bytes > 0)
  {
    FmtSize(p.bytes, a, sizeof(a));
    s.AppendFormatted(64, " · %s read", a);
  }
  if (p.current.GetLength())
  {
    s.Append(" · ");
    s.Append(p.current.Get());
  }
  SetDlgItemText(g_hwnd, IDC_STATUS, s.Get());

  const unsigned int el = (unsigned int)((GetTickCount() - g_scanStart) / 1000);
  char t[32];
  snprintf(t, sizeof(t), "%u:%02u", el / 60, el % 60);
  SetDlgItemText(g_hwnd, IDC_TOTALS, t);
}

/* ----------------------------------------------------------------------- */
/* Scanning                                                                 */
/* ----------------------------------------------------------------------- */

static DWORD WINAPI ScanThreadProc(LPVOID p)
{
  ((DiffScanner *)p)->Run();
  return 0;
}

static void SetBusy(bool busy)
{
  static const int ids[] = {
    IDC_PATH_A, IDC_BROWSE_A, IDC_PATH_B, IDC_BROWSE_B, IDC_METHOD, IDC_HIDESAME, IDC_SHOWFILES
  };
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    EnableWindow(GetDlgItem(g_hwnd, ids[i]), !busy);
  SetDlgItemText(g_hwnd, IDC_COMPARE, busy ? "Cancel" : "Compare");
}

// strips whitespace and quotes (e.g. Explorer's "Copy as path") and the trailing separator
static void NormalizePath(char *p)
{
  char *s = p;
  while (*s == ' ' || *s == '\t') s++;
  if (s != p) memmove(p, s, strlen(s) + 1);
  int l = (int)strlen(p);
  while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t' || p[l - 1] == '\r' || p[l - 1] == '\n')) p[--l] = 0;
  if (l >= 2 && p[0] == '"' && p[l - 1] == '"')
  {
    memmove(p, p + 1, l - 2);
    p[l - 2] = 0;
    l -= 2;
  }
#ifdef _WIN32
  for (char *q = p; *q; q++) if (*q == '/') *q = '\\';
  while (l > 1 && p[l - 1] == '\\' && !(l == 3 && p[1] == ':')) p[--l] = 0;
#else
  while (l > 1 && p[l - 1] == '/') p[--l] = 0;
#endif
}

static void ClearResults()
{
  g_sync++;
  for (int s = 0; s < 2; s++) if (g_tree[s]) TreeView_DeleteAllItems(g_tree[s]);
  g_sync--;
  g_sel = NULL;
  delete g_root;
  g_root = NULL;
  g_resultGen++;
}

static void Message(const char *text, const char *path)
{
  WDL_FastString s(text);
  if (path)
  {
    s.Append("\n");
    s.Append(path);
  }
  MessageBox(g_hwnd, s.Get(), APP_TITLE, MB_OK);
}

static char *DupStr(const char *s)
{
  const size_t l = strlen(s) + 1;
  char *p = (char *)malloc(l);
  if (p) memcpy(p, s, l);
  return p;
}

static void CollectExpanded(const DiffDir *d)
{
  if (d->uiExpanded)
  {
    WDL_FastString k;
    RelJoin(d, "/", &k);
    g_keepExpanded.Add(DupStr(k.Get()));
  }
  for (int i = 0; i < d->dirs.GetSize(); i++) CollectExpanded(d->dirs.Get(i));
}

static void StartCompare()
{
  if (g_scan)
  {
    g_scan->Cancel(); // during the scan the button is "Cancel"
    UpdateScanStatus();
    return;
  }

  char pa[4096], pb[4096];
  GetDlgItemText(g_hwnd, IDC_PATH_A, pa, sizeof(pa));
  GetDlgItemText(g_hwnd, IDC_PATH_B, pb, sizeof(pb));
  NormalizePath(pa);
  NormalizePath(pb);
  SetDlgItemText(g_hwnd, IDC_PATH_A, pa);
  SetDlgItemText(g_hwnd, IDC_PATH_B, pb);

  if (!pa[0] || !pb[0])
  {
    Message("Choose the two directories you want to compare.", NULL);
    return;
  }
  if (!DD_IsDirectory(pa)) { Message("Directory A not found:", pa); return; }
  if (!DD_IsDirectory(pb)) { Message("Directory B not found:", pb); return; }

  // remember the current view to restore it with the new result
  g_keepExpanded.Empty(true, free);
  g_keepValid = g_root != NULL;
  if (g_root)
  {
    CollectExpanded(g_root);
    RelJoin(g_sel, "/", &g_keepSelected);
  }

  ClearResults();
  UpdateTotals();

  const int m = (int)SendDlgItemMessage(g_hwnd, IDC_METHOD, CB_GETCURSEL, 0, 0);
  g_method = m == CMP_CONTENT ? CMP_CONTENT : CMP_SIZE_DATE;

  g_scan = new DiffScanner(pa, pb, g_method);
  for (int s = 0; s < 2; s++) g_rootName[s].Set(g_scan->GetRootName(s));
  g_scanStart = GetTickCount();

  DWORD tid = 0;
  g_thread = CreateThread(NULL, 0, ScanThreadProc, g_scan, 0, &tid);
  if (!g_thread)
  {
    delete g_scan;
    g_scan = NULL;
    Message("Could not create the worker thread.", NULL);
    return;
  }

  SetBusy(true);
  SetTimer(g_hwnd, IDT_SCAN, 100, NULL);
  UpdateScanStatus();
}

static void AutoExpand(DiffDir *d, int levels)
{
  if (levels <= 0) return;
  d->uiExpanded = 1;
  for (int i = 0; i < d->dirs.GetSize(); i++) AutoExpand(d->dirs.Get(i), levels - 1);
}

#ifdef _WIN32
#define IS_PATH_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define IS_PATH_SEP(c) ((c) == '/') // on macOS/Linux "\\" can be part of a name
#endif

// finds a node by its relative path ("photos/beach"); optionally marks
// its ancestors as expanded (for --select)
static DiffNode *FindRelPath(DiffDir *root, const char *rel, bool expandAncestors)
{
  DiffNode *cur = root;
  WDL_FastString part;
  const char *p = rel;
  while (*p && cur && cur->isDir)
  {
    while (IS_PATH_SEP(*p)) p++;
    const char *e = p;
    while (*e && !IS_PATH_SEP(*e)) e++;
    if (e == p) break;
    part.Set(p, (int)(e - p));
    DiffDir *d = (DiffDir *)cur;
    DiffNode *next = NULL;
    for (int i = 0; i < d->dirs.GetSize() && !next; i++)
      if (!strcmp(d->dirs.Get(i)->name, part.Get())) next = d->dirs.Get(i);
    for (int i = 0; i < d->files.GetSize() && !next; i++)
      if (!strcmp(d->files.Get(i)->name, part.Get())) next = d->files.Get(i);
    if (!next) return NULL;
    if (expandAncestors) d->uiExpanded = 1;
    cur = next;
    p = e;
  }
  return cur;
}

static void FinishScan(bool wait)
{
  if (!g_scan) return;
  if (!wait && !g_scan->IsDone())
  {
    UpdateScanStatus();
    return;
  }

  WaitForSingleObject(g_thread, INFINITE);
  CloseHandle(g_thread);
  g_thread = NULL;
  KillTimer(g_hwnd, IDT_SCAN);

  const bool cancelled = g_scan->WasCancelled();
  g_lastSeconds = (GetTickCount() - g_scanStart) / 1000.0;
  g_lastMethod = g_scan->GetMethod();
  DiffDir *root = cancelled ? NULL : g_scan->DetachRoot();
  delete g_scan;
  g_scan = NULL;

  if (!g_hwnd)
  {
    delete root;
    return;
  }
  SetBusy(false);

  if (!root)
  {
    SetDlgItemText(g_hwnd, IDC_STATUS, "Comparison cancelled.");
    SetDlgItemText(g_hwnd, IDC_TOTALS, "");
    return;
  }

  g_root = root;
  g_resultGen++;
  g_sel = g_root;
  if (g_keepValid)
  {
    // same view as before comparing again
    for (int i = 0; i < g_keepExpanded.GetSize(); i++)
    {
      DiffNode *n = FindRelPath(g_root, g_keepExpanded.Get(i), false);
      if (n && n->isDir) ((DiffDir *)n)->uiExpanded = 1;
    }
    DiffNode *n = FindRelPath(g_root, g_keepSelected.Get(), false);
    if (n) g_sel = n;
  }
  else
  {
    AutoExpand(g_root, g_expandLevels);
    if (g_cliSelect.GetLength())
    {
      DiffNode *n = FindRelPath(g_root, g_cliSelect.Get(), true);
      if (n) g_sel = n;
      g_cliSelect.Set("");
    }
  }
  g_keepExpanded.Empty(true, free);
  g_keepValid = false;
  BuildTrees();
  UpdateTotals();
  SetFocus(g_tree[0]);
}

/* ----------------------------------------------------------------------- */
/* Folder picker                                                            */
/* ----------------------------------------------------------------------- */

#ifdef _WIN32
static WCHAR *ToWide(const char *s)
{
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  WCHAR *w = (WCHAR *)malloc((n > 0 ? n : 1) * sizeof(WCHAR));
  if (w)
  {
    w[0] = 0;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  }
  return w;
}

static void FromWide(const WCHAR *w, char *out, int outsz)
{
  if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, outsz, NULL, NULL)) out[0] = 0;
}
#endif

static bool PickFolder(const char *title, const char *initial, char *out, int outsz)
{
#ifdef _WIN32
  bool ok = false;
  IFileOpenDialog *dlg = NULL;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;

  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  WCHAR *wt = ToWide(title);
  if (wt) { dlg->SetTitle(wt); free(wt); }
  if (initial && *initial)
  {
    WCHAR *wi = ToWide(initial);
    IShellItem *si = NULL;
    if (wi && SUCCEEDED(SHCreateItemFromParsingName(wi, NULL, IID_PPV_ARGS(&si))))
    {
      dlg->SetFolder(si);
      si->Release();
    }
    free(wi);
  }
  if (SUCCEEDED(dlg->Show(g_hwnd)))
  {
    IShellItem *res = NULL;
    if (SUCCEEDED(dlg->GetResult(&res)))
    {
      PWSTR p = NULL;
      if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)))
      {
        FromWide(p, out, outsz);
        CoTaskMemFree(p);
        ok = out[0] != 0;
      }
      res->Release();
    }
  }
  dlg->Release();
  return ok;
#else
  return BrowseForDirectory(title, initial && *initial ? initial : NULL, out, outsz);
#endif
}

static void Browse(int side)
{
  const int id = side ? IDC_PATH_B : IDC_PATH_A;
  char cur[4096], out[4096];
  GetDlgItemText(g_hwnd, id, cur, sizeof(cur));
  NormalizePath(cur);
  out[0] = 0;
  if (PickFolder(side ? "Choose directory B" : "Choose directory A", cur, out, sizeof(out)))
    SetDlgItemText(g_hwnd, id, out);
}

/* ----------------------------------------------------------------------- */
/* Main dialog                                                              */
/* ----------------------------------------------------------------------- */

static WDL_DLGRET MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_INITDIALOG:
      g_hwnd = hwndDlg;
      g_tree[0] = GetDlgItem(hwndDlg, IDC_TREE_A);
      g_tree[1] = GetDlgItem(hwndDlg, IDC_TREE_B);

#ifdef _WIN32
      for (int s = 0; s < 2; s++)
      {
        WDL_UTF8_HookTreeView(g_tree[s]);
        SetWindowTheme(g_tree[s], L"Explorer", NULL);
        TreeView_SetExtendedStyle(g_tree[s], TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        SetWindowSubclass(g_tree[s], TreeSubclassProc, (UINT_PTR)s, 0);
      }
      WDL_UTF8_HookComboBox(GetDlgItem(hwndDlg, IDC_METHOD));
#elif defined(__APPLE__)
      DirDiff_MacLinkScroll(g_tree[0], g_tree[1]);
#else
      for (int s = 0; s < 2; s++)
        g_treeOldProc[s] = (WNDPROC)SetWindowLong(g_tree[s], GWL_WNDPROC, (LONG_PTR)TreeWheelProc);
#endif

      SendDlgItemMessage(hwndDlg, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"Size and date");
      SendDlgItemMessage(hwndDlg, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)"Content (byte by byte)");
      SendDlgItemMessage(hwndDlg, IDC_METHOD, CB_SETCURSEL, g_method, 0);
      CheckDlgButton(hwndDlg, IDC_HIDESAME, g_hideSame ? BST_CHECKED : BST_UNCHECKED);
      CheckDlgButton(hwndDlg, IDC_SHOWFILES, g_showFiles ? BST_CHECKED : BST_UNCHECKED);
      SetDlgItemText(hwndDlg, IDC_PATH_A, g_cliPath[0].Get());
      SetDlgItemText(hwndDlg, IDC_PATH_B, g_cliPath[1].Get());
      SetDlgItemText(hwndDlg, IDC_STATUS, "Choose the two directories and press Compare.");

      g_sizer.init(hwndDlg);
      g_sizer.init_item(IDC_PATH_A, 0.0f, 0.0f, 0.5f, 0.0f);
      g_sizer.init_item(IDC_BROWSE_A, 0.5f, 0.0f, 0.5f, 0.0f);
      g_sizer.init_item(IDC_LBL_B, 0.5f, 0.0f, 0.5f, 0.0f);
      g_sizer.init_item(IDC_PATH_B, 0.5f, 0.0f, 1.0f, 0.0f);
      g_sizer.init_item(IDC_BROWSE_B, 1.0f, 0.0f, 1.0f, 0.0f);
      g_sizer.init_item(IDC_COMPARE, 1.0f, 0.0f, 1.0f, 0.0f);
      g_sizer.init_item(IDC_TOTALS, 1.0f, 0.0f, 1.0f, 0.0f);
      g_sizer.init_item(IDC_TREE_A, 0.0f, 0.0f, 0.5f, 1.0f);
      g_sizer.init_item(IDC_TREE_B, 0.5f, 0.0f, 1.0f, 1.0f);
      g_sizer.init_item(IDC_STATUS, 0.0f, 1.0f, 1.0f, 1.0f);

      {
        // minimum size: the design width (the options row cannot be
        // narrowed) and 70% of the height
        RECT r;
        GetWindowRect(hwndDlg, &r);
        g_minSize.x = r.right - r.left;
        g_minSize.y = (r.bottom > r.top ? r.bottom - r.top : r.top - r.bottom) * 7 / 10;
      }

      if (g_autoCompare) PostMessage(hwndDlg, WM_COMMAND, IDC_COMPARE, 0);
    return 1;

    case WM_GETMINMAXINFO:
      if (lParam)
      {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        if (g_minSize.x > 0) mmi->ptMinTrackSize.x = g_minSize.x;
        if (g_minSize.y > 0) mmi->ptMinTrackSize.y = g_minSize.y;
      }
    return 0;

    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) g_sizer.onResize();
    break;

    case WM_TIMER:
      if (wParam == IDT_SCAN) FinishScan(false);
    break;

    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_BROWSE_A: Browse(0); break;
        case IDC_BROWSE_B: Browse(1); break;
        case IDC_COMPARE: StartCompare(); break;
        case IDC_HIDESAME:
          g_hideSame = IsDlgButtonChecked(hwndDlg, IDC_HIDESAME) == BST_CHECKED;
          if (g_root) BuildTrees();
        break;
        case IDC_SHOWFILES:
          g_showFiles = IsDlgButtonChecked(hwndDlg, IDC_SHOWFILES) == BST_CHECKED;
          if (g_root) BuildTrees();
        break;
        case IDC_METHOD:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            const int m = (int)SendDlgItemMessage(hwndDlg, IDC_METHOD, CB_GETCURSEL, 0, 0);
            if (g_root && m != g_lastMethod)
              SetDlgItemText(hwndDlg, IDC_STATUS, "Press Compare to apply the new method.");
          }
        break;
        case IDCANCEL: // Esc: cancels the scan, does not close the window
          if (g_scan) StartCompare();
        break;
        case ID_QUIT:
          SendMessage(hwndDlg, WM_CLOSE, 0, 0);
        break;
      }
    break;

    case WM_NOTIFY:
      {
        NMHDR *hdr = (NMHDR *)lParam;
        if (!hdr) break;
        const int side = hdr->hwndFrom == g_tree[0] ? 0 : hdr->hwndFrom == g_tree[1] ? 1 : -1;
        if (side < 0) break;
        const int other = !side;

        if (IS_TVN_ITEMEXPANDING(hdr->code))
        {
          NMTREEVIEW *nm = (NMTREEVIEW *)lParam;
          DiffNode *n = (DiffNode *)nm->itemNew.lParam;
          if (!n || !n->isDir) break;
          DiffDir *d = (DiffDir *)n;
          const UINT act = nm->action & 0xff;
          const bool expanding = act == TVE_EXPAND || (act == TVE_TOGGLE && !d->uiExpanded);

          // lazy loading: children are inserted the first time it is expanded
          if (expanding) PopulateChildren(d, side);
          if (g_sync) break; // this is the mirror of an action in the other tree

          g_sync++;
          d->uiExpanded = expanding ? 1 : 0;
          if (expanding) PopulateChildren(d, other);
          if (d->ui[other])
            TreeView_Expand(g_tree[other], (HTREEITEM)d->ui[other], expanding ? TVE_EXPAND : TVE_COLLAPSE);
          g_sync--;

          PostMessage(hwndDlg, WM_DD_AFTEREXPAND, (WPARAM)g_resultGen, (LPARAM)(expanding ? 0 : d));
          PostMessage(hwndDlg, WM_DD_SYNCSCROLL, (WPARAM)side, 0);
        }
        else if (IS_TVN_SELCHANGED(hdr->code))
        {
          if (g_sync) break;
          NMTREEVIEW *nm = (NMTREEVIEW *)lParam;
          DiffNode *n = (DiffNode *)nm->itemNew.lParam;
          if (!n) break;
          g_sel = n;
          if (n->ui[other])
          {
            g_sync++;
            TreeView_SelectItem(g_tree[other], (HTREEITEM)n->ui[other]);
            g_sync--;
          }
          UpdateSelectionStatus();
          PostMessage(hwndDlg, WM_DD_SYNCSCROLL, (WPARAM)side, 0);
        }
      }
    break;

    case WM_DD_AFTEREXPAND:
      // when collapsing a directory that contained the selection, the directory is selected
      if ((int)wParam == g_resultGen && lParam)
      {
        DiffDir *d = (DiffDir *)lParam;
        if (g_sel && IsDescendant(g_sel, d)) SelectNode(d);
      }
    break;

    case WM_DD_SYNCSCROLL:
#ifdef _WIN32
      SyncScrollFrom((int)wParam);
#endif
    break;

    case WM_CLOSE:
      if (g_scan)
      {
        g_scan->Cancel();
        FinishScan(true);
      }
      DestroyWindow(hwndDlg);
    return 1;

    case WM_DESTROY:
      KillTimer(hwndDlg, IDT_SCAN);
      if (g_scan)
      {
        g_scan->Cancel();
        g_hwnd = NULL;
        FinishScan(true);
      }
#ifdef _WIN32
      for (int s = 0; s < 2; s++) RemoveWindowSubclass(g_tree[s], TreeSubclassProc, (UINT_PTR)s);
#elif defined(__APPLE__)
      DirDiff_MacUnlinkScroll();
#endif
      ClearResults();
      g_tree[0] = g_tree[1] = NULL;
      g_hwnd = NULL;
#ifdef _WIN32
      PostQuitMessage(0);
#elif defined(__APPLE__)
      SWELL_PostQuitMessage(0);
#else
      g_quit = true;
#endif
    break;
  }
  return 0;
}

/* ----------------------------------------------------------------------- */
/* Startup                                                                  */
/* ----------------------------------------------------------------------- */

static void ParseArgs()
{
  WDL_PtrList<char> args;
#ifdef _WIN32
  int n = 0;
  LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
  if (w)
  {
    for (int i = 1; i < n; i++)
    {
      char buf[4096];
      FromWide(w[i], buf, sizeof(buf));
      args.Add(DupStr(buf));
    }
    LocalFree(w);
  }
#else
  for (int i = 1; i < g_argc; i++) if (g_argv[i]) args.Add(DupStr(g_argv[i]));
#endif

  int npos = 0;
  for (int i = 0; i < args.GetSize(); i++)
  {
    const char *a = args.Get(i);
    if (!strcmp(a, "--content")) g_method = CMP_CONTENT;
    else if (!strcmp(a, "--hide-same")) g_hideSame = true;
    else if (!strcmp(a, "--files")) g_showFiles = true;
    else if (!strncmp(a, "--expand=", 9)) g_expandLevels = atoi(a + 9);
    else if (!strncmp(a, "--select=", 9)) g_cliSelect.Set(a + 9);
    else if (a[0] == '-') { } // other options (e.g. macOS -psn_): ignored
    else if (npos < 2) g_cliPath[npos++].Set(a);
  }
  g_autoCompare = npos == 2;
  args.Empty(true, free);
}

INT_PTR SWELLAppMain(int msg, INT_PTR parm1, INT_PTR parm2)
{
  switch (msg)
  {
    case SWELLAPP_ONLOAD:
      ParseArgs();
    break;
    case SWELLAPP_LOADED:
      {
#ifdef _WIN32
        HWND h = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDlgProc, 0);
#else
        HWND h = CreateDialog(g_hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, MainDlgProc);
#endif
        if (h) ShowWindow(h, SW_SHOW);
      }
    break;
    case SWELLAPP_DESTROY:
      if (g_hwnd) DestroyWindow(g_hwnd);
    break;
    case SWELLAPP_ONCOMMAND:
      // application menu commands (macOS)
      if (g_hwnd && parm1) SendMessage(g_hwnd, WM_COMMAND, (WPARAM)parm1, 0);
    break;
  }
  (void)parm2;
  return 0;
}

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  (void)hPrevInstance; (void)lpCmdLine; (void)nShowCmd;
  g_hInst = hInstance;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES };
  InitCommonControlsEx(&icc);

  SWELLAppMain(SWELLAPP_ONLOAD, 0, 0);
  SWELLAppMain(SWELLAPP_LOADED, 0, 0);

  // Unicode loop: the dialog is created with CreateDialogParamW so that the
  // edit controls accept any character in paths
  for (;;)
  {
    MSG msg;
    const BOOL r = GetMessageW(&msg, NULL, 0, 0);
    if (!r) break;
    if (r < 0) { Sleep(10); continue; }
    if (g_hwnd && IsDialogMessageW(g_hwnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  SWELLAppMain(SWELLAPP_DESTROY, 0, 0);
  CoUninitialize();
  return 0;
}

#else

#ifndef __APPLE__
int main(int argc, char **argv)
{
  g_argc = argc;
  g_argv = (const char **)argv;
  SWELL_initargs(&argc, &argv);
  SWELL_Internal_PostMessage_Init();
  SWELL_ExtendedAPI("APPNAME", (void *)"DirDiff");
  SWELLAppMain(SWELLAPP_ONLOAD, 0, 0);
  SWELLAppMain(SWELLAPP_LOADED, 0, 0);
  while (!g_quit)
  {
    SWELL_RunMessageLoop();
    Sleep(10);
  }
  SWELLAppMain(SWELLAPP_DESTROY, 0, 0);
  return 0;
}
#endif

// Win32 styles that SWELL does not define: no effect on macOS/Linux
#ifndef TVS_HASBUTTONS
#define TVS_HASBUTTONS 0
#endif
#ifndef TVS_LINESATROOT
#define TVS_LINESATROOT 0
#endif
#ifndef TVS_SHOWSELALWAYS
#define TVS_SHOWSELALWAYS 0
#endif
#ifndef TVS_FULLROWSELECT
#define TVS_FULLROWSELECT 0
#endif
#ifndef SS_ENDELLIPSIS
#define SS_ENDELLIPSIS 0
#endif

// resources generated with: perl WDL/WDL/swell/swell_resgen.pl dirdiff.rc
#include "../WDL/swell/swell-dlggen.h"
#include "dirdiff.rc_mac_dlg"
#undef BEGIN
#undef END
#include "../WDL/swell/swell-menugen.h"
#include "dirdiff.rc_mac_menu"

#endif
