include "other.lua"

player_texture = 0

function init() 
    player_texture = load_texture("./player.png")
end

x = 0
y = 0

function draw(event)
    rect = {
        x = 5,
        y = 5,
        width = 20,
        height = 20,
        color = {r = 1},
    }
    draw_rect2(rect)

    sprite = {
        texture = player_texture,
        x = x,
        y = y,
        scaleX = 1,
        scaleY = 1,
    }
    draw_sprite(sprite)
end

SPEED = 100

function update(event) 
    if Input.is_pressed(Actions.D) then
        x = x + SPEED * Time.delta()
    end

    if Input.is_pressed(Actions.A) then
        x = x - SPEED * Time.delta()
    end

    if Input.is_pressed(Actions.W) then
        y = y + SPEED * Time.delta()
    end

    if Input.is_pressed(Actions.S) then
        y = y - SPEED * Time.delta()
    end
end

push_system(EventIDs.Draw, draw)
push_system(EventIDs.Update, update)
push_system(EventIDs.Init, init)
