local position = v2(1, 2)
local prompt = "hello world"
local SPEED = 100

function Init() 
    PlayerTexture = load_texture "./player.png"
    PlayerID = alloc_id()
    Physics.create{id = PlayerID, position = v2(100), size = v2(100), type = Physics.Actor}

    WallID = alloc_id()
    Physics.create{id = WallID, position = v2(100, 250), size = v2(100), type = Physics.Solid}

    local vec = randv2_between(v2(0, 0), v2(640, 360))
    print(vec.x, vec.y)
end
push_system(EventIDs.Init, Init)

function Draw()
    draw_rect2{
        position = Input.mouse_position(),
        size = v2(20),
        color = {r = 1},
    }

    draw_text{
        position = v2(100),
        text = prompt,
    }

    draw_line{
        start = v2(0), 
        finish = Input.mouse_position(),
        color = {r = 1},
    }

    draw_sprite{
        texture = PlayerTexture,
        position = position,
    }

    Physics.debug()
end
push_system(EventIDs.Draw, Draw)

function Update()
    local velocity = Input.get_direction{up = Actions.W, left = Actions.A, down = Actions.S, right = Actions.D}
    velocity = v2_mul(velocity, v2(SPEED * Time.delta()))
    position = v2_add(position, velocity)

    Physics.move(PlayerID, velocity)

    local s = Input.string_typed()
    prompt = prompt .. s
    if Input.is_typed(Actions.Backspace) then
        prompt = prompt:sub(1, -2)
    end
end
push_system(EventIDs.Update, Update)
