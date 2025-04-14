#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include "dyn_array.h"

#define FLAG_1HY_HELP 'h'
#define FLAG_1HY_CONTROLS 'c'
#define FLAG_2HY_CONTROLS "--controls"
#define FLAG_2HY_HELP "--help"
#define FLAG_2HY_LS "--ls"
#define FLAG_2HY_CAT "--cat"
#define FLAG_2HY_CD "--cd"

#define INVERT "\033[7m"
#define RESET "\033[0m"

#define CTRL_Q 17

#define UP_ARROW      'A'
#define DOWN_ARROW    'B'
#define RIGHT_ARROW   'C'
#define LEFT_ARROW    'D'

#define BM_CONFIG ".bm"

#define ENTER(ch)     (ch) == '\n'
#define BACKSPACE(ch) (ch) == 8 || (ch) == 127
#define ESCSEQ(ch)    (ch) == 27
#define CSI(ch)       (ch) == '['
#define TAB(ch)       (ch) == '\t'

#define err(msg)                                \
    do {                                        \
        fprintf(stderr, "[Error]: " msg "\n");  \
        exit(1);                                \
    } while (0)

#define err_wargs(msg, ...)                                     \
    do {                                                        \
        fprintf(stderr, "[Error]: " msg "\n", __VA_ARGS__);     \
        exit(1);                                                \
    } while (0)

#define SAFE_PEEK(arr, i, el) ((arr)[(i)] && (arr)[(i)] == (el))

#define S_MALLOC(b) ({ \
        void *__p_ = malloc((b)); \
        if (!__p_) { \
                err_wargs("could not alloc %zu bytes", (b)); \
        } \
        (__p_); \
})

#define da_append(arr, len, cap, ty, value)                       \
    do {                                                          \
        if ((len) >= (cap)) {                                     \
            (cap) = !(cap) ? 2 : (cap) * 2;                       \
            (arr) = (ty)realloc((arr), (cap) * sizeof((arr)[0])); \
        }                                                         \
        (arr)[(len)] = (value);                                   \
        (len) += 1;                                               \
    } while (0)

typedef enum uint32_t {
        FLAG_TYPE_HELP,
        FLAG_TYPE_CONTROLS,
        FLAG_TYPE_LS,
        FLAG_TYPE_CAT,
        FLAG_TYPE_CD,
} Flag_Type;

typedef enum {
        USER_INPUT_TYPE_CTRL,
        USER_INPUT_TYPE_ALT,
        USER_INPUT_TYPE_ARROW,
        USER_INPUT_TYPE_SHIFT_ARROW,
        USER_INPUT_TYPE_NORMAL,
        USER_INPUT_TYPE_UNKNOWN,
} User_Input_Type;

typedef struct {
        struct {
                char **data;
                size_t len, cap;
        } paths;
        size_t r;
} Ctx;

static struct {
        struct {
                int width;
                int height;
        } window;
        struct termios old_termios;
        uint32_t flags;
} g_config = {0};

static const char *g_controls =
        "Controls:\n"
        "[UP ARROW]   - up\n"
        "[DOWN ARROW] - down\n"
        "d            - delete\n"
        "q            - quit\n"
        "[ENTER]      - select\n"
        "\n"
        "Upon selection, cd <path> will be copied\n"
        "to the clipboard with xclip (X support only).\n"
        "Paste using ctrl+shift+v.";

void copy_to_clipboard(const char *data) {
        FILE *pipe;
        char *command = "xclip -selection clipboard"; // X11
        // char *command = "wl-copy"; // Wayland

        pipe = popen(command, "w");
        if (pipe == NULL) {
                perror("Failed to open pipe to clipboard tool");
                return;
        }

        char buf[256] = {0};

        if ((g_config.flags & FLAG_TYPE_LS) != 0) {
                strcpy(buf, "ls '");
        } else if ((g_config.flags & FLAG_TYPE_CAT) != 0) {
                strcpy(buf, "cat '");
        } else if ((g_config.flags & FLAG_TYPE_CD) != 0) {
                strcpy(buf, "cd '");
        }

        strcat(buf, data);
        strcat(buf, "'");

        fputs(buf, pipe);

        if (pclose(pipe) == -1) {
                perror("Failed to close pipe");
        }
}

void out(const char *msg, int newline) {
        printf("%s", msg);
        if (newline) {
                putchar('\n');
        }
        fflush(stdout);
}

void color(const char *c) {
        out(c, 0);
}

