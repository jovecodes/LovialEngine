#include "Input/Input.h"
#include "Rendering/2D/Text.h"
#include "Rendering/Shader.h"
#include "Rng.h"
#include "lua.hpp"
#include "Jovial.h"
#include "Window.h"
#include "Util/Systems.h"
#include "OS/FileAccess.h"
#include "Util/JovialFont.h"
#include "Batteries/PhysicsPP.h"

using namespace jovial;

#define ERROR_LOG_PATH "./error_log.txt"

PixelFont default_font;
pp::Physics physics;

Arena static_arena;
Arena frame_arena;

static bool has_errored = false;

#define LOG_ERROR(...)                                          \
    do {                                                        \
        if (!has_errored) {                                     \
            fs::write_file(ERROR_LOG_PATH, "");                 \
            has_errored = true;                                 \
        }                                                       \
        fs::append_file(ERROR_LOG_PATH, tprint(__VA_ARGS__));   \
        fs::append_file(ERROR_LOG_PATH, "\n");                  \
        JV_LOG_ENGINE(LOG_ERROR, __VA_ARGS__);                  \
    } while (0)

#define RETURN_ERROR(L, ...)               \
    do {                                   \
        LOG_ERROR(__VA_ARGS__);            \
        return luaL_error(L, __VA_ARGS__); \
    } while(0)

struct LuaSystem {
    int func_ref = 0;
    lua_State *L;
};

void on_event(void *user_data, Event &event) {
    LuaSystem *system = (LuaSystem *) user_data;

    lua_rawgeti(system->L, LUA_REGISTRYINDEX, system->func_ref);  // Get the Lua function from the registry
    
    lua_newtable(system->L);
    switch (event.type) {
        case Events::DRAW_ID: {
            auto &draw = (Events::Draw &) event;
            lua_pushinteger(system->L, draw.viewport.id);
            lua_setfield(system->L, -2, "viewport");
        }
        case Events::UPDATE_ID: {
            auto &update = (Events::Update &) event;
            lua_pushinteger(system->L, update.viewport.id);
            lua_setfield(system->L, -2, "viewport");
        }
        default: break;
    }

    if (lua_pcall(system->L, 1, 0, 0) != LUA_OK) {
        LOG_ERROR("ERROR: could not call Lua callback: %\n", lua_tostring(system->L, -1));
    }
}

int lua_push_system(lua_State *L) {
    if (!lua_isfunction(L, 2)) {
        RETURN_ERROR(L, "Expected an int and a function as the arguments");
    }

    // Get the first argument, which should be an int
    int type = luaL_checkinteger(L, 1);

    // Store the Lua function in the registry and get a reference to it
    lua_pushvalue(L, 2);  // Push the function to the top of the stack
    int lua_func_ref = luaL_ref(L, LUA_REGISTRYINDEX);  // Get the reference to the Lua function

    LuaSystem *system = New(static_arena, LuaSystem{lua_func_ref, L});

    WM::get_main_window()->get_viewport()->push_system(type, on_event, system);

    return 0;  // No return value to Lua
}

