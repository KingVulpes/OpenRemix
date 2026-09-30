#include "ui/profiles.h"
#include <cstdio>
#include <imgui.h>

#if REMIX_HAS_USD
#include "usd/mods.h"
#endif

namespace remix::ui {
#if REMIX_HAS_USD
  namespace {
    bool part(const char *label, bool *value, size_t count, const char *tooltip) {
      char text[96];
      std::snprintf(text, sizeof text, count == size_t(-1) ? "%s" : "%s (%zu)", label, count);
      const bool changed = ImGui::Checkbox(text, value);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
      return changed;
    }
  } // namespace
#endif

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
      on += mod.parts.enabled;
    ImGui::Text("%zu of %zu mods enabled", on, status.mods.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("All"))
      for (const auto &mod : status.mods)
        usd::setModEnabled(mod.name, true);
    ImGui::SameLine();
    if (ImGui::SmallButton("None"))
      for (const auto &mod : status.mods)
        usd::setModEnabled(mod.name, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset parts"))
      for (const auto &mod : status.mods)
        usd::setModParts(mod.name, {.enabled = mod.parts.enabled});
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Apply every part of each enabled mod");
    ImGui::BeginDisabled(!status.enabled);
    for (const auto &mod : status.mods) {
      auto parts = mod.parts;
      ImGui::PushID(mod.name.c_str());
      bool changed = ImGui::Checkbox("##enabled", &parts.enabled);
      ImGui::SameLine();
      const bool open = ImGui::TreeNodeEx(mod.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
      if (!(parts == usd::ModParts{.enabled = parts.enabled})) {
        ImGui::SameLine();
        ImGui::TextDisabled("(partial)");
      }
      if (open) {
        ImGui::BeginDisabled(!parts.enabled);
        changed |= part("Meshes", &parts.meshes, mod.meshes,
                        "Replacement geometry. Off: the game's mesh draws instead (attached lights can stay).");
        changed |= part("Attached lights", &parts.lights, mod.lights, "Lights authored under mesh replacements");
        changed |= part("Light overrides", &parts.lightOverrides, mod.lightOverrides,
                        "Replacements and removals of the game's own lights");
        changed |= part("Materials", &parts.materials, mod.materials, "Material (texture) replacements");
        changed |= part("Keep original draws", &parts.keepOriginal, size_t(-1),
                        "Draw the game's mesh beside its replacement instead of hiding it");
        ImGui::EndDisabled();
        ImGui::TreePop();
      }
      if (changed)
        usd::setModParts(mod.name, parts);
      ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Where mods replace the same asset, the first one in this list that applies it wins.");
#else
    ImGui::TextWrapped("This runtime was built without USD support.");
#endif
  }
} // namespace remix::ui
