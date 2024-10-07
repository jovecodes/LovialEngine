#define JV_FREETYPE_SUPPORT

#ifdef BUNDLE_FONT
#include "Font.h"
#endif

#include "IO/Print.h"
#include "IO/StringBuilder.h"
#include "Input/Input.h"
#include "Jovial.h"
#include "OS/FileAccess.h"
#include "Rendering/2D/Renderer2D.h"
#include "Rendering/2D/Text.h"
#include "Rendering/2D/UI.h"
#include "Rendering/PostProcessRenderer.h"
#include "Rendering/Viewport.h"
#include "Window.h"
#include "OS/Command.h"

using namespace jovial;

enum class VimMode {
    Normal,
    Insert,
    Visual,
};

const char *vim_mode_to_string(VimMode mode) {
    switch (mode) {
        case VimMode::Normal:
            return "Normal";
        case VimMode::Insert:
            return "Insert";
        case VimMode::Visual:
            return "Visual";
    }
    return "Unknown";
}

struct AlphabeticalSort {
    inline bool operator()(StrView l, StrView r) const {
        return l.alpha_cmpn(r);
    }
};

struct Buffer {
    DArray<String> lines;
    String file;
    Vector2i position;
    int cam_offset = 0;

    enum Flags {
        READ_ONLY = 1 << 0,
        DIRECTORY = 1 << 1,
    };

    int flags = 0;
    StrView prompt;
    void (*on_selected)(Buffer &);

    inline int x() {
        return math::min(position.x, lines[position.y].size() - 1);
    }

    inline String &line() {
        return lines[position.y];
    }

    void save() {
        StringBuilder sb;
        for (const auto &line: lines) {
            sb.append(line);
            sb.append("\n");
        }

        fs::write_file(file, sb.build(talloc));
    }

    Vector2i remove_at(Vector2i at) {
        Vector2i old_position = position;
        position = at;

        if (position.x < 0) {
            if (position.y > 0) {
                move_y(-1);
                position.x = line().size();
                line().append(halloc, lines[position.y + 1]);

                remove_line(position.y + 1);
            }
        } else if (position.x >= line().size()) {
            line().pop();
        } else {
            line().remove_at(position.x);
        }

        move_x(0);

        Vector2i res = position;
        position = old_position;
        return res;
    }

    void move_y(int amount) {
        position.y += amount;

        // TODO: make these numbers based on window height
        if (position.y - cam_offset > 20) {
            cam_offset += amount;
        }
        if (position.y - cam_offset < 5) {
            cam_offset -= math::abs(amount);
        }
        cam_offset = math::CLAMPED(cam_offset, 0, (int) lines.size() - 1);

        math::CLAMP(position.y, 0, (int) lines.size() - 1);
    }

    void move_x(int amount) {
        position.x += amount;
        math::CLAMP(position.y, 0, (int) lines.size() - 1);
        math::CLAMP(position.x, 0, (int) line().size());
    }

    Vector2i insert_at(Vector2i at, char c) {
        if (flags & READ_ONLY) {
            return at;                
        }
        Vector2i old_position = position;

        if (!prompt.is_empty() && c == '\n') {
            on_selected(*this);
            return at;
        }

        if (c == '\n' && at.x >= line().size()) {
            lines.insert(halloc, position.y + 1, String());

            position.x = 0;
            move_y(1);
        } else if (c == '\n') {
            position = at;
            lines.insert(halloc, position.y + 1, String());
            StrView str = line().substr(x());
            lines[position.y + 1].copy_from(halloc, str.ptr(), str.size());

            StrView old = line().substr(0, x());
            lines[position.y].copy_from(halloc, old.ptr(), old.size());

            position.x = 0;
            move_y(1);
        } else if (at.x >= line().size()) {
            line().push(halloc, c);
            position.x += 1;
        } else {
            line().insert(halloc, x(), c);
            position.x += 1;
        }

        Vector2i result = position;
        position = old_position;
        return result;
    }

    inline void remove_line(int line) {
        lines[line].free();
        lines.remove_at(line);
    }

