#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <pty.h>
#include <sys/types.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <pwd.h>

#include <ncurses.h>

#define LOGFILE_NAME "cursed.log"
#define DEFAULT_SHELL "/bin/bash"
#define TERM_NAME "xterm"
#define UTF8_SIZE 4
#define ESCAPE_BUFFER_SIZE (128 * UTF8_SIZE)
#define ESCAPE_ARGUMENT_SIZE 16
// NOTE: 60 fps limit
#define DRAW_USEC_LIMIT 16666

#define LOG(...) fprintf(g_log, __VA_ARGS__);
#define ELEM_COUNT(static_array) (sizeof(static_array) / sizeof(static_array[0]))
#define CLAMP(a, min, max) (a = (a < min) ? min : (a > max) ? max : a);
#define BETWEEN(n, min, max) ((min <= n) && (n <= max))

typedef uint_least32_t Rune_t;

typedef enum{
     GLYPH_ATTRIBUTE_NONE       = 0,
     GLYPH_ATTRIBUTE_BOLD       = 1 << 0,
     GLYPH_ATTRIBUTE_FAINT      = 1 << 1,
     GLYPH_ATTRIBUTE_ITALIC     = 1 << 2,
     GLYPH_ATTRIBUTE_UNDERLINE  = 1 << 3,
     GLYPH_ATTRIBUTE_BLINK      = 1 << 4,
     GLYPH_ATTRIBUTE_REVERSE    = 1 << 5,
     GLYPH_ATTRIBUTE_INVISIBLE  = 1 << 6,
     GLYPH_ATTRIBUTE_STRUCK     = 1 << 7,
     GLYPH_ATTRIBUTE_WRAP       = 1 << 8,
     GLYPH_ATTRIBUTE_WIDE       = 1 << 9,
     GLYPH_ATTRIBUTE_WDUMMY     = 1 << 10,
     GLYPH_ATTRIBUTE_BOLD_FAINT = GLYPH_ATTRIBUTE_BOLD | GLYPH_ATTRIBUTE_FAINT,
}GlyphAttribute_t;

typedef enum{
     CURSOR_MODE_SAVE,
     CURSOR_MODE_LOAD,
}CursorMode_t;

typedef enum{
     CURSOR_STATE_DEFAULT = 0,
     CURSOR_STATE_WRAPNEXT = 1,
     CURSOR_STATE_ORIGIN = 2,
}CursorState_t;

typedef enum{
     TERMINAL_MODE_WRAP        = 1 << 0,
     TERMINAL_MODE_INSERT      = 1 << 1,
     TERMINAL_MODE_APPKEYPAD   = 1 << 2,
     TERMINAL_MODE_ALTSCREEN   = 1 << 3,
     TERMINAL_MODE_CRLF        = 1 << 4,
     TERMINAL_MODE_MOUSEBTN    = 1 << 5,
     TERMINAL_MODE_MOUSEMOTION = 1 << 6,
     TERMINAL_MODE_REVERSE     = 1 << 7,
     TERMINAL_MODE_KBDLOCK     = 1 << 8,
     TERMINAL_MODE_HIDE        = 1 << 9,
     TERMINAL_MODE_ECHO        = 1 << 10,
     TERMINAL_MODE_APPCURSOR   = 1 << 11,
     TERMINAL_MODE_MOUSEGR     = 1 << 12,
     TERMINAL_MODE_8BIT        = 1 << 13,
     TERMINAL_MODE_BLINK       = 1 << 14,
     TERMINAL_MODE_FBLINK      = 1 << 15,
     TERMINAL_MODE_FOCUS       = 1 << 16,
     TERMINAL_MODE_MOUSEEX10   = 1 << 17,
     TERMINAL_MODE_MOUSEEMANY  = 1 << 18,
     TERMINAL_MODE_BRCKTPASTE  = 1 << 19,
     TERMINAL_MODE_PRINT       = 1 << 20,
     TERMINAL_MODE_UTF8        = 1 << 21,
     TERMINAL_MODE_SIXEL       = 1 << 22,
     TERMINAL_MODE_MOUSE       = 1 << 23,
}TerminalMode_t;

