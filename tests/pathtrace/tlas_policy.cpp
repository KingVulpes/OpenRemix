#include "pathtrace/tlas_policy.h"
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) { if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
  void cpu() {
    using namespace remix::trace;
    TlasPolicy policy;
    std::vector<InstanceInput> inputs = {{1, 10, 0, 0, remix::scene::identity(), 2}};
    auto reason = policy.reason(inputs, true, false);
    require(reason == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Initial TLAS build"); policy.commit(inputs, reason);
    require(!policy.changed(inputs) && !policy.reason(inputs, true, false), "Warm scene did work");
    for (unsigned i = 0; i < 300; ++i) {
      inputs[0].model[12] += .001f;
      require(policy.changed(inputs) && !policy.reason(inputs, true, false), "Rigid motion must refit"); policy.commit(inputs, 0);
    }
    inputs[0].model[12] += .001f;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_REFIT_LIMIT_EXT, "Recovery refit limit");
    policy.commit(inputs, REMIXAPI_REBUILD_REFIT_LIMIT_EXT);
    inputs[0].model[12] += 17;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_DISPLACEMENT_EXT, "Displacement recovery");
    inputs[0].geometry = 11;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_GEOMETRY_EXT, "Geometry replacement rebuild");
    inputs[0].id = 2;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Object lifetime rebuild");
    require(policy.reason(inputs, true, true) == REMIXAPI_REBUILD_FORCED_EXT, "Forced rebuild");
    inputs.clear(); require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Actual count, not capacity");
    auto matrix = remix::scene::identity(); matrix[12] = 1024; matrix[13] = -64;
    require(remix::scene::multiply(matrix, remix::scene::inverse(matrix)) == remix::scene::identity(), "Separate camera/object inverse");
  }
}
int main() { cpu(); }
