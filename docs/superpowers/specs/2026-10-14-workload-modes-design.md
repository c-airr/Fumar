# Workload modes and the CPU/GPU profiler

Date: 2026-10-14. Status: proposed.

## Why

fumar is a teaching engine. The point of the "Runs on" switch is to SEE what
moving work between the processors does: the same scene, the same picture
(near enough), and a profiler that shows where the milliseconds went.

Today the switch barely changes anything. Only the node matrices (Gpu) or the
vertex transform (Cpu) move; shading, every ray, the acceleration structure and
post stay on the GPU in all three modes. This spec replaces that with a real
split.

## The three modes

| Stage | Gpu | Balanced (unchanged) | Cpu (~80%) |
|---|---|---|---|
| node matrices | compute | CPU | CPU |
| frustum culling | compute, per draw | none (as today) | CPU, per draw |
| TLAS instances | compute writes them | CPU | not built |
| vertex transform | vertex shader | vertex shader | CPU, all cores |
| sun + light shadows | ray query, per pixel | ray query, per pixel | CPU rays, per vertex |
| AO + bounced light | ray query, per pixel | ray query, per pixel | CPU rays, per vertex, over several frames |
| reflections | traced | traced | sky only |
| raster, BRDF, post | GPU | GPU | GPU |

Balanced is the renderer exactly as it is now and stays the default.

### Gpu mode

The CPU only records commands.

- `world.comp` (already there) gains frustum culling: for each draw it writes a
  `VkDrawIndexedIndirectCommand`, with `instanceCount = 0` when the node's
  world-space bounds are outside the frustum. The draw loop calls
  `drawIndexedIndirect` per draw instead of `drawIndexed`, so binding per mesh
  stays as it is. No `drawIndirectCount` needed.
- A second compute pass writes the TLAS instance array
  (`VkAccelerationStructureInstanceKHR`) straight from the world matrices, so
  the CPU no longer fills it. A barrier orders it before the TLAS build.

### Cpu mode

The GPU rasterises and evaluates the BRDF per pixel; everything that answers
"what can this point see" is answered on the CPU.

- **Thread pool.** New `fumar::JobPool` in `fumar_core`: hardware threads minus
  one workers, `parallelFor(count, chunk, fn)`. Used by the transform and the
  lighting. Nothing else in the engine is threaded today, so this is its first
  user.
- **CPU scene.** `CpuScene` in `fumar_render`: one `geometry::TriangleBvh` per
  mesh (built once from the mesh's CPU copy, rebuilt when the mesh is replaced),
  plus a flat instance list (world matrix, its inverse, world bounds). A ray is
  tested against instance bounds, then transformed into the instance and walked
  through its BVH. The BVH is the RayCaster's tree, generalised to take
  positions + indices instead of an `EditableMesh` (RayCaster keeps working on
  top of it).
- **Lighting mesh.** Per-vertex lighting on a 4-vertex ground plane shows no
  shadows at all, so in Cpu mode each mesh is drawn from a subdivided copy: any
  triangle edge longer than the "Vertex density" setting (default 0.5 m, in
  world units after scale) is split. Cached per mesh and scale; the starter
  ground becomes ~40k vertices, the blocks a few hundred each.
- **Per-vertex light, progressive.** A second vertex stream (48 B per vertex,
  location 4-6): sun visibility, sky visibility, visibility of up to 4
  shadow-casting placed lights, bounced light RGB. Every frame the pool spends a
  fixed time budget (default 8 ms of wall time across all cores) tracing:
  first one sun ray per vertex everywhere, then more sun/light samples, then AO
  and bounce rays, accumulating into running averages. Moving a node, the sun
  or a light resets the accumulation of what it can affect (simplest correct
  form: everything), so a drag looks coarse and settles within a few frames
  when it stops. Same sample patterns (Vogel disc, golden-angle rotation) as
  `raytrace.glsl`, so the picture converges to roughly what Balanced shows.
- **Shaders.** `mesh_cpu.vert` passes the stream through; `mesh_cpu.frag`
  includes `mesh_shading.glsl` with `FUMAR_CPU_LIGHTING`, under which the
  visibility functions in `raytrace.glsl` return the interpolated vertex values
  and reflections read only the sky. No ray query, no TLAS in this mode.
- Expected cost on the 5070 box (16 threads): transform + culling ~1-2 ms,
  lighting = its budget. The editor stays interactive; a big scene just
  converges more slowly instead of dropping frames.

## The profiler

- **CPU**: `ScopedCpuTimer` around each stage (hierarchy, culling, transform,
  lighting, TLAS instances, command recording, UI), summed per frame.
- **GPU**: one timestamp query pool per frame in flight, timestamps around
  each pass (compute, TLAS build, scene, temporal, bloom, tonemap, UI). Read
  back after that slot's fence, so the numbers are two frames old and never
  stall. Disabled gracefully when the queue reports no timestamp bits.
- **Statistics panel**: a table Stage | CPU ms | GPU ms (smoothed over ~30
  frames), a total row, and a bar showing the CPU/GPU share of the frame's
  work. Stages that do not run in the current mode show "-".

## Out of scope

- Moving rasterisation or post to the CPU (that is the 3 fps software renderer
  the user does not want).
- A job system for the rest of the engine; the pool is used only here.
- Selective reset of CPU lighting (only what a moved object affects) - noted as
  the next step in a comment.
- GPU culling with a merged vertex buffer and `drawIndirectCount`.

## Verification

- Unit tests: `TriangleBvh` against brute force (random rays, closest and any
  hit), JobPool covers every index exactly once, lighting-mesh subdivision keeps
  edge lengths under the limit and the surface unchanged.
- Smoke variant `workload`: cycles Balanced -> Gpu -> Cpu -> Balanced with the
  starter scene and the helmet, logging the profiler table for each; debug build
  with validation, 0 VUID.
- Screenshots of the three modes side by side; Cpu after convergence should
  show the blocks' shadows and AO on the ground.
- Balanced must be pixel-identical to before (same smoke screenshot).
