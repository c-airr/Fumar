-- Spins a node around its own vertical axis.
--
-- Every script gets its own environment, so two scripts can both define
-- on_update without colliding. The globals - print, math, and the fumar table -
-- are still reachable through a metatable fallback.

local degrees_per_second = 45.0

-- Called once per node, the first time it updates after Play or a recompile.
function on_start(node)
    fumar.log("spin started on " .. node:name())
end

-- Called every frame. dt is seconds since the last one, so multiplying by it
-- makes the speed independent of frame rate.
function on_update(node, dt)
    local pitch, yaw, roll = node:rotation()
    node:set_rotation(pitch, yaw + degrees_per_second * dt, roll)
end