#define load_v2(L, vector, name, arg)                                                \
    do {                                                                             \
    lua_getfield(L, arg, name);                                                      \
    if (lua_istable(L, -1)) {                                                        \
        lua_getfield(L, -1, "x");                                                    \
        vector.x = luaL_checknumber(L, -1);                                          \
        lua_pop(L, 1);                                                               \
                                                                                     \
        lua_getfield(L, -1, "y");                                                    \
        vector.y = luaL_checknumber(L, -1);                                          \
        lua_pop(L, 1);                                                               \
    } else {                                                                         \
        RETURN_ERROR(L, "'" #vector "' must be a table {x: number, y: number}");\
    }                                                                                \
    lua_pop(L, 1);                                                                   \
    } while(0)

#define load_rect2(L, rect, arg) \
    load_v2(L, rect.position, "position", arg); \
    load_v2(L, rect.size, "size", arg);

#define color_from_object(result, L)                                            \
    if (lua_isnil(L, -1)) {                                                     \
        /* pass */                                                              \
    } else if (lua_istable(L, -1)) {                                            \
        lua_getfield(L, -1, "r");                                               \
        result.r = luaL_optnumber(L, -1, 0.0);                                  \
        lua_pop(L, 1);                                                          \
                                                                                \
        lua_getfield(L, -1, "g");                                               \
        result.g = luaL_optnumber(L, -1, 0.0);                                  \
        lua_pop(L, 1);                                                          \
                                                                                \
        lua_getfield(L, -1, "b");                                               \
        result.b = luaL_optnumber(L, -1, 0.0);                                  \
        lua_pop(L, 1);                                                          \
                                                                                \
        lua_getfield(L, -1, "a");                                               \
        result.a = luaL_optnumber(L, -1, 1.0);                                  \
        lua_pop(L, 1);                                                          \
                                                                                \
        lua_pop(L, 1);                                                          \
    } else {                                                                    \
        RETURN_ERROR(L, "'color' must be an object: {r: 0, g: 0, b: 0, a: 0}"); \
    } 

int lua_load_texture(lua_State *L) {
    if (!lua_isstring(L, 1)) {
        RETURN_ERROR(L, "Expected a string (path to the texture) as the first argument");
    }

    u64 size = 0;
    const char *pointer = luaL_checklstring(L, 1, &size);

    StrView path = {pointer, size};
    TextureID id = TextureID::from_file(path);

    lua_pushinteger(L, id.id);

    return 1;
}

int lua_load_shader(lua_State *L) {
    StrView vertex, fragment;

    if (lua_isnil(L, 1)) {
        vertex = Renderer2D::vertex_shader_code();
    } else if (lua_isstring(L, 1)) {
        u64 size = 0;
        const char *pointer = luaL_checklstring(L, 1, &size);
        vertex = {pointer, size};
    } else {
        RETURN_ERROR(L, "Expected a string (path to the vertex shader) as the first argument");
    }

    if (lua_isnil(L, 1)) {
        fragment = Renderer2D::fragment_shader_code();
    } else if (lua_isstring(L, 1)) {
        u64 size = 0;
        const char *pointer = luaL_checklstring(L, 1, &size);
        fragment = {pointer, size};
    } else {
        RETURN_ERROR(L, "Expected a string (path to the fragment shader) as the second argument");
    }

    Shader shader = Shader::from_path(vertex, fragment);

    auto *r2d = Renderer2D::from(WM::get_main_window()->get_renderers()[0]);
    r2d->add_shader(shader);

    lua_pushinteger(L, shader.id);

    return 1;
}

int lua_draw_line(lua_State *L) {
    if (!lua_istable(L, 1)) {
        RETURN_ERROR(L, "Expected a table {start = v2(), end = v2(), color = {}, thickness = 1.0, z_index = 0} as the first argument");
    }

    Line2DCmd cmd;

    load_v2(L, cmd.start, "start", 1);
    load_v2(L, cmd.end, "finish", 1);

    lua_getfield(L, 1, "color");
    color_from_object(cmd.color, L);

    lua_getfield(L, 1, "thickness");  
    cmd.thickness = luaL_optnumber(L, -1, 1.0);  
    lua_pop(L, 1);  

    lua_getfield(L, 1, "z_index");  
    int z_index = luaL_optnumber(L, -1, 0.0);  
    lua_pop(L, 1);  

    cmd.draw(WM::get_main_window()->get_renderers()[0], z_index);
    return 0;
}

int lua_draw_text(lua_State *L) {
    if (!lua_istable(L, 1)) {
        RETURN_ERROR(L, "Expected a table {text = 'Hello, World', position = v2(), color = {}, z_index = 0} as the first argument");
    }

    Text2DCmd cmd;
    cmd.bitmap_font = &default_font;

    load_v2(L, cmd.position, "position", 1);

    lua_getfield(L, 1, "text");
    size_t len = 0;
    const char *text = luaL_checklstring(L, -1, &len);
    cmd.text = String(frame_arena, {text, len}).to_upper();

    lua_getfield(L, 1, "color");
    color_from_object(cmd.color, L);

    lua_getfield(L, 1, "z_index");  
    int z_index = luaL_optnumber(L, -1, 0.0);  
    lua_pop(L, 1);  

    cmd.draw(WM::get_main_window()->get_renderers()[0], z_index);
    return 0;

}

int lua_draw_rect2(lua_State *L) {
    if (!lua_istable(L, 1)) {
        RETURN_ERROR(L, "Expected a table {position = v2(), size = v2(), color = {}, z_index = 0} as the first argument");
    }

    Rect2DCmd cmd;

    Rect2 rect;
    load_rect2(L, rect, 1);
    cmd.set(rect);

    lua_getfield(L, 1, "color");
    color_from_object(cmd.color, L);

    lua_getfield(L, 1, "z_index");  
    int z_index = luaL_optnumber(L, -1, 0.0);  
    lua_pop(L, 1);  

    cmd.draw(WM::get_main_window()->get_renderers()[0], z_index);
    return 0;
}

int lua_draw_sprite(lua_State *L) {
    if (!lua_istable(L, 1)) {
        RETURN_ERROR(L, "Expected a table {position = {x = 0, y = 0}, texture = 0, color = {}, scale = {x = 1, y = 1}, rotation = 0, z_index = 0} as the first argument");
    }

    Sprite2DCmd cmd;

    load_v2(L, cmd.position, "position", 1);

    lua_getfield(L, 1, "texture");
    cmd.texture.id = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "color");
    color_from_object(cmd.color, L);

    lua_getfield(L, 1, "scale");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "x");
        cmd.scale.x = luaL_optnumber(L, -1, 1.0);  
        lua_pop(L, 1);  

        lua_getfield(L, -1, "y");
        cmd.scale.y = luaL_optnumber(L, -1, 1.0);  
        lua_pop(L, 1);
    } else {
        cmd.scale = {1.0f, 1.0f};  
    }
    lua_pop(L, 1);  

    lua_getfield(L, 1, "rotation");  
    cmd.rotation = luaL_optnumber(L, -1, 0.0);  
    lua_pop(L, 1);

    lua_getfield(L, 1, "z_index");  
    int z_index = luaL_optnumber(L, -1, 0);  
    lua_pop(L, 1);  

    lua_getfield(L, 1, "shader");
    int shader_id = luaL_optinteger(L, -1, -1);
    lua_pop(L, 1);

    auto &renderer = WM::get_main_window()->get_renderers()[0];
    if (shader_id != -1) {
        JV_LOG_ENGINE(LOG_WARNING, "Shaders are not yet implemented for lua. Sorry, too lazy.");
        // Sequence2DCmd seq;
        //
        // Shader2DCmd shader;
        // shader.id = shader_id;
        //
        // seq.push(shader.draw(renderer, 2));
        // seq.push(cmd.draw(renderer, 1));
        //
        // shader = {};
        // seq.push(shader.draw(renderer, 0));
        //
        // seq.draw(renderer, z_index);
    }

    cmd.draw(renderer, 0);
    return 0;
}

