-- A first-person character.
--
-- Attach this to a node, press Play, and you are walking around. Everything
-- here is done through the same API any script has: fumar.key for input,
-- fumar.raycast to find the floor, fumar.set_camera to take over the view.
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

function on_start(node)
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
    x = x + move_x
    z = z + move_z

    -- --- the floor ----------------------------------------------------------
    -- No physics engine here. A ray straight down finds whatever geometry is
    -- underneath, which is enough to stand on a block and walk off it. It is
    -- also all it is: nothing stops you walking THROUGH a wall, because that
    -- needs the character to have a shape and this one is a point.
    local probe_height = 0.6
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

    node:set_position(x, y, z)

    -- --- the view -----------------------------------------------------------
    -- Writing to the camera is what claims it: the editor hands control over
    -- the moment a script asks for it, and takes it back when Play stops.
    fumar.set_camera(x, y + eye_height, z, yaw, pitch)
end
