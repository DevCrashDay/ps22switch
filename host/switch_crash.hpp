#pragma once

// Switch only: a CPU fault prints its registers and call chain into ps2run.log (host/switch_crash.cpp). Elsewhere there is nothing to
// report to and nothing to call.
#ifdef __SWITCH__
void switch_crash_report();
#else
inline void switch_crash_report() {}
#endif