int lua_include(lua_State *L) {
    const char *pointer = luaL_checklstring(L, 1, nullptr);
    luaL_dofile(L, pointer);
    return 0;
}

void bind_function(lua_State *L, const char* lua_function_name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setglobal(L, lua_function_name);
}

void bind_event_ids_to_lua(lua_State *L) {
    lua_newtable(L);

    lua_pushinteger(L, Events::ANY_ID); lua_setfield(L, -2, "Any");
    lua_pushinteger(L, Events::INIT_ID); lua_setfield(L, -2, "Init");
    lua_pushinteger(L, Events::UPDATE_ID); lua_setfield(L, -2, "Update");
    lua_pushinteger(L, Events::QUIT_ID); lua_setfield(L, -2, "Quit");
    lua_pushinteger(L, Events::PRE_UPDATE_ID); lua_setfield(L, -2, "PreUpdate");
    lua_pushinteger(L, Events::POST_UPDATE_ID); lua_setfield(L, -2, "PostUpdate");
    lua_pushinteger(L, Events::DRAW_ID); lua_setfield(L, -2, "Draw");
    lua_pushinteger(L, Events::WINDOW_OPEN_ID); lua_setfield(L, -2, "WindowOpen");
    lua_pushinteger(L, Events::WINDOW_CLOSE_ID); lua_setfield(L, -2, "WindowClose");
    lua_pushinteger(L, Events::WINDOW_RESIZE_ID); lua_setfield(L, -2, "WindowResize");
    lua_pushinteger(L, Events::MOUSE_MOVED_ID); lua_setfield(L, -2, "MouseMoved");
    lua_pushinteger(L, Events::MOUSE_SCROLLED_ID); lua_setfield(L, -2, "MouseScrolled");
    lua_pushinteger(L, Events::MOUSE_BUTTON_PRESSED_ID); lua_setfield(L, -2, "MouseButtonPressed");
    lua_pushinteger(L, Events::MOUSE_BUTTON_RELEASED_ID); lua_setfield(L, -2, "MouseButtonReleased");
    lua_pushinteger(L, Events::MOUSE_LEAVE_WINDOW_ID); lua_setfield(L, -2, "MouseLeaveWindow");
    lua_pushinteger(L, Events::MOUSE_ENTER_WINDOW_ID); lua_setfield(L, -2, "MouseEnterWindow");
    lua_pushinteger(L, Events::KEY_PRESSED_ID); lua_setfield(L, -2, "KeyPressed");
    lua_pushinteger(L, Events::KEY_RELEASED_ID); lua_setfield(L, -2, "KeyReleased");
    lua_pushinteger(L, Events::KEY_TYPED_ID); lua_setfield(L, -2, "KeyTyped");
    lua_pushinteger(L, Events::VIEWPORT_DRAW_ID); lua_setfield(L, -2, "ViewportDraw");
    lua_pushinteger(L, Events::RENDERER_INIT_ID); lua_setfield(L, -2, "RendererInit");
    lua_pushinteger(L, Events::FIRST_CUSTOM_ID); lua_setfield(L, -2, "FirstCustom");

    lua_setglobal(L, "EventIDs");
}

