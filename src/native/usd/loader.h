#pragma once
#include "usd/snapshot.h"
#include <map>
#include <memory>
#include <remix/remix_usd_ext.h>
#include <set>

namespace remix::scene {
  struct Texture;
}

namespace remix::usd {
  struct Options {
    std::string path;
    double meters = 0;
    uint32_t axis = 0;
    bool preloadTextures = false;
  };
  struct Snapshot {
    Json scene;
    std::set<std::string> dependencies;
    std::map<std::string, std::shared_ptr<scene::Texture>> textures;
  };
  struct LoadError : std::runtime_error {
    std::set<std::string> dependencies;
    LoadError(const std::string &message, std::set<std::string> paths)
        : std::runtime_error(message), dependencies(std::move(paths)) {}
  };
  Snapshot load(const Options &options);
  // Decodes texture files in parallel; null entries could not be decoded.
  std::vector<std::shared_ptr<scene::Texture>> decodeTextures(const std::vector<std::string> &paths);
  uint64_t capturedHash(const std::string &path);
} // namespace remix::usd
