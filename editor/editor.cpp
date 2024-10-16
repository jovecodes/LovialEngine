#include "Templates/AssocArray.h"
#define JV_FREETYPE_SUPPORT

#ifdef BUNDLE_FONT
#include "RegularFontData.h"
#include "BoldFontData.h"
#include "ItalicFontData.h"
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
#include "IO/StringBuilder.h"

using namespace jovial;

#define SDF_AA 0.6
#define BITMAP_AA 0.8
#define TAB_WIDTH 4
#define MAX_HISTORY 50
#define ERROR_DURATION 2.0
#define SCROLL_OFF 7
#define PATH_MAX 256

#define STRINGIFY(s) #s

#ifdef _WIN32
#define PATH_SEP "\\"
#define PATH_SEP_CHAR '\\'
#else 
#define PATH_SEP "/"
#define PATH_SEP_CHAR '/'
#endif

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
    Vector2i start_position;
    Vector2i position;
    String text;
    String deleted_text;

    void free() {
        text.free();
        deleted_text.free();
    }
};

void push_error_str(String string);
#define push_error(...) push_error_str(auto_sprintf(__VA_ARGS__))

String lines_to_string(Allocator alloc, const DArray<String> &lines) {
    StringBuilder sb;
    for (const auto &line: lines) {
        sb.append(line);
        sb.append("\n");
    }
    return sb.build(alloc);
}

struct Token {
    u64 line;
    u64 end_line;
    u32 start;
    u32 end;

    enum Type {
        NORMAL,
        KEYWORD,
        COMMENT,
        STRING,
        PUNCT,
        NUMBER,
    } type;
};

static const StrView CPP_KEYWORDS[] = {
    CSV("struct"),
    CSV("enum"),
    CSV("class"),
    CSV("return"),

    CSV("if"),
    CSV("else"),
    CSV("for"),
    CSV("while"),
    CSV("switch"),
    CSV("case"),
    CSV("goto"),
    CSV("do"),

    CSV("using"),
    CSV("namespace"),

    CSV("#define"),
    CSV("#undef"),
    CSV("#include"),
    CSV("#if"),
    CSV("#else"),
    CSV("#endif"),
    CSV("#ifdef"),
    CSV("#ifndef"),

    CSV("const"),
    CSV("static"),
    CSV("inline"),
    CSV("extern"),
    CSV("constexpr"),

    CSV("auto"),
    CSV("void"),
    CSV("int"),
    CSV("float"),
    CSV("long"),
    CSV("double"),
    CSV("bool"),
    CSV("char"),
};

static const StrView LUA_KEYWORDS[] = {
    CSV("and"),
    CSV("break"),
    CSV("do"),
    CSV("else"),
    CSV("elseif"),
    CSV("end"),
    CSV("false"),
    CSV("for"),
    CSV("function"),
    CSV("if"),
    CSV("in"),
    CSV("local"),
    CSV("nil"),
    CSV("not"),
    CSV("or"),
    CSV("repeat"),
    CSV("return"),
    CSV("then"),
    CSV("true"),
    CSV("until"),
    CSV("while"),
};

struct Tokenizer {
    std::atomic<bool> done;
    std::thread thread;
    bool already_done = false;

    DArray<Token> tokens;
    String file;

    enum Type {
        CPP,
        LUA,
    } type;

    Tokenizer() {
        done.store(true);
    }

    bool _handle_space(u64 &i, u64 &line, u32 &line_offset) {
        if (isspace(file[i])) {
            if (file[i] == '\n') {
                line_offset = 0;
                line++;
            } else {
                ++line_offset;
            }
            return true;
        }
        return false;
    }

    bool _handle_single_line_comment(u64 &i, u64 &line, u32 &line_offset, StrView code) {
        if (i < file.size() - (code.size() - 1) && file.substr(i, code.size()) == code) {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;

            while (i < file.size() && file[i] != '\n') {
                i++, line_offset++;
            }

            tokens.push(halloc, {line, line, start, line_offset, Token::COMMENT});
            --i, --line_offset;
            return true;
        }
        return false;
    }

    bool _handle_multi_line_comment(u64 &i, u64 &line, u32 &line_offset, StrView begin, StrView end) {
        if (i < file.size() - (begin.size() - 1) && file.substr(i, begin.size()) == begin) {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            u64 start_line = line;

            while (i < file.size() - (end.size() - 1) && file.substr(i, end.size()) != end) {
                if (file[i] == '\n') {
                    line_offset = 0;
                    line++;
                    i++;
                } else {
                    line_offset++;
                    i++;
                }
            }

            tokens.push(halloc, {start_line, line, start, line_offset + 2, Token::COMMENT});
            i--;
            return true;
        }
        return false;
    }

    bool _handle_string(u64 &i, u64 &line, u32 &line_offset) {
        if (file[i] == '\'') {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            u64 start_line = line;
            i += 1, line_offset += 1;

            bool escape = false;
            while (i < file.size()) {
                if (file[i] == '\n') {
                    line_offset = 0;
                    line++;
                } else {
                    line_offset++;
                }

                // Handle escaped characters
                if (escape) {
                    escape = false;  // Reset escape flag after processing the escape
                } else if (file[i] == '\\') {
                    escape = true;   // Set escape flag when encountering backslash
                } else if (file[i] == '\'') {
                    break;  // End of string
                }

                i++;  // Move to the next character
            }
            
            tokens.push(halloc, {start_line, line, start, line_offset, Token::STRING});
            return true;
        } else if (file[i] == '"') {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            u64 start_line = line;
            i += 1, line_offset += 1;

            bool escape = false;
            while (i < file.size()) {
                if (file[i] == '\n') {
                    line_offset = 0;
                    line++;
                } else {
                    line_offset++;
                }

                // Handle escaped characters
                if (escape) {
                    escape = false;  // Reset escape flag after processing the escape
                } else if (file[i] == '\\') {
                    escape = true;   // Set escape flag when encountering backslash
                } else if (file[i] == '"') {
                    break;  // End of string
                }

                i++;  // Move to the next character
            }
            
            tokens.push(halloc, {start_line, line, start, line_offset, Token::STRING});
            return true;
        }
        return false;
    }

    bool _handle_keywords(u64 &i, u64 &line, u32 &line_offset, View<StrView> keywords) {
        if (isalpha(file[i]) || file[i] == '_' || file[i] == '#') {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            i += 1, line_offset += 1;

            while (i < file.size() && (isalnum(file[i]) || file[i] == '_' || file[i] == '#')) {
                i++, line_offset++;
            }

            StrView view = StrView(pointer, line_offset - start);
            for (int j = 0; j < keywords.size(); ++j) {
                if (keywords[j] == view) {
                    tokens.push(halloc, {line, line, start, line_offset, Token::KEYWORD});
                }
            }

            --i;
            return true;
        }
        return false;
    }

    bool _handle_numbers(u64 &i, u64 &line, u32 &line_offset, StrView extras) {
        if (isdigit(file[i])) {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            i += 1, line_offset += 1;

            while (i < file.size() && (isdigit(file[i]) || file[i] == '.' || extras.find_char(file[i]) != -1)) {
                i++, line_offset++;
            }

            tokens.push(halloc, {line, line, start, line_offset, Token::NUMBER});

            --i;
            return true;
        }
        return false;
    }

    bool _handle_punct(u64 &i, u64 &line, u32 &line_offset) {
        if (ispunct(file[i]) && file[i] != '"') {
            char *pointer = file.ptr() + i;
            u32 start = line_offset;
            i += 1, line_offset += 1;

            while (i < file.size() && ispunct(file[i]) && file[i] != '"' && file[i] != '\'') {
                i++, line_offset++;
            }

            tokens.push(halloc, {line, line, start, line_offset, Token::PUNCT});

            --i;
            return true;
        }
        return false;
    }


