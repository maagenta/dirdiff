# DirDiff — directory comparer built on WDL/SWELL

Desktop application for Windows, macOS and Linux, written against the Win32
API and built with Cockos' [WDL](https://github.com/justinfrankel/WDL):

| Platform | Window layer | Tree |
|---|---|---|
| Windows | native Win32 + Common Controls 6 (*Explorer* theme) | `SysTreeView32` |
| macOS | SWELL on Cocoa | `NSOutlineView` |
| Linux | generic SWELL on GDK 3, drawn with LICE | SWELL tree |

It shows two trees, one per root directory. Each directory shows how many
files in its subtree differ:

```
Data (238)
  documents
  photos (201)
    2023 (140)
    2024 (52)
    scans (8) [missing]
    beach (1)
  music (5)
  videos (32) [only in A]
```

## Layout

```
dirdiff/
  main.cpp              UI: dialog, trees, synchronization, scan thread
  diffengine.h/.cpp     comparison engine (no UI dependencies)
  dirdiff.rc            dialog template (Win32, UTF-8)
  dirdiff.rc_mac_dlg    the same template for SWELL (generated with swell_resgen.pl)
  dirdiff.rc_mac_menu   (generated; empty: the app has no Win32 menus)
  resource.h            control IDs
  mac_main.mm           macOS main() without a nib + synchronized scrolling in Cocoa
  dirdiff.manifest      Common Controls 6, per-monitor DPI, long paths
  Info.plist.in         macOS bundle
  CMakeLists.txt        Windows / macOS / Linux
  Makefile              Linux (same scheme as WDL/swell/sample_project)
  tests/engine_test.cpp headless engine test
  WDL/                  git submodule (sources in WDL/WDL/)
```

Tested with WDL at commit `2b260b9` (October 2026). WDL is a git submodule in
`WDL/`; after cloning, fetch it with:

```
git submodule update --init
```

## Building

### Windows (Visual Studio 2019 or later)

```
cd dirdiff
cmake -B build -A x64
cmake --build build --config Release
build\Release\dirdiff.exe
```

It is compiled with `/utf-8` (the strings in the code are UTF-8) and the
manifest lives inside `dirdiff.rc`, which is why it is linked with
`/MANIFEST:NO`. MinGW-GCC also works (CMake adds `-fpermissive` to WDL's
`wndsize.cpp`); clang-mingw rejects WDL's `FARPROC` to `void*` conversion.

### macOS (Command Line Tools or Xcode)

```
cd dirdiff
cmake -B build                                   # or: -G Xcode
cmake --build build
open build/DirDiff.app
```

Universal binary: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`. Interface Builder
is not needed: `mac_main.mm` creates the application menu in code and calls
`-[SWELLAppController awakeFromNib]`, which is what the nib would do.

### Linux

Dependencies (Debian/Ubuntu):

```
sudo apt install build-essential pkg-config perl libgtk-3-dev libxi-dev \
                 libgl-dev libfreetype-dev libfontconfig-dev
