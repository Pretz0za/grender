/**
 * Command console: a stateless, in-window command line. It owns exactly
 * three things -- the current input line, the last command's result message,
 * and open/closed state -- and nothing else persists between commands, so
 * there is no session/mode machinery to keep in sync with the rest of the
 * renderer. Text input (grConsoleProcessInput) and drawing (grConsoleBuild)
 * are kept separate from what commands actually *do*: every command is a
 * small static handler below, dispatched by name out of GR_CONSOLE_COMMANDS.
 *
 * To add a new command: write a `static void cmdFoo(grRenderer *r, int argc,
 * char **argv, grConsoleResult *out)` below and add one row to
 * GR_CONSOLE_COMMANDS. Handlers only ever touch grender's own state
 * (highlight, camera, styling, ...) through the same functions an
 * application embedding grender would use -- never gviz internals directly.
 */

#include "grInternal.h"

#include "ds/gvizSubgraph.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------------------
// Command table
// ------------------------------------------------------------------------------

/** Feedback from a command handler: whether it succeeded, and a short
 *  message to show in the console (an error description, or a confirmation
 *  like "found vertex 5"). Zero-initialized by grConsoleRun before dispatch,
 *  so a handler that doesn't touch @p out is reported as a silent success. */
typedef struct grConsoleResult {
  bool ok;
  char message[128];
} grConsoleResult;

typedef void (*grConsoleCommandFn)(grRenderer *r, int argc, char **argv,
                                   grConsoleResult *out);

typedef struct grConsoleCommandDef {
  const char *name;
  grConsoleCommandFn fn;
} grConsoleCommandDef;

/** Marks @p out as failed and formats @p fmt into its message. Handlers
 *  below call this on every early-return error path instead of writing
 *  out->ok = false themselves, so a failure can never be reported as a
 *  success by a forgotten flag. */
static void consoleFail(grConsoleResult *out, const char *fmt, ...) {
  out->ok = false;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out->message, sizeof(out->message), fmt, ap);
  va_end(ap);
}

/**
 * "find <id>": clears any active highlight, highlights vertex <id> alone
 * (no neighbors/edges -- unlike click-to-pick, this is a plain locate, not an
 * inspect-neighborhood action), and centers the camera on it without
 * changing zoom.
 */
static void cmdFind(grRenderer *r, int argc, char **argv,
                    grConsoleResult *out) {
  if (argc != 2) {
    // No angle brackets: the console's bitmap font (GR_FONT in grStats.c)
    // has no glyph for '<'/'>', which would otherwise silently leave gaps.
    consoleFail(out, "usage: find id");
    return;
  }
  if (!r->graph) {
    consoleFail(out, "no graph attached");
    return;
  }

  char *end = NULL;
  long id = strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || id < 0) {
    consoleFail(out, "invalid vertex id: %s", argv[1]);
    return;
  }

  size_t vertexCount = gvizEmbeddedGraphPositionCount(r->graph);
  if ((size_t)id >= vertexCount) {
    consoleFail(out, "no vertex %ld (graph has %zu)", id, vertexCount);
    return;
  }
  // r->posStaging holds exactly what's currently drawn for this vertex --
  // float, and already PCA-projected to 3D for a 4D embedding (see
  // uploadPositions) -- so reading it here to center the camera stays
  // correct for 2D/3D/4D alike without redoing that projection.
  if (!r->posStaging || (size_t)id >= r->posCapacity) {
    consoleFail(out, "vertex %ld not renderable yet", id);
    return;
  }

  grRendererClearHighlight(r);

  const gvizSubgraph *structure = gvizEmbeddedGraphStructure(r->graph);
  gvizSubgraph pick = gvizSubgraphCreateEmpty(structure->g);
  if (!pick.g) {
    consoleFail(out, "internal error selecting vertex %ld", id);
    return;
  }
  gvizSubgraphShowVertex(&pick, (size_t)id);
  gvizSubgraphRebuild(&pick);
  grRendererSetHighlight(r, &pick, GR_RGBA8(255, 210, 80, 255), 0);
  gvizSubgraphRelease(&pick);

  double point[3] = {0.0, 0.0, 0.0};
  for (size_t d = 0; d < r->posDim && d < 3; d++)
    point[d] = (double)r->posStaging[(size_t)id * r->posDim + d];
  grCameraCenterOn(&r->camera, point);

  out->ok = true;
  snprintf(out->message, sizeof(out->message), "found vertex %ld", id);
}

static const grConsoleCommandDef GR_CONSOLE_COMMANDS[] = {
    {"find", cmdFind},
};
#define GR_CONSOLE_COMMAND_COUNT                                             \
  (sizeof(GR_CONSOLE_COMMANDS) / sizeof(GR_CONSOLE_COMMANDS[0]))

#define GR_CONSOLE_MAX_ARGS 8

/** Splits @p buf in place on whitespace (like strtok, but reentrant and
 *  bounded), writing up to @p maxArgv token pointers into @p argv.
 *  @return the number of tokens found. */
static int consoleTokenize(char *buf, char **argv, int maxArgv) {
  int argc = 0;
  char *p = buf;
  while (*p && argc < maxArgv) {
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t')
      p++;
    if (*p)
      *p++ = '\0';
  }
  return argc;
}

