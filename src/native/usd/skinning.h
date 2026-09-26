// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#pragma once

#include "snapshot.h"
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/cache.h>
#include <set>

namespace remix::usd {
  // One cache per scene snapshot: poses are instance-specific, and no evaluated
  // points or skeleton opinions are ever written back to the USD document.
  class SceneSkinning {
  public:
    explicit SceneSkinning(pxr::UsdGeomXformCache &transforms) : transforms_(transforms) {}
    std::string deform(const pxr::UsdGeomMesh &mesh, pxr::VtVec3fArray &points, pxr::VtVec3fArray &normals,
                       pxr::TfToken &normal_interpolation, const pxr::VtIntArray &indices,
                       const pxr::VtIntArray &counts);
    const Json &skeletons() const { return skeletons_; }

  private:
    pxr::UsdGeomXformCache &transforms_;
    pxr::UsdSkelCache cache_;
    std::set<pxr::SdfPath> populated_;
    std::map<pxr::SdfPath, pxr::VtMatrix4dArray> poses_;
    Json skeletons_ = Json::object();
  };
} // namespace remix::usd
