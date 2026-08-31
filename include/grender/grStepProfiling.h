#ifndef _GRENDER_STEP_PROFILING_H_
#define _GRENDER_STEP_PROFILING_H_

/**
 * Opt-in timing for gviz embedder Step() calls, enabled by configuring with
 * -DGRENDER_ENABLE_STEP_PROFILING=ON (see CMakeLists.txt). Wrap each Step()
 * call site in a demo with GR_PROF_STEP_BEGIN()/GR_PROF_STEP_END(); a report
 * (call count, avg/min/max ms) prints to stderr on exit. When the flag is
 * off, both macros expand to nothing, so the wrapping can stay in demo code
 * unconditionally.
 */

#ifdef GRENDER_ENABLE_STEP_PROFILING

void grProfStepBegin(void);
void grProfStepEnd(void);

#define GR_PROF_STEP_BEGIN() grProfStepBegin()
#define GR_PROF_STEP_END() grProfStepEnd()

#else

#define GR_PROF_STEP_BEGIN()
#define GR_PROF_STEP_END()

#endif

#endif
