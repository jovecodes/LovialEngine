#define JV_FREETYPE_SUPPORT

#ifdef BUNDLE_FONT
#include "Font.h"
#endif

#include "OS/Clipboard.h"
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
#include "Util/EasingFuncs.h"

using namespace jovial;

#define SDF_AA 0.6
#define BITMAP_AA 0.8
#define TAB_WIDTH 4
#define MAX_HISTORY 50
#define ERROR_DURATION 2.0

#define STRINGIFY(s) #s

enum VimMode {
    VimNormal = 1 << 0,
    VimInsert = 1 << 1,
    VimVisual = 1 << 2,
    VimVisualLine = 1 << 3,
};

const char *vim_mode_to_string(VimMode mode) {
    switch (mode) {
        case VimNormal:
            return "Normal";
        case VimInsert:
            return "Insert";
        case VimVisual:
            return "Visual";
        case VimVisualLine:
            return "Visual Line";
    }
    return "Unknown";
}

struct AlphabeticalSort {
    inline bool operator()(StrView l, StrView r) const {
        return l.alpha_cmpn(r);
    }
};

struct Edit {
    Vector2i position;
    String text;
    String deleted_text;

    void maybe_free() {
        if (!text.is_empty()) text.free();
        if (!deleted_text.is_empty()) deleted_text.free();
    }
};

void set_error_str(String string);
#define set_error(...) set_error_str(sprint(halloc, __VA_ARGS__))

struct Buffer {
    DArray<String> lines;
    String file;
    String search;

    Vector2i selection_start = Vector2i(-1, -1);
    bool select_lines = false;
    Vector2i position;
    int cam_offset = 0;

    Vector2i copied_flash_position;
    Vector2i copied_flash_start;
    StopWatch copied_flash{};

    Edit history[MAX_HISTORY];
    int current_history = 0;
    int undo_level = 0;
    bool broken_edit = true;

    enum Flags {
        READ_ONLY = 1 << 0,
        DIRECTORY = 1 << 1,
    };

    int flags = 0;
    StrView prompt;
    void (*on_selected)(Buffer &);