typedef enum{
     ESCAPE_STATE_START      = 1 << 0,
     ESCAPE_STATE_CSI        = 1 << 1,
     ESCAPE_STATE_STR        = 1 << 2,
     ESCAPE_STATE_ALTCHARSET = 1 << 3,
     ESCAPE_STATE_STR_END    = 1 << 4,
     ESCAPE_STATE_TEST       = 1 << 5,
     ESCAPE_STATE_UTF8       = 1 << 6,
     ESCAPE_STATE_DCS        = 1 << 7,
}EscapeState_t;

typedef struct{
     Rune_t   rune;
     uint16_t attributes;
     uint32_t foreground;
     uint32_t background;
}Glyph_t;

typedef struct{
     Glyph_t attributes;
     int32_t x;
     int32_t y;
     uint8_t state;
}Cursor_t;

typedef struct{
     char buffer[ESCAPE_BUFFER_SIZE];
     uint32_t buffer_length;
     char private;
     int argument[ESCAPE_ARGUMENT_SIZE];
     uint32_t argument_count;
}CSIEscape_t;

typedef struct{
     char type;
     char buffer[ESCAPE_BUFFER_SIZE];
     uint32_t buffer_length;
     int argument[ESCAPE_ARGUMENT_SIZE];
     uint32_t argument_count;
}STREscape_t;

typedef struct{
     int32_t        rows;
     int32_t        columns;
     Glyph_t**      lines;
     Glyph_t**      alternate_lines;
     bool*          dirty_lines;
     Cursor_t       cursor;
     int32_t        top;
     int32_t        bottom;
     TerminalMode_t mode;
     EscapeState_t  escape_state;
     char           translation_table[4];
     int32_t        charset;
     int32_t        selected_charset;
     bool           numlock;
     int32_t*       tabs;
}Terminal_t;

typedef struct{
     int file_descriptor;
     Terminal_t* terminal;
}TTYReadData_t;

typedef struct{
     int file_descriptor;
}TTYWriteData_t;

FILE* g_log = NULL;
bool g_quit = false;

bool is_controller(Rune_t rune)
{
     // c0? idk what these mean
     if(BETWEEN(rune, 0, 0x1f) || rune == '\177'){
          return true;
     }

     // c1? idk what these mean
     if(BETWEEN(rune, 0x80, 0x9f)){
          return true;
     }

     return false;
}

void terminal_move_cursor_to(Terminal_t* terminal, int x, int y)
{
     int min_y;
     int max_y;

     if(terminal->cursor.state & CURSOR_STATE_ORIGIN){
          min_y = terminal->top;
          max_y = terminal->bottom;
     }else{
          min_y = 0;
          max_y = terminal->rows - 1;
     }

     terminal->cursor.state &= ~CURSOR_STATE_WRAPNEXT;
     terminal->cursor.x = CLAMP(x, 0, terminal->columns - 1);
     terminal->cursor.y = CLAMP(y, min_y, max_y);
}

void terminal_set_glyph(Terminal_t* terminal, Rune_t rune, Glyph_t* attributes, int x, int y)
{
     terminal->dirty_lines[y] = true;
     terminal->lines[y][x] = *attributes;
     terminal->lines[y][x].rune = rune;
}

void terminal_clear_region(Terminal_t* terminal, int left, int top, int right, int bottom)
{
     // probably going to assert since we are going to trust external data
     // TODO: just swap if they are wrong?
     assert(left <= right);
     assert(top <= bottom);

     CLAMP(left, 0, terminal->columns - 1);
     CLAMP(right, 0, terminal->columns - 1);
     CLAMP(top, 0, terminal->rows - 1);
     CLAMP(bottom, 0, terminal->rows - 1);

     for(int y = top; y <= bottom; ++y){
          for(int x = left; x <= right; ++x){
               Glyph_t* glyph = terminal->lines[y] + x;
               glyph->foreground = terminal->cursor.attributes.foreground;
               glyph->background = terminal->cursor.attributes.background;
               glyph->attributes = 0;
               glyph->rune = ' ';
          }
     }
}

void terminal_set_dirt(Terminal_t* terminal, int top, int bottom)
{
     assert(top <= bottom);

     CLAMP(top, 0, terminal->rows - 1);
     CLAMP(bottom, 0, terminal->rows - 1);

     for(int i = top; i <= bottom; ++i){
          terminal->dirty_lines[i] = true;
     }
}