void read_bm(Ctx *ctx) {
        const char *home = getenv("HOME");
        if (!home) {
                fprintf(stderr, "Error: HOME environment variable not set.\n");
                return;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/.bm", home);

        FILE *file = fopen(path, "r");
        if (!file) {
                perror("Failed to open file for reading");
                return;
        }

        char *line = NULL;
        size_t len = 0;
        ssize_t read;

        while ((read = getline(&line, &len, file)) != -1) {
                if (!strcmp(line, "\n")) continue;
                if (line[read - 1] == '\n') {
                        line[read - 1] = '\0'; // Remove newline
                }
                dyn_array_append(ctx->paths, strdup(line));
        }

        free(line);
        fclose(file);
}

void write_bm(Ctx *ctx) {
        const char *home = getenv("HOME");
        if (!home) {
                fprintf(stderr, "Error: HOME environment variable not set.\n");
                return;
        }

        char path[1024];
        snprintf(path, sizeof(path), "%s/.bm", home);

        FILE *file = fopen(path, "w");
        if (!file) {
                perror("Failed to open file for writing");
                return;
        }

        for (size_t i = 0; i < ctx->paths.len; i++) {
                fprintf(file, "%s\n", ctx->paths.data[i]);
        }

        fclose(file);
}

// Takes path, returns newly alloc'd path.
// Free old if needed.
char *expand_tilde(const char *path) {
        if (path[0] != '~') {
                // No tilde, return a copy of the original string
                return strdup(path);
        }

        const char *home = getenv("HOME");
        if (!home) {
                fprintf(stderr, "Error: HOME environment variable not set.\n");
                return NULL;
        }

        size_t home_len = strlen(home);
        size_t path_len = strlen(path);

        // +1 is already included since `~` is replaced
        char *expanded_path = (char *)S_MALLOC(home_len + path_len);

        strcpy(expanded_path, home);
        strcat(expanded_path, path + 1); // Skip the `~`

        return expanded_path;
}

char *get_absolute_path(const char *path) {
        char *expanded = expand_tilde(path);

        char *absolute_path = realpath(expanded, NULL);
        free(expanded);

        if (!absolute_path) {
                perror("realpath");
                exit(1);
        }

        return absolute_path;
}

void help(void) {
        printf("Usage: bm [paths...] [options...]\n");
        printf("Options:\n");
        printf("    %s, %c     - Print this help message\n", FLAG_2HY_HELP, FLAG_1HY_HELP);
        printf("    %s, %c - Show the controls\n", FLAG_2HY_CONTROLS, FLAG_1HY_CONTROLS);
        printf("If bm is ran with no paths, it will use the ones that have been\n");
        printf("previously saved. If none have been saved, make sure to provide\n");
        printf("some paths before running bm.\n");
        exit(1);
}

void controls(void) {
        printf("%s\n", g_controls);
        exit(0);
}

void handle_1hy_flag(const char *arg, int *argc, char ***argv) {
        const char *it = arg+1;
        while (it && *it != ' ' && *it != '\0') {
                if (*it == FLAG_1HY_HELP) {
                        help();
                } else if (*it == FLAG_1HY_CONTROLS) {
                        controls();
                } else {
                        err_wargs("Unknown option: `%c`", *it);
                }
                ++it;
        }
}

void handle_2hy_flag(const char *arg, int *argc, char ***argv) {
        if (!strcmp(arg, FLAG_2HY_HELP)) {
                help();
        } else if (!strcmp(arg, FLAG_2HY_CONTROLS)) {
                controls();
        } else if (!strcmp(arg, FLAG_2HY_LS)) {
                g_config.flags |= FLAG_TYPE_LS;
        } else if (!strcmp(arg, FLAG_2HY_CAT)) {
                g_config.flags |= FLAG_TYPE_CAT;
        } else if (!strcmp(arg, FLAG_2HY_CD)) {
                g_config.flags |= FLAG_TYPE_CD;
        } else {
                err_wargs("Unknown option: `%s`", arg);
        }
}

char *eat(int *argc, char ***argv) {
        if (!(*argc)) return NULL;
        (*argc)--;
        return *(*argv)++;
}

char get_char(void) {
        char ch;
        read(STDIN_FILENO, &ch, 1);
        return ch;
}

User_Input_Type get_user_input(char *c) {
        assert(c);
        while (1) {
                *c = get_char();
                if (ESCSEQ(*c)) {
                        int next0 = get_char();
                        if (CSI(next0)) {
                                int next1 = get_char();
                                if (next1 >= '0' && next1 <= '9') { // Modifier key detected
                                        int semicolon = get_char();
                                        if (semicolon == ';') {
                                                int modifier = get_char();
                                                int arrow_key = get_char();
                                                if (modifier == '2') { // Shift modifier
                                                        switch (arrow_key) {
                                                        case 'A': *c = UP_ARROW;    return USER_INPUT_TYPE_SHIFT_ARROW;
                                                        case 'B': *c = DOWN_ARROW;  return USER_INPUT_TYPE_SHIFT_ARROW;
                                                        case 'C': *c = RIGHT_ARROW; return USER_INPUT_TYPE_SHIFT_ARROW;
                                                        case 'D': *c = LEFT_ARROW;  return USER_INPUT_TYPE_SHIFT_ARROW;
                                                        default: return USER_INPUT_TYPE_UNKNOWN;
                                                        }
                                                }
                                        }
                                        return USER_INPUT_TYPE_UNKNOWN;
                                } else { // Regular arrow key
                                        switch (next1) {
                                        case DOWN_ARROW:
                                        case RIGHT_ARROW:
                                        case LEFT_ARROW:
                                        case UP_ARROW:
                                                *c = next1;
                                                return USER_INPUT_TYPE_ARROW;
                                        default:
                                                return USER_INPUT_TYPE_UNKNOWN;
                                        }
                                }
                        } else { // [ALT] key
                                *c = next0;
                                return USER_INPUT_TYPE_ALT;
                        }
                }
                else if (*c == CTRL_Q) {
                        return USER_INPUT_TYPE_CTRL;
                }
                else return USER_INPUT_TYPE_NORMAL;
        }
        return USER_INPUT_TYPE_UNKNOWN;
}

void display_paths(const Ctx *const ctx) {
        printf("%zu Directories, selection: cd '%s'\n", ctx->paths.len, ctx->paths.data[ctx->r]);
        size_t i;
        for (i = 0; i < ctx->paths.len; ++i) {
                if (i == ctx->r) {
                        color(INVERT);
                }
                printf("%s\n", ctx->paths.data[i]);
                if (i == ctx->r) {
                        color(RESET);
                }
        }
}

void reset_scrn(void) {
        printf("\033[2J");
        printf("\033[H");
        fflush(stdout);
}

void cleanup(void) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_config.old_termios);
}

