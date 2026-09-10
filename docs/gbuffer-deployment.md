# Rasterized primary visibility — 2026-09-10

The path-tracing backend now defaults to a rasterized G-buffer and an unjittered
raster depth pass. Shadows, diffuse indirect lighting, transparent layers behind
the first hit and sparse temporal-confidence retracing retain Vulkan ray queries.
The debug panel's **Primary visibility** selector supports **Rasterized G-buffer**
and the original **Ray traced** mode. The API key is
`rtx.native.pathtrace.primaryVisibility=raster|ray`.

Implementation details and attachment formats are in [path tracing](path-tracing.md).
The G-buffer shares the existing material/texture table and skinned geometry.
Switching primary visibility invalidates temporal history and accumulation.
Tracing GPU timestamps include the raster primary/depth passes.

Clang 22.1.8 / Meson release compilation completed with warnings as errors.
The new shaders were compiled and embedded using the shader-generation build tool.
The release configuration disables tests. No runtime tests, game launches or
performance measurements were run, as requested; performance improvement and
visual correctness on the RTX 2080 have not been measured for this change.

Deployment target: `192.168.50.13`, Prism instance `BetaRT-Raster-Profile`.
The renderer is based on `1abd70b9`. The matching pre-lighting client JAR and JNI
come from `_work/packages/denoise-final-payload`; their hashes match the recorded
[denoising deployment](performance-data/denoise-2026-09-09/final-deployment.json).
They replace the newer client pair so it no longer requires the removed lighting,
SHARC or local-light extensions. The renderer retains REBLUR SH and TAA.

The prepared payload and manifest are in `_work/packages/gbuffer-20260910`.
Build logs are in `_work/build/gbuffer-release`; source changes and deployment
records are in `_work/reports/gbuffer-20260910`.
The remote deployment directory is
`/home/cr/remix-raster-profiles/20260910-142219-gbuffer-deploy`;
`backup/` holds the three previous files and `deployment.json` records their hashes.
