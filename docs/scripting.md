# Scripting: Lua and the C++ bindings

*[Polska wersja](scripting.pl.md)*

fumar runs gameplay in two languages through one model:

- **Lua** (LuaJIT 2.1): text files in `scripts/lua/`, recompiled in milliseconds while the editor runs.
- **C++ components**: classes in `scripts/cpp/`, built into the shared library `fumar_game` and swapped underneath the running editor.

A node names a Lua script, a C++ component, or both. The engine calls the same two handlers either way, `on_start` and `on_update`, and both languages reach the same things:

- the node,
- the scene,
- keyboard and mouse,
- the camera,
- a raycast.

This document covers:

1. [Quick start](#1-quick-start)
2. [How a script runs](#2-how-a-script-runs)
3. [Lua API reference](#3-lua-api-reference)
4. [Conventions: units, spaces, angles](#4-conventions-units-spaces-angles)
5. [Errors and pitfalls](#5-errors-and-pitfalls)
6. [How the bindings work (C++ side)](#6-how-the-bindings-work-c-side)
7. [Adding a binding](#7-adding-a-binding)
8. [C++ components](#8-c-components)
9. [Lua and C++ side by side](#9-lua-and-c-side-by-side)
10. [Build and file layout](#10-build-and-file-layout)

---

## 1. Quick start

1. In the **Level** workspace, open the **Scripts** panel and type a name into *new script name*. The new file starts from a template showing both handlers.
   - Alternatively, put a `.lua` file into `scripts/lua/` yourself.
2. Write the script:

   ```lua
   -- scripts/lua/spin.lua
   local degrees_per_second = 45.0

   function on_start(node)
       fumar.log("spin started on " .. node:name())
   end

   function on_update(node, dt)
       local pitch, yaw, roll = node:rotation()
       node:set_rotation(pitch, yaw + degrees_per_second * dt, roll)
   end
   ```

3. Press **Compile** (or **F5**). The toolbar shows the error count, if there are any.
4. Select a node and pick the script in **Details → Script**. The combo only offers scripts that compiled.
5. Press **Play**. Scripts run only while playing, so an object you are positioning by hand is not fought by a script moving it. **Stop** ends it.

The script name is the file name without `.lua`. It is what the scene file stores (`"script": "spin"`), so a scene loads and saves without the scripting runtime.

---

## 2. How a script runs

### Handlers

| Handler | When | Arguments |
|---|---|---|
| `on_start(node)` | Once per node, on its first update after Play, Compile, or loading a scene / New Scene | the node |
| `on_update(node, dt)` | Every frame while playing | the node, seconds since the last frame |

Both are optional. A script that defines neither compiles and does nothing.

### Order within a frame

1. The editor fills in the input, copies the editor camera into the script context, and sets up the raycast.
2. **Lua pass.** Every node with a script is visited in depth-first scene order (the order of the Outliner). A node that has not started yet gets `on_start`, then `on_update`.
3. **C++ pass.** The same procedure for every node with a component, sharing the same context.
4. If anything wrote to the camera, the editor takes the view from the context and captures the mouse.

The list of nodes is collected before the pass begins, so nodes created during a pass are first updated next frame.

### One environment per script

Each `.lua` file is compiled into its own environment table, so two scripts can both define `on_update` without colliding. Globals the script does not define (`math`, `string`, `print`, `fumar`, …) fall through to `_G` via a metatable.

**One script file is shared by every node that uses it.** Top-level `local` variables, and globals the script assigns, exist once per *script*, not once per node. State that belongs to a node has to be keyed by the node:

```lua
-- scripts/lua/bob.lua (abridged)
local origins = {}

function on_start(node)
    local x, y, z = node:position()
    origins[node:id()] = { x = x, y = y, z = z }
end

function on_update(node, dt)
    local o = origins[node:id()]
    if not o then return end
    -- ...
end
```

A truly shared global between scripts has to go through `_G` explicitly (`_G.score = 0`). The usual reasons to avoid globals apply.

### What resets state

**Compile / F5** recompiles every `.lua` file into a **fresh** environment:

- every top-level variable starts over,
- a deleted file stops running,
- a renamed function does not linger,
- `on_start` runs again for every node.

**Play, loading a scene, and New Scene** only make `on_start` run again. The Lua environments, and anything stored in them, survive.

C++ components differ in one place: Play does not restart them. Their `onStart` runs again only after Compile, loading a scene, or New Scene. A component instance, and its members, lives on across Stop and Play.

### Standard library

The whole LuaJIT standard library is open, including:

- `io` and `os`,
- `jit`, `ffi` and `bit`.

That is deliberate during development: being able to read a file or print is worth more than a sandbox. A shipped game should drop `io`, `os` and `ffi`; see the comment in `ScriptEngine::ScriptEngine`.

---

## 3. Lua API reference

### Node handles

A node arrives as the first argument of every handler, or from `fumar.find`, `node:child`, or `node:parent`. It is userdata carrying the node's **id**, not a pointer. Every method looks the node up again and raises an error if the node is gone.

Methods use `:` syntax.

| Method | Returns | Notes |
|---|---|---|
| `node:id()` | integer | Stable while the node lives. Use it as a table key for per-node state. |
| `node:name()` | string | |
| `node:set_name(s)` | | |
| `node:position()` | `x, y, z` | Local, relative to the parent. Metres. |
| `node:set_position(x, y, z)` | | |
| `node:rotation()` | `pitch, yaw, roll` | Degrees around X, Y and Z. See [§4](#4-conventions-units-spaces-angles). |
| `node:set_rotation(pitch, yaw, roll)` | | |
| `node:scale()` | `x, y, z` | |
| `node:set_scale(x, y, z)` | | |
| `node:visible()` | boolean | |
| `node:set_visible(b)` | | Hiding a node hides its whole subtree, and takes it out of raycasts. |
| `node:child_count()` | integer | |
| `node:child(i)` | node or `nil` | **One-based**, like the rest of Lua. `nil` when out of range. |
| `node:parent()` | node or `nil` | Top-level nodes return the scene root (named `root`); only the root itself returns `nil`. |
| `tostring(node)` | string | `node<Block A #7>`, or `node<dead #7>` once deleted. |

There is no method to create, delete or reparent nodes. Mesh, material and light are not reachable from Lua yet.

### The `fumar` table

| Function | Returns | Notes |
|---|---|---|
| `fumar.log(msg)` | | Info line in the log, prefixed `[lua]`. |
| `fumar.warn(msg)` | | Warning line. |
| `fumar.find(name)` | node or `nil` | First node with this exact name, in depth-first order. Linear search, so cache the result in `on_start` rather than calling it every frame. |
| `fumar.key(name)` | boolean | True **while** the key is held: level-triggered. For "just pressed", compare with last frame. Key names are listed below. |
| `fumar.mouse_delta()` | `dx, dy` | Pixels since the last frame. Only meaningful while the mouse is captured, which happens once a script takes the camera. |
| `fumar.camera()` | `x, y, z, yaw, pitch` | The view as it is this frame. Returns nothing outside an update. |
| `fumar.set_camera(x, y, z, [yaw], [pitch])` | | Takes the view over until Stop. Omitted angles keep their current value. |
| `fumar.raycast(ox, oy, oz, dx, dy, dz, [max])` | distance or `nil` | World space. The direction is normalised for you. `max` defaults to 1000 m. `nil` means no hit. See the caveats in [§5](#5-errors-and-pitfalls). |

### Key names

`fumar.key` accepts:

| Group | Names |
|---|---|
| Letters | `"w"`, `"a"`, `"s"`, `"d"`, `"q"`, `"e"` |
| Modifiers | `"space"`, `"shift"`, `"ctrl"` |
| Arrows | `"up"`, `"down"`, `"left"`, `"right"` |
| Mouse buttons | `"mouse_left"`, `"mouse_right"` |

Shift and Ctrl are the left-hand keys. **An unknown name is not an error; it is simply never held**, so a typo shows up as a key that does nothing. To add a key, see [§7](#7-adding-a-binding).

---

## 4. Conventions: units, spaces, angles

- **Units:** metres, seconds, degrees. Y is up.
- **Transforms are local.** `position`, `rotation` and `scale` are relative to the parent, exactly as in the Details panel. For a node directly under the root, local and world are the same.
- **`raycast` is in world space.** For a child node, its position and a raycast hit are not in the same space.
- **Node rotation:**
  - `set_rotation(pitch, yaw, roll)` builds `yaw(Y) * pitch(X) * roll(Z)`.
  - `rotation()` converts the stored quaternion back.
  - Values that come back may differ from what was set (e.g. `-90` instead of `270`) while describing the same orientation. Accumulate your own angle if you need a continuous value.
  - Pitch near ±90° is the usual Euler singularity.
- **Camera angles** follow `Camera::forward`:

  ```lua
  forward = (cos(yaw) * cos(pitch), sin(pitch), sin(yaw) * cos(pitch))
  ```

  - Yaw 0 looks along **+X** and yaw 90 along **+Z**.
  - Positive pitch looks up.
  - Keep pitch inside ±89°: at exactly ±90° the view matrix collapses.
- **`dt`** is the real frame time. Multiply speeds by it.

---

## 5. Errors and pitfalls

### Compile errors

Syntax errors are caught on Compile, before anything runs. A file that fails is:

- skipped,
- logged,
- counted in the toolbar,
- listed at the bottom of the Scripts panel.

Code at the top level of a file also runs at compile time. An error there counts as a compile error.

### Runtime errors

An error inside `on_start` or `on_update` is caught (`lua_pcall`), logged with the script, the handler and the line, and added to the error list. **The script stays attached and is called again next frame**, so an error that repeats every frame logs every frame. Fix it and press Compile.

### Dead nodes

Calling a method on a node that has been deleted raises `node N no longer exists`. This is on purpose: a script holding on to a deleted object has a bug, and an error naming the line is better than silence.

### Node ids are reused

The scene is a slot map. A deleted node's id is given to the next node created. A table keyed by `node:id()` can therefore end up describing a different node after deletions. Undo/redo at the scene level rebuilds nodes with new ids, too.

In practice: build per-node state in `on_start`, and check for `nil` in `on_update`, as `bob.lua` does.

### Calls outside an update

The scene and the context are only set during an update. Top-level code runs at compile time, outside one, so there:

- `fumar.find` returns `nil`,
- `fumar.key` returns `false`,
- `fumar.camera` returns nothing,
- `fumar.raycast` returns `nil`,
- node methods raise `no scene is active`.

Do that work in `on_start` instead.

### Raycast caveats

`fumar.raycast` tests every **visible node that has a mesh**:

- It tests the node's **bounding box**, not its triangles. A ray can hit the empty corner of a box around a sphere.
- **The calling node's own box counts too.** A ray that **starts inside** a box hits it at distance `0`. Start the ray outside the node's own box, or give the scripted node no mesh.
- Hidden nodes, and everything under them, are ignored.

### Camera ownership

The first `fumar.set_camera` call in a Play session hands the view to scripts and captures the mouse. Escape releases the mouse. Stop gives the view back.

If both a Lua script and a C++ component write the camera in the same frame, the C++ one wins, because it runs second.

---

## 6. How the bindings work (C++ side)

### Files

| File | What it holds |
|---|---|
| `engine/script/include/fumar/script/script_engine.hpp` | `ScriptEngine`: compiling, running and restarting scripts. The public face. |
| `engine/script/include/fumar/script/script_context.hpp` | `ScriptContext`, `ScriptInput`, `ScriptCameraState`, `ScriptKey`: everything a script reaches that is not the scene. |
| `engine/script/src/script_engine.cpp` | The Lua state, per-script environments, handler calls. |
| `engine/script/src/lua_bindings.cpp` | Every function Lua can call: the `fumar` table and the `Node` methods. |
| `engine/script/src/script_context.cpp` | Key names → `ScriptKey`. |

### Dependencies

`fumar_script` depends on `fumar_core` and `fumar_scene` only, **not on the renderer**. Scripts can therefore be driven from a test with no window and no GPU.

Anything that needs the renderer reaches the bindings through a callback the application supplies. That is how `ScriptContext::raycast` works.

`lua_State` never appears in a public header: `ScriptEngine` is a pimpl. Code that holds a `ScriptEngine` does not compile against LuaJIT.

### The Lua state

`ScriptEngine` owns one `lua_State`, opened with `luaL_openlibs`, and then calls `script::registerBindings`. That function:

- creates the metatable `"fumar.Node"`, which is its own `__index` and has `__tostring`, and fills it from `kNodeMethods`;
- creates the global table `fumar` from `kFumarFunctions`.

LuaJIT implements the **Lua 5.1 C API**, so the code uses:

- `luaL_register`, not 5.2's `luaL_setfuncs`;
- `lua_setfenv`, not `_ENV`.

### Per-script environments

`compileAll()` does the following for each `.lua` file:

1. `luaL_loadfile`: compile only, which catches syntax errors.
2. Create an environment table whose metatable has `__index = _G`.
3. `lua_setfenv` the chunk to it.
4. `lua_pcall` the chunk, so its `function on_update…` definitions land in the environment.
5. Store the environment in a registry table `"fumar.scripts"` under the script name. The registry is out of reach of scripts.

Recompiling replaces that registry table wholesale. That is what drops deleted files and stale functions.

### Calling a handler

`ScriptEngine::update` runs:

1. `setActiveScene` and `setActiveContext`: store **light userdata** pointers in the registry (`"fumar.activeScene"`, `"fumar.activeContext"`).
2. Collect the nodes that have a script, then for each one call `callHandler`:
   - push the environment;
   - `lua_getfield` the handler, and return if it is missing;
   - push the node and, for `on_update`, `dt`;
   - `lua_pcall`, recording any error.
3. Set both registry pointers back to `nullptr`, so nothing can reach a stale scene between frames.

The pointers live in the registry rather than as upvalues because the bindings are installed once, while the scene and the context change every frame.

### Node handles

`pushNode` creates full userdata holding a `NodeHandle { NodeId id; }` and gives it the `"fumar.Node"` metatable.

It holds an id, not `Node*`, because the scene reallocates its storage when nodes are added. A pointer kept across that would read freed memory.

`checkNode(lua, index)` does three things:

1. validates the userdata type (`luaL_checkudata`),
2. fetches the active scene,
3. raises a Lua error if the id is dead.

Every method starts with it.

### Return conventions

| Kind of value | How it crosses |
|---|---|
| Vectors | Several numbers, not a table: `position()` pushes three numbers and returns `3`. No table is allocated per call, and `local x, y, z = node:position()` reads naturally. |
| Optional results | `nil` (`find`, `child`, `parent`, `raycast`). |
| Programming errors | `luaL_error` (dead node, wrong argument type via `luaL_check*`). |
| Values the engine forgives | Silently ignored (unknown key names). |

---

## 7. Adding a binding

### A function in the `fumar` table

Example: `fumar.time()`, the seconds since Play.

1. If the value comes from outside the scene, add it to `ScriptContext` (`script_context.hpp`):

   ```cpp
   struct ScriptContext {
       ScriptInput input;
       ScriptCameraState camera;
       f64 playSeconds = 0.0;   // new
       std::function<f32(Vec3, Vec3, f32)> raycast;
   };
   ```

2. Write the binding in `lua_bindings.cpp`, inside the anonymous namespace:

   ```cpp
   /// fumar.time() -> seconds since Play was pressed.
   int fumarTime(lua_State* lua) {
       const ScriptContext* context = contextOrNull(lua);
       lua_pushnumber(lua, context != nullptr ? context->playSeconds : 0.0);
       return 1;   // how many values were pushed
   }
   ```

3. Register it in `kFumarFunctions`, before the `{nullptr, nullptr}` terminator:

   ```cpp
   {"time", fumarTime},
   ```

4. Fill it in where the editor fills in the context (`editor/src/main.cpp`, the `if (state.scriptsRunning)` block).
5. C++ components see it at once as `context.script.playSeconds`, because they share `ScriptContext`. Document it in both languages; see [§9](#9-lua-and-c-side-by-side).

### A method on nodes

Example: `node:world_position()`.

```cpp
int nodeWorldPosition(lua_State* lua) {
    checkNode(lua, 1);   // raises if dead
    auto* handle = static_cast<NodeHandle*>(lua_touserdata(lua, 1));
    const Vec3 p = xyz(sceneOrNull(lua)->worldTransform(handle->id) * point(Vec3{}));
    lua_pushnumber(lua, p.x);
    lua_pushnumber(lua, p.y);
    lua_pushnumber(lua, p.z);
    return 3;
}
```

Then add `{"world_position", nodeWorldPosition},` to `kNodeMethods`.

`worldTransform` is from the last `updateWorldTransforms()`, which runs before rendering. A node moved earlier in the same frame reports where it was at the start of the frame.

### A key

Three edits, each in its own file:

1. A new value in `ScriptKey`, before `Count` (`script_context.hpp`).
2. Its name in `kNames` (`script_context.cpp`). Grow the entries with the enum: the array is sized by `ScriptKey::Count`, and a missing entry is not a compile error. It leaves an empty name, so the key is unreachable.
3. A line in `fillScriptInput` (`editor/src/main.cpp`).

### Rules for bindings

- **Use the Lua 5.1 API.** For example, `lua_objlen` rather than `lua_rawlen`.
- **Do not hold pointers into the scene across calls.** Look nodes up by id every time; `checkNode` does it.
- **`luaL_error` does a `longjmp`.** Raise it before creating any C++ object with a destructor in the function, or the destructor will not run. The existing bindings check arguments first and build values afterwards for this reason.
- **Never include renderer headers in `fumar_script`.** Pass what is needed through `ScriptContext`, as a value or a `std::function`.
- **Keep names `snake_case` in Lua** (`set_position`, `mouse_delta`), whatever the C++ side calls them.
- **Count the return value.** It must equal the number of values pushed.

---

## 8. C++ components

### The interface

`engine/native/include/fumar/native/component.hpp`, all reachable through `#include <fumar.hpp>`:

```cpp
class Component {
public:
    virtual ~Component() = default;
    virtual void onStart(Node& node, const ComponentContext& context) {}
    virtual void onUpdate(Node& node, const ComponentContext& context) = 0;
};

struct ComponentContext {
    Scene& scene;            // the whole scene
    ScriptContext& script;   // input, camera, raycast - the same object Lua gets
    f32 deltaSeconds;
};
```

### Writing one

Declare it in `scripts/cpp/public/components.h`:

```cpp
namespace game {
class Spinner final : public fumar::Component {
public:
    void onUpdate(fumar::Node& node, const fumar::ComponentContext& context) override;
private:
    fumar::f32 m_degreesPerSecond = 45.0f;
};
}
```

Implement it in `scripts/cpp/private/*.cpp` and register it in the one exported function:

```cpp
void game::Spinner::onUpdate(fumar::Node& node, const fumar::ComponentContext& context) {
    const fumar::f32 turn = fumar::radians(m_degreesPerSecond * context.deltaSeconds);
    node.transform.rotation = fumar::normalize(
        fumar::fromAxisAngle(fumar::Vec3{0.0f, 1.0f, 0.0f}, turn) * node.transform.rotation);
}

extern "C" FUMAR_GAME_EXPORT void fumarRegisterComponents(fumar::ComponentRegistry& registry) {
    FUMAR_REGISTER_COMPONENT(registry, game::Spinner);
}
```

A few details:

- The name in the editor is the stringised type as written in the macro (`#Type`). Register as `using namespace game; FUMAR_REGISTER_COMPONENT(registry, Spinner);` to get `Spinner` rather than `game::Spinner`; `components.cpp` does this.
- New `.cpp` files in `scripts/cpp/private/` are picked up automatically (`CONFIGURE_DEPENDS` glob).
- Attach a component in **Details → C++**. The scene stores `"component": "Spinner"`.

### Hot reload

**Compile / F5** recompiles Lua *and* rebuilds `fumar_game`:

1. It drops every component instance, then unloads the library.
2. It runs `cmake --build … --target fumar_game`. The editor is blocked while the compiler runs; the output goes to the terminal fumar was started from.
3. It copies `fumar_game.dll` to `fumar_game.live.dll` and loads the copy. Windows cannot overwrite a loaded DLL, so the original stays free for the next build. Linux does the same so there is one code path.
4. It calls `fumarRegisterComponents`.

The editor can only rebuild in a development tree. A distributed build has no compiler and says so.

### Rules that keep reloading safe

- **Member state does not survive a reload.** The objects are destroyed before the code that destroys them is unloaded. Anything that must persist goes into the scene: transform, name, children. `Hover` re-reads its base height from the node in `onStart` for this reason.
- **Do not pass engine containers across the boundary.** fumar links the CRT statically, so the executable and the library have **separate heaps**. Names cross as `const char*`, factories are plain function pointers, and the only object that crosses is a `Component`, whose virtual destructor frees it in the library that allocated it.
- **`fumarRegisterComponents` must be `extern "C"`**, because it is looked up by its unmangled name.
- **One instance per node.** Changing the component name in Details builds a new instance. Instances of deleted nodes are swept at the end of each update.
- **Mind dead nodes.** `onStart` and `onUpdate` receive `Node&` directly. If you destroy nodes or keep `NodeId`s, check `context.scene.isAlive(id)`; the engine does this between `onStart` and `onUpdate`.

---

## 9. Lua and C++ side by side

| Lua | C++ (`context` is the `ComponentContext`) |
|---|---|
| `on_start(node)` / `on_update(node, dt)` | `onStart(Node&, ctx)` / `onUpdate(Node&, ctx)` |
| `dt` | `context.deltaSeconds` |
| `node:position()` / `set_position` | `node.transform.position` (`Vec3`) |
| `node:rotation()` (degrees) | `node.transform.rotation` (quaternion; `fromAxisAngle`, `radians`) |
| `node:scale()` | `node.transform.scale` |
| `node:visible()` | `node.visible` |
| `node:name()` | `node.name` |
| `node:child(i)` (1-based) / `node:parent()` | `node.children[i]` (0-based) / `node.parent` (`kInvalidNode` = none) |
| `node:id()` | the `NodeId` from `scene.traverse`, or the one you stored |
| `fumar.find(name)` | `context.scene.traverse(...)` and compare `node(id).name`; see `Follow` |
| `fumar.key("w")` | `context.script.input.down(fumar::ScriptKey::W)` |
| `fumar.mouse_delta()` | `context.script.input.mouseDelta` |
| `fumar.camera()` | `context.script.camera.position / .yaw / .pitch` |
| `fumar.set_camera(...)` | write `context.script.camera.*` **and** set `.controlled = true` |
| `fumar.raycast(...)` | `if (context.script.raycast) d = context.script.raycast(origin, normalize(dir), max);` (negative = miss; normalise the direction yourself) |
| `fumar.log` / `fumar.warn` | `FUMAR_INFO(...)` / `FUMAR_WARN(...)` (std::format syntax) |

Gaps between the two are deliberate, but they should stay small. A new capability goes into `ScriptContext` so that both languages get it.

---

## 10. Build and file layout

```
scripts/
  lua/                 .lua files; read at runtime
  cpp/public/          headers other gameplay files may include
  cpp/private/         .cpp files compiled into fumar_game
engine/script/         fumar_script (LuaJIT bindings) + fumar_script_api
engine/native/         fumar_native (game-library loader) + <fumar.hpp>
cmake/FumarLuaJIT.cmake
```

### LuaJIT

LuaJIT is fetched and built by `cmake/FumarLuaJIT.cmake` through `ExternalProject`:

- It is pinned to a commit of the 2.1 branch.
- It is static, using `msvcbuild.bat static` on Windows and `make BUILDMODE=static` elsewhere.
- On Windows it is built with the same static CRT as the engine (`/MT` or `/MTd`, injected via the `CL` variable).

It is imported as `fumar_luajit` and linked `PRIVATE` into `fumar_script`.

### `fumar_script_api`

`fumar_script_api` is an `INTERFACE` target: the scripting headers with no implementation behind them.

`fumar_game` and `fumar_native` use it to get `ScriptContext` without linking LuaJIT. That matters because an `ExternalProject` re-runs its step on every build of anything depending on it, and the Compile button would otherwise wait on it.

### Where scripts are read from

| Build | Directory |
|---|---|
| Development | The source tree's `scripts/lua/` (`FUMAR_LUA_SOURCE_DIR`), so edits from the editor land in the repository. |
| Release | `<exe dir>/scripts/lua/`. The build copies scripts there and `install()` ships them. |

A missing directory is not an error: the project simply has no scripts yet.

### Hot reload of `fumar_game` needs a dev tree

The executable has the build directory baked in (`FUMAR_GAME_BUILD_DIR`). On Windows the build goes through `tools/dev.ps1`, which sets up the clang-cl and Ninja environment the build directory was configured with.