void bind_input_actions_to_lua(lua_State *L) {
    lua_newtable(L);

    lua_pushinteger(L, (int) Actions::LeftMouseButton); lua_setfield(L, -2, "LeftMouseButton");
    lua_pushinteger(L, (int) Actions::RightMouseButton); lua_setfield(L, -2, "RightMouseButton");
    lua_pushinteger(L, (int) Actions::MiddleMouseButton); lua_setfield(L, -2, "MiddleMouseButton");
    lua_pushinteger(L, (int) Actions::MouseButtonX1); lua_setfield(L, -2, "MouseButtonX1");
    lua_pushinteger(L, (int) Actions::MouseButtonX2); lua_setfield(L, -2, "MouseButtonX2");
    lua_pushinteger(L, (int) Actions::MouseButtonX3); lua_setfield(L, -2, "MouseButtonX3");
    lua_pushinteger(L, (int) Actions::MouseButtonX4); lua_setfield(L, -2, "MouseButtonX4");
    lua_pushinteger(L, (int) Actions::MouseButtonX5); lua_setfield(L, -2, "MouseButtonX5");
    lua_pushinteger(L, (int) Actions::Space); lua_setfield(L, -2, "Space");
    lua_pushinteger(L, (int) Actions::Apostrophe); lua_setfield(L, -2, "Apostrophe");
    lua_pushinteger(L, (int) Actions::Comma); lua_setfield(L, -2, "Comma");
    lua_pushinteger(L, (int) Actions::Minus); lua_setfield(L, -2, "Minus");
    lua_pushinteger(L, (int) Actions::Period); lua_setfield(L, -2, "Period");
    lua_pushinteger(L, (int) Actions::Slash); lua_setfield(L, -2, "Slash");
    lua_pushinteger(L, (int) Actions::Num0); lua_setfield(L, -2, "Num0");
    lua_pushinteger(L, (int) Actions::Num1); lua_setfield(L, -2, "Num1");
    lua_pushinteger(L, (int) Actions::Num2); lua_setfield(L, -2, "Num2");
    lua_pushinteger(L, (int) Actions::Num3); lua_setfield(L, -2, "Num3");
    lua_pushinteger(L, (int) Actions::Num4); lua_setfield(L, -2, "Num4");
    lua_pushinteger(L, (int) Actions::Num5); lua_setfield(L, -2, "Num5");
    lua_pushinteger(L, (int) Actions::Num6); lua_setfield(L, -2, "Num6");
    lua_pushinteger(L, (int) Actions::Num7); lua_setfield(L, -2, "Num7");
    lua_pushinteger(L, (int) Actions::Num8); lua_setfield(L, -2, "Num8");
    lua_pushinteger(L, (int) Actions::Num9); lua_setfield(L, -2, "Num9");
    lua_pushinteger(L, (int) Actions::Semicolon); lua_setfield(L, -2, "Semicolon");
    lua_pushinteger(L, (int) Actions::Equal); lua_setfield(L, -2, "Equal");
    lua_pushinteger(L, (int) Actions::A); lua_setfield(L, -2, "A");
    lua_pushinteger(L, (int) Actions::B); lua_setfield(L, -2, "B");
    lua_pushinteger(L, (int) Actions::C); lua_setfield(L, -2, "C");
    lua_pushinteger(L, (int) Actions::D); lua_setfield(L, -2, "D");
    lua_pushinteger(L, (int) Actions::E); lua_setfield(L, -2, "E");
    lua_pushinteger(L, (int) Actions::F); lua_setfield(L, -2, "F");
    lua_pushinteger(L, (int) Actions::G); lua_setfield(L, -2, "G");
    lua_pushinteger(L, (int) Actions::H); lua_setfield(L, -2, "H");
    lua_pushinteger(L, (int) Actions::I); lua_setfield(L, -2, "I");
    lua_pushinteger(L, (int) Actions::J); lua_setfield(L, -2, "J");
    lua_pushinteger(L, (int) Actions::K); lua_setfield(L, -2, "K");
    lua_pushinteger(L, (int) Actions::L); lua_setfield(L, -2, "L");
    lua_pushinteger(L, (int) Actions::M); lua_setfield(L, -2, "M");
    lua_pushinteger(L, (int) Actions::N); lua_setfield(L, -2, "N");
    lua_pushinteger(L, (int) Actions::O); lua_setfield(L, -2, "O");
    lua_pushinteger(L, (int) Actions::P); lua_setfield(L, -2, "P");
    lua_pushinteger(L, (int) Actions::Q); lua_setfield(L, -2, "Q");
    lua_pushinteger(L, (int) Actions::R); lua_setfield(L, -2, "R");
    lua_pushinteger(L, (int) Actions::S); lua_setfield(L, -2, "S");
    lua_pushinteger(L, (int) Actions::T); lua_setfield(L, -2, "T");
    lua_pushinteger(L, (int) Actions::U); lua_setfield(L, -2, "U");
    lua_pushinteger(L, (int) Actions::V); lua_setfield(L, -2, "V");
    lua_pushinteger(L, (int) Actions::W); lua_setfield(L, -2, "W");
    lua_pushinteger(L, (int) Actions::X); lua_setfield(L, -2, "X");
    lua_pushinteger(L, (int) Actions::Y); lua_setfield(L, -2, "Y");
    lua_pushinteger(L, (int) Actions::Z); lua_setfield(L, -2, "Z");
    lua_pushinteger(L, (int) Actions::Left_bracket); lua_setfield(L, -2, "Left_bracket");
    lua_pushinteger(L, (int) Actions::Backslash); lua_setfield(L, -2, "Backslash");
    lua_pushinteger(L, (int) Actions::Right_bracket); lua_setfield(L, -2, "Right_bracket");
    lua_pushinteger(L, (int) Actions::Grave_accent); lua_setfield(L, -2, "Grave_accent");
    lua_pushinteger(L, (int) Actions::World1); lua_setfield(L, -2, "World1");
    lua_pushinteger(L, (int) Actions::World2); lua_setfield(L, -2, "World2");
    lua_pushinteger(L, (int) Actions::Escape); lua_setfield(L, -2, "Escape");
    lua_pushinteger(L, (int) Actions::Enter); lua_setfield(L, -2, "Enter");
    lua_pushinteger(L, (int) Actions::Tab); lua_setfield(L, -2, "Tab");
    lua_pushinteger(L, (int) Actions::Backspace); lua_setfield(L, -2, "Backspace");
    lua_pushinteger(L, (int) Actions::Insert); lua_setfield(L, -2, "Insert");
    lua_pushinteger(L, (int) Actions::Delete); lua_setfield(L, -2, "Delete");
    lua_pushinteger(L, (int) Actions::Right); lua_setfield(L, -2, "Right");
    lua_pushinteger(L, (int) Actions::Left); lua_setfield(L, -2, "Left");
    lua_pushinteger(L, (int) Actions::Down); lua_setfield(L, -2, "Down");
    lua_pushinteger(L, (int) Actions::Up); lua_setfield(L, -2, "Up");
    lua_pushinteger(L, (int) Actions::PageUp); lua_setfield(L, -2, "PageUp");
    lua_pushinteger(L, (int) Actions::PageDown); lua_setfield(L, -2, "PageDown");
    lua_pushinteger(L, (int) Actions::Home); lua_setfield(L, -2, "Home");
    lua_pushinteger(L, (int) Actions::End); lua_setfield(L, -2, "End");
    lua_pushinteger(L, (int) Actions::CapsLock); lua_setfield(L, -2, "CapsLock");
    lua_pushinteger(L, (int) Actions::ScrollLock); lua_setfield(L, -2, "ScrollLock");
    lua_pushinteger(L, (int) Actions::NumLock); lua_setfield(L, -2, "NumLock");
    lua_pushinteger(L, (int) Actions::PrintScreen); lua_setfield(L, -2, "PrintScreen");
    lua_pushinteger(L, (int) Actions::Pause); lua_setfield(L, -2, "Pause");
    lua_pushinteger(L, (int) Actions::F1); lua_setfield(L, -2, "F1");
    lua_pushinteger(L, (int) Actions::F2); lua_setfield(L, -2, "F2");
    lua_pushinteger(L, (int) Actions::F3); lua_setfield(L, -2, "F3");
    lua_pushinteger(L, (int) Actions::F4); lua_setfield(L, -2, "F4");
    lua_pushinteger(L, (int) Actions::F5); lua_setfield(L, -2, "F5");
    lua_pushinteger(L, (int) Actions::F6); lua_setfield(L, -2, "F6");
    lua_pushinteger(L, (int) Actions::F7); lua_setfield(L, -2, "F7");
    lua_pushinteger(L, (int) Actions::F8); lua_setfield(L, -2, "F8");
    lua_pushinteger(L, (int) Actions::F9); lua_setfield(L, -2, "F9");
    lua_pushinteger(L, (int) Actions::F10); lua_setfield(L, -2, "F10");
    lua_pushinteger(L, (int) Actions::F11); lua_setfield(L, -2, "F11");
    lua_pushinteger(L, (int) Actions::F12); lua_setfield(L, -2, "F12");
    lua_pushinteger(L, (int) Actions::F13); lua_setfield(L, -2, "F13");
    lua_pushinteger(L, (int) Actions::F14); lua_setfield(L, -2, "F14");
    lua_pushinteger(L, (int) Actions::F15); lua_setfield(L, -2, "F15");
    lua_pushinteger(L, (int) Actions::F16); lua_setfield(L, -2, "F16");
    lua_pushinteger(L, (int) Actions::F17); lua_setfield(L, -2, "F17");
    lua_pushinteger(L, (int) Actions::F18); lua_setfield(L, -2, "F18");
    lua_pushinteger(L, (int) Actions::F19); lua_setfield(L, -2, "F19");
    lua_pushinteger(L, (int) Actions::F20); lua_setfield(L, -2, "F20");
    lua_pushinteger(L, (int) Actions::F21); lua_setfield(L, -2, "F21");
    lua_pushinteger(L, (int) Actions::F22); lua_setfield(L, -2, "F22");
    lua_pushinteger(L, (int) Actions::F23); lua_setfield(L, -2, "F23");
    lua_pushinteger(L, (int) Actions::F24); lua_setfield(L, -2, "F24");
    lua_pushinteger(L, (int) Actions::F25); lua_setfield(L, -2, "F25");
    lua_pushinteger(L, (int) Actions::Kp0); lua_setfield(L, -2, "Kp0");
    lua_pushinteger(L, (int) Actions::Kp1); lua_setfield(L, -2, "Kp1");
    lua_pushinteger(L, (int) Actions::Kp2); lua_setfield(L, -2, "Kp2");
    lua_pushinteger(L, (int) Actions::Kp3); lua_setfield(L, -2, "Kp3");
    lua_pushinteger(L, (int) Actions::Kp4); lua_setfield(L, -2, "Kp4");
    lua_pushinteger(L, (int) Actions::Kp5); lua_setfield(L, -2, "Kp5");
    lua_pushinteger(L, (int) Actions::Kp6); lua_setfield(L, -2, "Kp6");
    lua_pushinteger(L, (int) Actions::Kp7); lua_setfield(L, -2, "Kp7");
    lua_pushinteger(L, (int) Actions::Kp8); lua_setfield(L, -2, "Kp8");
    lua_pushinteger(L, (int) Actions::Kp9); lua_setfield(L, -2, "Kp9");
    lua_pushinteger(L, (int) Actions::KpDecimal); lua_setfield(L, -2, "KpDecimal");
    lua_pushinteger(L, (int) Actions::KpDivide); lua_setfield(L, -2, "KpDivide");
    lua_pushinteger(L, (int) Actions::KpMultiply); lua_setfield(L, -2, "KpMultiply");
    lua_pushinteger(L, (int) Actions::KpSubtract); lua_setfield(L, -2, "KpSubtract");
    lua_pushinteger(L, (int) Actions::KpAdd); lua_setfield(L, -2, "KpAdd");
    lua_pushinteger(L, (int) Actions::KpEnter); lua_setfield(L, -2, "KpEnter");
    lua_pushinteger(L, (int) Actions::KpEqual); lua_setfield(L, -2, "KpEqual");
    lua_pushinteger(L, (int) Actions::LeftShift); lua_setfield(L, -2, "LeftShift");
    lua_pushinteger(L, (int) Actions::LeftControl); lua_setfield(L, -2, "LeftControl");
    lua_pushinteger(L, (int) Actions::LeftAlt); lua_setfield(L, -2, "LeftAlt");
    lua_pushinteger(L, (int) Actions::LeftSuper); lua_setfield(L, -2, "LeftSuper");
    lua_pushinteger(L, (int) Actions::RightShift); lua_setfield(L, -2, "RightShift");
    lua_pushinteger(L, (int) Actions::RightControl); lua_setfield(L, -2, "RightControl");
    lua_pushinteger(L, (int) Actions::RightAlt); lua_setfield(L, -2, "RightAlt");
    lua_pushinteger(L, (int) Actions::RightSuper); lua_setfield(L, -2, "RightSuper");
    lua_pushinteger(L, (int) Actions::Menu); lua_setfield(L, -2, "Menu");

    lua_setglobal(L, "Actions");
}

