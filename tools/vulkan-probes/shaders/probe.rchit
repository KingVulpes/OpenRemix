#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT float distance;
hitAttributeEXT vec2 barycentrics;
void main() { distance = gl_HitTEXT; }
