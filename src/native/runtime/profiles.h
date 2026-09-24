#pragma once
#include "runtime/settings.h"
#include <filesystem>

namespace remix::config {
  struct PanelState {
    bool right = false;
    float width = 520;
    int tab = 0;
    bool operator==(const PanelState &) const = default;
  };

  // CPU-only session owned by the runtime. File operations never mutate a live
  // snapshot until parsing/writing succeeds. UI and API edits share this state.
  class Profiles {
  public:
    void startup(const std::filesystem::path &directory = defaultDirectory());
    static std::filesystem::path defaultDirectory();
    static bool validName(std::string_view name);
    bool edit(const Settings &settings);
    bool load(const std::string &name);
    bool create(const std::string &name);
    bool duplicate(const std::string &name);
    bool save();
    void revert();
    void refresh();
    void previewPanel(const PanelState &panel) {
      m_panel = panel;
      m_panelDirty = true;
    }
    void savePanel(const PanelState &panel);
    bool dirty() const { return m_needsSave || m_current != m_saved; }
    const Settings &current() const { return m_current; }
    const std::string &active() const { return m_active; }
    const std::vector<std::string> &files() const { return m_files; }
    const std::filesystem::path &folder() const { return m_folder; }
    const PanelState &panel() const { return m_panel; }
    const std::string &message() const { return m_message; }
    bool hasError() const { return m_error; }
    void clearMessage() {
      m_message.clear();
      m_error = false;
    }

  private:
    bool unique(const std::string &name);
    bool writeProfile(const std::string &name, bool replace);
    bool readProfile(const std::string &name, Settings &settings, UnknownSettings &unknown);
    bool fail(const std::string &message);
    void remember();
    Settings m_current, m_saved;
    UnknownSettings m_unknown;
    std::filesystem::path m_directory, m_folder;
    std::vector<std::string> m_files;
    std::string m_active = "Default", m_last, m_message;
    bool m_persisted = false, m_needsSave = false, m_error = false;
    PanelState m_panel;
    bool m_panelDirty = false;
  };
} // namespace remix::config
