/**
 * Command console: a stateless, in-window command line. It owns exactly
 * three things -- the current input line, the last command's result message,
 * and open/closed state -- and nothing else persists between commands, so
 * there is no session/mode machinery to keep in sync with the rest of the
 * renderer. Text input AND drawing are both just one ImGui::InputText call
 * (grConsoleBuild) -- ImGui owns the caret/selection/history state, grender
 * only owns the underlying char buffer (r->consoleInput) and what happens
 * when Enter is pressed. That's kept separate from what commands actually
 * *do*: every command is a small static handler below, dispatched by name
 * out of GR_CONSOLE_COMMANDS.
 *
 * To add a new command: write a `static void cmdFoo(grRenderer *r, int argc,
 * char **argv, grConsoleResult *out)` below and add one row to
 * GR_CONSOLE_COMMANDS. Handlers only ever touch grender's own state
 * (highlight, camera, styling, ...) through the same functions an
 * application embedding grender would use -- never gviz internals directly.
 */

#include "grInternal.h"

#include <cfloat>
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
 * inspect-neighborhood action), and focuses the camera on it (see
 * grRendererFocusVertex) -- the same center-and-zoom behavior as the C key.
 */
static void cmdFind(grRenderer *r, int argc, char **argv,
                    grConsoleResult *out) {
  if (argc != 2) {
    consoleFail(out, "usage: find id");
    return;
  }
  if (!r->graph) {
    consoleFail(out, "no graph attached");
    return;
  }
  if (!r->backingGraph) {
    // Vertex selection needs raw parent-graph access (EnsureLayout) that
    // gviz::Subgraph deliberately never exposes -- see grRendererSetGraph's
    // doc comment on backingGraph.
    consoleFail(out, "no backing graph attached");
    return;
  }

  char *end = NULL;
  long id = strtol(argv[1], &end, 10);
  if (end == argv[1] || *end != '\0' || id < 0) {
    consoleFail(out, "invalid vertex id: %s", argv[1]);
    return;
  }

  size_t vertexCount = r->graph->PositionCount();
  if ((size_t)id >= vertexCount) {
    consoleFail(out, "no vertex %ld (graph has %zu)", id, vertexCount);
    return;
  }
  // r->posStaging holds exactly what's currently drawn for this vertex --
  // float, and already PCA-projected to 3D for a 4D embedding (see
  // uploadPositions) -- so grRendererFocusVertex reading it to frame the
  // camera stays correct for 2D/3D/4D alike without redoing that projection.
  if (!r->posStaging || (size_t)id >= r->posCapacity) {
    consoleFail(out, "vertex %ld not renderable yet", id);
    return;
  }

  grRendererClearHighlight(r);

  try {
    /* See grHighlightCopySubgraph: refresh the shared layout on demand so
     * the full-subgraph pick works on graphs that grew since the last use. */
    r->backingGraph->EnsureLayout();
    gviz::Subgraph pick = gviz::Subgraph::CreateEmpty(*r->backingGraph);
    // (size_t)id is a local index (this command's own contract, matching
    // the rest of grender's public API -- see grRendererSetGraph's INDEXING
    // CONVENTION); Subgraph::ShowVertex needs the backing graph's raw id.
    size_t raw = r->structureView ? r->structureView->LocalToRaw((size_t)id)
                                  : (size_t)id;
    pick.ShowVertex(raw);
    pick.Rebuild();
    grRendererSetHighlight(r, pick, GR_RGBA8(255, 210, 80, 255), 0);
  } catch (const std::exception &) {
    consoleFail(out, "internal error selecting vertex %ld", id);
    return;
  }

  grRendererFocusVertex(r, (size_t)id);

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
// Open/close
// ------------------------------------------------------------------------------

void grConsoleOpen(grRenderer *r) {
  if (!r || r->consoleOpen)
    return;
  r->consoleOpen = true;
  r->consoleInput[0] = '\0';
  r->consoleMessage[0] = '\0';
  r->consoleMessageIsError = false;
}

void grConsoleClose(grRenderer *r) {
  if (!r)
    return;
  r->consoleOpen = false;
}

// ------------------------------------------------------------------------------
// Drawing + input (both are just one ImGui::InputText call -- see the file
// header comment)
// ------------------------------------------------------------------------------

void grConsoleBuild(grRenderer *r, double fbw, double fbh) {
  if (!r->consoleOpen)
    return;

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                           ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoMove;
  ImGui::SetNextWindowPos(ImVec2(20.0f, (float)fbh - 20.0f), ImGuiCond_Always,
                          ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2((float)fbw - 40.0f, 0.0f), ImGuiCond_Always);
  ImGui::Begin("##Console", nullptr, flags);

  // Grabs keyboard focus the frame the console opens (and every frame after,
  // if nothing has stolen it -- SetKeyboardFocusHere before the widget is
  // the standard "always-focused single input" idiom) so a user can type a
  // command immediately without an extra click, matching the old renderer's
  // "console owns all input while open" behavior.
  if (ImGui::IsWindowAppearing())
    ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##input", r->consoleInput, sizeof(r->consoleInput),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    grConsoleRun(r, r->consoleInput);
    r->consoleInput[0] = '\0';
    ImGui::SetKeyboardFocusHere(-1); // keep focus in the input box after Enter
  }

  if (r->consoleMessage[0] != '\0') {
    ImVec4 color = r->consoleMessageIsError ? ImVec4(1.0f, 0.46f, 0.46f, 1.0f)
                                            : ImVec4(0.49f, 0.85f, 0.51f, 1.0f);
    ImGui::TextColored(color, "%s", r->consoleMessage);
  }

  ImGui::End();
}
