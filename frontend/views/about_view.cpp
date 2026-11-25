#include "about_view.h"

#include "view_helpers.h"

namespace wave {

void render_about()
{
    ImGui::TextColored(view::mint, "Wave 0.1.0");
    ImGui::TextWrapped("Wave is a browser panel for the WaterWall core. This workspace uses sample data.");
    ImGui::SeparatorText("Projects");
    ImGui::TextLinkOpenURL("Wave source", "https://github.com/radkesvat/wave");
    ImGui::TextLinkOpenURL("WaterWall core", "https://github.com/radkesvat/WaterWall");
    ImGui::SeparatorText("Notices");
    ImGui::TextLinkOpenURL("Wave (Mozilla Public License 2.0)", "/licenses/wave.txt");
    ImGui::TextLinkOpenURL("Dear ImGui (MIT)", "/licenses/dear-imgui.txt");
    ImGui::TextLinkOpenURL("Karla (SIL Open Font License)", "/licenses/karla.txt");
    ImGui::TextLinkOpenURL("nlohmann/json (MIT)", "/licenses/nlohmann-json.txt");
    ImGui::TextLinkOpenURL("nlohmann/json third-party notices", "/licenses/nlohmann-json-third-party.txt");
    ImGui::TextLinkOpenURL("Nayuki QR Code generator (MIT)", "/licenses/nayuki-qr.txt");
}

} // namespace wave
