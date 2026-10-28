#pragma once

#include <functional>

bool open_window(bool fullscreen);
int run_window(const std::function<int()> &emulate);
// reads SDL's events, the keyboard and the game controllers into the console's two controller ports (once a frame in a window
// run); host/pad_test.cpp calls it without a window
void poll_input();