void get_v2_op_args(Vector2 *lhs, Vector2 *rhs, lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 1, "x");
    lhs->x = luaL_checknumber(L, -1);
    lua_getfield(L, 1, "y");
    lhs->y = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    lua_getfield(L, 2, "x");
    rhs->x = luaL_checknumber(L, -1);
    lua_getfield(L, 2, "y");
    rhs->y = luaL_checknumber(L, -1);
    lua_pop(L, 2);
}

void push_v2(lua_State *L, const Vector2 &vector) {
    lua_newtable(L);
    lua_pushnumber(L, vector.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, vector.y); lua_setfield(L, -2, "y");
}

int lua_v2(lua_State *L) {
    float x = luaL_checknumber(L, 1);
    float y = luaL_optnumber(L, 2, x); // default to same as x so you can go: v2(1)
    push_v2(L, {x, y});
    return 1;
}

int lua_v2_add(lua_State *L) {
    Vector2 lhs, rhs;
    get_v2_op_args(&lhs, &rhs, L);
    push_v2(L, lhs + rhs);
    return 1;
}

int lua_v2_sub(lua_State *L) {
    Vector2 lhs, rhs;
    get_v2_op_args(&lhs, &rhs, L);
    push_v2(L, lhs - rhs);
    return 1;
}