void terminal_all_dirty(Terminal_t* terminal)
{
     terminal_set_dirt(terminal, 0, terminal->rows - 1);
}

void terminal_scroll_up(Terminal_t* terminal, int original, int n)
{
     Glyph_t* temp_line = NULL;

     CLAMP(n, 0, terminal->bottom - original + 1);

     terminal_clear_region(terminal, 0, original, terminal->columns - 1, original + n - 1);
     terminal_set_dirt(terminal, original + n, terminal->bottom);

     for(int i = original; i <= terminal->bottom - n; ++i){
          temp_line = terminal->lines[i];
          terminal->lines[i] = terminal->lines[i + n];
          terminal->lines[i + n] = temp_line;
     }
}

void terminal_scroll_down(Terminal_t* terminal, int original, int n)
{
	Glyph_t* temp_line;

	CLAMP(n, 0, terminal->bottom - original + 1);

	terminal_clear_region(terminal, 0, original, terminal->columns - 1, original + n - 1);
	terminal_set_dirt(terminal, original + n, terminal->bottom);

	for (int i = original; i <= terminal->bottom - n; i++) {
		temp_line = terminal->lines[i];
		terminal->lines[i] = terminal->lines[i + n];
		terminal->lines[i + n] = temp_line;
	}
}

void terminal_put_newline(Terminal_t* terminal, bool first_column)
{
     int y = terminal->cursor.y;

     if(y == terminal->bottom){
          terminal_scroll_up(terminal, terminal->top, 1);
     }else{
          y++;
     }

     terminal_move_cursor_to(terminal, first_column ? 0 : terminal->cursor.x, y);
     terminal->dirty_lines[y] = true;
}

void terminal_control_code(Terminal_t* terminal, Rune_t rune)
{
     assert(is_controller(rune));

     switch(rune){
     default:
          break;
     case '\t': // HT
          // TODO
          return;
     case '\b': // BS
          terminal_move_cursor_to(terminal, terminal->cursor.x - 1, terminal->cursor.y);
          return;
     case '\r': // CR
          terminal_move_cursor_to(terminal, 0, terminal->cursor.y);
          return;
     case '\f': // LF
     case '\v': // VT
     case '\n': // LF
          terminal_put_newline(terminal, terminal->mode & TERMINAL_MODE_CRLF);
          return;
     case '\a': // BEL
          break;
     case '\033': // ESC
          // TODO
          // csireset();
          terminal->escape_state &= ~(ESCAPE_STATE_CSI | ESCAPE_STATE_ALTCHARSET | ESCAPE_STATE_TEST);
          terminal->escape_state |= ESCAPE_STATE_START;
          return;
     case '\016': // SO
     case '\017': // SI
          // TODO
          break;
     case '\032': // SUB
          terminal_set_glyph(terminal, '?', &terminal->cursor.attributes, terminal->cursor.x, terminal->cursor.y);
     case '\030':
          // TODO: csireset()
          break;
     case '\005': // ENQ
     case '\000': // NULL
     case '\021': // XON
     case '\023': // XOFF
     case 0177:   // DEL
          // ignored
          return;
     case 0x80: // PAD
     case 0x81: // HOP
     case 0x82: // BPH
     case 0x83: // NBH
     case 0x84: // IND
          // TODO
          break;
     case 0x85: // NEL
          terminal_put_newline(terminal, true);
          break;
	case 0x86: // SSA
	case 0x87: // ESA
          // TODO
		break;
	case 0x88: // HTS
          // TODO
          break;
	case 0x89: // HTJ
	case 0x8a: // VTS
	case 0x8b: // PLD
	case 0x8c: // PLU
	case 0x8d: // RI
	case 0x8e: // SS2
	case 0x8f: // SS3
	case 0x91: // PU1
	case 0x92: // PU2
	case 0x93: // STS
	case 0x94: // CCH
	case 0x95: // MW
	case 0x96: // SPA
	case 0x97: // EPA
	case 0x98: // SOS
	case 0x99: // SGCI
          // TODO
		break;
	case 0x9a: // DECID
          // TODO
		//ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 0x9b: // CSI
	case 0x9c: // ST
		break;
	case 0x90: // DCS
	case 0x9d: // OSC
	case 0x9e: // PM
	case 0x9f: // APC
          // TODO
		//tstrsequence(ascii);
		return;
     }

     terminal->escape_state &= ~(ESCAPE_STATE_STR_END | ESCAPE_STATE_STR);
}