void grConsoleRun(grRenderer *r, const char *line) {
  char buf[sizeof(r->consoleInput)];
  snprintf(buf, sizeof(buf), "%s", line);

  char *argv[GR_CONSOLE_MAX_ARGS];
  int argc = consoleTokenize(buf, argv, GR_CONSOLE_MAX_ARGS);
  if (argc == 0)
    return;

  const grConsoleCommandDef *cmd = NULL;
  for (size_t i = 0; i < GR_CONSOLE_COMMAND_COUNT; i++) {
    if (strcasecmp(argv[0], GR_CONSOLE_COMMANDS[i].name) == 0) {
      cmd = &GR_CONSOLE_COMMANDS[i];
      break;
    }
  }

  grConsoleResult result = {0};
  if (!cmd) {
    snprintf(result.message, sizeof(result.message), "unknown command: %s",
             argv[0]);
  } else {
    result.ok = true; // default to success unless the handler says otherwise
    cmd->fn(r, argc, argv, &result);
  }

  r->consoleMessageIsError = !result.ok;
  snprintf(r->consoleMessage, sizeof(r->consoleMessage), "%s",
           result.message);
}

// ------------------------------------------------------------------------------
// Open/close + input
// ------------------------------------------------------------------------------

void grConsoleOpen(grRenderer *r) {
  if (!r || r->consoleOpen)
    return;
  r->consoleOpen = true;
  r->consoleInput[0] = '\0';
  r->consoleInputLen = 0;
  r->consoleMessage[0] = '\0';
  r->consoleMessageIsError = false;
}

void grConsoleClose(grRenderer *r) {
  if (!r)
    return;
  r->consoleOpen = false;
  // The overlay rebuild that draws the console panel only runs every frame
  // while it's open (see statsOverlayNeedsRebuild); force one more so the
  // panel doesn't linger on screen as a stale primitive after this frame.
  r->statsOverlayDirty = true;
}

/** Appends @p cp to the input line if it's printable ASCII (the bitmap font,
 *  see grOverlayCharHasGlyph, can't draw anything else) and there's room. */
static void consoleAppendChar(grRenderer *r, uint32_t cp) {
  if (cp < 32 || cp > 126)
    return;
  if (r->consoleInputLen + 1 < sizeof(r->consoleInput)) {
    r->consoleInput[r->consoleInputLen++] = (char)cp;
    r->consoleInput[r->consoleInputLen] = '\0';
  }
}

void grConsoleProcessInput(grRenderer *r) {
  // Drained as one queue, in delivery order, rather than as separate key and
  // char queues -- see grPendingConsoleEvent for why: it's what makes "type
  // '3', then press Enter" within the same frame apply in that order.
  const grPendingConsoleEvent *events = r->pendingConsoleEvents.arr;
  for (size_t i = 0; i < r->pendingConsoleEvents.count; i++) {
    const grPendingConsoleEvent *ev = &events[i];
    if (ev->isChar) {
      consoleAppendChar(r, (uint32_t)ev->code);
      continue;
    }
    switch (ev->code) {
    case GR_KEY_ENTER:
      grConsoleRun(r, r->consoleInput);
      r->consoleInput[0] = '\0';
      r->consoleInputLen = 0;
      break;
    case GR_KEY_ESCAPE:
      grConsoleClose(r);
      break;
    case GR_KEY_BACKSPACE:
      if (r->consoleInputLen > 0)
        r->consoleInput[--r->consoleInputLen] = '\0';
      break;
    default:
      break;
    }
  }
  r->pendingConsoleEvents.count = 0;
}

// ------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------

void grConsoleBuild(grRenderer *r, double fbw, double fbh) {
  if (!r->consoleOpen)
    return;

  double s = r->contentScale > 0.0 ? r->contentScale : 1.0;
  const double margin = 12.0 * s;
  const double pad = 10.0 * s;
  const double fontPx = 1.4 * s;
  const double lineH = GR_FONT_ROWS * fontPx + 4.0 * s;
  bool hasMessage = r->consoleMessage[0] != '\0';

  double panelH = pad * 2.0 + lineH * (hasMessage ? 2.0 : 1.0);
  double x0 = margin, x1 = fbw - margin;
  double y1 = fbh - margin, y0 = y1 - panelH;
  if (x1 <= x0 || y0 < 0.0)
    return;

  const uint32_t bgColor = GR_RGBA8(15, 17, 22, 235);
  const uint32_t frameColor = GR_RGBA8(101, 197, 255, 130);
  const uint32_t promptColor = GR_RGBA8(101, 197, 255, 255);
  const uint32_t textColor = GR_RGBA8(235, 235, 240, 255);
  const uint32_t cursorColor = GR_RGBA8(235, 235, 240, 210);
  const uint32_t okColor = GR_RGBA8(126, 217, 130, 255);
  const uint32_t errColor = GR_RGBA8(255, 118, 118, 255);

  grOverlayPushRect(r, x0, y0, x1, y1, bgColor);
  grOverlayPushFrame(r, x0, y0, x1, y1, 1.0 * s, frameColor);

  // ">" has no glyph in the tiny bitmap font (see GR_FONT in grStats.c), so
  // it would silently draw nothing; ":" is both available and a reasonably
  // conventional command-line prompt.
  double promptW = GR_FONT_ADVANCE * fontPx * 2.0; // ": "
  double tx = x0 + pad, ty = y0 + pad;
  grOverlayPushText(r, tx, ty, fontPx, promptColor, ":");
  grOverlayPushText(r, tx + promptW, ty, fontPx, textColor, r->consoleInput);

  double cursorX =
      tx + promptW + grOverlayTextWidth(r->consoleInput, fontPx);
  grOverlayPushRect(r, cursorX, ty, cursorX + 1.5 * s, ty + GR_FONT_ROWS * fontPx,
                    cursorColor);

  if (hasMessage)
    grOverlayPushText(r, x0 + pad, ty + lineH, fontPx,
                      r->consoleMessageIsError ? errColor : okColor,
                      r->consoleMessage);
}