int lua_v2_mul(lua_State *L) {
    Vector2 lhs, rhs;
    get_v2_op_args(&lhs, &rhs, L);
    push_v2(L, lhs * rhs);
    return 1;
}

int lua_v2_div(lua_State *L) {
    Vector2 lhs, rhs;
    get_v2_op_args(&lhs, &rhs, L);
    push_v2(L, lhs / rhs);
    return 1;
}

int lua_v2_normalize(lua_State *L) {
    lua_getfield(L, 1, "x");
    float x = luaL_checknumber(L, -1);
    lua_getfield(L, 1, "y");
    float y = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    push_v2(L, Vector2(x, y).normalized());
    return 1;
}

int lua_v2_length(lua_State *L) {
    lua_getfield(L, 1, "x");
    float x = luaL_checknumber(L, -1);
    lua_getfield(L, 1, "y");
    float y = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    lua_pushinteger(L, Vector2(x, y).length());
    return 1;
}

int lua_v2_angle(lua_State *L) {
    lua_getfield(L, 1, "x");
    float x = luaL_checknumber(L, -1);
    lua_getfield(L, 1, "y");
    float y = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    lua_pushinteger(L, Vector2(x, y).angle());
    return 1;
}

int lua_rectangles_overlap(lua_State *L) {
    Rect2 a, b;
    load_rect2(L, a, 1);
    load_rect2(L, b, 1);
    lua_pushboolean(L, a.intersects(b));
    return 1;
}

int lua_get_axis(lua_State *L) {
    int negitive = luaL_checkinteger(L, 1);
    int positive = luaL_checkinteger(L, 2);
    lua_pushnumber(L, Input::get_axis((Actions) negitive, (Actions) positive));
    return 1;
}

int lua_get_direction(lua_State *L) {
    if (!lua_istable(L, 1)) {
        RETURN_ERROR(L, "{up: Actions.W, left: Actions.A, down: Actions.S, right: Actions.D} as the first argument");
    }

    lua_getfield(L, 1, "left");
    Actions left = (Actions) luaL_checkinteger(L, -1);

    lua_getfield(L, 1, "right");
    Actions right = (Actions) luaL_checkinteger(L, -1);

    lua_getfield(L, 1, "up");
    Actions up = (Actions) luaL_checkinteger(L, -1);

    lua_getfield(L, 1, "down");
    Actions down = (Actions) luaL_checkinteger(L, -1);

    push_v2(L, Input::get_direction(up, down, left, right));
    return 1;
}

int lua_mouse_position(lua_State *L) {
    push_v2(L, Input::get_mouse_position());
    return 1;
}

int lua_mouse_delta(lua_State *L) {
    push_v2(L, Input::get_mouse_delta());
    return 1;
}

int lua_is_pressed(lua_State *L) {
    int action = luaL_checkinteger(L, 1);
    lua_pushboolean(L, Input::is_pressed((Actions) action));
    return 1;
}

int lua_is_typed(lua_State *L) {
    int action = luaL_checkinteger(L, 1);
    lua_pushboolean(L, Input::is_typed((Actions) action));
    return 1;
}

int lua_is_just_pressed(lua_State *L) {
    int action = luaL_checkinteger(L, 1);
    lua_pushboolean(L, Input::is_just_pressed((Actions) action));
    return 1;
}

int lua_is_just_released(lua_State *L) {
    int action = luaL_checkinteger(L, 1);
    lua_pushboolean(L, Input::is_just_released((Actions) action));
    return 1;
}

int lua_string_typed(lua_State *L) {
    View<char> chars = Input::get_chars_typed();
    lua_pushlstring(L, chars.ptr(), chars.size());
    return 1;
}

int lua_delta(lua_State *L) {
    lua_pushnumber(L, Time::delta());
    return 1;
}

int lua_randi_between(lua_State *L) {
    int low = luaL_checkinteger(L, 1);
    int high = luaL_checkinteger(L, 2);

    lua_pushinteger(L, rng::between(low, high));
    return 1;
}

int lua_randf_between(lua_State *L) {
    float low = luaL_checknumber(L, 1);
    float high = luaL_checknumber(L, 2);

    lua_pushnumber(L, rng::between(low, high));
    return 1;
}

int lua_randv2_between(lua_State *L) {
    lua_getfield(L, 1, "x");
    float x1 = luaL_checknumber(L, -1);
    lua_getfield(L, 1, "y");
    float y1 = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    lua_getfield(L, 2, "x");
    float x2 = luaL_checknumber(L, -1);
    lua_getfield(L, 2, "y");
    float y2 = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    push_v2(L, Vector2(rng::between(x1, x2), rng::between(y1, y2)));
    return 1;
}

