#ifndef _GRENDER_PROFILING_H_
#define _GRENDER_PROFILING_H_

#ifdef GRENDER_ENABLE_PROFILING

void grProfFrameBegin(void);
void grProfFrameEnd(void);

#define GR_PROF_FRAME_BEGIN() grProfFrameBegin()
#define GR_PROF_FRAME_END() grProfFrameEnd()

#else

#define GR_PROF_FRAME_BEGIN()
#define GR_PROF_FRAME_END()

#endif

#endif
