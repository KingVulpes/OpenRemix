#include "ui/profiles.h"
#include <imgui.h>

#if REMIX_HAS_USD
#include "usd/mods.h"
#endif

namespace remix::ui {
  void drawMods() {
#if REMIX_HAS_USD
    const auto status = usd::modStatus();
    if (!status.active) {
      ImGui::TextWrapped("Mod loading is disabled for this session.");
      return;
    }
    bool enabled = status.enabled;
    if (ImGui::Checkbox("Enable mods", &enabled))
      usd::setModsEnabled(enabled);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Apply replacements. Mods stay loaded while off, so turning them back on is instant.");
    ImGui::SameLine();
    ImGui::BeginDisabled(status.loading);
    if (ImGui::Button("Reload"))
      usd::reloadMods();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Parse the mods directory again");
    ImGui::TextWrapped("%s", status.directory.c_str());
    if (status.loading)
      ImGui::TextUnformatted("Loading...");
    if (!status.diagnostic.empty())
      ImGui::TextColored(ImVec4(1, .55f, .4f, 1), "%s", status.diagnostic.c_str());
    ImGui::Separator();
    if (status.mods.empty()) {
      ImGui::TextWrapped(status.loading ? "No mods loaded yet." : "No mods found (mod.usda, mod.usdc or mod.usd).");
      return;
    }
    size_t on = 0;
    for (const auto &mod : status.mods)
      on += mod.enabled;
    ImGui::Text("%zu of %zu mods enabled", on, status.mods.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("All"))
      for (const auto &mod : status.mods)
        usd::setModEnabled(mod.name, true);
    ImGui::SameLine();
    if (ImGui::SmallButton("None"))
      for (const auto &mod : status.mods)
        usd::setModEnabled(mod.name, false);
    ImGui::BeginDisabled(!status.enabled);
    if (ImGui::BeginTable("Mods", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Meshes");
      ImGui::TableSetupColumn("Lights");
      ImGui::TableSetupColumn("Materials");
      ImGui::TableHeadersRow();
      for (const auto &mod : status.mods) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool modEnabled = mod.enabled;
        ImGui::PushID(mod.name.c_str());
        if (ImGui::Checkbox(mod.name.c_str(), &modEnabled))
          usd::setModEnabled(mod.name, modEnabled);
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::Text("%zu", mod.meshes);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", mod.lights);
        ImGui::TableNextColumn();
        ImGui::Text("%zu", mod.materials);
      }
      ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Where mods replace the same asset, the first enabled one in this list wins.");
#else
    ImGui::TextWrapped("This runtime was built without USD support.");
#endif
  }
} // namespace remix::ui
