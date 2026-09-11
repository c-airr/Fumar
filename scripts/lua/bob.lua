-- Moves a node up and down, and pulses its scale slightly.
--
-- State per node lives in a table keyed by node id: the script itself is shared
-- by every node using it, so anything remembered has to say which node it
-- belongs to.

local origins = {}
local elapsed = {}

local height = 0.6
local speed = 2.0

function on_start(node)
    local x, y, z = node:position()
    origins[node:id()] = { x = x, y = y, z = z }
    elapsed[node:id()] = 0.0
end

function on_update(node, dt)
    local id = node:id()
    local origin = origins[id]
    if not origin then
        return
    end

    elapsed[id] = elapsed[id] + dt
    local wave = math.sin(elapsed[id] * speed)

    node:set_position(origin.x, origin.y + wave * height, origin.z)

    local pulse = 1.0 + wave * 0.08
    node:set_scale(pulse, pulse, pulse)
end