void terminal_put(Terminal_t* terminal, Rune_t rune)
{
     if(is_controller(rune)){
          terminal_control_code(terminal, rune);
          return;
     }

     terminal_set_glyph(terminal, rune, &terminal->cursor.attributes, terminal->cursor.x, terminal->cursor.y);

     int new_cursor = terminal->cursor.x + 1;
     if(new_cursor >= terminal->columns){
          terminal_put_newline(terminal, true);
     }else{
          terminal_move_cursor_to(terminal, new_cursor, terminal->cursor.y);
     }
}

void handle_signal_child(int signal)
{
     LOG("%s(%d)\n", __FUNCTION__, signal);
}

bool tty_create(int rows, int columns, pid_t* pid, int* tty_file_descriptor)
{
     int master_file_descriptor;
     int slave_file_descriptor;
     struct winsize window_size = {rows, columns, 0, 0};

     if(openpty(&master_file_descriptor, &slave_file_descriptor, NULL, NULL, &window_size) < 0){
          LOG("openpty() failed: '%s'\n", strerror(errno));
          return false;
     }

     switch(*pid = fork()){
     case -1:
          LOG("fork() failed\n");
          break;
     case 0:
          setsid();

          dup2(slave_file_descriptor, 0);
          dup2(slave_file_descriptor, 1);
          dup2(slave_file_descriptor, 2);

          if(ioctl(slave_file_descriptor, TIOCSCTTY, NULL)){
               LOG("ioctl() TIOCSCTTY failed: '%s'\n", strerror(errno));
               return false;
          }

          close(slave_file_descriptor);
          close(master_file_descriptor);

          {
               const struct passwd* pw;
               char* shell = getenv("SHELL");
               if(!shell) shell = DEFAULT_SHELL;

               pw = getpwuid(getuid());
               if(pw == NULL){
                    LOG("getpwuid() failed: '%s'\n", strerror(errno));
                    return false;
               }

               char** args = (char *[]){NULL};

               unsetenv("COLUMNS");
               unsetenv("LINES");
               unsetenv("TERMCAP");
               setenv("LOGNAME", pw->pw_name, 1);
               setenv("USER", pw->pw_name, 1);
               setenv("SHELL", shell, 1);
               setenv("HOME", pw->pw_dir, 1);
               setenv("TERM", TERM_NAME, 1);

               signal(SIGCHLD, SIG_DFL);
               signal(SIGHUP, SIG_DFL);
               signal(SIGINT, SIG_DFL);
               signal(SIGQUIT, SIG_DFL);
               signal(SIGTERM, SIG_DFL);
               signal(SIGALRM, SIG_DFL);

               execvp(shell, args);
               _exit(1);
          }
          break;
     default:
          close(slave_file_descriptor);
          *tty_file_descriptor = master_file_descriptor;
          signal(SIGCHLD, handle_signal_child);
          break;
     }

     return true;
}

void* tty_read(void* data)
{
     TTYReadData_t* thread_data = (TTYReadData_t*)(data);

     char buffer[BUFSIZ];
     int buffer_length = 0;

     while(true){
          int rc = read(thread_data->file_descriptor, buffer, ELEM_COUNT(buffer));

          if(rc < 0){
               LOG("%s() failed to read from tty file descriptor: '%s'\n", __FUNCTION__, strerror(errno));
               return NULL;
          }

          buffer_length = rc;

          for(int i = 0; i < buffer_length; ++i){
               terminal_put(thread_data->terminal, buffer[i]);
          }

          sleep(0);
     }
}

void* tty_write_keys(void* data)
{
     TTYWriteData_t* thread_data = (TTYWriteData_t*)(data);

     while(true){
          int key = getch();
          switch(key){
          default:
          {
               int rc = write(thread_data->file_descriptor, &key, sizeof(key));
               if(rc < 0){
                    LOG("failed to write to tty descriptor: '%s'\n", strerror(errno));
                    g_quit = true;
               }
          } break;
          case 17: // ctrl + q
               g_quit = true;
               break;
          }
     }
}

