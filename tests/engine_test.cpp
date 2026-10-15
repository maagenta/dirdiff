// Headless engine test: prints the compared tree with its counters.
//   tests/engine_test [--content] dirA dirB
#include "../diffengine.h"
#include <stdio.h>
#include <string.h>

static const char *St(int s)
{
  switch (s) { case FS_SAME: return "same"; case FS_MODIFIED: return "modified"; case FS_ONLY_A: return "only A";
               case FS_ONLY_B: return "only B"; default: return "error"; }
}

static void Dump(const DiffDir *d, int depth)
{
  printf("%*s%s/ (%d) pres=%d differs=%d mod=%d a=%d b=%d err=%d files=%d\n", depth * 2, "", d->name, d->Diffs(),
         d->presence, d->differs ? 1 : 0, d->nModified, d->nOnlyA, d->nOnlyB, d->nErrors, d->nFiles);
  for (int i = 0; i < d->dirs.GetSize(); i++) Dump(d->dirs.Get(i), depth + 1);
  for (int i = 0; i < d->files.GetSize(); i++)
  {
    const DiffFile *f = d->files.Get(i);
    printf("%*s%s [%s] %lld/%lld", (depth + 1) * 2, "", f->name, St(f->status), (long long)f->size[0], (long long)f->size[1]);
    if (f->firstDiff >= 0) printf(" firstDiff=%lld", (long long)f->firstDiff);
    printf("\n");
  }
}

int main(int argc, char **argv)
{
  int method = CMP_SIZE_DATE, a = 1;
  if (argc > 1 && !strcmp(argv[1], "--content")) { method = CMP_CONTENT; a++; }
  if (argc - a < 2) { fprintf(stderr, "usage: engine_test [--content] dirA dirB\n"); return 1; }
  DiffScanner s(argv[a], argv[a + 1], method);
  s.Run();
  DiffDir *root = s.DetachRoot();
  Dump(root, 0);
  delete root;
  return 0;
}

// generic SWELL references it even though the test creates no windows
INT_PTR SWELLAppMain(int, INT_PTR, INT_PTR) { return 0; }
