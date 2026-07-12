#include "grInternal.h"

#import <AppKit/AppKit.h>

static grRenderer *g_statsMenuRenderer = NULL;
static NSMenu *g_chartsSubmenu = NULL;
static NSMenuItem *g_overlayMenuItem = NULL;

@interface GRStatsMenuTarget : NSObject
@end

@implementation GRStatsMenuTarget

- (void)toggleOverlay:(NSMenuItem *)item {
  if (!g_statsMenuRenderer)
    return;
  bool show = item.state != NSControlStateValueOn;
  grRendererShowStats(g_statsMenuRenderer, show);
  item.state = show ? NSControlStateValueOn : NSControlStateValueOff;
}

- (void)toggleSeries:(NSMenuItem *)item {
  if (!g_statsMenuRenderer)
    return;
  size_t idx = (size_t)item.tag;
  bool show = item.state != NSControlStateValueOn;
  grRendererShowStatSeries(g_statsMenuRenderer, idx, show);
  item.state = show ? NSControlStateValueOn : NSControlStateValueOff;
}

@end

static GRStatsMenuTarget *g_statsMenuTarget = nil;

static NSMenuItem *makeCheckItem(NSString *title, SEL action, NSInteger tag,
                                 bool checked) {
  NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                action:action
                                         keyEquivalent:@""];
  item.target = g_statsMenuTarget;
  item.tag = tag;
  item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
  return item;
}

static void installWindowMenu(NSMenu *menubar) {
  NSMenuItem *windowMenuItem =
      [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
  NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
  [NSApp setWindowsMenu:windowMenu];
  [windowMenuItem setSubmenu:windowMenu];

  [windowMenu addItemWithTitle:@"Minimize"
                        action:@selector(performMiniaturize:)
                 keyEquivalent:@"m"];
  [windowMenu addItemWithTitle:@"Zoom"
                        action:@selector(performZoom:)
                 keyEquivalent:@""];
  [windowMenu addItem:[NSMenuItem separatorItem]];
  [windowMenu addItemWithTitle:@"Bring All to Front"
                        action:@selector(arrangeInFront:)
                 keyEquivalent:@""];
  [menubar addItem:windowMenuItem];
}

void grPlatformInitApplication(void) {
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;

  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

  g_statsMenuTarget = [[GRStatsMenuTarget alloc] init];

  NSString *appName = [[NSProcessInfo processInfo] processName];

  NSMenu *menubar = [[NSMenu alloc] init];

  NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
  NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
  [appMenu addItemWithTitle:[NSString stringWithFormat:@"About %@", appName]
                     action:@selector(orderFrontStandardAboutPanel:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %@", appName]
                     action:@selector(hide:)
              keyEquivalent:@"h"];
  [appMenu addItemWithTitle:@"Hide Others"
                     action:@selector(hideOtherApplications:)
              keyEquivalent:@"h"];
  [[appMenu itemAtIndex:appMenu.numberOfItems - 1]
      setKeyEquivalentModifierMask:NSEventModifierFlagOption |
                                   NSEventModifierFlagCommand];
  [appMenu addItemWithTitle:@"Show All"
                     action:@selector(unhideAllApplications:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  [appMenuItem setSubmenu:appMenu];
  [menubar addItem:appMenuItem];

  SEL setAppleMenu = NSSelectorFromString(@"setAppleMenu:");
  if ([NSApp respondsToSelector:setAppleMenu])
    [NSApp performSelector:setAppleMenu withObject:appMenu];

  NSMenuItem *viewMenuItem = [[NSMenuItem alloc] initWithTitle:@"View"
                                                        action:nil
                                                 keyEquivalent:@""];
  NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];

  g_overlayMenuItem =
      makeCheckItem(@"Show Stats Overlay", @selector(toggleOverlay:), -1, true);
  [viewMenu addItem:g_overlayMenuItem];
  [viewMenu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *chartsItem = [[NSMenuItem alloc] initWithTitle:@"Charts"
                                                      action:nil
                                               keyEquivalent:@""];
  g_chartsSubmenu = [[NSMenu alloc] initWithTitle:@"Charts"];
  [chartsItem setSubmenu:g_chartsSubmenu];
  [viewMenu addItem:chartsItem];

  [viewMenuItem setSubmenu:viewMenu];
  [menubar addItem:viewMenuItem];

  installWindowMenu(menubar);

  [NSApp setMainMenu:menubar];
}

void grPlatformStatsMenuRefresh(grRenderer *r) {
  if (!g_chartsSubmenu)
    grPlatformInitApplication();

  g_statsMenuRenderer = r;

  if (g_overlayMenuItem)
    g_overlayMenuItem.state =
        grRendererStatsShown(r) ? NSControlStateValueOn : NSControlStateValueOff;

  while (g_chartsSubmenu.numberOfItems > 0)
    [g_chartsSubmenu removeItemAtIndex:0];

  size_t count = grRendererStatSeriesCount(r);
  if (count == 0) {
    NSMenuItem *empty =
        [[NSMenuItem alloc] initWithTitle:@"(no stat series on graph)"
                                   action:nil
                            keyEquivalent:@""];
    empty.enabled = NO;
    [g_chartsSubmenu addItem:empty];
    return;
  }

  for (size_t i = 0; i < count; i++) {
    const char *name = grRendererStatSeriesName(r, i);
    if (!name)
      continue;
    NSString *title = [NSString stringWithUTF8String:name];
    bool shown = grRendererStatSeriesShown(r, i);
    [g_chartsSubmenu addItem:makeCheckItem(title, @selector(toggleSeries:),
                                           (NSInteger)i, shown)];
  }
}
