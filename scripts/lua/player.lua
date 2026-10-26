-- A first-person character.
--
-- Attach this to a node, press Play, and you are walking around. Everything
-- here is done through the same API any script has: fumar.key for input,
-- fumar.raycast to find the floor and the walls, fumar.set_camera to take over the view.
-- There is no special "player" type in the engine, and that is the point.
--
-- Escape releases the mouse.

local eye_height = 1.7
local walk_speed = 6.0
local run_multiplier = 1.9
local look_sensitivity = 0.12
local gravity = -20.0
local jump_speed = 7.0

-- The character's own copy of where it is looking. Read from the camera once at
-- the start so that pressing Play does not snap the view somewhere else.
local yaw = 0.0
local pitch = 0.0

-- Vertical speed, integrated separately from the horizontal movement: falling
-- accelerates, walking does not.
local fall_speed = 0.0

-- How far the node's origin sits above its feet. The player made by Add is a
-- unit cube centred on its origin, so that is half its height; everything
-- below works in feet and converts at the node.
local origin_height = 0.0

-- The character's shape for walls: a column this far out from its middle.
-- Rays go out at several heights, starting above step_height so a low kerb or
-- a stair is left to the floor probe to climb rather than counted as a wall.
local radius = 0.35
local step_height = 0.45
local probe_heights = { 0.5, 1.0, 1.6 }

local function sign(value)
    if value < 0.0 then return -1.0 end
    return 1.0
end

-- The part of `move` along (dir_x, dir_z) the character can take before its
-- side touches a wall. Nodes with Collision unticked in Details are not walls.
local function clear_distance(x, feet_y, z, dir_x, dir_z, move)
    local wanted = math.abs(move)
    local allowed = wanted
    for _, height in ipairs(probe_heights) do
        local distance = fumar.raycast(x, feet_y + height, z, dir_x, 0, dir_z, wanted + radius)
        if distance ~= nil then
            allowed = math.min(allowed, math.max(0.0, distance - radius))
        end
    end
    return allowed * sign(move)
end

function on_start(node)
    local _, height, _ = node:scale()
    origin_height = height * 0.5

    local _, _, _, camera_yaw, camera_pitch = fumar.camera()
    yaw = camera_yaw
    pitch = camera_pitch
    fumar.log("player: WASD to walk, shift to run, space to jump, Escape for the mouse")
end

function on_update(node, dt)
    -- --- looking ------------------------------------------------------------
    local dx, dy = fumar.mouse_delta()
    yaw = yaw + dx * look_sensitivity

    -- Clamped just short of straight up and down. Exactly 90 degrees makes the
    -- forward direction and the up axis parallel, and the view matrix built
    -- from them collapses.
    pitch = math.max(-89.0, math.min(89.0, pitch - dy * look_sensitivity))

    -- --- walking ------------------------------------------------------------
    -- Matching Camera::forward in engine/render/src/camera.cpp: yaw 0 looks
    -- along +X. The vertical component is dropped so that looking up does not
    -- make you walk into the sky.
    local rad = math.rad(yaw)
    local forward_x, forward_z = math.cos(rad), math.sin(rad)
    local right_x, right_z = -forward_z, forward_x

    local move_x, move_z = 0.0, 0.0
    if fumar.key("w") then move_x = move_x + forward_x; move_z = move_z + forward_z end
    if fumar.key("s") then move_x = move_x - forward_x; move_z = move_z - forward_z end
    if fumar.key("d") then move_x = move_x + right_x;   move_z = move_z + right_z   end
    if fumar.key("a") then move_x = move_x - right_x;   move_z = move_z - right_z   end

    -- Normalised, so walking diagonally is not faster than walking straight -
    -- the classic bug where W+D outruns W.
    local length = math.sqrt(move_x * move_x + move_z * move_z)
    if length > 0.0 then
        local speed = walk_speed
        if fumar.key("shift") then speed = speed * run_multiplier end
        move_x = move_x / length * speed * dt
        move_z = move_z / length * speed * dt
    end

    local x, y, z = node:position()
    y = y - origin_height

    -- --- the walls ----------------------------------------------------------
    -- Each axis is tried on its own, so walking into a wall at an angle slides
    -- along it instead of stopping dead: the blocked axis is dropped and the
    -- other one still moves.
    if move_x ~= 0.0 then
        move_x = clear_distance(x, y, z, sign(move_x), 0.0, move_x)
    end
    x = x + move_x
    if move_z ~= 0.0 then
        move_z = clear_distance(x, y, z, 0.0, sign(move_z), move_z)
    end
    z = z + move_z

    -- --- the floor ----------------------------------------------------------
    -- No physics engine here. A ray straight down finds whatever geometry is
    -- underneath, which is enough to stand on a block and walk off it, and to
    -- step up anything lower than the probe.
    local probe_height = step_height + 0.15
    local distance = fumar.raycast(x, y + probe_height, z, 0, -1, 0, 60.0)

    local ground = 0.0
    if distance ~= nil then
        ground = y + probe_height - distance
    end

    fall_speed = fall_speed + gravity * dt
    y = y + fall_speed * dt

    if y <= ground then
        y = ground
        fall_speed = 0.0
        if fumar.key("space") then
            fall_speed = jump_speed
        end
    end

    node:set_position(x, y + origin_height, z)

    -- --- the view -----------------------------------------------------------
    -- Writing to the camera is what claims it: the editor hands control over
    -- the moment a script asks for it, and takes it back when Play stops.
    fumar.set_camera(x, y + eye_height, z, yaw, pitch)
end