int lua_randf(lua_State *L) {
    lua_pushnumber(L, rng::randf());
    return 1;
}

int lua_randi(lua_State *L) {
    lua_pushinteger(L, rng::randi());
    return 1;
}

int lua_randb(lua_State *L) {
    lua_pushboolean(L, rng::randb());
    return 1;
}

int lua_alloc_id(lua_State *L) {
    lua_pushinteger(L, alloc_id().id);
    return 1;
}

int lua_physics_get(lua_State *L) {
    ID id;
    id.id = luaL_checkinteger(L, 1);

    const pp::PhysicsObject *obj = physics.objects.get(id);

    lua_newtable(L);
    if (obj == nullptr) {
        push_v2(L, obj->aabb.position); lua_setfield(L, -2, "position");
        push_v2(L, obj->aabb.size); lua_setfield(L, -2, "size");
        lua_pushinteger(L, obj->layer); lua_setfield(L, -1, "layer");
        lua_pushinteger(L, obj->mask); lua_setfield(L, -1, "mask");
        lua_pushinteger(L, obj->type); lua_setfield(L, -1, "type");
    }

    return 1;
}

int lua_physics_move(lua_State *L) {
    ID id;
    id.id = luaL_checkinteger(L, 1);

    lua_getfield(L, 2, "x");
    float x = luaL_checknumber(L, -1);
    lua_getfield(L, 2, "y");
    float y = luaL_checknumber(L, -1);
    lua_pop(L, 2);

    lua_pushinteger(L, physics.move_actor(id, {x, y}).id);
    return 1;
}

int lua_physics_create(lua_State *L) {
    ID id;
    lua_getfield(L, 1, "id");
    id.id = luaL_checkinteger(L, -1);

    Vector2 position, size;
    load_v2(L, position, "position", 1);
    load_v2(L, size, "size", 1);

    lua_getfield(L, 1, "layer");
    int layer = luaL_optinteger(L, -1, 1);

    lua_getfield(L, 1, "mask");
    int mask = luaL_optinteger(L, -1, 1);

    lua_getfield(L, 1, "type");
    int type = luaL_optinteger(L, -1, 0);

    physics.objects.insert(id, {{position, size}, (pp::PhysicsObject::Type) type, mask, layer});

    return 0;
}

int lua_physics_destroy(lua_State *L) {
    ID id;
    id.id = luaL_checkinteger(L, 1);

    physics.objects.erase(id);

    return 0;
}

int lua_physics_aabb_cast(lua_State *L) {
    Vector2 position, size;
    load_v2(L, position, "position", 1);
    load_v2(L, size, "size", 1);

    lua_getfield(L, 1, "mask");
    int mask = luaL_checkinteger(L, -1);

    lua_pushinteger(L, physics.aabb_cast({position, size}, mask).id);
    return 1;
}

int lua_physics_ray_cast(lua_State *L) {
    Vector2 start, finish;
    load_v2(L, start, "start", 1);
    load_v2(L, finish, "finish", 1);

    lua_getfield(L, 1, "mask");
    int mask = luaL_checkinteger(L, -1);

    lua_pushinteger(L, physics.ray_cast(start, finish, mask).id);
    return 1;
}

int lua_physics_circle_cast(lua_State *L) {
    Vector2 center;
    load_v2(L, center, "center", 1);

    lua_getfield(L, 1, "radius");
    float radius = luaL_checknumber(L, -1);

    lua_getfield(L, 1, "mask");
    int mask = luaL_checkinteger(L, -1);

    lua_pushinteger(L, physics.circle_cast(center, radius, mask).id);
    return 1;
}

int lua_physics_debug(lua_State *L) {
    auto &renderer = WM::get_main_window()->get_renderers()[0];
    physics.debug_draw(renderer);
    return 0;
}

void bind_physics_to_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_physics_get); lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_physics_move); lua_setfield(L, -2, "move");
    lua_pushcfunction(L, lua_physics_create); lua_setfield(L, -2, "create");
    lua_pushcfunction(L, lua_physics_destroy); lua_setfield(L, -2, "destroy");
    lua_pushcfunction(L, lua_physics_aabb_cast); lua_setfield(L, -2, "aabb_cast");
    lua_pushcfunction(L, lua_physics_ray_cast); lua_setfield(L, -2, "ray_cast");
    lua_pushcfunction(L, lua_physics_circle_cast); lua_setfield(L, -2, "circle_cast");
    lua_pushcfunction(L, lua_physics_debug); lua_setfield(L, -2, "debug");

    lua_pushinteger(L, (int) pp::PhysicsObject::Type::Actor); lua_setfield(L, -2, "Actor");
    lua_pushinteger(L, (int) pp::PhysicsObject::Type::Solid); lua_setfield(L, -2, "Solid");

    lua_setglobal(L, "Physics");
}

void bind_input_to_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_is_pressed); lua_setfield(L, -2, "is_pressed");
    lua_pushcfunction(L, lua_is_typed); lua_setfield(L, -2, "is_typed");
    lua_pushcfunction(L, lua_is_just_pressed); lua_setfield(L, -2, "is_just_pressed");
    lua_pushcfunction(L, lua_is_just_released); lua_setfield(L, -2, "is_just_released");
    lua_pushcfunction(L, lua_string_typed); lua_setfield(L, -2, "string_typed");
    lua_pushcfunction(L, lua_get_axis); lua_setfield(L, -2, "get_axis");
    lua_pushcfunction(L, lua_get_direction); lua_setfield(L, -2, "get_direction");
    lua_pushcfunction(L, lua_mouse_position); lua_setfield(L, -2, "mouse_position");
    lua_pushcfunction(L, lua_mouse_delta); lua_setfield(L, -2, "mouse_delta");
    lua_setglobal(L, "Input");
}

