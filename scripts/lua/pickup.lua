-- Something to pick up: it turns in place until the player walks into it, then
-- it is gone - hidden, and no longer solid, so nothing stands on it or bumps
-- into it afterwards. There is no inventory yet; this is the part that
-- switches the object off, which an inventory will need anyway.
--
-- The player is found by name, "Player", which is what Add and the character
-- templates call it. Positions are compared as they are, so the pickup and the
-- player should sit under the same parent (or under groups left at the origin).
--
-- Every Play puts it back: on_start makes it visible and solid again.

local spin_speed = 90.0 -- degrees per second
local reach = 1.2       -- metres, measured flat

local angle = {}
local taken = {}

function on_start(node)
    local id = node:id()
    angle[id] = 0.0
    taken[id] = false
    node:set_visible(true)
    node:set_collision(true)
end

function on_update(node, dt)
    local id = node:id()
    if taken[id] == nil or taken[id] then
        return
    end

    angle[id] = angle[id] + spin_speed * dt
    node:set_rotation(0.0, angle[id], 0.0)

    local player = fumar.find("Player")
    if player == nil then
        return
    end

    local px, py, pz = player:position()
    local x, y, z = node:position()
    local dx, dz = px - x, pz - z
    if dx * dx + dz * dz < reach * reach and math.abs(py - y) < 2.0 then
        taken[id] = true
        node:set_visible(false)
        node:set_collision(false)
        fumar.log("picked up " .. node:name())
    end
end
