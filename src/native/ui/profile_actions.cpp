#include "ui/profiles.h"

namespace remix::ui {
  bool ProfileActions::execute(config::Profiles &profiles) {
    bool ok = true;
    switch (pending) {
    case ProfileAction::Load:
      ok = profiles.load(name);
      break;
    case ProfileAction::New:
      ok = profiles.create(name);
      break;
    case ProfileAction::Revert:
      profiles.revert();
      break;
    case ProfileAction::None:
      break;
    }
    if (ok)
      pending = ProfileAction::None;
    return ok;
  }
  bool ProfileActions::request(config::Profiles &profiles, ProfileAction action, const std::string &target) {
    pending = action;
    name = target;
    if (profiles.dirty())
      return false;
    // A failure without dirty edits is reported inline, with no confirmation.
    bool ok = execute(profiles);
    pending = ProfileAction::None;
    return ok;
  }
  bool ProfileActions::resolve(config::Profiles &profiles, UnsavedChoice choice) {
    if (choice == UnsavedChoice::Cancel) {
      pending = ProfileAction::None;
      return true;
    }
    if (choice == UnsavedChoice::Save && !profiles.save())
      return false;
    return execute(profiles);
  }
} // namespace remix::ui
