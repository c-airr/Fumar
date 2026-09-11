-- Obraca kostkę wokół osi Y (yaw) oraz lekko wokół osi X (pitch) i Z (roll).

local speed_yaw = 60.0    -- stopnie na sekundę wokół pionu
local speed_pitch = 25.0  -- stopnie na sekundę pochylenia
local speed_roll = 15.0   -- stopnie na sekundę przechylenia

function on_start(node)
    fumar.log("Uruchomiono obrót dla: " .. node:name())
end

function on_update(node, dt)
    local pitch, yaw, roll = node:rotation()

    -- Normalizacja kątów za pomocą modulo (% 360), żeby wartości nie rosły w nieskończoność
    local new_pitch = (pitch + speed_pitch * dt) % 360.0
    local new_yaw   = (yaw + speed_yaw * dt) % 360.0
    local new_roll  = (roll + speed_roll * dt) % 360.0

    node:set_rotation(new_pitch, new_yaw, new_roll)
end