    void load(StrView path) {
        file = String(halloc, path);
        Arena content_arena;

        Error error = OK;

        if (fs::is_directory(path.tcstr())) {
            flags |= READ_ONLY | DIRECTORY;
            DArray<String> files = fs::read_dir(path, &error);
            if (error != OK) return;

            lines.push(halloc, String(halloc, ".."));
            for (u32 i = 0; i < files.size(); ++i) {
                if (files[i] == "." || files[i] == "..") continue;

                StringBuilder sb;
                sb.append(files[i]);
                if (fs::is_directory(files[i].view().tcstr())) sb.append("/");
                
                lines.push(halloc, sb.build(halloc));
            }
            // lines.sort_custom<AlphabeticalSort>();
            return;
        }

        if (!fs::file_exists(path, &error)) {
            fs::write_file(path, "");
        }

        String contents = fs::read_file(content_arena, path, &error);
        if (error != OK) return;

        if (contents.is_empty()) {
            lines.push(halloc, String());
            return;
        }

        DArray<StrView> splits = contents.view().split_lines();
        lines.resize(halloc, splits.size());
        for (u32 i = 0; i < splits.size(); ++i) {
            lines[i].copy_from(halloc, splits[i].ptr(), splits[i].size());
        }
    }

    void free() {
        for (auto &line: lines) {
            line.free();
        }
        lines.free();
        file.free();
    }
};

enum Bindings {
    MOUSE_BINDINGS,
    VIM_BINDINGS,
};

struct Global {
    FreeFont font;
    ui::Theme theme;
    OwnedDArray<Buffer> buffers;
    int buffer_index = -1;
    int last_buffer_index = -1;

    float line_spacing = 1.2f;

    StrView path_to_game;
    Arena path_to_game_arena;
    os::Proc game_proc;

    Bindings bindings = VIM_BINDINGS;
    VimMode vim_mode = VimMode::Insert;
    Arena command_arena;
    String command;

    StrView error = "";
    bool using_sdf = false;

    void flush_command() {
        command_arena.reset();
        command = String();
    }

    Buffer *current_buffer() {
        if (buffer_index < 0) return nullptr;
        return &buffers[buffer_index];
    }

    void open_file(StrView file) {
        buffers.push({});
        buffers.back().load(file);

        vim_mode = VimMode::Normal;

        set_buffer(buffers.size() - 1);
    }

    void open_prompt(StrView prompt, void (*callback)(Buffer &)) {
        buffers.push({});
        buffers.back().prompt = prompt;
        buffers.back().on_selected = callback;
        buffers.back().lines.push(halloc, String());
        
        vim_mode = VimMode::Insert;

        set_buffer(buffers.size() - 1);
    }

    void set_buffer(int index) {
        last_buffer_index = buffer_index;
        buffer_index = index;
        if (last_buffer_index == -1) {
            last_buffer_index = buffer_index;
        }
    }

    void load_font(float size = 20, bool sdf = false) {
        using_sdf = sdf;
        if (font.is_loaded()) {
            font.unload();
            font = {};
        }
#ifdef BUNDLE_FONT
        if (sdf) {
            font.load_buffer(FONT_DATA, FONT_DATA_LEN, size, FreeFont::SDF);
            freetype_set_anti_aliasing_factor(0.6);
        } else {
            font.load_buffer(FONT_DATA, FONT_DATA_LEN, size, FreeFont::BITMAP);
            freetype_set_anti_aliasing_factor(0.8);
        }
#else
        const char *path = "./editor/jet_brains.ttf";
        JV_ASSERT(fs::file_exists(path, nullptr));
        if (sdf) {
            font.load(path, size, FreeFont::SDF);
            freetype_set_anti_aliasing_factor(0.6);
        } else {
            font.load(path, size, FreeFont::BITMAP);
            freetype_set_anti_aliasing_factor(0.8);
        }
#endif
    }

    void load_default_theme() {
        theme.named_colors.clear();
        theme.named_colors.insert("theme_green", Color::hex(0xa9b665ff));
        theme.named_colors.insert("theme_purple", Color::hex(0xd3869bff));
        theme.named_colors.insert("theme_orange", Color::hex(0xcc7d49ff));
        theme.named_colors.insert("theme_red", Color::hex(0xe96962ff));
        theme.primary = Colors::GRUVBOX_WHITE.darkened(0.1);
        theme.secondary = Colors::GRUVBOX_GREY;
        theme.accent = Colors::BLACK;
        theme.text_color = Colors::WHITE;
        theme.normal_font = &font;
        theme.muted = Colors::GRUVBOX_LIGHTGRAY.lightened(0.1);
        theme.outline_thickness = 3.0f;
        theme.text_padding = 10.0f;
    }

