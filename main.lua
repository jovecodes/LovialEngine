include "other.lua"

function Init() 
    PlayerTexture = load_texture("./player.png")
end

local position = v2(1, 2)

function Draw()
    local rect = {
        position = v2(5, 5),
        size = v2(20),
        color = {r = 1},
    }
    draw_rect2(rect)

    local sprite = {
        texture = PlayerTexture,
        position = position,
    }
    draw_sprite(sprite)
end

SPEED = 100

function Update()
    local velocity = Input.get_direction{up = Actions.W, left = Actions.A, down = Actions.S, right = Actions.D}
    velocity = v2_mul(velocity, v2(SPEED * Time.delta()))
    position = v2_add(position, velocity)
end

push_system(EventIDs.Draw, Draw)
push_system(EventIDs.Update, Update)
push_system(EventIDs.Init, Init)