    void _lua_tokenize() {
        u64 line = 0;
        u32 line_offset = 0;
        for (u64 i = 0; i < file.size(); ++i) {
            if (_handle_space(i, line, line_offset)) continue;
            if (_handle_single_line_comment(i, line, line_offset, "--")) continue;
            if (_handle_multi_line_comment(i, line, line_offset, "--[[", "]]--")) continue;
            if (_handle_string(i, line, line_offset)) continue;
            if (_handle_numbers(i, line, line_offset, "")) continue;
            if (_handle_keywords(i, line, line_offset, LUA_KEYWORDS)) continue;
            if (_handle_punct(i, line, line_offset)) continue;
            ++line_offset;
        }
    }

    void _cpp_tokenize() {
        u64 line = 0;
        u32 line_offset = 0;
        for (u64 i = 0; i < file.size(); ++i) {
            if (_handle_space(i, line, line_offset)) continue;
            if (_handle_single_line_comment(i, line, line_offset, "//")) continue;
            if (_handle_multi_line_comment(i, line, line_offset, "/*", "*/")) continue;
            if (_handle_string(i, line, line_offset)) continue;
            if (_handle_numbers(i, line, line_offset, "fe")) continue;
            if (_handle_keywords(i, line, line_offset, CPP_KEYWORDS)) continue;
            if (_handle_punct(i, line, line_offset)) continue;
            ++line_offset;
        }
    }

    static void _tokenize(Tokenizer *tokenizer) {
        switch (tokenizer->type) {
            case CPP:
                tokenizer->_cpp_tokenize();
                break;
            case LUA:
                tokenizer->_lua_tokenize();
                break;
        }
        tokenizer->done.store(true);
    }

    void tokenize(const DArray<String> &lines, StrView path) {
        StrView extension = path.get_extension();
        if (extension == "c" || extension == "cpp") {
            type = CPP;
        } else if (extension == "lua") {
            type = LUA;
        } else {
            return;
        }

        tokens.free();
        file.free();
        file = lines_to_string(halloc, lines);

        already_done = false;
        done.store(false);
        thread = std::thread(_tokenize, this);
    }
};

struct Buffer {
    DArray<String> lines;
    DArray<Token> tokens;
    Tokenizer *tokenizer;

    String file;

    String search;
    DArray<Vector2i> search_positions;

    Vector2i selection_start = Vector2i(-1, -1);
    bool select_lines = false;
    Vector2i position;
    int cam_offset = 0;

    Vector2i copied_flash_position;
    Vector2i copied_flash_start;
    StopWatch copied_flash{};

    DArray<Edit> history;
    int undo_level = 0;

    bool broken_edit = true;

    enum Flags {
        READ_ONLY         = 1 << 0,
        DIRECTORY         = 1 << 1,
        NEEDS_RETOKENIZE  = 1 << 2,
        MODIFIED          = 1 << 3,
        UNSAVED           = 1 << 4,
    };

    int flags = 0;
    StrView prompt;
    void (*on_selected)(Buffer &);
    void (*on_selected_key_pressed)(Buffer &, Events::KeyTyped &);

    inline int x() {
        return math::MIN(position.x, (int) line().size());
    }

    inline String &line() {
        return lines[position.y];
    }

    void save() {
        fs::write_file(file, lines_to_string(talloc, lines));
        flags &= ~UNSAVED;
    }

    void paste() {
        const char *contents = clipboard::get(WM::get_main_window_id());

        if (StrView(contents).find_char('\n') != -1) {
            position.x = line().size(); 
            insert('\n');

            Vector2i pos = position;

            if (*contents == '\n') contents++;
            for (; *contents; ++contents) insert(*contents);
            position = pos;
        } else {
            for (; *contents; ++contents) insert(*contents);
        }

        broken_edit = true;
    }

    int get_indent_at(int line) {
        View<StrView> opens, closes;
        StrView extension = file.get_extension();
        if (extension == "c" || extension == "cpp" || extension == "rs") {
            static const StrView OPENS[] = {
                CSV("{"),
                CSV("("),
            };
            static const StrView CLOSES[] = {
                CSV("}"),
                CSV(")"),
            };

            opens = OPENS;
            closes = CLOSES;
        } else if (extension == "lua") {
            static const StrView OPENS[] = {
                CSV("function"),
                CSV("if"),
                CSV("do"),
            };
            static const StrView CLOSES[] = {
                CSV("end"),
            };
            opens = OPENS;
            closes = CLOSES;
        }

        int indent = 0;
        for (u64 i = 0; i <= line; ++i) {
            for (u64 j = 0; j < opens.size(); ++j) {
                int offset = 0;
                while (true) {
                    offset = lines[i].find(opens[j], offset);
                    if (offset == -1) {
                        break;
                    } else {
                        offset += 1;
                        indent += 1;
                        if (offset >= lines[i].size()) break;
                    }
                }
            }
            for (u64 j = 0; j < closes.size(); ++j) {
                int offset = 0;
                while (true) {
                    offset = lines[i].find(closes[j], offset);
                    if (offset == -1) {
                        break;
                    } else {
                        offset = math::MAX(offset, offset + 1);
                        indent = math::MAX(indent - 1, 0);
                        if (offset >= lines[i].size()) break;
                    }
                }
            }
        }
        return indent;
    }

    void copy(bool flash = true) {
        clipboard::set(WM::get_main_window_id(), selected_text().to_cstr(talloc));
        if (flash) {
            copied_flash.restart(0.15);

            Vector2i start = is_pos_less_or_equal(selection_start, position) ? selection_start : position;
            Vector2i end = is_pos_less_or_equal(selection_start, position) ? position : selection_start;
            copied_flash_position = start;
            copied_flash_start = end;
        }

        selection_start = Vector2i(-1, -1);
    }

    String selected_text(Allocator alloc = talloc) {
        String res;

        Vector2i start = is_pos_less_or_equal(selection_start, position) ? selection_start : position;
        Vector2i end = is_pos_less_or_equal(selection_start, position) ? position : selection_start;

        if (select_lines) {
            start.x = 0;
            end.x = lines[end.y].size() - 1;
            res.push(alloc, '\n');
        }

        while (is_pos_less_or_equal(start, end)) {
            if (lines[start.y].is_empty()) {
                res.push(alloc, '\n');
                start.y += 1;
                start.x = 0;
                continue;
            }
            res.push(alloc, lines[start.y][start.x]);
            start.x += 1;
            if (start.x >= lines[start.y].size()) {
                res.push(alloc, '\n');
                start.y += 1;
                start.x = 0;
            }
        }

        if (select_lines) {
            res.pop();
        }

        return res;
    }

    void perform(Edit &edit) {
        if (flags & READ_ONLY) return;
        position = edit.start_position;

        for (auto c: edit.text) {
            if (c == '\b') {
                edit.deleted_text.push(halloc, char_at({x() - 1, position.y}));
                position = remove_at({x() - 1, position.y});
            } else if (c == 127) {
                edit.deleted_text.push(halloc, char_at({x(), position.y}));
                position = remove_at({x(), position.y});
            } else {
                insert_char(c);
            }
        }
    }

    void undo_edit(Edit edit) {
        if (flags & READ_ONLY) return;

        position = edit.position;
        if (!edit.text.is_empty() && edit.text[0] != '\b') {
            position.x -= 1;
        }

        int delete_index = edit.deleted_text.size() - 1;

        for (int i = 0; i < edit.text.size(); ++i) {
            if (edit.text[i] == '\b') {
                insert_char(edit.deleted_text[delete_index--]);
            } else if (edit.text[i] == 127) {
                insert_char(edit.deleted_text[delete_index--]);
                move_x(-1);
            } else {
                if (x() == 0) position.x = -1;
                position = remove_at({x(), position.y});
            }
        }
    }

    void undo() {
        if (flags & READ_ONLY) return;
        if (undo_level >= history.size()) {
            push_error("already at oldest change");
            return;
        }

        undo_level += 1;
        undo_edit(history[history.size() - undo_level]);
    }

    void redo() {
        if (flags & READ_ONLY) return;
        if (undo_level <= 0) {
            push_error("already at newest change");
            return;
        }

        undo_level -= 1;
        perform(history[history.size() - 1 - undo_level]);
    }

