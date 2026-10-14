# Workload modes - plan

Spec: `docs/superpowers/specs/2026-10-14-workload-modes-design.md`. Commit and
push after every step; debug build under validation, 0 VUID.

1. **Foundations.** `fumar::JobPool` in core (+ Threads on Linux); `RayCaster`
   from a plain triangle list. Tests in `geometry_tests`: brute-force ray
   comparison, every pool index exactly once, back-to-back loops.
2. **Profiler.** `FrameProfiler` in render: CPU scopes per stage, GPU timestamp
   query pool per frame in flight, read after the slot's fence. Statistics
   panel table + CPU/GPU share bar.
3. **Cpu mode.**
   - `CpuScene`: RayCaster per mesh (cached by mesh handle + generation),
     instance list with world/inverse/bounds, `occluded` / `closest`.
   - Lighting mesh: subdivide triangles until world edges <= density; cached.
   - Per-vertex light stream (3 x vec4) accumulated with a time budget on the
     pool; reset on any change of scene, sun, lights or ray settings.
   - `mesh_cpu.vert` / `mesh_cpu.frag` (`FUMAR_CPU_LIGHTING`), pipeline with
     two vertex bindings, no TLAS in this mode.
   - Details: "Vertex density" and "CPU light budget" under Runs on.
4. **Gpu mode.** `world.comp` writes indirect draw commands (frustum culled)
   and TLAS instances; the draw loop uses `drawIndexedIndirect`; CPU skips the
   instance fill.
5. **Smoke + docs.** `FUMAR_MODELER_SMOKE=workload` cycles the modes and logs
   the profiler; `docs/workload.md` + `.pl.md`; screenshots of the three modes.
