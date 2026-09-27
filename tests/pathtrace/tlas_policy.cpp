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
    // Instance flags: handedness picks the front face; hardware face culling
    // leaves facing culling on only for one-sided instances.
    InstanceInput facing{1, 10, 0, 0, remix::scene::identity(), 2};
    require(instanceFlags(facing, false) == (instanceFacingCullDisable | instanceFrontCounterClockwise) &&
                instanceFlags(facing, true) == (instanceFacingCullDisable | instanceFrontCounterClockwise),
            "Double-sided instance flags changed");
    facing.oneSided = true;
    require(instanceFlags(facing, false) == (instanceFacingCullDisable | instanceFrontCounterClockwise),
            "One-sided instance culls without hardware face culling");
    require(instanceFlags(facing, true) == instanceFrontCounterClockwise,
            "One-sided instance does not cull with hardware face culling");
    facing.model[0] = -1; // Mirrored: clockwise front faces.
    require(instanceFlags(facing, true) == 0 && instanceFlags(facing, false) == instanceFacingCullDisable,
            "Mirrored instance kept counterclockwise front faces");
    facing.model[0] = 0; // Degenerate: as before, not counterclockwise.
    require(instanceFlags(facing, true) == 0, "Degenerate instance changed facing");
    std::vector<InstanceInput> sided = {{1, 10, 0, 0, remix::scene::identity(), 2}};
    TlasPolicy facingPolicy;
    facingPolicy.commit(sided, facingPolicy.reason(sided, true, false));
    sided[0].oneSided = true;
    require(facingPolicy.reason(sided, true, false) == REMIXAPI_REBUILD_GEOMETRY_EXT &&
                facingPolicy.reason(sided, false, false) == REMIXAPI_REBUILD_GEOMETRY_EXT,
            "A facing change was refitted");
    auto matrix = remix::scene::identity(); matrix[12] = 1024; matrix[13] = -64;
    require(remix::scene::multiply(matrix, remix::scene::inverse(matrix)) == remix::scene::identity(), "Separate camera/object inverse");
  }
}
int main() { cpu(); }