    inline int x() {
        return math::MIN(position.x, lines[position.y].size() - 1);
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

    void paste() {
        const char *contents = clipboard::get(WM::get_main_window_id());

        if (StrView(contents).find('\n') != -1) {
            position.x = line().size(); 
            insert('\n');

            Vector2i pos = position;
            for (; *contents; ++contents) insert(*contents);
            position = pos;
        } else {
            for (; *contents; ++contents) insert(*contents);
        }

        broken_edit = true;
    }

    void copy() {
        clipboard::set(WM::get_main_window_id(), selected_text().to_cstr(talloc));
        copied_flash.restart(0.15);
        copied_flash_position = position;
        copied_flash_start = selection_start;
    }

    String selected_text(Allocator alloc = talloc) {
        String res;

        Vector2i start = is_pos_less_or_equal(selection_start, position) ? selection_start : position;
        Vector2i end = is_pos_less_or_equal(selection_start, position) ? position : selection_start;

        if (select_lines) {
            start.x = 0;
            end.x = lines[end.y].size() - 1;
        }

        while (is_pos_less_or_equal(start, end)) {
            if (lines[start.y].is_empty()) {
                res.append(alloc, '\n');
                start.y += 1;
                start.x = 0;
                continue;
            }
            res.push(alloc, lines[start.y][start.x]);
            start.x += 1;
            if (start.x >= lines[start.y].size()) {
                res.append(alloc, '\n');
                start.y += 1;
                start.x = 0;
            }
        }

        return res;
    }

    Vector2i perform(Edit &edit) {
        Vector2i pos = edit.position;
        for (auto c: edit.text) {
            if (c == '\b') {
                edit.deleted_text.push(halloc, char_at({pos.x - 1, pos.y}));
                pos = remove_at({pos.x - 1, pos.y});
            } else if (c == 127) {
                edit.deleted_text.push(halloc, char_at(pos));
                pos = remove_at(pos);
            } else {
                set_error("%", c);
                pos = insert_at(pos, c);
            }
        }
        return pos;
    }

    Vector2i undo_edit(Edit edit) {
        Vector2i position = edit.position;
        int delete_index = edit.deleted_text.size() - 1;
        for (int i = 0; i < edit.text.size(); ++i) {
            if (edit.text[i] == '\b') {
                position = insert_at(position, edit.deleted_text[delete_index--]);
            } else if (edit.text[i] == 127) {
                position = insert_at({position.x + 1, position.y}, edit.deleted_text[delete_index++]);
            } else {
                if (edit.text[i] == '\n') {
                    position.y += 1;
                    position.x = -1;
                }
                position = remove_at(position);
            }
        }
        return position;
    }

    void undo() {
        if (undo_level >= MAX_HISTORY - 1) {
            set_error("already at oldest change (max history is 50)");
            return;
        }

        current_history -= 1;
        if (current_history < 0) current_history = MAX_HISTORY - 1;

        if (history[current_history].text.is_empty()) {
            current_history = (current_history + 1) % MAX_HISTORY;
            set_error("already at oldest change (max history is 50)");
            return;
        }

        undo_level += 1;
        position = undo_edit(history[current_history]);
    }

    void redo() {
        if (undo_level <= 0) {
            set_error("already at newest change");
            return;
        }
        if (history[current_history].text.is_empty()) return;

        undo_level -= 1;
        position = perform(history[current_history]);
        current_history = (current_history + 1) % MAX_HISTORY;
    }

    Vector2i edit(Edit edit) {
        Vector2i res = perform(edit);
        history[current_history].maybe_free();
        history[current_history] = edit;
        current_history = (current_history + 1) % MAX_HISTORY;
        return res;
    }

    void insert(char c) {
        if (broken_edit) {
            String s;
            s.push(halloc, c);
            position = edit({position, s});
        } else {
            int last = current_history - 1;
            if (last < 0) last = MAX_HISTORY - 1;
            history[last].text.push(halloc, c);
            position = insert_at(position, c);
        }
        broken_edit = false;
    }

    char char_at(Vector2i at) {
        if (at.x == -1 || at.x > lines[at.y].size()) return '\n';
        return lines[at.y][math::CLAMPED(at.x, 0, lines[at.y].size() - 1)];
    }

    void backspace() {
        if (broken_edit) {
            position = edit({position, String(halloc, "\b")});
        } else {
            int last = current_history - 1;
            if (last < 0) last = MAX_HISTORY - 1;
            if (history[last].text.size() >= 1 && history[last].text.back() != '\b') {
                history[last].text.pop();
            } else {
                history[last].deleted_text.push(halloc, char_at({position.x - 1, position.y}));
                history[last].text.push(halloc, '\b');
            }
            position = remove_at({position.x - 1, position.y});
        }
        broken_edit = false;
    }

    // void del() {
    //     char ch = 127;
    //     if (broken_edit) {
    //         position = edit({position, String(halloc, ch)});
    //         broken_edit = false;
    //     } else {
    //         int last = current_history - 1;
    //         if (last < 0) last = MAX_HISTORY - 1;
    //         history[last].text.push(halloc, ch);
    //         position = remove_at(position);
    //     }
    // }

    Vector2i remove_at(Vector2i at) {
        Vector2i old_position = position;
        position = at;

        // TODO: delete entire lines where possible
        if (selection_start != Vector2i(-1, -1)) {
            Vector2i start = is_pos_less_or_equal(selection_start, position) ? selection_start : position;
            Vector2i end = is_pos_less_or_equal(selection_start, position) ? position : selection_start;

            selection_start = Vector2i(-1, -1);

            while (is_pos_less_or_equal(start, end)) {
                end = remove_at(end);
                end.x -= 1;
            }
            // TODO: shouldn't this not be necessary
            remove_at(start);

            position = old_position;
            return start;
        }

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

    static bool is_pos_less_or_equal(Vector2i a, Vector2i b) {
        if (a.y < b.y) return true;
        if (a.y == b.y && a.x <= b.x) return true;
        return false;
    }

    static bool is_pos_between(Vector2i pos, Vector2i a, Vector2i b) {
        if (a == Vector2i(-1, -1) || b == Vector2i(-1, -1)) return false;

        Vector2i sel_start = is_pos_less_or_equal(a, b) ? a : b;
        Vector2i sel_end = is_pos_less_or_equal(a, b) ? b : a;

        return is_pos_less_or_equal(sel_start, pos) && is_pos_less_or_equal(pos, sel_end);
    }

    bool is_selected(Vector2i at) {
        Vector2i start = selection_start, end = position;

        if (is_pos_less_or_equal(position, selection_start)) {
            start = position;
            end = selection_start;
        }

        if (select_lines) {
            start.x = 0;
            end.x = lines[end.y].size();
        }

        return is_pos_between(at, start, end);
    }

    void move_y(int amount) {
        broken_edit = true;
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
        broken_edit = true;
        position.x += amount;
        math::CLAMP(position.y, 0, (int) lines.size() - 1);
        math::CLAMP(position.x, 0, (int) line().size());
    }

    void create_line(u64 at) {
        if (flags & READ_ONLY) return;
        lines.insert(halloc, at, String());
    }

    Vector2i append_at(Vector2i at, StrView text) {
        for (auto c: text) {
            at = insert_at(at, c);
        }
        return at;
    }

    Vector2i insert_at(Vector2i at, char c) {
        if (flags & READ_ONLY) {
            return at;                
        }
        Vector2i old_position = position;
        position = at;

        if (selection_start != Vector2i(-1, -1)) {
            position = remove_at(position);
        }

        if (!prompt.is_empty() && c == '\n') {
            on_selected(*this);
            position = old_position;
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
        if (flags & READ_ONLY) return;
        lines[line].free();
        lines.remove_at(line);
        if (lines.is_empty()) {
            lines.push(halloc, String());
        }
    }

    void delete_selection(Vector2i start, Vector2i end) {
        Vector2i old_selection = selection_start;
        selection_start = start;
        position = end;
        remove_at(end);
        selection_start = old_selection;
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
            if (lines.is_empty()) {
                lines.push(halloc, String());
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
        for (int i = 0; i < MAX_HISTORY; ++i) {
            history[i].maybe_free();
        }
        lines.free();
        file.free();
        search.free();
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

    StrView compile_command;
    Arena compile_command_arena;
    os::Proc game_proc;

    Bindings bindings = VIM_BINDINGS;
    VimMode vim_mode = VimInsert;
    Arena command_arena;
    String command;

    struct Error {
        StopWatch timer{};
        String text;
    };
    DArray<Error> errors;
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

        vim_mode = VimNormal;

        set_buffer(buffers.size() - 1);
    }

    void open_prompt(StrView prompt, void (*callback)(Buffer &)) {
        buffers.push({});
        buffers.back().prompt = prompt;
        buffers.back().on_selected = callback;
        buffers.back().lines.push(halloc, String());
        
        vim_mode = VimInsert;

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
            freetype_set_anti_aliasing_factor(SDF_AA);
        } else {
            font.load_buffer(FONT_DATA, FONT_DATA_LEN, size, FreeFont::BITMAP);
            freetype_set_anti_aliasing_factor(BITMAP_AA);
        }
#else
        const char *path = "./editor/jet_brains.ttf";
        JV_ASSERT(fs::file_exists(path, nullptr));
        if (sdf) {
            font.load(path, size, FreeFont::SDF);
            freetype_set_anti_aliasing_factor(SDF_AA);
        } else {
            font.load(path, size, FreeFont::BITMAP);
            freetype_set_anti_aliasing_factor(BITMAP_AA);
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

    void compile() {
        if (game_proc.is_valid()) game_proc.wait();
        os::Command command;
        command.append(compile_command);
        game_proc = command.run_async();
    }

    void find_game() {
        compile_command_arena.reset();
        String dir(talloc, ".");
        auto files = fs::read_dir(".", nullptr);
        for (const auto &i: files) {
            if (i == "LovialEngine.exe" || i == "LovialEngine" || i == "build.jov.sh" | i == "build.jov.bat") {
                StringBuilder sb;
#ifdef _WIN32
                sb.append(dir, "\\", i);
#else 
                sb.append(dir, "/", i);
#endif
                compile_command = sb.build(compile_command_arena);
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

        for (auto e: errors) e.text.free();
        errors.free();
    }
} global;

void set_error_str(String string) {
    global.errors.push(halloc, {StopWatch(ERROR_DURATION), string});
}

inline bool is_control_pressed() {
    return Input::is_pressed(Actions::LeftControl) || Input::is_pressed(Actions::RightControl);
}

inline bool is_shift_pressed() {
    return Input::is_pressed(Actions::LeftShift) || Input::is_pressed(Actions::RightShift);
}

struct VimMotion {
    int mode;
    const char *match;
    void (*action)(Buffer &buffer);
};

const VimMotion VIM_MOTIONS[] = {
    {VimNormal | VimVisual | VimVisualLine, "i", [](Buffer &buf) {global.vim_mode = VimInsert;}},
    {VimNormal | VimVisual | VimVisualLine, "h", [](Buffer &buf) { buf.move_x(-1); }},
    {VimNormal | VimVisual | VimVisualLine, "j", [](Buffer &buf) { buf.move_y(1); }},
    {VimNormal | VimVisual | VimVisualLine, "k", [](Buffer &buf) { buf.move_y(-1); }},
    {VimNormal | VimVisual | VimVisualLine, "l", [](Buffer &buf) { buf.move_x(1); }},
    {VimNormal | VimVisual | VimVisualLine, "gg", [](Buffer &buf) { buf.position = Vector2i(0, 0); buf.move_y(0); }},
    {VimNormal | VimVisual | VimVisualLine, "G", [](Buffer &buf) { buf.move_y(buf.lines.size()); }},
    {VimNormal | VimVisual | VimVisualLine, "o", [](Buffer &buf) { buf.position.x = buf.line().size(); buf.insert('\n'); global.vim_mode = VimInsert; }},
    {VimNormal | VimVisual | VimVisualLine, "O", [](Buffer &buf) { buf.move_y(1); buf.position.x = buf.line().size(); buf.insert('\n'); global.vim_mode = VimInsert; }},
    {VimNormal | VimVisual | VimVisualLine, "a", [](Buffer &buf) { buf.move_x(1); global.vim_mode = VimInsert; }},
    {VimNormal | VimVisual | VimVisualLine, "A", [](Buffer &buf) { buf.move_x(buf.line().size()); global.vim_mode = VimInsert; }},
    {VimNormal | VimVisual | VimVisualLine, "0", [](Buffer &buf) { buf.position.x = 0; }},
    {VimNormal | VimVisual | VimVisualLine, "$", [](Buffer &buf) { buf.position.x = buf.line().size() - 1; }},
    {VimNormal | VimVisual | VimVisualLine, "p", [](Buffer &buf) { buf.paste(); }},
    {VimNormal, "dd", [](Buffer &buf) { buf.remove_line(buf.position.y); }},
    {VimNormal, " v", [](Buffer &buf) { global.open_file(buf.file.get_base_dir()); }},
    {VimNormal, " r", [](Buffer &buf) { global.compile(); }},
    {VimNormal, "v", [](Buffer &buf) { buf.selection_start = buf.position; global.vim_mode = VimVisual; }},
    {VimNormal, "V", [](Buffer &buf) { buf.selection_start = buf.position; global.vim_mode = VimVisualLine; }},
    {VimNormal, "u", [](Buffer &buf) { buf.undo(); }},
    {VimVisual | VimVisualLine, "y", [](Buffer &buf) { buf.copy(); global.vim_mode = VimNormal; }},
    {VimVisual | VimVisualLine, "d", [](Buffer &buf) { buf.remove_at(buf.position); global.vim_mode = VimNormal; }},
};

void on_typed(Global &g, Events::KeyTyped &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;

    if (g.bindings == VIM_BINDINGS) {
        Vector2i delete_position(-1, -1);

        if (g.vim_mode == VimInsert) {
            buf->insert(event.character);
            // buf->position = buf->insert_at(buf->position, event.character);
        } else {
            g.command.push(g.command_arena, event.character);

            if (g.command.size() >= 2 && g.command[g.command.size() - 2] == 'd') {
                delete_position = buf->position;
                g.command.remove_at(g.command.size() - 2);
            } 
            if (g.command.size() >= 2 && g.command[g.command.size() - 2] == 'f') {
                int pos = buf->line().find_char(g.command.back(), buf->position.x + 1);
                if (pos != -1) {
                    buf->position.x = pos;
                }
                g.command.pop();
                g.command.pop();
            } 
            if (g.command.size() >= 2 && g.command[g.command.size() - 2] == 't') {
                int pos = buf->line().find_char(g.command.back(), buf->position.x + 1);
                if (pos != -1) {
                    buf->position.x = pos - 1;
                }
                g.command.pop();
                g.command.pop();
            }
        }

        for (int i = 0; i < JV_ARRAY_LEN(VIM_MOTIONS); ++i) {
            if (g.vim_mode & VIM_MOTIONS[i].mode && g.command.view().ends_with(VIM_MOTIONS[i].match)) {
                g.flush_command();
                VIM_MOTIONS[i].action(*buf);
            }
        }

        if (delete_position != Vector2i(-1, -1)) {
            if (buf->position.y != delete_position.y) {
                buf->select_lines = true;
            }
            buf->delete_selection(delete_position, buf->position);
        }

    } else {
        buf->position = buf->insert_at(buf->position, event.character);
    }
}

void on_open_file(Buffer &buffer) {
    global.open_file(buffer.line());
    global.close_last_buffer();
}

void set_compile_command(Buffer &buffer) {
    global.compile_command_arena.reset();
    global.compile_command = String(global.compile_command_arena, buffer.line());
    global.close_current_buffer();
}

void on_pressed(Global &g, Events::KeyPressed &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;

    if (g.bindings == VIM_BINDINGS) {
        buf->select_lines = g.vim_mode == VimVisualLine;
        if (!(g.vim_mode & (VimVisual | VimVisualLine))) {
            buf->selection_start = Vector2i(-1, -1);
        }
    }

    // Handle text editing actions
    if ((g.vim_mode == VimInsert || g.bindings == MOUSE_BINDINGS)) {
        switch (event.keycode) {
            case Actions::Backspace: {
                if (buf->position.x >= TAB_WIDTH) {
                    bool tab = true;
                    for (int i = 0; i < TAB_WIDTH; ++i) {
                        if (buf->line()[buf->x() - i] != ' ') tab = false;
                    }
                    if (tab) {
                        for (int i = 0; i < TAB_WIDTH - 1; ++i) {
                            buf->backspace();
                        }
                    }
                }
                
                buf->backspace();
             } break;
            // case Actions::Delete:
            //     buf->del();
            //     break;
            case Actions::Enter:
                buf->insert('\n');
                return;
            case Actions::Tab:
                for (int i = 0; i < TAB_WIDTH; ++i) {
                    buf->position = buf->insert_at(buf->position, ' ');
                }
                return;
            default: break;
        }
    }

    // Handle directory-related actions
    if (buf->flags & Buffer::DIRECTORY) {
        if (event.keycode == Actions::Enter || Input::just_double_clicked()) {
#ifdef _WIN32
            g.open_file(tprint("%\\%", buf->file, buf->line()));
#else
            g.open_file(tprint("%/%", buf->file, buf->line()));
#endif
            g.close_last_buffer();
            return;
        }
        if (event.keycode == Actions::Minus) {
#ifdef _WIN32
            g.open_file(tprint("%\\..", buf->file));
#else
            g.open_file(tprint("%/..", buf->file));
#endif
            return;
        }
    }

    // Handle prompt-related actions
    if (!buf->prompt.is_empty()) {
        if (event.keycode == Actions::Escape) {
            g.close_current_buffer();
            g.vim_mode = VimNormal;
            return;
        }
    }

    if (is_control_pressed() && is_shift_pressed() && event.keycode == Actions::C) {
        g.open_prompt("Compile Command: ", set_compile_command);
        return;
    }

    // Handle control key actions
    if (is_control_pressed()) {
        switch (event.keycode) {
            case Actions::S:
                buf->save();
                break;
            case Actions::Z:
                buf->undo();
                break;
            case Actions::R:
            case Actions::Y:
                buf->redo();
                break;
            case Actions::C:
                buf->copy();
                break;
            case Actions::V:
                buf->paste();
                break;
            case Actions::Space:
                g.compile();
                break;
            case Actions::Equal:
                g.load_font(g.font.size + 2);
                break;
            case Actions::Minus:
                g.load_font(g.font.size - 2);
                break;
            case Actions::Num0:
                g.load_font();
                break;
            case Actions::U:
                buf->move_y(-20);
                break;
            case Actions::D:
                buf->move_y(20);
                break;
            case Actions::K: {
                StrView extension = buf->file.get_extension();
#define DEFINE_COMMENT(buf, str) \
                do { \
                    int at = buf->line().find(str); \
                    if (at != -1) { \
                        for (int i = 0; i < sizeof(str) - 1; ++i) { \
                            buf->line().remove_at(at); \
                        } \
                    } else { \
                        buf->append_at({0, buf->position.y}, str); \
                        buf->position.x += sizeof(str) - 1; \
                    } \
                } while (0)

                if (extension == "lua") {
                    DEFINE_COMMENT(buf, "--");
                } else if (extension == "c" || extension == "cpp" || extension == "rs") {
                    DEFINE_COMMENT(buf, "//");
                } else if (extension == "py") {
                    DEFINE_COMMENT(buf, "#");
                }
            } break;
            case Actions::O:
                if (is_shift_pressed()) {
                    g.open_file(buf->file.get_base_dir());
                } else {
                    g.open_prompt("Path: ", on_open_file);
                }
                return;
            case Actions::Num6: 
                g.set_buffer(g.last_buffer_index);
                return;
            default: break;
        }
    }

    // Handle Vim-specific actions
    if (g.bindings == VIM_BINDINGS && g.vim_mode != VimNormal && event.keycode == Actions::Escape) {
        buf->selection_start = Vector2i(-1, -1);
        g.vim_mode = VimNormal;
        buf->move_x(-1);
    }

    // Handle arrow keys for movement
    switch (event.keycode) {
        case Actions::Right:
            buf->move_x(1);
            break;
        case Actions::Left:
            buf->move_x(-1);
            break;
        case Actions::Up:
            buf->move_y(-1);
            break;
        case Actions::Down:
            buf->move_y(1);
            break;
        default: break;
    }

    // Handle function keys
    switch (event.keycode) {
        case Actions::F1:
            g.load_font(g.font.size, !g.using_sdf);
            break;
        case Actions::F2:
            g.bindings = (g.bindings == VIM_BINDINGS) ? MOUSE_BINDINGS : VIM_BINDINGS;
            break;
        default: break;
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

        if (Input::is_just_pressed(Actions::LeftMouseButton)) {
            buf->selection_start = buf->position;
        }
    }

    if (Input::get_scroll() != 0) {
        buf->cam_offset += Input::get_scroll() * 2;
        buf->move_y(0);
    }
}

void draw(Global &g, Events::Draw &e) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;
    if (!g.buffers[g.buffer_index].prompt.is_empty()) {
        buf = &g.buffers[g.last_buffer_index];
    }

    ui::Layout layout(talloc, {{0, 0}, WM::get_main_window()->get_size()});
    const float line_percent = 1 - (g.font.size * g.line_spacing * 2) / WM::get_main_window()->get_height();

    { // bottom status line
        Rect2 rect = layout.push_percent(ui::BOTTOM, line_percent, 1 - line_percent);
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        StrView msg = tprint("%:%:%", buf->file, buf->position.y + 1, buf->position.x + 1);
        ui::label(e.renderers[0], g.font, rect, msg, g.theme.primary, ui::RIGHT);
        ui::label(e.renderers[0], g.font, rect, g.compile_command.is_empty() ? "no compile command" : g.compile_command, g.theme.muted, ui::CENTER);
        
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


        { // selection 
            // TODO: this seems painfully slow. 
            buf->copied_flash.tick_down();
            if (buf->selection_start != Vector2i(-1, -1) || !buf->copied_flash.is_finished()) {
                Color selection_color = g.theme.muted;
                if (!buf->copied_flash.is_finished()) {
                    selection_color = *g.theme.named_colors.get("theme_orange");
                }

                Vector2 selection_pos = pos;
                for (int y = buf->cam_offset; y < buf->lines.size(); ++y) {
                    Rect2DCmd cmd{selection_pos, {g.font.metrics[0].advance.x, g.font.size * g.line_spacing}, selection_color};
                    cmd.position.y -= g.font.size * (g.line_spacing - 1);

                    for (int x = 0; x <= buf->lines[y].size(); ++x) {
                        if (buf->is_selected({x, y}) || Buffer::is_pos_between({x, y}, buf->copied_flash_position, buf->copied_flash_start)) {
                            cmd.immediate_draw(e.renderers[0]);
                        }
                        cmd.position.x += cmd.size.x;
                    }

                    selection_pos.y -= g.font.size * g.line_spacing;
                    if (selection_pos.y < -global.font.size) {
                        break;
                    }
                }
            } else {
                buf->copied_flash_position = Vector2i(-1, -1);
            }
        }

        for (u32 i = buf->cam_offset; i < buf->lines.size(); ++i) {
            Vector2 line_pos(rect.position.x, pos.y);
            if (buf->position.y == i) {
                g.font.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, i + 1), *g.theme.named_colors.get("theme_purple"));
            } else {
                g.font.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, math::abs((int) buf->position.y - (int) i)), g.theme.muted);
            }

            if (buf->position.y == i) {
                bool has_cursor = buf->position.x < buf->line().size();
                
                if (has_cursor && buf->x() != 0) {
                    g.font.immediate_draw(e.renderers[0], pos, buf->lines[i].substr(0, buf->x()), g.theme.primary);
                }
                
                Rect2DCmd cmd{pos, {g.font.metrics[0].advance.x, g.font.size * g.line_spacing}, g.theme.primary};
                cmd.position.y -= g.font.size * (g.line_spacing - 1);

                if (has_cursor) {
                    cmd.immediate_draw(e.renderers[0]);

                    StrView single = buf->lines[i].substr(buf->x(), 1);
                    g.font.immediate_draw(e.renderers[0], pos, single, g.theme.secondary);
                    
                    g.font.immediate_draw(e.renderers[0], pos, buf->lines[i].substr(buf->x() + 1), g.theme.primary);
                } else {
                    g.font.immediate_draw(e.renderers[0], pos, buf->lines[i], g.theme.primary);

                    cmd.position.x = pos.x;
                    cmd.immediate_draw(e.renderers[0]);
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

    const float pad = 10;

    { // top status line
        Rect2 rect = layout.push_percent(ui::TOP, line_percent, 1 - line_percent);
        rect.position.y -= pad;
        rect.position.x += pad;
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        for (int i = g.errors.size() - 1; i >= 0; --i) {
            auto &it = g.errors[i];
            if (it.timer.tick_down()) {
                it.text.free();
                g.errors.remove_at(i);
                continue;
            }

            Vector2 size(pad * 2, g.font.size * g.line_spacing + pad);
            g.font.measure(size, it.text);

            Vector2 position = rect.top_right() - size;

            const float animation_len = 0.5;
            if (it.timer.time < animation_len) {
                float t = 1 - it.timer.time * 1.0f/animation_len;
                position.x = math::lerp(position.x, (float) WM::get_main_window()->get_width(), (float) easers::in(t));
            }

            rect = {position, size};

            Renderer2D::from(e.renderers[0])->set_scissor(rect);
            ClearScreen2DCmd{g.theme.muted}.immediate_draw(e.renderers[0]);
            ui::label(e.renderers[0], g.font, rect, it.text, *g.theme.named_colors.get("theme_red"), ui::CENTER);

            rect.position.y -= rect.size.y + pad;
        }

        layout.pop();
    }

     // prompt
    if (!g.buffers[g.buffer_index].prompt.is_empty()) {
        buf = &g.buffers[g.buffer_index];

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

int main(int argc, char* argv[]) {
    Jovial game;

    WindowProps props{
            .size  = {1280, 720},
            .title = "Lovial Editor",
            .bg    = Colors::GRUVBOX_GREY,
    };

    Time::systems(game);
    WindowManager::systems(game, props, 0);
    ViewportID vp = WM::get_main_window()->get_viewport_id();
    Renderer2D::attach_to(vp);
    PostProcessRenderer::attach_to(vp, true);
    freetype_systems(game, WM::get_main_window()->get_renderers()[0]);

    global.load_font();
    global.load_default_theme();
    global.find_game();

    // Use the first argument if provided, otherwise use "."
    const char* file_to_open = (argc > 1) ? argv[1] : ".";
    global.open_file(file_to_open);

    WindowManager::get_main_window()->get_viewport()->push_system(global, draw);
    game.push_system(global, on_typed);
    game.push_system(global, on_pressed);
    game.push_system(global, update_mouse);

    game.run();
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
