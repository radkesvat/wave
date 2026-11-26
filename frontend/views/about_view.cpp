#include "about_view.h"

#include "theme.h"

namespace wave {

void render_about(std::string_view version)
{
    ImGui::TextColored(theme::rgba(theme::Color::accent), "Wave %.*s", static_cast<int>(version.size()),
                       version.data());
    ImGui::TextWrapped("Wave is a browser panel for the WaterWall core. This workspace uses sample data.");
    ImGui::SeparatorText("Projects");
    ImGui::TextLinkOpenURL("Wave source", "https://github.com/radkesvat/wave");
    ImGui::TextLinkOpenURL("WaterWall core", "https://github.com/radkesvat/WaterWall");
    ImGui::SeparatorText("Notices");
    ImGui::TextLinkOpenURL("Wave (Mozilla Public License 2.0)", "/licenses/wave.txt");
    ImGui::TextLinkOpenURL("Dear ImGui (MIT)", "/licenses/dear-imgui.txt");
    ImGui::TextLinkOpenURL("Poppins Medium (SIL OFL 1.1)", "/licenses/poppins.txt");
    ImGui::TextLinkOpenURL("nlohmann/json (MIT)", "/licenses/nlohmann-json.txt");
    ImGui::TextLinkOpenURL("nlohmann/json third-party notices", "/licenses/nlohmann-json-third-party.txt");
    ImGui::TextLinkOpenURL("Nayuki QR Code generator (MIT)", "/licenses/nayuki-qr.txt");
    ImGui::TextLinkOpenURL("ImPlot (MIT)", "/licenses/implot.txt");
}

} // namespace wave
