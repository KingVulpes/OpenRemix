// EON, Portsmouth, Kutz and Hill, JCGT 14(1), 2025, Listings 1–4.
// Adapted from https://github.com/portsmouth/EON-diffuse (MIT; LICENSE-eon.txt).
// Directions point away from the surface in a frame with normal +Z.
const float eonPi = 3.141592653589793f;
const float eonA = 0.5f - 2.0f / (3.0f * eonPi);
const float eonB = 2.0f / 3.0f - 28.0f / (15.0f * eonPi);
float eonAlbedo(float mu, float r) {
  float x = 1.0f - clamp(mu, 0.0f, 1.0f);
  float g = x * (0.0571085289f + x * (0.491881867f + x * (-0.332181442f + x * 0.0714429953f)));
  return (1.0f + r * g) / (1.0f + eonA * r);
}
// Isotropy makes the evaluator depend only on these three dot products. NEE
// already has the cosines, so it need not construct/rotate a tangent frame.
vec3 eonBrdfCosines(vec3 rho, float r, float ni, float no, float io) {
  if (ni <= 0.0f || no <= 0.0f) return vec3(0.0f);
  if (r == 0.0f) return rho / eonPi;
  float s = io - ni * no;
  float st = s > 0.0f ? s / max(ni, no) : s;
  float a = 1.0f / (1.0f + eonA * r);
  float avg = a * (1.0f + eonB * r);
  vec3 ms = rho * rho * avg / (vec3(1.0f) - rho * (1.0f - avg));
  return (rho * a * (1.0f + r * st) + ms *
      (max(0.0f, 1.0f - eonAlbedo(ni, r)) * max(0.0f, 1.0f - eonAlbedo(no, r)) /
       max(1e-7f, 1.0f - avg))) / eonPi;
}
vec3 eonBrdf(vec3 rho, float r, vec3 wi, vec3 wo) {
  return eonBrdfCosines(rho, r, wi.z, wo.z, dot(wi, wo));
}
vec4 eonCoefficients(float mu, float r) {
  return vec4(
    1.0f + r * (0.303392f + (-0.518982f + 0.111709f * mu) * mu + (-0.276266f + 0.335918f * mu) * r),
    r * (-1.16407f + 1.15859f * mu + (0.150815f - 0.150105f * mu) * r) / (mu * mu * mu - 1.43545f),
    1.0f + r * (0.20013f + (-0.506373f + 0.261777f * mu) * mu),
    r * (0.540852f + (-1.01625f + 0.475392f * mu) * mu) / (-1.0743f + (0.0725628f + mu) * mu));
}
mat3 eonBasis(vec3 wo) {
  float len2 = wo.x * wo.x + wo.y * wo.y;
  vec3 x = len2 > 0.0f ? vec3(wo.x, wo.y, 0.0f) * inversesqrt(len2) : vec3(1.0f, 0.0f, 0.0f);
  return mat3(x, vec3(-x.y, x.x, 0.0f), vec3(0.0f, 0.0f, 1.0f));
}
float eonUniformProbability(float mu, float r) {
  return pow(r, 0.1f) * (0.162925f + (-0.372058f + (0.538233f - 0.290822f * mu) * mu) * mu);
}
float eonCltcPdf(vec3 wo, vec3 wi, float r) {
  vec4 m = eonCoefficients(wo.z, r);
  vec3 v = transpose(eonBasis(wo)) * wi;
  float det = m.z * (m.x - m.y * m.w);
  vec3 h = vec3(m.z * (v.x - m.y * v.z), (m.x - m.y * m.w) * v.y, m.z * (m.x * v.z - m.w * v.x));
  float len2 = dot(h, h);
  float s = 0.5f * (1.0f + inversesqrt(1.0f + m.w * m.w));
  return det * det * max(h.z, 0.0f) / (len2 * len2 * eonPi * s);
}
float eonPdf(vec3 wo, vec3 wi, float r) {
  if (wo.z <= 0.0f || wi.z < 0.0f) return 0.0f;
  float p = eonUniformProbability(wo.z, r);
  return p / (2.0f * eonPi) + (1.0f - p) * eonCltcPdf(wo, wi, r);
}
vec4 eonSample(vec3 wo, float r, float u1, float u2) {
  float p = eonUniformProbability(wo.z, r);
  float phi = 2.0f * eonPi * u2;
  vec3 wi;
  // Strict comparison also avoids 0/0 for u1 == r == 0.
  if (u1 < p) {
    float z = u1 / p;
    float radius = sqrt(max(0.0f, 1.0f - z * z));
    wi = vec3(radius * cos(phi), radius * sin(phi), z);
  } else {
    float u = (u1 - p) / (1.0f - p);
    vec4 m = eonCoefficients(wo.z, r);
    float x = sqrt(u) * cos(phi), y = sqrt(u) * sin(phi);
    float s = 0.5f * (1.0f + inversesqrt(1.0f + m.w * m.w));
    x = -mix(sqrt(max(0.0f, 1.0f - y * y)), x, s);
    float z = sqrt(max(0.0f, 1.0f - x * x - y * y));
    wi = normalize(eonBasis(wo) * vec3(m.x * x + m.y * z, m.z * y, m.w * x + z));
    wi.z = max(0.0f, wi.z);
  }
  return vec4(wi, eonPdf(wo, wi, r));
}
