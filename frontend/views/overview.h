#pragma once

#include "overview_data.h"

#include <string>

namespace wave {

void render_overview(const OverviewData* data, bool loading);
std::string describe_overview(const OverviewData* data, bool loading);

} // namespace wave