    void edit(String s) {
        Edit edit{{x(), position.y}, {x(), position.y}, s};
        perform(edit);
        edit.position = position;

        for (int i = 0; i < undo_level; ++i) {
            history.back().free();
            history.pop();
        }
        undo_level = 0;

        history.push(halloc, edit);
    }

    void insert(char c) {
        if (flags & READ_ONLY) return;
        if (!prompt.is_empty() && c == '\n') {
            on_selected(*this);
            return;
        }

        if (broken_edit) {
            String s;
            s.push(halloc, c);
            edit(s);
        } else {
            history.back().text.push(halloc, c);
            insert_char(c);
            history.back().position = position;
        }
        broken_edit = false;
    }

    void insert_with_indent(char c, int extra_indent = 0) {
        insert(c);

        if (c == '\n') {
            int indent = math::MAX(get_indent_at(position.y) + extra_indent, 0);
            for (int i = 0; i < indent; ++i) {
                for (int j = 0; j < TAB_WIDTH; ++j) {
                    insert(' ');
                }
            }
        }
    }

    void user_insert(char c) {
        if (c == '{' && x() >= line().size()) {
            insert(c);
            Vector2i pos = position;
            insert('}');
            broken_edit = true;
            position = pos;
        } else if (c == '\n' && x() > 0 && x() < line().size() && line()[x()] == '}') {
            insert_with_indent(c, 1);
            Vector2i pos = position;

            insert_with_indent(c);
            broken_edit = true;
            position = pos;
        } else {
            insert_with_indent(c);
        }

    }

    char char_at(Vector2i at) {
        if (at.x == -1 || at.x > lines[at.y].size()) return '\n';
        return lines[at.y][math::CLAMPED(at.x, 0, lines[at.y].size() - 1)];
    }

    void select_line() {
        selection_start = position;
        select_lines = true;
    }

    void _backspace() {
        if (flags & READ_ONLY) return;
        if (selection_start != Vector2i(-1, -1)) {
            Vector2i start = is_pos_less_or_equal(selection_start, position) ? selection_start : position;
            Vector2i end = is_pos_less_or_equal(selection_start, position) ? position : selection_start;

            selection_start = Vector2i(-1, -1);

            position = end;

            if (select_lines) {
                position.x = line().size();
                start.x = 0;
            }

            move_x(1);
            while (is_pos_less(start, position)) {
                _backspace();
            }

            if (select_lines) {
                if (lines.size() != 1 && position.y == 0) {
                    position.y += 1;
                    position.x = -1;
                }
                _backspace();
            }
        } else if (broken_edit) {
            edit(String(halloc, "\b"));
        } else {
            if (history.back().text.size() >= 1 && history.back().text.back() != '\b' && history.back().text.back() != 127) {
                history.back().text.pop();
            } else {
                history.back().deleted_text.push(halloc, char_at({x() - 1, position.y}));
                history.back().text.push(halloc, '\b');
            }
            position = remove_at({x() - 1, position.y});
            history.back().position = position;
        }
        broken_edit = false;
    }

    void backspace() {
        int _x = x();
        math::CLAMP(_x, 0, (int) line().size() - 1);
        if (_x >= TAB_WIDTH - 1) {
            bool tab = true;
            for (int i = 1; i < TAB_WIDTH; ++i) {
                if (line()[_x - i] != ' ') tab = false;
            }
            if (tab) {
                for (int i = 0; i < TAB_WIDTH - 1; ++i) {
                    _backspace();
                }
            }
        }
        _backspace();
    }

    void del() {
        JV_LOG_ENGINE(LOG_WARNING, "TODO: the undo on delete is broken.");

        if (position.x >= line().size()) {
            backspace();
            return;
        }

        // if (selection_start != Vector2i(-1, -1)) {
        //     backspace();
        // } else if (broken_edit) {
        //     edit(String(halloc, char(127)));
        // } else {
        //     int last = current_history - 1;
        //     if (last < 0) last = MAX_HISTORY - 1;
        //     history[last].deleted_text.push(halloc, char_at({x(), position.y}));
        //     history[last].text.push(halloc, 127);
        //     position = remove_at({x(), position.y});
        //     history[last].position = position;
        //     // history[last].position = position;
        // }
        // broken_edit = false;
    }

