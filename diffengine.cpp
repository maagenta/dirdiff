/*
  DirDiff - directory comparison engine (see diffengine.h).
*/

#include "diffengine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "../WDL/dirscan.h"
#include "../WDL/wdlcstring.h"

#ifdef _WIN32
#define DD_SEP '\\'
#else
#define DD_SEP '/'
#endif

static const WDL_INT64 MTIME_TOLERANCE = 2;     // seconds (FAT/exFAT store the date with 2 s resolution)
static const int CONTENT_CHUNK = 1 << 20;       // 1 MiB per side per read

static char *dd_strdup(const char *s)
{
  const size_t l = strlen(s) + 1;
  char *p = (char *)malloc(l);
  if (p) memcpy(p, s, l);
  return p;
}

static bool IsAscii(const char *s)
{
  while (*s) if ((unsigned char)*s++ >= 0x80) return false;
  return true;
}

#ifdef _WIN32
// UTF-8 -> UTF-16, with the \\?\ prefix for long paths (the caller releases with free)
static WCHAR *WidePath(const char *p)
{
  const int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
  if (n <= 0) return NULL;
  WCHAR *w = (WCHAR *)malloc((n + 8) * sizeof(WCHAR));
  if (!w) return NULL;
  int o = 0;
  const bool isLong = n >= MAX_PATH - 12;
  if (isLong && p[0] && p[1] == ':')
  {
    memcpy(w, L"\\\\?\\", 4 * sizeof(WCHAR));
    o = 4;
  }
  else if (isLong && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') && p[2] != '?')
  {
    memcpy(w, L"\\\\?\\UNC", 7 * sizeof(WCHAR));
    o = 7;
    p++; // "\\server\share" -> "\\?\UNC\server\share"
  }
  MultiByteToWideChar(CP_UTF8, 0, p, -1, w + o, n + 1);
  if (o) for (WCHAR *q = w + o; *q; q++) if (*q == L'/') *q = L'\\';
  return w;
}

static WDL_INT64 FileTimeToUnix(const FILETIME &ft)
{
  const WDL_INT64 t = ((WDL_INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100 ns since 1601
  return t / 10000000 - (WDL_INT64)11644473600;
}

// reparse tag of a directory (so junctions and symbolic links are not followed)
static DWORD ReparseTag(const char *fullpath)
{
  WCHAR *w = WidePath(fullpath);
  if (!w) return 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(w, &fd);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return 0;
  FindClose(h);
  return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
}
#endif

static void AppendPath(WDL_FastString *s, const char *name)
{
  const int l = s->GetLength();
  if (l > 0)
  {
    const char c = s->Get()[l - 1];
    if (c != '/' && c != '\\') s->Append(DD_SEP == '/' ? "/" : "\\");
  }
  s->Append(name);
}

/*
  Matching key: two names that the file system considers the same must
  produce the same key.
   - Windows (NTFS): case-insensitive -> invariant uppercase.
   - macOS (APFS/HFS+): insensitive to case and to Unicode normalization
     -> NFD + case folding (CFStringFold).
   - Linux: exact byte comparison.
*/
static char *MakeKey(const char *name)
{
#if defined(_WIN32) || defined(__APPLE__)
  if (IsAscii(name))
  {
    char *k = dd_strdup(name);
    if (k) for (char *p = k; *p; p++)
    {
#ifdef _WIN32
      if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
#else
      if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
#endif
    }
    return k;
  }
#endif

#ifdef _WIN32
  char *ret = NULL;
  const int wl = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
  if (wl > 0)
  {
    WCHAR *w = (WCHAR *)malloc(wl * sizeof(WCHAR));
    WCHAR *u = (WCHAR *)malloc(wl * sizeof(WCHAR));
    if (w && u && MultiByteToWideChar(CP_UTF8, 0, name, -1, w, wl) &&
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w, -1, u, wl, NULL, NULL, 0))
    {
      const int bl = WideCharToMultiByte(CP_UTF8, 0, u, -1, NULL, 0, NULL, NULL);
      if (bl > 0 && (ret = (char *)malloc(bl)) != NULL)
        WideCharToMultiByte(CP_UTF8, 0, u, -1, ret, bl, NULL, NULL);
    }
    free(w);
    free(u);
  }
  return ret ? ret : dd_strdup(name);
#elif defined(__APPLE__)
  char *ret = NULL;
  CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
  if (s)
  {
    CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, s);
    CFRelease(s);
    if (m)
    {
      CFStringNormalize(m, kCFStringNormalizationFormD);
      CFStringFold(m, kCFCompareCaseInsensitive, NULL);
      const CFIndex sz = CFStringGetMaximumSizeForEncoding(CFStringGetLength(m), kCFStringEncodingUTF8) + 1;
      ret = (char *)malloc(sz);
      if (ret && !CFStringGetCString(m, ret, sz, kCFStringEncodingUTF8))
      {
        free(ret);
        ret = NULL;
      }
      CFRelease(m);
    }
  }
  return ret ? ret : dd_strdup(name);
#else
  return dd_strdup(name);
#endif
}

/* ----------------------------------------------------------------------- */
/* Sequential file reading                                                 */
/* ----------------------------------------------------------------------- */

class FileIn
{
public:
#ifdef _WIN32
  FileIn() : m_h(INVALID_HANDLE_VALUE) { }
#else
  FileIn() : m_fd(-1) { }
#endif
  ~FileIn() { Close(); }

  bool Open(const char *path)
  {
    Close();
#ifdef _WIN32
    WCHAR *w = WidePath(path);
    if (!w) return false;
    m_h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(w);
    return m_h != INVALID_HANDLE_VALUE;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    m_fd = open(path, flags);
    if (m_fd < 0) return false;
#if defined(__linux__)
    posix_fadvise(m_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#elif defined(__APPLE__)
    fcntl(m_fd, F_RDAHEAD, 1);
#endif
    return true;
#endif
  }

  // reads up to len bytes (fewer only at end of file). -1 = I/O error
  int Read(char *buf, int len)
  {
    int got = 0;
    while (got < len)
    {
#ifdef _WIN32
      DWORD r = 0;
      if (!ReadFile(m_h, buf + got, (DWORD)(len - got), &r, NULL)) return -1;
#else
      const ssize_t r = read(m_fd, buf + got, (size_t)(len - got));
      if (r < 0)
      {
        if (errno == EINTR) continue;
        return -1;
      }
#endif
      if (r == 0) break;
      got += (int)r;
    }
    return got;
  }

  void Close()
  {
#ifdef _WIN32
    if (m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
    m_h = INVALID_HANDLE_VALUE;
#else
    if (m_fd >= 0) close(m_fd);
    m_fd = -1;
#endif
  }

private:
#ifdef _WIN32
  HANDLE m_h;
#else
  int m_fd;
#endif
};

/* ----------------------------------------------------------------------- */
/* Nodes                                                                    */
/* ----------------------------------------------------------------------- */

DiffNode::DiffNode(DiffDir *par, const char *nm, bool dir)
  : parent(par), name(dd_strdup(nm ? nm : "")), isDir(dir)
{
  ui[0] = ui[1] = 0;
}

DiffNode::~DiffNode()
{
  free(name);
}

DiffFile::DiffFile(DiffDir *par, const char *nm)
  : DiffNode(par, nm, false), status(FS_SAME), firstDiff(-1)
{
  size[0] = size[1] = -1;
  mtime[0] = mtime[1] = 0;
}

DiffDir::DiffDir(DiffDir *par, const char *nm, int pres)
  : DiffNode(par, nm, true), presence((unsigned char)pres), readError(0), differs(false), uiPopulated(0), uiExpanded(0),
    nModified(0), nOnlyA(0), nOnlyB(0), nErrors(0), nFiles(0), nDirectDiffs(0), nDiffSubdirs(0)
{
}

DiffDir::~DiffDir()
{
  dirs.Empty(true);
  files.Empty(true);
}

/* ----------------------------------------------------------------------- */
/* Scanner                                                                  */
/* ----------------------------------------------------------------------- */

struct DiffScanner::Entry
{
  char *name;
  char *key;
  bool isDir;
  WDL_INT64 size, mtime;
};

static int CmpEntryKey(const void *a, const void *b)
{
  const DiffScanner::Entry *ea = *(DiffScanner::Entry * const *)a;
  const DiffScanner::Entry *eb = *(DiffScanner::Entry * const *)b;
  return strcmp(ea->key, eb->key);
}

static void FreeEntries(WDL_PtrList<DiffScanner::Entry> *l)
{
  for (int i = 0; i < l->GetSize(); i++)
  {
    DiffScanner::Entry *e = l->Get(i);
    free(e->name);
    free(e->key);
    delete e;
  }
  l->Empty(false);
}

// presentation order: "logical" (img2 < img10), case-insensitive
static int CmpDisplay(const char *a, const char *b)
{
  const int r = WDL_strcmp_logical_ex(a, b, 0, WDL_STRCMP_LOGICAL_EX_FLAG_UTF8CONVERT);
  return r ? r : strcmp(a, b);
}
static int CmpDirDisplay(const void *a, const void *b)
{
  return CmpDisplay((*(DiffDir * const *)a)->name, (*(DiffDir * const *)b)->name);
}
static int CmpFileDisplay(const void *a, const void *b)
{
  return CmpDisplay((*(DiffFile * const *)a)->name, (*(DiffFile * const *)b)->name);
}

DiffScanner::DiffScanner(const char *rootA, const char *rootB, int method)
  : m_method(method), m_cancel(0), m_done(0), m_root(NULL),
    m_filesDone(0), m_diffsDone(0), m_bytesDone(0), m_lastReport(0)
{
  m_rootPath[0].Set(rootA);
  m_rootPath[1].Set(rootB);
  DD_BaseName(rootA, &m_rootName[0]);
  DD_BaseName(rootB, &m_rootName[1]);
  m_prog.files = m_prog.diffs = 0;
  m_prog.bytes = 0;
  m_buf[0] = m_buf[1] = NULL;
}

DiffScanner::~DiffScanner()
{
  delete m_root;
  free(m_buf[0]);
  free(m_buf[1]);
}

bool DiffScanner::IsDone()
{
  WDL_MutexLock lock(&m_mutex);
  return m_done != 0;
}

void DiffScanner::GetProgress(Progress *p)
{
  WDL_MutexLock lock(&m_mutex);
  p->files = m_prog.files;
  p->diffs = m_prog.diffs;
  p->bytes = m_prog.bytes;
  p->current.Set(m_prog.current.Get());
}

DiffDir *DiffScanner::DetachRoot()
{
  DiffDir *r = m_root;
  m_root = NULL;
  return r;
}

void DiffScanner::Report(bool force)
{
  const DWORD now = GetTickCount();
  if (!force && now - m_lastReport < 50) return;
  m_lastReport = now;
  WDL_MutexLock lock(&m_mutex);
  m_prog.files = m_filesDone;
  m_prog.diffs = m_diffsDone;
  m_prog.bytes = m_bytesDone;
  m_prog.current.Set(m_curDir.Get());
}

void DiffScanner::Run()
{
  if (m_method == CMP_CONTENT)
  {
    m_buf[0] = (char *)malloc(CONTENT_CHUNK);
    m_buf[1] = (char *)malloc(CONTENT_CHUNK);
  }

  m_root = new DiffDir(NULL, m_rootName[0].Get(), PRESENT_BOTH);

  WDL_FastString pa(m_rootPath[0].Get()), pb(m_rootPath[1].Get());
  ScanDir(m_root, &pa, &pb);

  Report(true);
  WDL_MutexLock lock(&m_mutex);
  m_done = 1;
}

bool DiffScanner::ListDir(const char *path, WDL_PtrList<Entry> *out)
{
  WDL_DirScan ds;
  if (ds.First(path))
  {
#ifdef _WIN32
    // FindFirstFile fails on the empty root of a drive: that is not an error
    return DD_IsDirectory(path);
#else
    return false;
#endif
  }

#ifndef _WIN32
  WDL_FastString full;
#endif
  do
  {
    if (m_cancel) break;
    const char *fn = ds.GetCurrentFN();
    if (fn[0] == '.' && (!fn[1] || (fn[1] == '.' && !fn[2]))) continue;

    bool isDir = false;
    WDL_INT64 size = 0, mtime = 0;

#ifdef _WIN32
    const DWORD attr = ds.GetCurrentAttributes();
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
      {
        // junctions and symbolic links are not followed (they can form cycles);
        // other reparse points (OneDrive, deduplication...) are walked
        WDL_FastString full(path);
        AppendPath(&full, fn);
        const DWORD tag = ReparseTag(full.Get());
        if (tag == IO_REPARSE_TAG_MOUNT_POINT || tag == IO_REPARSE_TAG_SYMLINK) continue;
      }
      isDir = true;
    }
    else
    {
      DWORD hi = 0;
      const DWORD lo = ds.GetCurrentFileSize(&hi);
      size = ((WDL_INT64)hi << 32) | lo;
      FILETIME ft;
      ds.GetCurrentLastWriteTime(&ft);
      mtime = FileTimeToUnix(ft);
    }
#else
    full.Set(path);
    AppendPath(&full, fn);
    struct stat st;
    if (lstat(full.Get(), &st)) continue;
    if (S_ISLNK(st.st_mode))
    {
      if (stat(full.Get(), &st)) continue;  // broken link
      if (S_ISDIR(st.st_mode)) continue;    // links to directories are not followed (avoids cycles)
    }
    if (S_ISDIR(st.st_mode)) isDir = true;
    else if (S_ISREG(st.st_mode))
    {
      size = (WDL_INT64)st.st_size;
      mtime = (WDL_INT64)st.st_mtime;
    }
    else continue; // FIFOs, sockets, devices: reading them could block
#endif

    Entry *e = new Entry;
    e->name = dd_strdup(fn);
    e->key = MakeKey(fn);
    e->isDir = isDir;
    e->size = size;
    e->mtime = mtime;
    if (!e->name || !e->key)
    {
      free(e->name);
      free(e->key);
      delete e;
      continue;
    }
    out->Add(e);
  } while (!ds.Next());

  return true;
}

void DiffScanner::ScanDir(DiffDir *node, WDL_FastString *pa, WDL_FastString *pb)
{
  WDL_PtrList<Entry> la, lb;

  m_curDir.Set((node->presence & PRESENT_A) ? pa->Get() : pb->Get());
  Report(false);

  if ((node->presence & PRESENT_A) && !ListDir(pa->Get(), &la)) node->readError |= 1;
  if ((node->presence & PRESENT_B) && !ListDir(pb->Get(), &lb)) node->readError |= 2;

  if (la.GetSize() > 1) qsort(la.GetList(), la.GetSize(), sizeof(Entry *), CmpEntryKey);
  if (lb.GetSize() > 1) qsort(lb.GetList(), lb.GetSize(), sizeof(Entry *), CmpEntryKey);

  const int lenA = pa->GetLength(), lenB = pb->GetLength();
  int ia = 0, ib = 0;

  // merge-walk of the two listings sorted by key
  while (!m_cancel && (ia < la.GetSize() || ib < lb.GetSize()))
  {
    Entry *ea = la.Get(ia), *eb = lb.Get(ib);
    int c;
    if (!ea) c = 1;
    else if (!eb) c = -1;
    else
    {
      c = strcmp(ea->key, eb->key);
      // same name but one is a file and the other a directory: they are treated as
      // two distinct entries (the file on one side only, the directory on the other)
      if (!c && ea->isDir != eb->isDir) c = ea->isDir ? 1 : -1;
    }

    Entry *one = c <= 0 ? ea : eb;
    if (one->isDir)
    {
      const int pres = c < 0 ? PRESENT_A : c > 0 ? PRESENT_B : PRESENT_BOTH;
      DiffDir *sub = new DiffDir(node, one->name, pres);
      node->dirs.Add(sub);
      if (pres & PRESENT_A) AppendPath(pa, ea->name);
      if (pres & PRESENT_B) AppendPath(pb, eb->name);
      ScanDir(sub, pa, pb);
      pa->SetLen(lenA);
      pb->SetLen(lenB);

      node->nModified += sub->nModified;
      node->nOnlyA += sub->nOnlyA;
      node->nOnlyB += sub->nOnlyB;
      node->nErrors += sub->nErrors;
      node->nFiles += sub->nFiles;
      if (sub->differs) node->nDiffSubdirs++;
    }
    else
    {
      DiffFile *f = new DiffFile(node, one->name);
      if (c <= 0) { f->size[0] = ea->size; f->mtime[0] = ea->mtime; }
      if (c >= 0) { f->size[1] = eb->size; f->mtime[1] = eb->mtime; }

      if (c < 0) f->status = FS_ONLY_A;
      else if (c > 0) f->status = FS_ONLY_B;
      else f->status = (unsigned char)CompareFiles(f, pa, ea->name, pb, eb->name);

      node->files.Add(f);
      node->nFiles++;
      switch (f->status)
      {
        case FS_MODIFIED: node->nModified++; break;
        case FS_ONLY_A: node->nOnlyA++; break;
        case FS_ONLY_B: node->nOnlyB++; break;
        case FS_ERROR: node->nErrors++; break;
      }
      if (f->status != FS_SAME)
      {
        node->nDirectDiffs++;
        m_diffsDone++;
      }
      m_filesDone++;
      Report(false);
    }

    if (c <= 0) ia++;
    if (c >= 0) ib++;
  }

  FreeEntries(&la);
  FreeEntries(&lb);

  node->differs = node->Diffs() > 0 || node->nDiffSubdirs > 0 ||
                  node->presence != PRESENT_BOTH || node->readError != 0;

  if (node->dirs.GetSize() > 1) qsort(node->dirs.GetList(), node->dirs.GetSize(), sizeof(DiffDir *), CmpDirDisplay);
  if (node->files.GetSize() > 1) qsort(node->files.GetList(), node->files.GetSize(), sizeof(DiffFile *), CmpFileDisplay);
}

int DiffScanner::CompareFiles(DiffFile *f, WDL_FastString *pa, const char *na, WDL_FastString *pb, const char *nb)
{
  if (f->size[0] != f->size[1]) return FS_MODIFIED;

  if (m_method == CMP_SIZE_DATE)
  {
    WDL_INT64 d = f->mtime[0] - f->mtime[1];
    if (d < 0) d = -d;
    return d > MTIME_TOLERANCE ? FS_MODIFIED : FS_SAME;
  }

  if (f->size[0] == 0) return FS_SAME;

  const int la = pa->GetLength(), lb = pb->GetLength();
  AppendPath(pa, na);
  AppendPath(pb, nb);
  WDL_INT64 off = -1;
  const int r = CompareContent(pa->Get(), pb->Get(), f->size[0], &off);
  pa->SetLen(la);
  pb->SetLen(lb);

  if (r < 0) return FS_ERROR;
  if (r > 0)
  {
    f->firstDiff = off;
    return FS_MODIFIED;
  }
  return FS_SAME;
}

// 0 = equal, 1 = different (firstDiff = offset), -1 = read error or cancelled
int DiffScanner::CompareContent(const char *fa, const char *fb, WDL_INT64 size, WDL_INT64 *firstDiff)
{
  if (!m_buf[0] || !m_buf[1]) return -1;

  FileIn a, b;
  if (!a.Open(fa) || !b.Open(fb)) return -1;

  WDL_INT64 pos = 0;
  for (;;)
  {
    if (m_cancel) return -1;
    const int ra = a.Read(m_buf[0], CONTENT_CHUNK);
    const int rb = b.Read(m_buf[1], CONTENT_CHUNK);
    if (ra < 0 || rb < 0) return -1;

    const int n = ra < rb ? ra : rb;
    if (memcmp(m_buf[0], m_buf[1], (size_t)n))
    {
      int i = 0;
      while (m_buf[0][i] == m_buf[1][i]) i++;
      *firstDiff = pos + i;
      return 1;
    }
    m_bytesDone += n;
    pos += n;

    if (ra != rb)
    {
      // the file changed size while it was being read
      *firstDiff = pos;
      return 1;
    }
    if (ra < CONTENT_CHUNK) break; // end of both files
    Report(false);
  }
  (void)size;
  return 0;
}

/* ----------------------------------------------------------------------- */

bool DD_IsDirectory(const char *path)
{
  if (!path || !*path) return false;
#ifdef _WIN32
  WCHAR *w = WidePath(path);
  if (!w) return false;
  const DWORD a = GetFileAttributesW(w);
  free(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return !stat(path, &st) && S_ISDIR(st.st_mode);
#endif
}

void DD_BaseName(const char *path, WDL_FastString *out)
{
  int end = (int)strlen(path);
  while (end > 1 && (path[end - 1] == '/' || path[end - 1] == '\\')) end--;
  int start = end;
  while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\') start--;
  if (start == end) out->Set(path, end); // "/" o "C:"
  else out->Set(path + start, end - start);
  if (!out->GetLength()) out->Set(path);
}