    void play() {
        if (game_proc.is_valid()) game_proc.wait();
        os::Command command;
        command.append(path_to_game);
        game_proc = command.run_async();
    }

    void find_game() {
        path_to_game_arena.reset();
        String dir(talloc, ".");
        auto files = fs::read_dir(".", nullptr);
        for (auto i: files) {
            if (i == "LovialEngine.exe" || i == "main" || i == "LovialEngine") {
                StringBuilder sb;
#ifdef _WIN32
                sb.append(dir, "\\", i);
#else 
                sb.append(dir, "/", i);
#endif
                path_to_game = sb.build(path_to_game_arena);
            }
        }
    }

    void close_buffer(int index) {
        buffers[index].free();
        buffers.remove_at(index);
        if (last_buffer_index >= index) last_buffer_index -= 1;
        if (buffer_index >= index) buffer_index -= 1;
    }

    void close_current_buffer() {
        close_buffer(buffer_index);
        buffer_index = last_buffer_index;
    }

    void close_last_buffer() {
        close_buffer(last_buffer_index);
        last_buffer_index = -1;
    }

    ~Global() {
        for (auto &buffer: buffers) {
            buffer.free();
        }
        font.unload();
    }
} global;

inline bool is_control_pressed() {
    return Input::is_pressed(Actions::LeftControl) || Input::is_pressed(Actions::RightControl);
}

inline bool is_shift_pressed() {
    return Input::is_pressed(Actions::LeftShift) || Input::is_pressed(Actions::RightShift);
}

#define VIM_BINDING_LISTS(X) \
    X(VimMode::Normal, "i", g.vim_mode = VimMode::Insert) \
    X(VimMode::Normal, "h", buf->move_x(-1)) \
    X(VimMode::Normal, "j", buf->move_y(1)) \
    X(VimMode::Normal, "k", buf->move_y(-1)) \
    X(VimMode::Normal, "l", buf->move_x(1)) \

void on_typed(Global &g, Events::KeyTyped &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;

    if (g.bindings == VIM_BINDINGS) {
        g.command.push(g.command_arena, event.character);

        if (g.vim_mode == VimMode::Insert) {
            buf->position = buf->insert_at(buf->position, event.character);
        }

#define X(mode, str, action) if (g.vim_mode == mode && g.command.view().ends_with(str)) { g.flush_command(); action; }
        VIM_BINDING_LISTS(X)
#undef X

    } else {
        buf->position = buf->insert_at(buf->position, event.character);
    }
}

void on_open_file(Buffer &buffer) {
    global.open_file(buffer.line());
    global.close_last_buffer();
}

void on_pressed(Global &g, Events::KeyPressed &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;

    if (event.keycode == Actions::Backspace) {
        buf->position = buf->remove_at({buf->position.x - 1, buf->position.y});
    } else if (event.keycode == Actions::Delete) {
        buf->remove_at(buf->position);
    } else if (event.keycode == Actions::Enter) {
        if (buf->flags & Buffer::DIRECTORY) {
#ifdef _WIN32
            g.open_file(tprint("%\\%", buf->file, buf->line()));
#else
            g.open_file(tprint("%/%", buf->file, buf->line()));
#endif
            g.close_last_buffer();
            return;
        } else {
            buf->position = buf->insert_at(buf->position, '\n');
        }
    } 

    if (buf->flags & Buffer::DIRECTORY) {
        if (event.keycode == Actions::Minus) {
#ifdef _WIN32
            g.open_file(tprint("%\\..", buf->file));
#else
            g.open_file(tprint("%/..", buf->file));
#endif
            return;
        }
    }

    if (is_control_pressed() && event.keycode == Actions::S) {
        buf->save();
    } else if (is_control_pressed() && event.keycode == Actions::Space) {
        g.play();
    } 

    if (g.bindings == VIM_BINDINGS) {
        if (g.vim_mode == VimMode::Insert) {
            if (event.keycode == Actions::Escape) {
                g.vim_mode = VimMode::Normal;
                buf->move_x(-1);
            } 
        }
    }

    if (event.keycode == Actions::Right) {
        buf->move_x(1);
    } else if (event.keycode == Actions::Left) {
        buf->move_x(-1);
    } else if (event.keycode == Actions::Up) {
        buf->move_y(-1);
    } else if (event.keycode == Actions::Down) {
        buf->move_y(1);
    }

    if (event.keycode == Actions::F2) {
        if (g.bindings == VIM_BINDINGS) {
            g.bindings = MOUSE_BINDINGS;
        } else if (g.bindings == MOUSE_BINDINGS) {
            g.bindings = VIM_BINDINGS;
        }
    } if (event.keycode == Actions::F1) {
        g.load_font(g.font.size, !g.using_sdf);
    } else if (is_control_pressed() && event.keycode == Actions::Equal) {
        g.load_font(g.font.size + 2);
    } else if (is_control_pressed() && event.keycode == Actions::Minus) {
        g.load_font(g.font.size - 2);
    } else if (is_control_pressed() && event.keycode == Actions::Num0) {
        g.load_font();
    }

    if (is_control_pressed() && event.keycode == Actions::O) {
        if (is_shift_pressed()) {
            g.open_file(buf->file.get_base_dir());
            return;
// #ifdef _WIN32
//             g.open_file(tprint("%\\.", buf->file));
// #else
//             g.open_file(tprint("%/.", buf->file));
// #endif
        } else {
            g.open_prompt("Path: ", on_open_file);
        }
    }
}