    Vector2i remove_at(Vector2i at) {
        Vector2i old_position = position;
        position = at;

        flags |= NEEDS_RETOKENIZE;
        flags |= MODIFIED;
        flags |= UNSAVED;

        if (x() < 0 || line().size() == 0) {
            if (position.y > 0) {
                move_y(-1);
                position.x = line().size();
                line().append(halloc, lines[position.y + 1]);

                remove_line(position.y + 1);
            }
        } else if (x() >= line().size()) {
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

    static bool is_pos_less(Vector2i a, Vector2i b) {
        if (a.y < b.y) return true;
        if (a.y == b.y && a.x < b.x) return true;
        return false;
    }

    static bool is_pos_between(Vector2i pos, Vector2i a, Vector2i b) {
        if (a == Vector2i(-1, -1) || b == Vector2i(-1, -1)) return false;

        Vector2i sel_start = is_pos_less_or_equal(a, b) ? a : b;
        Vector2i sel_end = is_pos_less_or_equal(a, b) ? b : a;

        return is_pos_less_or_equal(sel_start, pos) && is_pos_less_or_equal(pos, sel_end);
    }

    bool is_selected(Vector2i at) {
        if (selection_start == Vector2i(-1, -1)) {
            return false;
        }

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
    
    bool is_flash_selected(Vector2i at) {
        if (copied_flash_position == Vector2i(-1, -1) || copied_flash_start == Vector2i(-1, -1)) return false;
        Vector2i start = copied_flash_start, end = copied_flash_position;

        if (is_pos_less_or_equal(copied_flash_position, copied_flash_start)) {
            start = copied_flash_position;
            end = copied_flash_start;
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

        // // TODO: make these numbers based on window height
        // if (position.y - cam_offset > 20) {
        //     cam_offset += amount;
        // }
        // if (position.y - cam_offset < 5) {
        //     cam_offset -= math::abs(amount);
        // }
        // cam_offset = math::CLAMPED(cam_offset, 0, (int) lines.size() - 1);

        math::CLAMP(position.y, 0, (int) lines.size() - 1);
    }

    // returns if the move was 'successful'
    bool move_x_wrap(int amount) {
        if (amount >= 0) {
            move_x(amount);
            if (x() >= line().size()) {
                if (position.y != lines.size() - 1) {
                    position.x = 0;
                    move_y(1);
                } else {
                    position.x = line().size() - 1;
                    return false;
                }
            }
        } else {
            println("amount: %, x: %", amount, x());
            if (x() <= -amount) {
                if (position.y != 0) {
                    move_y(-1);
                    position.x = line().size() - 1;
                } else {
                    position.x = 0;
                    return false;
                }
            } else {
                move_x(amount);
            }
        }
        return true;
    }

    bool is_current_char(bool (*check)(char)) {
        if (lines.is_empty()) return false;
        if (x() < 0 || x() >= line().size()) return check('\n');
        return check(line()[x()]);
    }

    bool is_current_char(int (*check)(int)) {
        if (lines.is_empty()) return false;
        if (x() < 0 || x() >= line().size()) return check('\n');
        return check(line()[x()]);
    }

    static bool is_ident(char c) {
        return isalnum(c) || c == '_';
    }

    void word_move(int direction) {
        move_x_wrap(0);

        if (is_current_char(ispunct)) {
            while (is_current_char(ispunct)) {
                if (!move_x_wrap(direction)) return;
            }
        } else if (is_current_char(is_ident)) {
            while (is_current_char(is_ident)) {
                if (!move_x_wrap(direction)) return;
            }
        }

        move_x_wrap(0);
        while (is_current_char(isspace)) {
            while (is_current_char(isspace)) {
                if (!move_x_wrap(direction)) return;
            }
        }
    }

    void move_x(int amount) {
        broken_edit = true;
        position.x = x() + amount;
        math::CLAMP(position.y, 0, (int) lines.size() - 1);
        math::CLAMP(position.x, 0, (int) line().size());
    }

    void create_line(u64 at) {
        if (flags & READ_ONLY) return;
        lines.insert(halloc, at, String());
    }

    void append(StrView text) {
        for (auto c: text) {
            insert(c);
        }
    }

    void insert_char(char c) {
        if (flags & READ_ONLY) {
            return;
        }

        flags |= UNSAVED;
        flags |= NEEDS_RETOKENIZE;
        flags |= MODIFIED;

        if (selection_start != Vector2i(-1, -1)) {
            position = remove_at(position);
        }

        if (c == '\n' && x() >= line().size()) {
            lines.insert(halloc, position.y + 1, String());

            position.x = 0;
            move_y(1);
        } else if (c == '\n') {
            lines.insert(halloc, position.y + 1, String());
            StrView str = line().substr(x());
            lines[position.y + 1].copy_from(halloc, str.ptr(), str.size());

            StrView old = line().substr(0, x());
            lines[position.y].copy_from(halloc, old.ptr(), old.size());

            position.x = 0;
            move_y(1);
        } else if (position.x >= line().size()) {
            line().push(halloc, c);
            move_x(1);
        } else {
            line().insert(halloc, x(), c);
            move_x(1);
        }
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
        backspace();
        selection_start = old_selection;
    }

    void load(StrView path) {
        // history = halloc.array<Edit>(MAX_HISTORY);

        { // clean up the path

            String tfile = String(talloc, path);
            if (tfile.size() >= 2) {
                if (tfile[0] == '.' && tfile[1] == PATH_SEP_CHAR) {
                    tfile.remove_at(0);
                    tfile.remove_at(0);
                }
            }
            tfile = tfile.replace(PATH_SEP PATH_SEP, PATH_SEP);  // Replace multiple path separators

            int index = tfile.find(PATH_SEP "..");
            while (index != -1 && index != 2) {
                // Go back to the previous PATH_SEP and remove the segment
                int at = index - 1;
                // Ensure `at >= 0` to avoid accessing invalid index
                while (at >= 0 && tfile[at] != PATH_SEP_CHAR) {
                    tfile.remove_at(at--);  // Decrement after removing
                }

                // Remove the ".." and preceding separator
                if (at >= 0) {
                    tfile.remove_at(at--);  // Also remove the PATH_SEP_CHAR
                }
                index = tfile.find(PATH_SEP "..", index + 1);
            }

            file = String(halloc, tfile);  // Final assignment
        }

        Arena content_arena;

        Error error = OK;

        char *path_cstring = file.view().tcstr();
        if (fs::is_directory(path_cstring)) {
            flags |= READ_ONLY | DIRECTORY;
            DArray<String> files = fs::read_dir(path, &error);
            if (error != OK) return;

            lines.push(halloc, fs::get_full_path(file.view().tcstr(), halloc));

            lines.push(halloc, String(halloc, ".."));
            for (u32 i = 0; i < files.size(); ++i) {
                if (files[i] == "." || files[i] == "..") continue;

                StringBuilder sb;
                sb.append(files[i]);
                auto full_path = tprint("%" PATH_SEP "%", path, files[i].view());
                if (fs::is_directory(full_path.to_cstr(talloc))) sb.append(PATH_SEP);
                
                lines.push(halloc, sb.build(halloc));
            }
            if (lines.is_empty()) {
                lines.push(halloc, String());
            }
            // lines.sort_custom<AlphabeticalSort>();
            return;
        }

        flags |= NEEDS_RETOKENIZE;

        tokenizer = halloc.single<Tokenizer>();
        if (!fs::file_exists(file, &error)) {
            lines.push(halloc, String());
            return;
        }

        String contents = fs::read_file(content_arena, file, &error);
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

    void find_search() {
        search_positions.free();
        search_positions = {};

        for (int i = 0; i < lines.size(); ++i) {
            int offset = 0;
            while (true) {
                offset = lines[i].find(search, offset);
                if (offset == -1) {
                    break;
                } else {
                    search_positions.push(halloc, {offset, i});
                    offset += 1;
                    if (offset >= lines[i].size()) break;
                }
            }
        }
    }

    void goto_next_search() {
        if (search_positions.is_empty()) return;

        for (int i = 0; i < search_positions.size(); ++i) {
            if (is_pos_less(position, search_positions[i])) {
                position = search_positions[i];
                return;
            }
        }

        position = search_positions[0];
    }

    void goto_next_search_if_not_at_one() {
        if (search_positions.has(position)) return;
        goto_next_search();
    }

    void goto_prev_search() {
        if (search_positions.is_empty()) return;

        for (int i = search_positions.size() - 1; i >= 0; --i) {
            if (is_pos_less(search_positions[i], position)) {
                position = search_positions[i];
                return;
            }
        }

        position = search_positions[0];
    }

    void free() {
        for (auto &line: lines) {
            line.free();
        }
        lines.free();
        file.free();

        search.free();
        search_positions.free();

        for (auto &edit: history) {
            edit.free();
        }
        history.free();
        history = {};

        if (tokenizer) {
            if (tokenizer->thread.joinable()) tokenizer->thread.join();
            tokenizer->file.free();
            tokenizer->tokens.free();
        }
        tokens.free();
        ::free(tokenizer);
    }
};

enum Bindings {
    MOUSE_BINDINGS,
    VIM_BINDINGS,
};

struct Global {
    FreeFont regular;
    FreeFont bold;
    FreeFont italic;

    ui::Theme theme;
    Color comment_color;
    Color keyword_color;
    Color string_color;
    Color number_color;
    Color punct_color;

    Color selection_color;
    Color bright_selection_color;

    OwnedDArray<Buffer> buffers;

    Arena macro_arena;
    AssocArray<char, String> macros;
    char recording_macro = '\0';

    float line_spacing = 1.2f;

    Arena compile_command_arena;
    DArray<String> compile_command;
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
        if (buffers.is_empty()) return nullptr;
        return &buffers.back();
    }

    Buffer *last_buffer() {
        if (buffers.size() <= 1) return nullptr;
        return &buffers[buffers.size() - 2];
    }

    void open_parent_folder(Buffer &buf) {
        StrView base = buf.file.get_base_dir();
        if (base.is_empty()) {
            base = CSV(".");
        }
        if (buf.file != base) {
            open_file(base); 
        }
    }

    void open_file(StrView file) {
        for (int i = 0; i < buffers.size(); ++i) {
            if (buffers[i].file == file) {
                set_buffer(i);
                vim_mode = VimNormal;
                return;
            }
        }

        buffers.push({});
        buffers.back().load(file);

        vim_mode = VimNormal;

        set_buffer(buffers.size() - 1);
    }

    void open_prompt(StrView prompt, void (*callback)(Buffer &), void (*on_press)(Buffer &, Events::KeyTyped &) = nullptr) {
        buffers.push({});
        buffers.back().prompt = prompt;
        buffers.back().on_selected = callback;
        buffers.back().on_selected_key_pressed = on_press;
        buffers.back().lines.push(halloc, String());
        // buffers.back().history = halloc.array<Edit>(MAX_HISTORY);
        
        vim_mode = VimInsert;

        set_buffer(buffers.size() - 1);
    }

    void set_buffer(int index) {
        Buffer buffer = buffers[index];
        buffers.remove_at(index);
        buffers.push(buffer);
    }

    void load_font(float size = 20, bool sdf = false) {
        using_sdf = sdf;
        if (regular.is_loaded()) {
            regular.unload();
            regular = {};
        }
        if (bold.is_loaded()) {
            bold.unload();
            bold = {};
        }
        if (italic.is_loaded()) {
            italic.unload();
            italic = {};
        }

#ifdef BUNDLE_FONT
        if (sdf) {
            regular.load_buffer(__fonts_ttf_JetBrainsMono_Regular_ttf, __fonts_ttf_JetBrainsMono_Regular_ttf_len, size, FreeFont::SDF);
            bold.load_buffer(__fonts_ttf_JetBrainsMono_Bold_ttf, __fonts_ttf_JetBrainsMono_Bold_ttf_len, size, FreeFont::SDF);
            italic.load_buffer(__fonts_ttf_JetBrainsMono_Italic_ttf, __fonts_ttf_JetBrainsMono_Italic_ttf_len, size, FreeFont::SDF);
            freetype_set_anti_aliasing_factor(SDF_AA);
        } else {
            regular.load_buffer(__fonts_ttf_JetBrainsMono_Regular_ttf, __fonts_ttf_JetBrainsMono_Regular_ttf_len, size, FreeFont::BITMAP);
            bold.load_buffer(__fonts_ttf_JetBrainsMono_Bold_ttf, __fonts_ttf_JetBrainsMono_Bold_ttf_len, size, FreeFont::BITMAP);
            italic.load_buffer(__fonts_ttf_JetBrainsMono_Italic_ttf, __fonts_ttf_JetBrainsMono_Italic_ttf_len, size, FreeFont::BITMAP);
            freetype_set_anti_aliasing_factor(BITMAP_AA);
        }
#else
        const char *regular_path = "editor/fonts/ttf/JetBrainsMono-Regular.ttf";
        const char *bold_path = "editor/fonts/ttf/JetBrainsMono-Bold.ttf";
        const char *italic_path = "editor/fonts/ttf/JetBrainsMono-Italic.ttf";
        JV_ASSERT(fs::file_exists(regular_path, nullptr));
        JV_ASSERT(fs::file_exists(bold_path, nullptr));
        JV_ASSERT(fs::file_exists(italic_path, nullptr));
        if (sdf) {
            regular.load(regular_path, size, FreeFont::SDF);
            bold.load(bold_path, size, FreeFont::SDF);
            italic.load(italic_path, size, FreeFont::SDF);
            freetype_set_anti_aliasing_factor(SDF_AA);
        } else {
            regular.load(regular_path, size, FreeFont::BITMAP);
            bold.load(bold_path, size, FreeFont::BITMAP);
            italic.load(italic_path, size, FreeFont::BITMAP);
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
        theme.normal_font = &regular;
        theme.muted = Colors::GRUVBOX_LIGHTGRAY.lightened(0.1);
        theme.outline_thickness = 3.0f;
        theme.text_padding = 10.0f;

        number_color = Color::hex(0xd3869bff);
        string_color = Color::hex(0xa9b665ff);
        keyword_color = Color::hex(0xd8a657ff);
        comment_color = Color::hex(0x7c6f64ff);
        punct_color = Color::hex(0xcc7d49ff);

        selection_color = theme.muted;
        selection_color.a = 0.75;

        bright_selection_color = Color::hex(0xcc7d49ff);
    }

    void compile() {
        if (game_proc.is_valid()) game_proc.wait();
        os::Command command;
        for (const auto &arg: compile_command) command.append(arg);
        game_proc = command.run_async();
    }

    void find_game() {
        compile_command_arena.reset();
        String dir(talloc, ".");
        auto files = fs::read_dir(".", nullptr);
        for (const auto &i: files) {
            if (i == "LovialEngine.exe" || i == "LovialEngine" || i == "build.jov.sh" || i == "build.jov.bat") {
                StringBuilder sb;
                sb.append(dir, PATH_SEP, i);
                push_compile_command_argument(sb.build(compile_command_arena));
            }
        }
    }

    void close_buffer(int index) {
        buffers[index].free();
        buffers.remove_at(index);
    }

    void close_current_buffer() {
        if (!buffers.is_empty()) {
            buffers.back().free();
            buffers.pop();
        }
    }

    void close_last_buffer() {
        if (buffers.size() >= 2) {
            close_buffer(buffers.size() - 2);
        }
    }

    void push_compile_command_argument(StrView arg) {
        compile_command.push(compile_command_arena, String(compile_command_arena, arg));
    }

    ~Global() {
        for (auto &buffer: buffers) {
            buffer.free();
        }
        if (regular.is_loaded()) regular.unload();
        if (bold.is_loaded()) bold.unload();
        if (italic.is_loaded()) italic.unload();

        for (auto e: errors) e.text.free();
        errors.free();
    }
} global;

void push_error_str(String string) {
    global.errors.push(halloc, {StopWatch(ERROR_DURATION), string});
}

inline bool is_control_pressed() {
    return Input::is_pressed(Actions::LeftControl) || Input::is_pressed(Actions::RightControl);
}

inline bool is_shift_pressed() {
    return Input::is_pressed(Actions::LeftShift) || Input::is_pressed(Actions::RightShift);
}

void cmd(Buffer &buffer) {
    Buffer *last = global.last_buffer();
    if (last == nullptr) return;
    if (buffer.line().is_empty()) return;
    String &prompt = buffer.line();

    bool found = false;

    if (prompt == "w") {
        found = true;
        if (last) last->save();
    }

    if (prompt == "q") {
        found = true;
        Jovial::singleton->queue_emit(Events::Quit());
    }

    DArray<StrView> words = prompt.view().split_spaces();
    if (words.size() >= 2) {
        if (words[0] == "e") {
            found = true;
            global.vim_mode = VimNormal;
            global.open_file(words[1]);
            global.close_last_buffer();
            return;
        }
    }

    // TODO: 
    //  - Confirmation
    //  - Being able to see the change as you type
    //  - Undo support
    //  - sed commands ie . * ^ \(\) [] \n etc
    if (prompt.find("s/") != -1) {
        found = true;

        bool whole_file = false;
        bool is_global = false;
        bool confirm = false;
        
        bool escape = false;

        enum {
            PREFIX,
            FIND,
            REPLACE,
            POSTFIX,
        };
        int mode = PREFIX;

        String find;
        String replace;

        for (auto c: prompt) {
            if (c == '\\' && !escape) {
                escape = true;
                continue;
            }

            if (mode == PREFIX) {
                if (c == 's') {
                    continue;
                } else if (c == '%') {
                    whole_file = true;
                } else if (c == '/' && !escape) {
                    mode += 1;
                } else {
                    push_error("unknown prefix flag '%c'", c);
                }
            } else if (mode == FIND) {
                if (!escape && c == '/') {
                    mode += 1;
                } else {
                    find.push(talloc, c);
                }
            } else if (mode == REPLACE) {
                if (!escape && c == '/') {
                    mode += 1;
                } else {
                    replace.push(talloc, c);
                }
            } else if (mode == POSTFIX) {
                if (c == 'g') {
                    is_global = true;
                } else if (c == 'c') {
                    confirm = true;
                } else {
                    push_error("unknown postfix flag '%c'", c);
                }
            } else {
                push_error("extraneous '/' found in find and replace");
                goto done;
            }

            escape = false;
        }

        if (!confirm) {
            if (whole_file) {
                for (int i = 0; i < last->lines.size(); ++i) {
                    if (is_global) {
                        String res = last->lines[i].replace(find, replace, halloc);
                        last->lines[i].free();
                        last->lines[i] = res;
                    } else {
                        String res = last->lines[i].replace_first(find, replace, halloc);
                        last->lines[i].free();
                        last->lines[i] = res;
                    }
                }
            } else {
                Vector2i start = Buffer::is_pos_less_or_equal(last->selection_start, last->position) ? last->selection_start : last->position;
                Vector2i end = Buffer::is_pos_less_or_equal(last->selection_start, last->position) ? last->position : last->selection_start;

                for (int i = start.y; i <= end.y; ++i) {
                    if (is_global) {
                        String res = last->lines[i].replace(find, replace, halloc);
                        last->lines[i].free();
                        last->lines[i] = res;
                    } else {
                        String res = last->lines[i].replace_first(find, replace, halloc);
                        last->lines[i].free();
                        last->lines[i] = res;
                    }
                }
            }
            last->flags |= Buffer::MODIFIED;
            last->flags |= Buffer::UNSAVED;
            last->flags |= Buffer::NEEDS_RETOKENIZE;

            last->move_x(0);
        } else {
            push_error("Confirmation for find a replace is not implemented yet!");
        }
    }


done:
    if (!found) {
        push_error("Not an editor command");
    }

    last->selection_start = Vector2i(-1, -1);

    global.vim_mode = VimNormal;
    global.close_current_buffer();
}


void search_on_press(Buffer &buffer, Events::KeyTyped &) {
    auto *lb = global.last_buffer();
    if (lb) {
        lb->search.free();
        lb->search = String(halloc, buffer.line());
        lb->flags |= Buffer::MODIFIED;
    }
}

void search(Buffer &buffer) {
    auto *lb = global.last_buffer();
    if (lb) {
        lb->search.free();
        lb->search = String(halloc, buffer.line());
        lb->flags |= Buffer::MODIFIED;
    }
    global.vim_mode = VimNormal;
    global.close_current_buffer();
}

struct VimMotion {
    int mode;
    StrView match;
    bool (*action)(Buffer &buffer, StrView rest);
};

StrView vim_move(StrView cmd, bool no_flush);

const VimMotion VIM_MOTIONS[] = {
    {VimNormal | VimVisual | VimVisualLine, "i", [](Buffer &buf, StrView rest) {
		global.vim_mode = VimInsert;
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "h", [](Buffer &buf, StrView rest) {
		buf.move_x(-1); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "j", [](Buffer &buf, StrView rest) {
		buf.move_y(1); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "k", [](Buffer &buf, StrView rest) {
		buf.move_y(-1); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "l", [](Buffer &buf, StrView rest) {
		buf.move_x(1); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "f", [](Buffer &buf, StrView rest) {
        if (rest.is_empty()) return false; 
        
        int pos = buf.line().find_char(rest[0], buf.position.x + 1);
        if (pos != -1) {
            buf.position.x = pos;
        }

        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "t", [](Buffer &buf, StrView rest) {
        if (rest.is_empty()) return false; 
        
        int pos = buf.line().find_char(rest[0], buf.position.x + 1);
        if (pos != -1) {
            buf.position.x = pos - 1;
        }

        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "q", [](Buffer &buf, StrView rest) {
        if (rest.is_empty()) return false; 
        global.recording_macro = rest[0];
        return true;
	}},

    {VimNormal, "d", [](Buffer &buf, StrView rest) {
        if (rest.is_empty()) return false; 
        
        buf.selection_start = buf.position;
        StrView res = vim_move(rest, false);

        if (buf.position != buf.selection_start) {
            if (buf.position.y != buf.selection_start.y) {
                buf.select_lines = true;
            } else {
                if (buf.position.x < buf.selection_start.x) {
                    buf.selection_start.x -= 1;
                } else {
                    buf.position.x -= 1;
                }
            }
            Vector2i start = buf.selection_start;
            buf.copy(false);
            buf.selection_start = start;

            buf.backspace();
            buf.selection_start = Vector2i(-1, -1);

            buf.select_lines = false;
            return true;
        } else {
            return res != rest;
        }
	}},

    {VimNormal, "w", [](Buffer &buf, StrView rest) {
        buf.word_move(1);
        return true;
	}},

    {VimNormal, "b", [](Buffer &buf, StrView rest) {
        buf.word_move(-1);
        return true;
	}},

    {VimNormal, "C", [](Buffer &buf, StrView rest) {
        if (buf.position.x < buf.line().size()) {
            buf.line().resize(halloc, buf.position.x);
        }
		global.vim_mode = VimInsert; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "gg", [](Buffer &buf, StrView rest) {
		buf.position = Vector2i(0, 0); buf.move_y(0); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "G", [](Buffer &buf, StrView rest) {
		buf.move_y(buf.lines.size()); 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "n", [](Buffer &buf, StrView rest) {
        buf.goto_next_search();
        return true;
	}},
    
    {VimNormal | VimVisual | VimVisualLine, "N", [](Buffer& buf, StrView rest) {
        buf.goto_prev_search();
        return true;
    }},

    {VimNormal, "x", [](Buffer& buf, StrView rest) {
        if (!buf.line().is_empty()) {
            buf.move_x(1);
            buf.backspace();
        }
        return true;
    }},

    {VimNormal | VimVisual | VimVisualLine, "o", [](Buffer &buf, StrView rest) {
		buf.position.x = buf.line().size();
		buf.user_insert('\n');
		global.vim_mode = VimInsert; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "O", [](Buffer &buf, StrView rest) {
		buf.move_y(1); 
        buf.position.x = buf.line().size(); 
        buf.user_insert('\n'); 
        global.vim_mode = VimInsert; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "a", [](Buffer &buf, StrView rest) {
		buf.move_x(1); 
        global.vim_mode = VimInsert; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "A", [](Buffer &buf, StrView rest) {
		buf.move_x(buf.line().size());
		global.vim_mode = VimInsert;
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "0", [](Buffer &buf, StrView rest) {
		buf.position.x = 0; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "$", [](Buffer &buf, StrView rest) {
		buf.position.x = buf.line().size() - 1; 
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, "p", [](Buffer &buf, StrView rest) {
		buf.paste(); 
        return true;
	}},

    {VimNormal, "dd", [](Buffer &buf, StrView rest) {
		buf.select_line();
        buf.copy(false);

        buf.select_line();
        buf.backspace(); 
        return true;
	}},

    {VimNormal, "yy", [](Buffer& buf, StrView rest) {
        buf.select_line();
        buf.copy();
        return true;
    }},

    {VimNormal | VimVisual | VimVisualLine, "/", [](Buffer &buf, StrView rest) {
        global.open_prompt("/", search, search_on_press);
        return true;
	}},

    {VimNormal | VimVisual | VimVisualLine, " /", [](Buffer& buf, StrView rest) {
        buf.search.free();
        buf.search = {};
        buf.search_positions.free();
        buf.search_positions = {};
        return true;
    }},

    {VimNormal | VimVisual | VimVisualLine, ":", [](Buffer &buf, StrView rest) {
        global.open_prompt(":", cmd);
        return true;
	}},

    {VimNormal, " v", [](Buffer &buf, StrView rest) {
        global.open_parent_folder(buf);
        return true;
	}},

    {VimNormal, " r", [](Buffer &buf, StrView rest) {
		global.compile(); 
        return true;
	}},

    {VimNormal, "v", [](Buffer &buf, StrView rest) {
		buf.selection_start = buf.position;
		global.vim_mode = VimVisual;
        return true;
	}},

    {VimNormal, "V", [](Buffer &buf, StrView rest) {
		buf.selection_start = buf.position;
        buf.selection_start.x = 0;
        buf.select_lines = true;
		global.vim_mode = VimVisualLine;
        return true;
	}},

    {VimNormal, "u", [](Buffer &buf, StrView rest) {
		buf.undo(); 
        return true;
	}},

    {VimVisual | VimVisualLine, "y", [](Buffer &buf, StrView rest) {
		buf.copy();
		global.vim_mode = VimNormal;
        return true;
	}},

    {VimVisual | VimVisualLine, "d", [](Buffer &buf, StrView rest) {
        Vector2i start = buf.selection_start;
        buf.copy(false);

        buf.selection_start = start;
		buf.backspace();
		global.vim_mode = VimNormal;
        return true;
	}},
};

StrView vim_move(StrView cmd, bool no_flush = false) {
    Buffer *buf = global.current_buffer();
    if (!buf) return cmd;

    for (int i = 0; i < cmd.size(); ++i) {
        for (int j = 0; j < JV_ARRAY_LEN(VIM_MOTIONS); ++j) {
            const auto &motion = VIM_MOTIONS[j];
            StrView view = cmd.substr(i, i + motion.match.size());

            if (global.vim_mode & motion.mode && view == motion.match) {
                StrView rest = cmd.substr(i + motion.match.size());
                bool flush = motion.action(*buf, rest);

                if (flush && !no_flush) {
                    return StrView();
                }
            }
        }
    }

    return cmd;
}

void update_buffer(Buffer *buf) {
    if (!buf) return;

    if (buf->flags & Buffer::MODIFIED) {
        if (!buf->search.is_empty()) {
            buf->find_search();
            buf->goto_next_search_if_not_at_one();
        }
        
        buf->flags &= ~Buffer::MODIFIED;
    }

    if (buf->tokenizer) {
        if (!buf->tokenizer->already_done && buf->tokenizer->done.load()) {
            if (buf->tokenizer->thread.joinable()) buf->tokenizer->thread.join();

            buf->tokens.free();
            buf->tokens = buf->tokenizer->tokens;
            buf->tokenizer->tokens = {};

            buf->tokenizer->already_done = true;
        }

        if (buf->flags & Buffer::NEEDS_RETOKENIZE && buf->tokenizer->done.load()) {
            buf->tokenizer->tokenize(buf->lines, buf->file);
            buf->flags &= ~Buffer::NEEDS_RETOKENIZE;
        }
    }
}

void update_buffers(Global &g, Events::Update &event) {
    Buffer *buf = g.current_buffer();
    update_buffer(buf);
    if (!buf->prompt.is_empty() && g.last_buffer()) {
        update_buffer(g.last_buffer());
    }
}

void on_typed(Global &g, Events::KeyTyped &event) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;

    if (!buf->prompt.is_empty()) {
        if (g.last_buffer()) {
            g.last_buffer()->flags |= Buffer::MODIFIED;
        }
    }

    if (g.bindings == VIM_BINDINGS) {
        g.command.push(g.command_arena, event.character);

        StrView res = vim_move(g.command);
        if (g.command != res) {
            g.flush_command();
            g.command.append(halloc, res);

            return;
        }

        if (g.vim_mode == VimInsert) {
            buf->user_insert(event.character);
            g.flush_command();
        }
    } else {
        buf->user_insert(event.character);
    }

    if (buf->on_selected_key_pressed) {
        buf->on_selected_key_pressed(*buf, event);
    }
}

void on_open_file(Buffer &buffer) {
    global.open_file(buffer.line());
    global.close_last_buffer();
}

void set_compile_command(Buffer &buffer) {
    global.compile_command_arena.reset();
    global.compile_command = {};

    // TODO: don't split on ' ' and " "
    for (auto v: buffer.line().view().split_spaces()) {
        global.push_compile_command_argument(v);
    }
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

    // Handle directory-related actions
    if (buf->flags & Buffer::DIRECTORY) {
        if (event.keycode == Actions::Enter) {
            g.open_file(tprint("%" PATH_SEP "%", buf->file, buf->line()));
            g.close_last_buffer();
            return;
        }
        if (event.keycode == Actions::Minus) {
            g.open_file(tprint("%" PATH_SEP "..", buf->file));
            return;
        }
    }

    // Handle text editing actions
    if ((g.vim_mode == VimInsert || g.bindings == MOUSE_BINDINGS)) {
        switch (event.keycode) {
            case Actions::Backspace: {
                buf->backspace();
             } break;
            case Actions::Delete:
                buf->del();
                break;
            case Actions::Enter: {
                 buf->user_insert('\n');
                 return;
            }
            case Actions::Tab: {
                for (int i = 0; i < TAB_WIDTH; ++i) {
                    buf->insert(' ');
                }
                return;
            }
            default: break;
        }
    }

    if (event.keycode == Actions::Escape) {
        g.recording_macro = 0;
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
                g.load_font(g.regular.size + 2);
                break;
            case Actions::Minus:
                g.load_font(g.regular.size - 2);
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
            case Actions::Semicolon:
                g.open_prompt(":", cmd);
                break;
            case Actions::F:
            case Actions::Slash:
                g.open_prompt("/", search, search_on_press);
                break;
            case Actions::K: {
                StrView extension = buf->file.get_extension();
// #define DEFINE_COMMENT(buf, str) \
//                 do { \
//                     int at = buf->line().find(str); \
//                     if (at != -1) { \
//                         for (int i = 0; i < sizeof(str) - 1; ++i) { \
//                             buf->line().remove_at(at); \
//                         } \
//                     } else { \
//                         buf->append_at({0, buf->position.y}, str); \
//                         buf->position.x += sizeof(str) - 1; \
//                     } \
//                 } while (0)
//
//                 if (extension == "lua") {
//                     DEFINE_COMMENT(buf, "--");
//                 } else if (extension == "c" || extension == "cpp" || extension == "rs") {
//                     DEFINE_COMMENT(buf, "//");
//                 } else if (extension == "py") {
//                     DEFINE_COMMENT(buf, "#");
//                 }
            } break;
            case Actions::O:
                if (is_shift_pressed()) {
                    g.open_parent_folder(*buf);
                } else {
                    g.open_prompt("Path: ", on_open_file);
                }
                return;
            case Actions::Num6:  {
                if (g.buffers.size() >= 2) {
                    g.set_buffer(g.buffers.size() - 2);
                }
            } return;
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
            g.load_font(g.regular.size, !g.using_sdf);
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
        const float line_percent = 1 - (g.regular.size * g.line_spacing * 2) / WM::get_main_window()->get_height();
        const char *line_number_fmt = " % ";
        StrView max_number = tprint(line_number_fmt, buf->lines.size());
        Vector2 offset;
        g.regular.measure(offset, max_number);

        float y = WM::get_main_window()->get_height() - Input::get_mouse_position().y;
        y /= global.regular.size * global.line_spacing;
        y = math::floor(y);

        float x = Input::get_mouse_position().x - offset.x;

        buf->position.y = math::CLAMPED(y + buf->cam_offset, 0, buf->lines.size() - 1);

        buf->position.x = 0;
        while (buf->position.x < buf->line().size()) {
            x -= g.regular.metrics[' '].advance.x;
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

    if (Input::just_double_clicked()) {
        if (buf->flags & Buffer::DIRECTORY) {
            g.open_file(tprint("%" PATH_SEP "%", buf->file, buf->line()));
            g.close_last_buffer();
        }
    }

    if (Input::get_scroll() != 0) {
        int scroll = Input::get_scroll() * 2;
        buf->move_y(scroll);
        buf->cam_offset += scroll;
    }
}

Color get_color(Buffer &buf, FreeFont **font, u64 token_index, u64 i, u64 j) {
    Color color = global.theme.primary;

    if (token_index >= buf.tokens.size()) return color;
    if (i < buf.tokens[token_index].line) return color;
    if (i > buf.tokens[token_index].end_line) return color;

    if (buf.tokens[token_index].line == i && j < buf.tokens[token_index].start) return color;
    if (buf.tokens[token_index].end_line == i && j > buf.tokens[token_index].end) return color;

    switch (buf.tokens[token_index].type) {
        case Token::NORMAL: {
            *font = &global.regular;
        } break;
        case Token::KEYWORD: {
            color = global.keyword_color;
            *font = &global.bold;
        } break;
        case Token::COMMENT: {
            color = global.comment_color;
            *font = &global.italic;
        } break;
        case Token::STRING: {
            *font = &global.regular;
            color = global.string_color;
        } break;
        case Token::PUNCT: {
            *font = &global.regular;
            color = global.punct_color;
        } break;
        case Token::NUMBER: {
            *font = &global.regular;
            color = global.number_color;
        } break;
    }

    return color;
}

void draw(Global &g, Events::Draw &e) {
    Buffer *buf = g.current_buffer();
    if (!buf) return;
    if (!buf->prompt.is_empty()) {
        if (g.buffers.size() >= 2) {
            buf = &g.buffers[g.buffers.size() - 2];
        } else {
            buf = nullptr;
        }
    }

    ui::Layout layout(talloc, {{0, 0}, WM::get_main_window()->get_size()});
    const float line_percent = 1 - (g.regular.size * g.line_spacing * 2) / WM::get_main_window()->get_height();

    { // bottom status line
        Rect2 rect = layout.push_percent(ui::BOTTOM, line_percent, 1 - line_percent);
        Renderer2D::from(e.renderers[0])->set_scissor(rect);

        StrView unsaved = buf->flags & Buffer::UNSAVED ? "[+] " : "";
        StrView msg = tprint("%%:%:%", unsaved, buf->file, buf->position.y + 1, buf->x() + 1);
        ui::label(e.renderers[0], g.regular, rect, msg, g.theme.primary, ui::RIGHT);

        StrView compile_cmd = "no compile command";
        if (!g.compile_command.is_empty()) compile_cmd = join_strings(talloc, " ", g.compile_command.view());
        
        ui::label(e.renderers[0], g.regular, rect, compile_cmd, g.theme.muted, ui::CENTER);
        
        if (g.recording_macro != 0) {
            ui::label(e.renderers[0], g.regular, rect, tprint("recording macro @%", g.recording_macro), g.theme.primary, ui::LEFT);
        } else {
            if (g.bindings == VIM_BINDINGS) {
                ui::label(e.renderers[0], g.regular, rect, tprint("Vim: % %", vim_mode_to_string(g.vim_mode), g.command), g.theme.muted, ui::LEFT);
            } else if (g.bindings == MOUSE_BINDINGS) {
                ui::label(e.renderers[0], g.regular, rect, "Mouse", g.theme.muted, ui::LEFT);
            }
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
        g.regular.measure(pos, max_number);

        float x = pos.x;
        pos.y -= g.regular.size;

        if (buf->lines.is_empty() || (buf->lines.size() == 1 && buf->lines[0].size() == 0)) {
            Vector2 pos_copy = pos;
            g.regular.immediate_draw(e.renderers[0], pos_copy, "  empty file", g.theme.muted);
        }


        {

            { // Camera movement
                int cam_move = 0;
                int visable_rows = math::floor(rect.size.y / (g.regular.size * g.line_spacing));
                int offset = buf->position.y - buf->cam_offset;

                if (offset <= SCROLL_OFF && visable_rows - offset <= SCROLL_OFF) {
                    cam_move = 0;
                } else if (offset < SCROLL_OFF) {
                    cam_move = -(SCROLL_OFF - offset);
                } else if (visable_rows - offset < SCROLL_OFF) {
                    cam_move = SCROLL_OFF - (visable_rows - offset);
                }

                buf->cam_offset = math::CLAMPED(buf->cam_offset + cam_move, 0, (int) buf->lines.size() - 1);
            }

            { // selection 
                // TODO: this seems painfully slow. 
                buf->copied_flash.tick_down();
                if (buf->selection_start != Vector2i(-1, -1) || !buf->copied_flash.is_finished()) {
                    Color selection_color = g.selection_color;
                    if (!buf->copied_flash.is_finished()) {
                        selection_color = g.bright_selection_color;
                    }

                    Vector2 selection_pos = pos;
                    for (int y = buf->cam_offset; y < buf->lines.size(); ++y) {
                        Rect2DCmd cmd{selection_pos, {g.regular.metrics[0].advance.x, g.regular.size * g.line_spacing}, selection_color};
                        cmd.position.y -= g.regular.size * (g.line_spacing - 1);

                        for (int x = 0; x <= buf->lines[y].size(); ++x) {
                            if (buf->is_selected({x, y}) || buf->is_flash_selected({x, y})) {
                                cmd.immediate_draw(e.renderers[0]);
                            }

                            if (cmd.position.x >= WM::get_main_window()->get_width() - g.regular.metrics[' '].advance.x) {
                                cmd.position.x = pos.x;
                                cmd.position.y -= g.regular.size * g.line_spacing;
                                selection_pos.y -= g.regular.size * g.line_spacing;
                            }

                            cmd.position.x += cmd.size.x;
                        }

                        selection_pos.y -= g.regular.size * g.line_spacing;
                        if (selection_pos.y < -global.regular.size) {
                            break;
                        }
                    }
                } else {
                    buf->copied_flash_position = Vector2i(-1, -1);
                }
            }
        }

        u64 token_index = 0;

        for (u64 i = buf->cam_offset; i < buf->lines.size(); ++i) {
            Vector2 line_pos(rect.position.x, pos.y);
            if (buf->position.y == i) {
                g.regular.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, i + 1), *g.theme.named_colors.get("theme_purple"));
            } else {
                g.regular.immediate_draw(e.renderers[0], line_pos, tprint(line_number_fmt, math::abs((int) buf->position.y - (int) i)), g.theme.muted);
            }

            while (token_index < buf->tokens.size() && buf->tokens[token_index].end_line < i) {
                token_index++;
            }

            for (u64 j = 0; j < buf->lines[i].size(); ++j) {
                FreeFont *font = &g.regular;
                Color color = get_color(*buf, &font, token_index, i, j);

                // Move to the next token if we've reached the end of the current one
                if (token_index < buf->tokens.size() && i == buf->tokens[token_index].end_line && j == buf->tokens[token_index].end - 1) {
                    token_index++;
                }

                // TODO: maybe here would be a more convienient place to put the selection
                // if (buf->is_selected({(int) i, (int) j}))

                bool inverted = false;
                for (const auto &search_pos: buf->search_positions) {
                    if (search_pos.y == i) {
                        if ((int) j - search_pos.x >= 0 && (int) j - search_pos.x < buf->search.size()) {
                            Rect2DCmd cmd{pos, {font->metrics[0].advance.x, font->size * g.line_spacing}, g.bright_selection_color};
                            cmd.position.y -= font->size * (g.line_spacing - 1);
                            cmd.immediate_draw(e.renderers[0]);
                            inverted = true;
                        }
                    }
                }

                if (i == buf->position.y && j == buf->x()) {
                    Rect2DCmd cmd{pos, {font->metrics[0].advance.x, font->size * g.line_spacing}, color};
                    cmd.position.y -= font->size * (g.line_spacing - 1);

                    cmd.immediate_draw(e.renderers[0]);
                    inverted = true;
                }
                
                if (inverted) color = g.theme.secondary;
                if (pos.x >= WM::get_main_window()->get_width() - font->metrics[' '].advance.x) {
                    pos.x = x;
                    pos.y -= g.regular.size * g.line_spacing;
                }
                font->immediate_draw(e.renderers[0], pos, buf->lines[i].substr(j, 1), color);
            }

            if (buf->position.y == i && buf->x() >= buf->lines[i].size()) {
                Rect2DCmd cmd{pos, {g.regular.metrics[0].advance.x, g.regular.size * g.line_spacing}, g.theme.primary};
                cmd.position.y -= g.regular.size * (g.line_spacing - 1);

                cmd.immediate_draw(e.renderers[0]);
            }

            pos.x = x;
            pos.y -= g.regular.size * g.line_spacing;

            if (pos.y < -global.regular.size) {
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

            Vector2 size(pad * 2, g.regular.size * g.line_spacing + pad);
            g.regular.measure(size, it.text);

            Vector2 position = rect.top_right() - size;

            const float animation_len = 0.5;
            if (it.timer.time < animation_len) {
                float t = 1 - it.timer.time * 1.0f/animation_len;
                position.x = math::lerp(position.x, (float) WM::get_main_window()->get_width(), (float) easers::in(t));
            }

            rect = {position, size};

            Renderer2D::from(e.renderers[0])->set_scissor(rect);
            ClearScreen2DCmd{g.theme.muted}.immediate_draw(e.renderers[0]);
            ui::label(e.renderers[0], g.regular, rect, it.text, *g.theme.named_colors.get("theme_red"), ui::CENTER);

            rect.position.y -= rect.size.y + pad;
        }

        layout.pop();
    }

     // prompt
    if (!g.current_buffer()->prompt.is_empty()) {
        buf = g.current_buffer();

        Vector2 size(pad * 2, g.regular.size * g.line_spacing + pad);
        g.regular.measure(size, buf->prompt);
        g.regular.measure(size, buf->line());

        Rect2 rect = layout.push_percent(ui::CENTER, 0, 0);
        rect.position -= size / 2;
        rect.size = size;

        Renderer2D::from(e.renderers[0])->set_scissor(rect);
        ClearScreen2DCmd{g.theme.muted}.immediate_draw(e.renderers[0]);

        Vector2 position = rect.position + Vector2(pad);
        g.regular.immediate_draw(e.renderers[0], position, buf->prompt, g.theme.primary);
        g.regular.immediate_draw(e.renderers[0], position, buf->line(), g.theme.primary);
    }
}

int main(int argc, char* argv[]) {
    Jovial game;

    WindowProps props{
            .size  = {1280, 720},
            .title = "Jovial Editor",
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
    game.push_system(global, update_buffers);
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
