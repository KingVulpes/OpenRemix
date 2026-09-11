// Prism port of ref_tonemap/prism/prismTM.hlsl.
// MIT License
// Copyright (c) 2026 Musa Haji
// Copyright (c) 2026 OopyDoopy / KickFister / Jon
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

vec3 prismToneMap(vec3 color) {
  // Constant-folded D65 / BT.709 and supplied model primaries. GLSL columns.
  const mat3 inset = mat3(
    .72634649, .13423698, .07263955,
    .16665483, .70013888, .06012051,
    .10699868, .16562414, .86723994);
  const mat3 outset = mat3(
    1.45021560, -.25347057, -.10389769,
    -.33533138, 1.51071102, -.07664130,
    -.11488422, -.25724045, 1.18053899);
  color = inset * max(color, vec3(0));
  // Anchored C-infinity shoulder: anchor .18, SDR peak 1, compression 4.
  vec3 distance = max(color - .18, vec3(0));
  vec3 weight = exp2(-.82 / max(4.0 * distance, vec3(1e-20)));
  color = color - distance + .82 * (distance / (.82 + distance * weight));
  return clamp(outset * color, 0.0, 1.0);
}

vec3 displayColor(vec3 color, uint prism, uint gamma22, uint encode, uint hardwareSrgb) {
  if (prism != 0) color = prismToneMap(color);
  if (gamma22 != 0 && (encode != 0 || hardwareSrgb != 0)) {
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    // Cancel the attachment's subsequent sRGB encode, keeping UI blending linear.
    if (hardwareSrgb != 0)
      color = mix(color / 12.92, pow((color + .055) / 1.055, vec3(2.4)),
                  greaterThan(color, vec3(.04045)));
  } else if (encode != 0) {
    color = max(color, vec3(0));
    color = mix(color * 12.92, 1.055 * pow(color, vec3(1.0 / 2.4)) - .055,
                greaterThan(color, vec3(.0031308)));
  }
  return color;
}