void init_term(void) {
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
                g_config.window.width = w.ws_col-1,
                        g_config.window.height = w.ws_row-1;
        }
        else {
                perror("ioctl");
                fprintf(stderr, "[Warning]: Could not get size of terminal. Undefined behavior may occur.");
        }

        tcgetattr(STDIN_FILENO, &g_config.old_termios);
        struct termios raw = g_config.old_termios;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void selection_down(Ctx *ctx) {
        if (ctx->r < ctx->paths.len - 1) {
                ++ctx->r;
        } else {
                ctx->r = 0;
        }
}

void selection_up(Ctx *ctx) {
        if (ctx->r > 0) {
                --ctx->r;
        } else {
                ctx->r = ctx->paths.len-1;
        }
}

void remove_path(Ctx *ctx) {
        if (ctx->r >= ctx->paths.len) {
                return;
        }
        dyn_array_rm_at(ctx->paths, ctx->r);
        if (ctx->r >= ctx->paths.len) {
                --ctx->r;
        }
}

int main(int argc, char **argv) {
        ++argv, --argc;

        init_term();
        atexit(cleanup);

        Ctx ctx = (Ctx) {
                .paths = {
                        .data = NULL,
                        .len = 0,
                        .cap = 0,
                },
                .r = 0,
        };

        read_bm(&ctx);

        char *arg = NULL;
        int user_inputted_path = 0;
        while ((arg = eat(&argc, &argv)) != NULL) {
                if (arg[0] == '-' && SAFE_PEEK(arg, 1, '-')) {
                        handle_2hy_flag(arg, &argc, &argv);
                }
                else if (arg[0] == '-' && arg[1]) {
                        handle_1hy_flag(arg, &argc, &argv);
                }
                else {
                        user_inputted_path = 1;
                        int found = 0;
                        char *path = get_absolute_path(arg);
                        for (size_t i = 0; i < ctx.paths.len; ++i) {
                                if (!strcmp(ctx.paths.data[i], path)) {
                                        found = 1;
                                        break;
                                }
                        }
                        if (!found) {
                                dyn_array_append(ctx.paths, path);
                        }
                }
        }

        if (ctx.paths.len == 0) {
                err("No bookmarks found");
        }

        if (user_inputted_path) {
                for (size_t i = 0; i < ctx.paths.len; ++i) {
                        printf("Bookmarked %s\n", ctx.paths.data[i]);
                }
                goto end;
        }

        while (1) {
                if (ctx.paths.len == 0) {
                        reset_scrn();
                        printf("No entries\n");
                        goto end;
                }

                reset_scrn();
                display_paths(&ctx);

                char ch;
                User_Input_Type ty = get_user_input(&ch);
                switch (ty) {
                case USER_INPUT_TYPE_CTRL: {
                        if (ch == CTRL_Q) goto end;
                } break;
                case USER_INPUT_TYPE_ALT: break;
                case USER_INPUT_TYPE_ARROW: {
                        if (ch == UP_ARROW) {
                                selection_up(&ctx);
                        } else if (ch == DOWN_ARROW) {
                                selection_down(&ctx);
                        }
                } break;
                case USER_INPUT_TYPE_SHIFT_ARROW: break;
                case USER_INPUT_TYPE_NORMAL: {
                        if (ch == 'q') {
                                reset_scrn();
                                goto end;
                        }
                        else if (ch == 'd') {
                                remove_path(&ctx);
                        } else if (ENTER(ch)) {
                                reset_scrn();
                                copy_to_clipboard(ctx.paths.data[ctx.r]);
                                printf("copied: cd '%s' to the clipboard\n", ctx.paths.data[ctx.r]);
                                goto end;
                        }
                } break;
                case USER_INPUT_TYPE_UNKNOWN:
                default: break;
                }
        }
 end:
        write_bm(&ctx);

        dyn_array_free(ctx.paths);

        return 0;
}