int main(int argc, char** argv)
{
     // setup log
     {
          g_log = fopen(LOGFILE_NAME, "w");
          if(!g_log){
               fprintf(stderr, "failed to create log file: %s\n", LOGFILE_NAME);
               return 1;
          }
     }

     Terminal_t terminal = {};
     //CSIEscape_t csi_escape = {};
     //STREscape_t str_escape = {};
     int tty_file_descriptor;
     pid_t tty_pid;

     // init terminal structure
     {
          terminal.columns = 80;
          terminal.rows = 24;
          terminal.bottom = terminal.columns - 1;

          // allocate lines
          terminal.lines = calloc(terminal.rows, sizeof(*terminal.lines));
          for(int r = 0; r < terminal.rows; ++r){
               terminal.lines[r] = calloc(terminal.columns, sizeof(*terminal.lines[r]));
          }

          terminal.dirty_lines = calloc(terminal.rows, sizeof(*terminal.dirty_lines));
          terminal_all_dirty(&terminal);
          terminal_clear_region(&terminal, 0, 0, terminal.columns, terminal.rows);
     }

     WINDOW* view = NULL;
     int entire_window_width;
     int entire_window_height;

     int view_x = 0;
     int view_y = 0;
     int view_width = terminal.columns + 2; // account for borders
     int view_height = terminal.rows + 2; // account for borders

     // init curses
     {
          initscr();
          keypad(stdscr, TRUE);
          raw();
          cbreak();
          noecho();
          start_color();
          use_default_colors();

          getmaxyx(stdscr, entire_window_height, entire_window_width);

          // find the top left corner of a centered window
          view_x = ((entire_window_width - view_width) / 2);
          view_y = ((entire_window_height - view_height) / 2);

          view = newwin(view_height, view_width, view_y, view_x);
     }

     pthread_t tty_read_thread;
     pthread_t tty_write_thread;

     // create terminal
     {
          if(!tty_create(terminal.rows, terminal.columns, &tty_pid, &tty_file_descriptor)){
               return 1;
          }

          TTYReadData_t* data = calloc(1, sizeof(*data));
          data->file_descriptor = tty_file_descriptor;
          data->terminal = &terminal;

          int rc = pthread_create(&tty_read_thread, NULL, tty_read, data);
          if(rc != 0){
               LOG("pthread_create() failed: '%s'\n", strerror(errno));
               return 1;
          }
     }

     // setup getch thread
     {
          TTYWriteData_t* data = calloc(1, sizeof(*data));
          data->file_descriptor = tty_file_descriptor;
          int rc = pthread_create(&tty_write_thread, NULL, tty_write_keys, data);
          if(rc != 0){
               LOG("pthread_create() failed: '%s'\n", strerror(errno));
               return 1;
          }
     }

     clear();
     refresh();

     struct timeval previous_draw_time;
     struct timeval current_draw_time;
     uint64_t elapsed = 0;

     // main program loop
     while(!g_quit){
          gettimeofday(&previous_draw_time, NULL);

          do{
               gettimeofday(&current_draw_time, NULL);
               elapsed = (current_draw_time.tv_sec - previous_draw_time.tv_sec) * 1000000LL +
                         (current_draw_time.tv_usec - previous_draw_time.tv_usec);
          }while(elapsed < DRAW_USEC_LIMIT);

          box(view, 0, 0);

          for(int r = 0; r < terminal.rows; ++r){
               if(terminal.dirty_lines[r]){
                    for(int c = 0; c < terminal.columns; ++c){
                         mvwaddch(view, r + 1, c + 1, terminal.lines[r][c].rune);
                    }

                    terminal.dirty_lines[r] = false;
               }
          }

          wmove(view, terminal.cursor.y + 1, terminal.cursor.x + 1);
          wrefresh(view);
     }

     pthread_cancel(tty_read_thread);
     pthread_join(tty_read_thread, NULL);

     // cleanup curses
     delwin(view);
     endwin();

     fclose(g_log);

     return 0;
}
