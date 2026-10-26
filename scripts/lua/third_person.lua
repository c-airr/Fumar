-- A third-person character: the camera hangs behind and above it on a boom,
-- the mouse swings the boom round, and the character turns to face the way it
-- walks. Attach it to a node whose origin is at the character's feet; the body
-- is whatever children it has.
--
-- The children should have Collision off (the Third Person template does
-- that). The rays below start inside the body, which already keeps the body
-- from blocking its own walls test, but the camera boom starts at the head and
-- would otherwise stop on the character's own shoulders.
--
-- Walls and floor work exactly as in player.lua. Escape releases the mouse.

local walk_speed = 5.0
local run_multiplier = 1.9
local look_sensitivity = 0.12
local gravity = -20.0
local jump_speed = 7.0
local turn_speed = 720.0 -- degrees per second the body turns towards the walk

-- The boom: how far behind the camera wants to be, how high above the feet it
-- pivots, and how far it stays in front of a wall it would otherwise sink into.
local boom_length = 4.5
local pivot_height = 1.5
local boom_margin = 0.25

-- Same shape as player.lua: a column of this radius, rays above the step
-- height so stairs and kerbs are left to the floor probe.
local radius = 0.35
local step_height = 0.45
local probe_heights = { 0.5, 1.0, 1.6 }

local yaw = 0.0     -- the camera's, which is also what W means
local pitch = 0.0
local facing = 0.0  -- the body's, about Y, in set_rotation's degrees
local fall_speed = 0.0

local function sign(value)
    if value < 0.0 then return -1.0 end
    return 1.0
end

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

-- Turns `from` towards `to` by at most `step` degrees, the short way round.
local function turn_towards(from, to, step)
    local delta = (to - from + 180.0) % 360.0 - 180.0
    if math.abs(delta) <= step then
        return to
    end
    return from + sign(delta) * step
end

function on_start(node)
    local _, _, _, camera_yaw, camera_pitch = fumar.camera()
    yaw = camera_yaw
    pitch = camera_pitch
    local _, node_yaw, _ = node:rotation()
    facing = node_yaw
    fall_speed = 0.0
    fumar.log("third person: WASD to walk, mouse to look, shift to run, space to jump, Escape for the mouse")
end

function on_update(node, dt)
    -- --- looking ------------------------------------------------------------
    local dx, dy = fumar.mouse_delta()
    yaw = yaw + dx * look_sensitivity
    -- Narrower than first person: straight down would put the camera on top of
    -- the head, straight up under the floor.
    pitch = math.max(-70.0, math.min(50.0, pitch - dy * look_sensitivity))

    -- --- walking, relative to the camera ------------------------------------
    local rad = math.rad(yaw)
    local forward_x, forward_z = math.cos(rad), math.sin(rad)
    local right_x, right_z = -forward_z, forward_x

    local move_x, move_z = 0.0, 0.0
    if fumar.key("w") then move_x = move_x + forward_x; move_z = move_z + forward_z end
    if fumar.key("s") then move_x = move_x - forward_x; move_z = move_z - forward_z end
    if fumar.key("d") then move_x = move_x + right_x;   move_z = move_z + right_z   end
    if fumar.key("a") then move_x = move_x - right_x;   move_z = move_z - right_z   end

    local length = math.sqrt(move_x * move_x + move_z * move_z)
    if length > 0.0 then
        -- A node faces -Z, and a turn of `a` degrees about Y points that at
        -- (-sin a, -cos a): solved for the walk direction, this is the turn.
        local target = math.deg(math.atan2(-move_x, -move_z))
        facing = turn_towards(facing, target, turn_speed * dt)

        local speed = walk_speed
        if fumar.key("shift") then speed = speed * run_multiplier end
        move_x = move_x / length * speed * dt
        move_z = move_z / length * speed * dt
    end

    -- The origin is at the feet, so there is no offset to take off.
    local x, y, z = node:position()

    -- --- the walls ----------------------------------------------------------
    if move_x ~= 0.0 then
        move_x = clear_distance(x, y, z, sign(move_x), 0.0, move_x)
    end
    x = x + move_x
    if move_z ~= 0.0 then
        move_z = clear_distance(x, y, z, 0.0, sign(move_z), move_z)
    end
    z = z + move_z

    -- --- the floor ----------------------------------------------------------
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

    node:set_position(x, y, z)
    node:set_rotation(0.0, facing, 0.0)

    -- --- the camera ---------------------------------------------------------
    -- Back along the view direction from a point at head height. A ray along
    -- the boom finds the first wall behind the character, and the camera stops
    -- short of it rather than looking at the back of the bricks.
    local pitch_rad = math.rad(pitch)
    local back_x = -math.cos(rad) * math.cos(pitch_rad)
    local back_y = -math.sin(pitch_rad)
    local back_z = -math.sin(rad) * math.cos(pitch_rad)

    local pivot_x, pivot_y, pivot_z = x, y + pivot_height, z
    local boom = boom_length
    local hit = fumar.raycast(pivot_x, pivot_y, pivot_z, back_x, back_y, back_z, boom_length)
    if hit ~= nil then
        boom = math.max(0.3, hit - boom_margin)
    end

    fumar.set_camera(pivot_x + back_x * boom, pivot_y + back_y * boom, pivot_z + back_z * boom, yaw, pitch)
end
