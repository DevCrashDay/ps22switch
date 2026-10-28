#pragma once

#include "gs.hpp"

namespace ps2::gs::raster {

void reset();
// Pixel history for the draw trace (gs.cpp): with PS2_TRACE_PIXEL=<x>,<y>, every primitive of a traced frame that reaches
// that pixel of whatever buffer it draws to prints what happened there (software renderer only: PS2_GS=cpu).
struct Probe {
    bool on = false;
    std::uint64_t frame = 0, number = 0;
};
extern Probe g_probe;
void draw(const State &s, const Vertex *v, int count);

}