```

```
cd dirdiff
make            # or: cmake -B build && cmake --build build
./dirdiff
```

If `dirdiff.rc` is edited, `make`/CMake regenerate `dirdiff.rc_mac_dlg` with
`perl WDL/WDL/swell/swell_resgen.pl dirdiff.rc`.

## Usage

```
dirdiff [--content] [--hide-same] [--files] [--expand=N] [--select=path] [dirA dirB]
```

- `dirA dirB`: fills in the paths and compares on startup.
- `--content`: compares content byte by byte instead of size and date.
- `--hide-same`, `--files`: enable "Hide identical" and "Show files".
- `--expand=N`: levels expanded after comparing (1 by default: the root).
- `--select=photos/beach`: selects that node and expands its ancestors.

In the window: Enter compares, Esc cancels a scan in progress, and when
comparing again the expanded directories and the selection are preserved.

## What counts as "different"

A directory's number is the sum, over its whole subtree, of:

- **modified**: the file exists in A and in B and differs;
- **only in A** / **only in B**: the file exists on one side;
- **errors**: could not be read (content mode only).

With **Size and date**, a pair differs if the size changes or if the
modification date differs by more than 2 s (FAT/exFAT store the time with 2 s
resolution). With **Content**, if the sizes match both files are read in 1 MiB
blocks and compared with `memcmp`; the status bar shows the offset of the first
differing byte. The date is ignored.

A directory that exists on only one side appears in both trees, marked
`[only in A]` in one and `[missing]` in the other, so the rows stay aligned.
If it is empty it adds no files, but it still counts as a difference for
"Hide identical".

## How it works

**Engine (`diffengine.cpp`).** A worker thread walks both trees at the same
time. At each level it lists both directories with `WDL_DirScan`, sorts the
entries by a *matching key* and merges them as in a merge sort: equal key =
same item on both sides. The key follows the rules of the platform's file
system:

- Windows: invariant uppercase (`LCMapStringEx`), because NTFS is case-insensitive;
- macOS: NFD + *case folding* (`CFStringFold`), because APFS/HFS+ are
  insensitive to case and to Unicode normalization (`café` in NFC and in NFD is
  the same name);
- Linux: the bytes as they are.

Totals are aggregated bottom-up as the recursion returns, so each node knows
its number without walking anything later. For display, children and files are
sorted with `WDL_strcmp_logical_ex` ("natural" order: `img2` before `img10`).

Symbolic links to directories (and NTFS junctions) are not followed, to avoid
cycles; links to files are compared by their target. FIFOs, sockets and devices
are ignored (opening them for reading could block the scan).

**UI (`main.cpp`).** The two trees have exactly the same structure (the union
of A and B) and each item stores in `lParam` a pointer to its node, which in
turn stores its `HTREEITEM` in both trees. With that:

- *Lazy loading*: items are inserted with `cChildren = 1` and the children are
  created in `TVN_ITEMEXPANDING`, so opening a tree of millions of files does
  not insert millions of items.
- *Synchronized expansion, selection and scrolling*: each notification from one
  tree is mirrored in the other with a re-entrancy counter (`g_sync`). Scrolling
  is synchronized with `TVGN_FIRSTVISIBLE` on Windows, with `NSClipView`
  notifications on macOS and by forwarding `WM_MOUSEWHEEL` on Linux (generic
  SWELL does not expose the tree's scroll position).
- *Progress*: the thread publishes counters under a `WDL_Mutex`; the UI reads
  them with a 100 ms `WM_TIMER`. Cancelling sets a flag that the thread checks on
  every entry and on every block read.
- *Layout*: `WDL_WndSizer` distributes the width between the two trees on
  resize.

On Windows the dialog is created with `CreateDialogParamW` and the message loop
is Unicode, so paths can contain any character; the rest of the code works in
UTF-8 thanks to WDL's `win32_utf8` (`WDL_UTF8_HookTreeView`,
`SetDlgItemTextUTF8`...).

## Known limitations

- SWELL's native trees only show text: there are no icons or per-item colors,
  so the state goes in the text (`[only in A]`, `[missing]`, `[modified]`). On
  Windows, color could be added with `NM_CUSTOMDRAW`.
- On Linux, dragging one tree's scrollbar does not move the other (the wheel
  and the keyboard do, via selection).
- Directories with hundreds of thousands of files and "Show files" enabled are
  slow to expand, especially on macOS (`NSOutlineView` inserts row by row).

## Tests

`tests/engine_test.cpp` prints the compared tree with its counters, without a
UI. On Linux, after `make`:

```
g++ -O1 -DSWELL_TARGET_GDK=3 -IWDL/WDL $(pkg-config --cflags gdk-3.0) -o tests/engine_test \
    tests/engine_test.cpp diffengine.o swell*.o lice*.o wndsize.o \
    -lpthread -ldl $(pkg-config --libs gdk-3.0 freetype2 fontconfig) -lX11 -lXi -lGL
./tests/engine_test [--content] dirA dirB
```