void update_mouse(Global &g, Events::Update &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;


    if (Input::is_pressed(Actions::LeftMouseButton)) {
        ui::Layout layout(talloc, {{0, 0}, WM::get_main_window()->get_size()});
        const float line_percent = 1 - (g.font.size * g.line_spacing * 2) / WM::get_main_window()->get_height();
        const char *line_number_fmt = " % ";
        StrView max_number = tprint(line_number_fmt, buf->lines.size());
        Vector2 offset;
        g.font.measure(offset, max_number);

        float y = WM::get_main_window()->get_height() - Input::get_mouse_position().y;
        y /= global.font.size * global.line_spacing;
        y = math::floor(y);

        float x = Input::get_mouse_position().x - offset.x;

        buf->position.y = math::CLAMPED(y + buf->cam_offset, 0, buf->lines.size() - 1);

        buf->position.x = 0;
        while (buf->position.x < buf->line().size()) {
            x -= g.font.metrics[' '].advance.x;
            if (x <= 0) {
                break;
            }
            buf->position.x += 1;
        }
        buf->position.x = math::CLAMPED(buf->position.x, 0, buf->line().size());
    }

    buf->cam_offset += Input::get_scroll() * 2;
    buf->move_y(0);
}

void draw(Global &g, Events::Draw &e) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;
    if (!g.buffers[g.buffer_index].prompt.is_empty()) {
        buf = &g.buffers[g.last_buffer_index];
    }

    ui::Layout layout(talloc, {{0, 0}, WM::get_main_window()->get_size()});
    const float line_percent = 1 - (g.font.size * g.line_spacing * 2) / WM::get_main_window()->get_height();

    { // top status line
        Rect2 rect = layout.push_percent(ui::TOP, line_percent, 1 - line_percent);
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        if (!g.error.is_empty()) {
            ui::label(e.renderers[0], g.font, rect, g.error, *g.theme.named_colors.get("theme_red"), ui::RIGHT);
        }

        layout.pop();
    }

    { // bottom status line
        Rect2 rect = layout.push_percent(ui::BOTTOM, line_percent, 1 - line_percent);
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        StrView msg = tprint("%:%:%", buf->file, buf->position.y + 1, buf->position.x + 1);
        ui::label(e.renderers[0], g.font, rect, msg, g.theme.primary, ui::RIGHT);
        ui::label(e.renderers[0], g.font, rect, g.path_to_game.is_empty() ? "unfound" : g.path_to_game, g.theme.muted, ui::CENTER);
        
        if (g.bindings == VIM_BINDINGS) {
            ui::label(e.renderers[0], g.font, rect, tprint("Vim: % %", vim_mode_to_string(g.vim_mode), g.command), g.theme.muted, ui::LEFT);
        } else if (g.bindings == MOUSE_BINDINGS) {
            ui::label(e.renderers[0], g.font, rect, "Mouse", g.theme.muted, ui::LEFT);
        }

        layout.pop();
        layout.push_percent(ui::TOP, 1, line_percent);
    }

    { // buffer text
        Rect2 rect = layout.currrent();
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        const char *line_number_fmt = " % ";
        StrView max_number = tprint(line_number_fmt, buf->lines.size());

        Vector2 pos = layout.currrent().top_left();
        g.font.measure(pos, max_number);

        float x = pos.x;
        pos.y -= g.font.size;

        if (buf->lines.is_empty() || (buf->lines.size() == 1 && buf->lines[0].size() == 0)) {
            Vector2 pos_copy = pos;
            g.font.immediate_draw(e.renderers[0], pos_copy, "  empty file", g.theme.muted);
        }

        for (u32 i = buf->cam_offset; i < buf->lines.size(); ++i) {
            Vector2 line_pos(rect.position.x, pos.y);
            if (buf->position.y == i) {
                g.font.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, i + 1), *g.theme.named_colors.get("theme_purple"));
            } else {
                g.font.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, math::abs((int) buf->position.y - (int) i)), g.theme.muted);
            }

            if (buf->position.y == i) {
                if (buf->position.x >= buf->line().size()) {
                    g.font.immediate_draw(e.renderers[0], pos, buf->lines[i], g.theme.primary);
                    Rect2DCmd cmd{pos, {g.font.metrics[0].advance.x, g.font.size * g.line_spacing}, g.theme.primary};
                    cmd.position.y -= g.font.size * (g.line_spacing - 1);
                    cmd.immediate_draw(e.renderers[0]);
                } else {
                    if (buf->x() != 0) {
                        g.font.immediate_draw(e.renderers[0], pos, buf->lines[i].substr(0, buf->x()), g.theme.primary);
                    }
                    Rect2DCmd cmd{pos, {g.font.metrics[0].advance.x, g.font.size * g.line_spacing}, g.theme.primary};
                    cmd.position.y -= g.font.size * (g.line_spacing - 1);
                    cmd.immediate_draw(e.renderers[0]);

                    StrView single = buf->lines[i].substr(buf->x(), 1);
                    g.font.immediate_draw(e.renderers[0], pos, single, g.theme.secondary);

                    g.font.immediate_draw(e.renderers[0], pos, buf->lines[i].substr(buf->x() + 1), g.theme.primary);
                }
            } else {
                g.font.immediate_draw(e.renderers[0], pos, buf->lines[i], g.theme.primary);
            }

            pos.x = x;
            pos.y -= g.font.size * g.line_spacing;
            if (pos.y < -global.font.size) {
                break;
            }
        }
    }

     // prompt
    if (!g.buffers[g.buffer_index].prompt.is_empty()) {
        buf = &g.buffers[g.buffer_index];

        const float pad = 10;
        Vector2 size(pad * 2, g.font.size * g.line_spacing + pad);
        g.font.measure(size, buf->prompt);
        g.font.measure(size, buf->line());

        Rect2 rect = layout.push_percent(ui::CENTER, 0, 0);
        rect.position -= size / 2;
        rect.size = size;

        Renderer2D::from(e.renderers[0])->set_scissor(rect);
        ClearScreen2DCmd{g.theme.muted}.immediate_draw(e.renderers[0]);

        Vector2 position = rect.position + Vector2(pad);
        g.font.immediate_draw(e.renderers[0], position, buf->prompt, g.theme.primary);
        g.font.immediate_draw(e.renderers[0], position, buf->line(), g.theme.primary);
    }
}

int main() {
    Jovial game;

    WindowProps props{
            .size  = {1280, 720},
            .title = "Lovial Editor",
            .bg    = Colors::GRUVBOX_GREY,
    };

    WindowManager::systems(game, props, 0);
    ViewportID vp = WM::get_main_window()->get_viewport_id();
    Renderer2D::attach_to(vp);
    PostProcessRenderer::attach_to(vp, true);
    freetype_systems(game, WM::get_main_window()->get_renderers()[0]);

    global.load_font();
    global.load_default_theme();
    global.find_game();
    global.open_file(".");

    WindowManager::get_main_window()->get_viewport()->push_system(global, draw);
    game.push_system(global, on_typed);
    game.push_system(global, on_pressed);
    game.push_system(global, update_mouse);

    game.run();
}
