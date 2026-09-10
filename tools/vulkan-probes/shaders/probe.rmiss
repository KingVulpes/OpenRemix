#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT float distance;
void main() { distance = -1.0; }
