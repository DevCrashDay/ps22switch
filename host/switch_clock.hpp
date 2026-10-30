#pragma once

#ifdef __SWITCH__
// The clocks launch.txt asks for (PS2_CPU_MHZ, PS2_GPU_MHZ, PS2_MEM_MHZ; 1785, 921.6 and 1600 when not set, 0 keeps the system's), set
// when the game starts; switch_clock_restore() puts the system's back (also run by exit()).
void switch_clock_start();
void switch_clock_restore();
#endif
