/*
  DirDiff - directory comparison engine.

  It does not depend on the UI: it walks two directory trees in parallel
  (merge-walk over sorted listings), compares each pair of files and
  aggregates, for each directory, how many files in its subtree differ.

  It runs on a worker thread (DiffScanner::Run). The UI polls progress
  with GetProgress() and, when finished, takes ownership of the tree with
  DetachRoot().
*/

#ifndef DIRDIFF_DIFFENGINE_H
#define DIRDIFF_DIFFENGINE_H

#ifdef _WIN32
#include <windows.h>
#endif
#include "../WDL/swell/swell.h"
#include "../WDL/wdltypes.h"
#include "../WDL/ptrlist.h"
#include "../WDL/wdlstring.h"
#include "../WDL/mutex.h"

enum CompareMethod
{
  CMP_SIZE_DATE = 0,  // size + modification date (2 s tolerance, because of FAT/exFAT)
  CMP_CONTENT = 1     // size + byte-by-byte content
};

enum FileStatus
{
  FS_SAME = 0,
  FS_MODIFIED,        // exists in A and in B but differs
  FS_ONLY_A,
  FS_ONLY_B,
  FS_ERROR            // one of the two could not be read (content mode)
};

enum { SIDE_A = 0, SIDE_B = 1 };
enum { PRESENT_A = 1, PRESENT_B = 2, PRESENT_BOTH = 3 };

struct DiffDir;

struct DiffNode
{
  DiffNode(DiffDir *par, const char *nm, bool dir);
  ~DiffNode();

  DiffDir *parent;
  char *name;         // UTF-8: the name in A if it exists in A, otherwise the one in B
  bool isDir;
  INT_PTR ui[2];      // reserved for the UI: HTREEITEM of this node in tree A and in tree B
};

struct DiffFile : DiffNode
{
  DiffFile(DiffDir *par, const char *nm);

  unsigned char status;   // FileStatus
  WDL_INT64 size[2];      // per side; -1 = does not exist on that side
  WDL_INT64 mtime[2];     // seconds since 1970-01-01 UTC
  WDL_INT64 firstDiff;    // content mode: offset of the first differing byte, -1 if not applicable
};

struct DiffDir : DiffNode
{
  DiffDir(DiffDir *par, const char *nm, int presence);
  ~DiffDir();

  unsigned char presence;     // PRESENT_A | PRESENT_B
  unsigned char readError;    // bit 0: could not be listed in A; bit 1: in B
  bool differs;               // something differs in the subtree: files, directories that exist
                              // on only one side (even if empty) or read errors
  unsigned char uiPopulated;  // reserved for the UI: children inserted (one bit per tree)
  unsigned char uiExpanded;   // reserved for the UI: expanded in both trees

  WDL_PtrList<DiffDir> dirs;  // in presentation order (logical order, like the file explorer)
  WDL_PtrList<DiffFile> files;

  // totals for the whole subtree (this directory and all its descendants)
  int nModified, nOnlyA, nOnlyB, nErrors;
  int nFiles;                 // files distinct by name (union of A and B)

  // this directory's level only
  int nDirectDiffs;           // files in this directory that differ
  int nDiffSubdirs;           // subdirectories with differs == true

  int Diffs() const { return nModified + nOnlyA + nOnlyB + nErrors; }
};

class DiffScanner
{
public:
  DiffScanner(const char *rootA, const char *rootB, int method);
  ~DiffScanner();

  // Runs on the worker thread. When it returns, IsDone() is true.
  void Run();

  void Cancel() { m_cancel = 1; }
  bool WasCancelled() const { return m_cancel != 0; }
  bool IsDone();

  struct Progress
  {
    int files;            // files processed
    int diffs;            // differing files found
    WDL_INT64 bytes;      // bytes read (content mode)
    WDL_FastString current; // directory currently being walked
  };
  void GetProgress(Progress *p);

  // The UI takes ownership of the resulting tree (only after IsDone()).
  DiffDir *DetachRoot();

  const char *GetRootPath(int side) const { return m_rootPath[side].Get(); }
  const char *GetRootName(int side) const { return m_rootName[side].Get(); }
  int GetMethod() const { return m_method; }

  struct Entry; // entry of a directory listing (internal)

private:
  void ScanDir(DiffDir *node, WDL_FastString *pa, WDL_FastString *pb);
  bool ListDir(const char *path, WDL_PtrList<Entry> *out);
  int CompareFiles(DiffFile *f, WDL_FastString *pa, const char *na, WDL_FastString *pb, const char *nb);
  int CompareContent(const char *fa, const char *fb, WDL_INT64 size, WDL_INT64 *firstDiff);
  void Report(bool force);

  WDL_FastString m_rootPath[2], m_rootName[2];
  int m_method;

  volatile int m_cancel;
  int m_done;                 // protected by m_mutex
  DiffDir *m_root;

  // worker-thread counters; copied to m_prog under m_mutex
  int m_filesDone, m_diffsDone;
  WDL_INT64 m_bytesDone;
  DWORD m_lastReport;
  WDL_FastString m_curDir;    // current directory (used only by the worker thread)

  WDL_Mutex m_mutex;
  Progress m_prog;

  char *m_buf[2];             // content-mode read buffers
};

// utilities shared with the UI
bool DD_IsDirectory(const char *utf8path);
void DD_BaseName(const char *path, WDL_FastString *out);

#endif