void bind_time_to_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, lua_delta); lua_setfield(L, -2, "delta");
    lua_setglobal(L, "Time");
}

lua_State *init(int argc, char **argv) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    const char *program = "main.lua";
    if (argc > 1) program = argv[1];
    
    int status = luaL_loadfile(L, program);
    if (status) {
        LOG_ERROR("Couldn't load file: %", lua_tostring(L, -1));
        return nullptr;
    }

    bind_function(L, "push_system", lua_push_system);
    bind_function(L, "draw_rect2", lua_draw_rect2);
    bind_function(L, "draw_line", lua_draw_line);
    bind_function(L, "draw_text", lua_draw_text);
    bind_function(L, "draw_sprite", lua_draw_sprite);
    bind_function(L, "load_texture", lua_load_texture);
    bind_function(L, "load_shader", lua_load_shader);
    bind_function(L, "include", lua_include);

    bind_function(L, "v2", lua_v2);
    bind_function(L, "v2_add", lua_v2_add);
    bind_function(L, "v2_sub", lua_v2_sub);
    bind_function(L, "v2_mul", lua_v2_mul);
    bind_function(L, "v2_div", lua_v2_div);
    bind_function(L, "v2_normalize", lua_v2_normalize);
    bind_function(L, "v2_length", lua_v2_length);
    bind_function(L, "v2_angle", lua_v2_angle);

    bind_function(L, "randi_between", lua_randi_between);
    bind_function(L, "randf_between", lua_randf_between);
    bind_function(L, "randv2_between", lua_randv2_between);

    bind_function(L, "randi", lua_randi);
    bind_function(L, "randf", lua_randf);
    bind_function(L, "randb", lua_randb);

    bind_function(L, "alloc_id", lua_alloc_id);
    bind_function(L, "rectangles_overlap", lua_rectangles_overlap);

    bind_event_ids_to_lua(L);
    bind_input_actions_to_lua(L);
    bind_input_to_lua(L);
    bind_time_to_lua(L);
    bind_physics_to_lua(L);

    rng::set_seed();

    int result = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (result) {
        LOG_ERROR("Failed to run script: %", lua_tostring(L, -1));
        return nullptr;
    }

    return L;
}

bool load_config(int argc, char **argv, WindowProps &window_props) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);

    const char *program = "config.lua";
    if (argc > 2) program = argv[2];

    if (!fs::file_exists(program, nullptr)) {
        lua_close(L);
        return true;
    }

    int status = luaL_loadfile(L, program);
    if (status) {
        LOG_ERROR("Couldn't load file: %", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }

    int result = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (result) {
        LOG_ERROR("Failed to run script: %", lua_tostring(L, -1));
        lua_close(L);
        return false;
    }

    lua_getfield(L, -1, "window_title");
    if (lua_isstring(L, -1)) {
        const char *title = lua_tostring(L, -1);
        StrView view = title;
        window_props.title = view.cstr(static_arena);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "window_width");
    if (lua_isinteger(L, -1)) {
        window_props.size.x = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "window_height");
    if (lua_isinteger(L, -1)) {
        window_props.size.y = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "resolution_x");
    if (lua_isinteger(L, -1)) {
        window_props.content_scale_size.x = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "resolution_y");
    if (lua_isinteger(L, -1)) {
        window_props.content_scale_size.y = lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "background_color");
    if (lua_istable(L, -1)) {
        color_from_object(window_props.bg, L);
    } else {
        lua_pop(L, 1);
    }

    lua_getfield(L, -1, "aspect_keep");
    if (lua_isboolean(L, -1)) {
        window_props.content_scale_aspect = lua_toboolean(L, -1) ? CONTENT_SCALE_ASPECT_KEEP : CONTENT_SCALE_ASPECT_IGNORE;
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "scale_viewport");
    if (lua_isboolean(L, -1)) {
        window_props.content_scale_mode = lua_toboolean(L, -1) ? CONTENT_SCALE_MODE_VIEWPORT : CONTENT_SCALE_MODE_DISABLED;
    }
    lua_pop(L, 1);

    lua_close(L);
    return true;
}

void clear_frame_arena(Events::PreUpdate &) {
    frame_arena.reset();
}

#ifdef _WIN32
#include <Windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    Jovial game;
    WindowProps props{
        .size = {1280, 720},
        .title = "My Jovial Game",
        .bg = Colors::GRUVBOX_GREY,
    };

    // Parse command line arguments
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;

    load_config(argc, (char **) argv, props);
    systems2d(game, props);
    game.push_system(clear_frame_arena);
    load_jovial_font(&default_font);

    lua_State* L = init(argc, (char**) argv);
    if (!L) {
        LOG_ERROR("Could not open 'main.lua'");
        return -1;
    }

    game.run();

    // Free the memory allocated by CommandLineToArgvW
    LocalFree(argv);

    return 0;

}
#else

int main(int argc, char **argv) {
    Jovial game;
    WindowProps props{
            .size  = {1280, 720},
            .title = "My Jovial Game",
            .bg    = Colors::GRUVBOX_GREY,
    };
    load_config(argc, argv, props);
    systems2d(game, props);
    game.push_system(clear_frame_arena);
    load_jovial_font(&default_font);

    lua_State *L = init(argc, argv);
    if (!L) return -1;

    game.run();
    return 0;
}

#endif
