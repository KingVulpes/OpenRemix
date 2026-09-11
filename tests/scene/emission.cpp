#include "scene/fixtures.h"
#include "scene/emission.h"
#include "scene/light_bvh.h"
#include <cmath>
#include <limits>
using namespace remix::scene;
using namespace remix::test;
int main() {
  Draw draw;
  draw.material = std::make_shared<Material>();
  draw.surface = std::make_shared<Surface>();
  draw.surface->vertices = {{{0,0,0},{0,0},{}},{{1,0,0},{.49f,0},{}},{{0,1,0},{0,1},{}}};
  draw.surface->indices = {0,1,2};
  draw.instanceId = 5;
  auto &m = *draw.material;
  std::fill_n(m.emission,3,2.f);
  m.emissionTexture = std::make_shared<Texture>();
  m.emissionTexture->pixels = {2,1,true,{{128,0,0,255,0,255,0,255}}};
  const float uv[] = {1,1,0,0};
  auto color = triangleEmission(draw,0,uv);
  require(std::abs(color[0] - .43172f)<.0001f && color[1]==0, "UV region/linear emission average");
  require(triangleEmission(draw,0,uv)==color && m.emissionCache.size()==1, "Emission reduction cache");
  auto light = triangleEmitter(draw,0,color);
  require(light.triangle==1 && std::abs(light.position[0]-1.f/3)<1e-6f, "Area emitter centroid");
  LightBvh bvh;
  bvh.update({light});
  const float expected=.5f*color[0]*.2126f/(4*3.14159265359f);
  require(std::abs(bvh.nodes()[0].minimumPower[3]-expected)<1e-6f, "Area emitter flux");
  draw.model[0]=2; draw.model[5]=3; draw.model[12]=10;
  auto moved = triangleEmitter(draw,0,color);
  require(moved.id==light.id && moved.corners[1][0]==12, "Transform changed emitter identity");
  bvh.update({moved});
  require(bvh.refits==1 && std::abs(bvh.nodes()[0].minimumPower[3]-6*expected)<1e-6f, "Scaled area/refit");
  m.emissionTexture.reset();
  require(triangleEmission(draw,0,uv)[0]==2, "Constant emission fallback");
  remix::NativeResources resources;
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant={1,1,1}; opaque.opacityConstant=1; opaque.alphaTestType=7;
  remixapi_MaterialInfo info{};
  info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; info.pNext=&opaque; info.hash=1;
  info.emissiveIntensity=2; info.emissiveColorConstant={1,0,0};
  remixapi_MaterialHandle handle{};
  resources.createMaterial(info,handle);
  auto mesh=quad(resources,2,handle);
  info.emissiveIntensity=std::numeric_limits<float>::quiet_NaN();
  rejected([&]{resources.createMaterial(info,handle);});
  resources.drawInstance(instance(mesh));
  require(resources.draws()[0].material->emission[0]==2, "Rejected material replaced accepted emission");
  std::puts("Emission color, atlas region, transforms, flux, caching and validation passed");
}
