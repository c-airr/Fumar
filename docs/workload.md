# Workload modes

*[Polska wersja](workload.pl.md)*

fumar can move the frame's work between the processors. The switch is **Runs on**, under **Workload** in the Details panel (select nothing to see the world's settings). It has three modes:
- **Balanced**, the default: the ordinary renderer.
- **GPU**: the CPU does little more than record commands.
- **CPU**: the CPU answers every "what can this point see" question; the GPU only rasterises and evaluates materials.

The point is to see what moving work does. The scene and the picture stay (nearly) the same, and the Statistics panel shows where the milliseconds went.

## What runs where

| Stage | GPU | Balanced | CPU |
|---|---|---|---|
| node matrices | compute shader | CPU | CPU |
| frustum culling | compute shader, per draw | none | CPU, per draw |
| ray-tracing instances | compute shader writes them | CPU | not built |
| vertex transform | vertex shader | vertex shader | CPU, all cores |
| sun and lamp shadows | ray query, per pixel | ray query, per pixel | CPU rays, per vertex |
| AO and bounced light | ray query, per pixel | ray query, per pixel | CPU rays, per vertex, over several frames |
| reflections | traced | traced | sky only |
| raster, BRDF, post | GPU | GPU | GPU |

**GPU mode.** A compute pass builds the node matrices. A second one culls each draw against the view, writing an indirect draw command with zero or one instance, and writes the object's ray-tracing instance from its world matrix. The CPU records one indirect draw per object and never touches a matrix.

**CPU mode.** Everything is traced on a pool of worker threads (one per hardware thread, minus one), against the CPU's own copy of the scene: a bounding-volume tree per mesh and a flat list of instances. The answers ride to the GPU as a second vertex stream. No ray-tracing structure is built on the GPU, so this mode works without ray-tracing hardware too. Objects outside the view are not drawn, but still cast shadows.

## Reading the profiler

The Statistics panel has a table, **Where the frame goes**, with one row per stage of the frame:

| Row | What it measures |
|---|---|
| Waiting for the GPU | the CPU blocked on a frame slot's fence |
| Node matrices | world matrices, on the CPU or in the compute pass |
| Culling | the view test, on the CPU (CPU mode) or in the compute pass (GPU mode) |
| Vertex transform | the CPU transform and upload (CPU mode) |
| Shadows, AO, bounce | the CPU tracing (CPU mode) |
| Materials, uniforms | filling the per-frame buffers |
| Ray structure | the CPU instance fill and the GPU build of the ray-tracing structure |
| Scene pass, Temporal filter, Bloom, Tone mapping, Interface | the GPU passes |
| Submit, present | handing the frame over |

How to read it:
- **CPU ms** is time the CPU spent on the stage. For a GPU pass this is only the time to *record* its commands, which costs microseconds whatever the pass costs to run.
- **GPU ms** is the time the pass took on the GPU, measured with timestamps the GPU writes itself. They are read two frames late, after the frame's fence, so measuring never makes anything wait.
- **"-"** means the stage did not run on that processor in this mode. A stage that runs on neither is left out of the table.
- **Every figure is smoothed** over roughly the last 25 frames, because one frame's timing jitters too much to read.
- **Work, total** sums the busy time of each processor. Waiting and presenting are the CPU standing still, so they are left out: otherwise a GPU-bound frame would look CPU-heavy.
- **The bar** below shows the same totals as a share, for example `CPU 72% / GPU 28%`. The processors run at the same time, so a frame takes about as long as the busier one, not the sum.

If the GPU's queue cannot write timestamps, the GPU column shows "-", the total reads `n/a`, and the bar is hidden.

## CPU mode settings

When **Runs on** is CPU, three lines appear below it:
- **Vertex spacing** (0.1 to 2 m, default 0.5 m). The CPU lights each *vertex*, and a ground plane of four vertices would show no shadow at all. So each mesh is drawn from a subdivided copy, split until no edge is longer than this, measured in world units after scaling. The split stops at 262,144 triangles per mesh. Smaller spacing: finer shadows, but more vertices to transform and trace.
- **Ray budget** (1 to 30 ms, default 8 ms). Wall time per frame the CPU spends tracing, on every core. More converges faster after a change, and costs frame rate.
- **Samples**: a read-only line, `16 threads, lighting 40/64 samples`. The lighting is progressive: each frame adds samples per vertex, up to 64, and keeps a running mean. The count is the least of any visible vertex; `(done)` appears once all have 64.

Moving an object, the sun or a lamp, or changing a ray setting, starts the samples over. The old values stay on screen until replaced, so a drag looks coarse rather than flashing, and settles within a few frames once it stops. The sample patterns are the GPU's, so the picture converges to roughly what Balanced shows.

## Limits of CPU mode

- **Per-vertex shadows.** Shadows and occlusion are as sharp as the vertex spacing allows. A shadow edge smaller than a triangle is blurred across it.
- **Sky-only reflections.** Per-vertex reflections would be a smear, so none are traced: reflective surfaces show the sky, not other objects.
- **Textured albedo taken as 0.5 × factor.** The CPU has no copy of texture pixels. Where a ray lands on a textured material, its colour is taken as half the colour factor, a stand-in for a typical texture's middle tone. The same goes for textured emission. This only colours bounced light, which is broad and dim enough that the difference rarely shows.
- **Only the first four lamps cast shadows.** Lamps beyond the fourth light the scene without shadows.
