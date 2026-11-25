#pragma once

#include "overview_data.h"

#include <string>

struct ImDrawList;

namespace wave {

float overview_height(float content_width, bool has_data);
void render_overview(ImDrawList& draw, float x, float y, float width,
                     const OverviewData* data, bool loading);
std::string describe_overview(const OverviewData* data, bool loading);

} // namespace wave
