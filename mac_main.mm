/*
  DirDiff - macOS entry point (SWELL on Cocoa).

  Instead of a MainMenu.xib, the application menu is built in code, so the
  app can be compiled without Xcode/Interface Builder (clang and the
  Command Line Tools are enough; see CMakeLists.txt).

  It also synchronizes the vertical scrolling of the two trees: on macOS
  each SWELL SysTreeView32 is an NSOutlineView inside an NSScrollView.

  Compile without ARC (like the rest of SWELL).
*/

#import <Cocoa/Cocoa.h>

#include "../WDL/swell/swell.h"
#import "../WDL/swell/swellappmain.h"
#include "resource.h"

extern const char **g_argv; // main.cpp
extern int g_argc;

// SWELL's look switch (swell-wnd.mm): &1 = rounded Aqua buttons / popups,
// &2 = Big Sur style lists. 0 gives the old square REAPER-style controls.
extern int g_swell_osx_style;

/* ----------------------------------------------------------------------- */
/* Synchronized scrolling                                                   */
/* ----------------------------------------------------------------------- */

@interface DirDiffScrollLinker : NSObject
{
@public
  NSScrollView *m_views[2];
  BOOL m_busy;
}
- (void)boundsChanged:(NSNotification *)notification;
@end

@implementation DirDiffScrollLinker
- (void)boundsChanged:(NSNotification *)notification
{
  if (m_busy) return;
  NSClipView *src = (NSClipView *)[notification object];
  NSScrollView *dst = nil;
  if (m_views[0] && src == [m_views[0] contentView]) dst = m_views[1];
  else if (m_views[1] && src == [m_views[1] contentView]) dst = m_views[0];
  if (!dst) return;

  m_busy = YES;
  NSClipView *clip = [dst contentView];
  NSPoint p = [clip bounds].origin;
  p.y = [src bounds].origin.y;
  [clip scrollToPoint:p];
  [dst reflectScrolledClipView:clip];
  m_busy = NO;
}
@end

static DirDiffScrollLinker *s_linker;

static NSScrollView *ScrollViewFor(HWND h)
{
  id v = (id)h;
  if (!v) return nil;
  if ([v isKindOfClass:[NSScrollView class]]) return (NSScrollView *)v;
  if ([v isKindOfClass:[NSView class]]) return [(NSView *)v enclosingScrollView];
  return nil;
}

void DirDiff_MacUnlinkScroll()
{
  if (!s_linker) return;
  [[NSNotificationCenter defaultCenter] removeObserver:s_linker];
  s_linker->m_views[0] = s_linker->m_views[1] = nil;
}

void DirDiff_MacLinkScroll(HWND a, HWND b)
{
  NSScrollView *sa = ScrollViewFor(a), *sb = ScrollViewFor(b);
  if (!sa || !sb) return;
  if (!s_linker) s_linker = [[DirDiffScrollLinker alloc] init]; // lives as long as the application
  DirDiff_MacUnlinkScroll();

  s_linker->m_views[0] = sa;
  s_linker->m_views[1] = sb;
  s_linker->m_busy = NO;

  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  for (int i = 0; i < 2; i++)
  {
    NSClipView *clip = [s_linker->m_views[i] contentView];
    [clip setPostsBoundsChangedNotifications:YES];
    [nc addObserver:s_linker selector:@selector(boundsChanged:) name:NSViewBoundsDidChangeNotification object:clip];
  }
}

/* ----------------------------------------------------------------------- */
/* main                                                                     */
/* ----------------------------------------------------------------------- */

static NSMenuItem *AddItem(NSMenu *menu, NSString *title, SEL action, NSString *key, id target, int tag)
{
  NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
  if (target) [it setTarget:target];
  if (tag) [it setTag:tag];
  [menu addItem:it];
  [it release];
  return it;
}

int main(int argc, const char *argv[])
{
  g_argc = argc;
  g_argv = argv;
  g_swell_osx_style = 1 | 2;

  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  [SWELLApplication sharedApplication]; // NSApp becomes SWELLApplication
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  SWELLAppController *controller = [[SWELLAppController alloc] init];

  // application menu (what would normally come from MainMenu.xib)
  NSMenu *menubar = [[NSMenu alloc] initWithTitle:@""];

  NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"DirDiff"];
  AddItem(appMenu, @"Hide DirDiff", @selector(hide:), @"h", nil, 0);
  [appMenu addItem:[NSMenuItem separatorItem]];
  // the tag reaches SWELLAppMain(SWELLAPP_ONCOMMAND) -> WM_COMMAND(ID_QUIT)
  AddItem(appMenu, @"Quit DirDiff", @selector(onSysMenuCommand:), @"q", controller, ID_QUIT);
  [appItem setSubmenu:appMenu];
  [menubar addItem:appItem];

  NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  AddItem(editMenu, @"Cut", @selector(cut:), @"x", nil, 0);
  AddItem(editMenu, @"Copy", @selector(copy:), @"c", nil, 0);
  AddItem(editMenu, @"Paste", @selector(paste:), @"v", nil, 0);
  AddItem(editMenu, @"Select All", @selector(selectAll:), @"a", nil, 0);
  [editItem setSubmenu:editMenu];
  [menubar addItem:editItem];

  [NSApp setMainMenu:menubar];

  // what NSApplicationMain does when loading the nib: SWELLAppController
  // registers as delegate and initializes SWELL (SWELLAPP_ONLOAD); once
  // launching finishes, applicationDidFinishLaunching sends SWELLAPP_LOADED
  [controller awakeFromNib];

  [NSApp activateIgnoringOtherApps:YES];
  [NSApp run];

  [pool release];
  return 0;
}
