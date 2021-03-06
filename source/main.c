#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <locale.h>
#include <ctype.h>
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
//NOTE: used for testing
//#define TERM_NAME "dumb"
#define TERM_NAME "xterm"
#define UTF8_SIZE 4
#define ESCAPE_BUFFER_SIZE (128 * UTF8_SIZE)
#define ESCAPE_ARGUMENT_SIZE 16
// NOTE: 60 fps limit
#define DRAW_USEC_LIMIT 16666
#define VT_IDENTIFIER "\033[?6c"
#define TAB_SPACES 5

#define COLOR_BACKGROUND -1
#define COLOR_FOREGROUND -1
#define COLOR_BRIGHT_BLACK 8
#define COLOR_BRIGHT_RED 9
#define COLOR_BRIGHT_GREEN 10
#define COLOR_BRIGHT_YELLOW 11
#define COLOR_BRIGHT_BLUE 12
#define COLOR_BRIGHT_MAGENTA 13
#define COLOR_BRIGHT_CYAN 14
#define COLOR_BRIGHT_WHITE 15

#define LOG(...) fprintf(g_log, __VA_ARGS__);
#define ELEM_COUNT(static_array) (sizeof(static_array) / sizeof(static_array[0]))
#define CLAMP(a, min, max) (a = (a < min) ? min : (a > max) ? max : a);
#define BETWEEN(n, min, max) ((min <= n) && (n <= max))
#define DEFAULT(a, value) (a = (a == 0) ? value : a)
#define CHANGE_BIT(a, set, bit) ((set) ? ((a) |= (bit)) : ((a) &= ~(bit)))

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
     int32_t foreground;
     int32_t background;
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
     int arguments[ESCAPE_ARGUMENT_SIZE];
     uint32_t argument_count;
     char mode[2];
}CSIEscape_t;

typedef struct{
     char type;
     char buffer[ESCAPE_BUFFER_SIZE];
     uint32_t buffer_length;
     char* arguments[ESCAPE_ARGUMENT_SIZE];
     uint32_t argument_count;
}STREscape_t;

typedef struct{
     int            file_descriptor;
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
     CSIEscape_t    csi_escape;
     STREscape_t    str_escape;
}Terminal_t;

typedef struct{
     Terminal_t* terminal;
}TTYThreadData_t;

typedef struct{
     int32_t foreground;
     int32_t background;
}ColorPair_t;

typedef struct{
     int32_t count;
     ColorPair_t pairs[256]; // NOTE: this is what COLOR_PAIRS was for me (which is for some reason not const?)
}ColorDefs_t;

FILE* g_log = NULL;
bool g_quit = false;

Cursor_t g_cursor[2];

bool tty_write(int file_descriptor, const char* string, size_t len);
void str_handle(Terminal_t* terminal);
void str_sequence(Terminal_t* terminal, Rune_t rune);
bool utf8_decode(const char* buffer, size_t buffer_len, size_t* len, Rune_t* u);
bool utf8_encode(Rune_t u, char* buffer, size_t buffer_len, int* len);

bool is_controller_c0(Rune_t rune)
{
     if(BETWEEN(rune, 0, 0x1f) || rune == '\177'){
          return true;
     }

     return false;
}

bool is_controller_c1(Rune_t rune)
{
     if(BETWEEN(rune, 0x80, 0x9f)){
          return true;
     }

     return false;
}

bool is_controller(Rune_t rune)
{
     if(is_controller_c0(rune)) return true;
     if(is_controller_c1(rune)) return true;

     return false;
}

void csi_reset(CSIEscape_t* csi)
{
     memset(csi, 0, sizeof(*csi));
}

void csi_parse(CSIEscape_t* csi)
{
     char* str = csi->buffer;
     char* end = NULL;
     long int value = 0;

     csi->argument_count = 0;

     if(*str == '?'){
          csi->private = 1;
          str++;
     }

     csi->buffer[csi->buffer_length] = 0;
     while(str < csi->buffer + csi->buffer_length){
          end = NULL;
          value = strtol(str, &end, 10);

          if(end == str) value = 0;
          else if(value == LONG_MAX || value == LONG_MIN) value = -1;

          csi->arguments[csi->argument_count] = value;
          csi->argument_count++;

          str = end;

          if(*str != ';' || csi->argument_count == ESCAPE_ARGUMENT_SIZE) break;

          str++;
     }

     csi->mode[0] = *str++;
     csi->mode[1] = (str < (csi->buffer + csi->buffer_length)) ? *str : 0;
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

void terminal_move_cursor_to_absolute(Terminal_t* terminal, int x, int y)
{
	terminal_move_cursor_to(terminal, x, y + ((terminal->cursor.state & CURSOR_STATE_ORIGIN) ? terminal->top : 0));

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
     if(left > right){
          int tmp = left;
          left = right;
          right = tmp;
     }

     if(top > bottom){
          int tmp = top;
          top = bottom;
          bottom = tmp;
     }

     CLAMP(left, 0, terminal->columns - 1);
     CLAMP(right, 0, terminal->columns - 1);
     CLAMP(top, 0, terminal->rows - 1);
     CLAMP(bottom, 0, terminal->rows - 1);

     for(int y = top; y <= bottom; ++y){
          terminal->dirty_lines[y] = true;

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

void terminal_scroll_down(Terminal_t* terminal, int original, int n)
{
	Glyph_t* temp_line;

	CLAMP(n, 0, terminal->bottom - original + 1);

	terminal_set_dirt(terminal, original, terminal->bottom - n);
	terminal_clear_region(terminal, 0, terminal->bottom - n + 1, terminal->columns - 1, terminal->bottom);

	for (int i = terminal->bottom; i >= original + n; i--) {
		temp_line = terminal->lines[i];
		terminal->lines[i] = terminal->lines[i - n];
          terminal->lines[i - n] = temp_line;
	}
}

void terminal_scroll_up(Terminal_t* terminal, int original, int n)
{
     Glyph_t* temp_line = NULL;

     CLAMP(n, 0, terminal->bottom - original + 1);

     // clear the original line plus the scroll
     terminal_clear_region(terminal, 0, original, terminal->columns - 1, original + n - 1);
     terminal_set_dirt(terminal, original + n, terminal->bottom);

     // swap lines to move them all up
     // the cleared lines will end up at the bottom
     for(int i = original; i <= terminal->bottom - n; ++i){
          temp_line = terminal->lines[i];
          terminal->lines[i] = terminal->lines[i + n];
          terminal->lines[i + n] = temp_line;
     }
}

void terminal_set_scroll(Terminal_t* terminal, int top, int bottom)
{
     CLAMP(top, 0, terminal->rows - 1);
     CLAMP(bottom, 0, terminal->rows - 1);

     if(top > bottom){
          int temp = top;
          top = bottom;
          bottom = temp;
     }

     terminal->top = top;
     terminal->bottom = bottom;
}

void terminal_insert_blank_line(Terminal_t* terminal, int n)
{
     if(BETWEEN(terminal->cursor.y, terminal->top, terminal->bottom)){
          terminal_scroll_down(terminal, terminal->cursor.y, n);
     }
}

void terminal_delete_line(Terminal_t* terminal, int n)
{
     if(BETWEEN(terminal->cursor.y, terminal->top, terminal->bottom)){
          terminal_scroll_up(terminal, terminal->cursor.y, n);
     }
}

void terminal_delete_char(Terminal_t* terminal, int n)
{
	int dst, src, size;
	Glyph_t* line;

	CLAMP(n, 0, terminal->columns - terminal->cursor.x);

	dst = terminal->cursor.x;
	src = terminal->cursor.x + n;
	size = terminal->columns - src;
	line = terminal->lines[terminal->cursor.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph_t));
	terminal_clear_region(terminal, terminal->columns - n, terminal->cursor.y, terminal->columns - 1, terminal->cursor.y);
}

void terminal_insert_blank(Terminal_t* terminal, int n)
{
	int dst, src, size;
	Glyph_t* line;

	CLAMP(n, 0, terminal->columns - terminal->cursor.x);

	dst = terminal->cursor.x + n;
	src = terminal->cursor.x;
	size = terminal->columns - dst;
	line = terminal->lines[terminal->cursor.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph_t));
	terminal_clear_region(terminal, src, terminal->cursor.y, dst - 1, terminal->cursor.y);
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

void terminal_put_tab(Terminal_t* terminal, int n)
{
     unsigned int new_x = terminal->cursor.x;

     if(n > 0){
          while(new_x < terminal->columns && n--){
               new_x++;
               while(new_x < terminal->columns && !terminal->tabs[new_x]){
                    new_x++;
               }
          }
     }else if(n < 0){
          while(new_x > 0 && n++){
               new_x--;
               while(new_x > 0 && !terminal->tabs[new_x]){
                    new_x--;
               }
          }
     }

     terminal->cursor.x = CLAMP(new_x, 0, terminal->columns - 1);
}

void terminal_cursor_save(Terminal_t* terminal)
{
	int alt = terminal->mode & TERMINAL_MODE_ALTSCREEN;
     g_cursor[alt] = terminal->cursor;
}

void terminal_cursor_load(Terminal_t* terminal)
{
	int alt = terminal->mode & TERMINAL_MODE_ALTSCREEN;
     terminal->cursor = g_cursor[alt];
     terminal_move_cursor_to(terminal, g_cursor[alt].x, g_cursor[alt].y);
}

void terminal_swap_screen(Terminal_t* terminal)
{
     Glyph_t** tmp_lines = terminal->lines;

     terminal->lines = terminal->alternate_lines;
     terminal->alternate_lines = tmp_lines;
     terminal->mode ^= TERMINAL_MODE_ALTSCREEN;
     terminal_all_dirty(terminal);
}

void terminal_reset(Terminal_t* terminal)
{
     terminal->cursor.attributes.attributes = GLYPH_ATTRIBUTE_NONE;
     terminal->cursor.attributes.foreground = COLOR_FOREGROUND;
     terminal->cursor.attributes.background = COLOR_BACKGROUND;
     terminal->cursor.x = 0;
     terminal->cursor.y = 0;
     terminal->cursor.state = CURSOR_STATE_DEFAULT;

     memset(terminal->tabs, 0, terminal->columns * sizeof(*terminal->tabs));
     for(int i = TAB_SPACES; i < terminal->columns; ++i) terminal->tabs[i] = 1;
     terminal->top = 0;
     terminal->bottom = terminal->rows - 1;
     terminal->mode = TERMINAL_MODE_WRAP | TERMINAL_MODE_UTF8;

     //TODO: clear character translation table
     terminal->charset = 0;
     terminal->escape_state = 0;

     terminal_move_cursor_to(terminal, 0, 0);
     terminal_cursor_save(terminal);
     terminal_clear_region(terminal, 0, 0, terminal->columns - 1, terminal->rows - 1);
     terminal_swap_screen(terminal);

     terminal_move_cursor_to(terminal, 0, 0);
     terminal_cursor_save(terminal);
     terminal_clear_region(terminal, 0, 0, terminal->columns - 1, terminal->rows - 1);
     terminal_swap_screen(terminal);
}

void terminal_control_code(Terminal_t* terminal, Rune_t rune)
{
     assert(is_controller(rune));

     switch(rune){
     default:
          LOG("unhandled control code: '%c'\n", rune);
          break;
     case '\t': // HT
          terminal_put_tab(terminal, 1);
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
          if(terminal->escape_state & ESCAPE_STATE_STR_END){
               str_handle(terminal);
          }
          break;
     case '\033': // ESC
          csi_reset(&terminal->csi_escape);
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
          csi_reset(&terminal->csi_escape);
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
          break;
     case 0x85: // NEL
          terminal_put_newline(terminal, true);
          break;
	case 0x86: // SSA
	case 0x87: // ESA
		break;
	case 0x88: // HTS
          terminal->tabs[terminal->cursor.x] = 1;
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
		break;
	case 0x9a: // DECID
		tty_write(terminal->file_descriptor, VT_IDENTIFIER, sizeof(VT_IDENTIFIER) - 1);
		break;
	case 0x9b: // CSI
	case 0x9c: // ST
		break;
	case 0x90: // DCS
	case 0x9d: // OSC
	case 0x9e: // PM
	case 0x9f: // APC
          str_sequence(terminal, rune);
		return;
     }

     terminal->escape_state &= ~(ESCAPE_STATE_STR_END | ESCAPE_STATE_STR);
}

void terminal_set_mode(Terminal_t* terminal, bool set)
{
     CSIEscape_t* csi = &terminal->csi_escape;
     int* arg;
     int* last_arg = csi->arguments + csi->argument_count;
     //int mode;
     int alt;

     for(arg = csi->arguments; arg <= last_arg; ++arg){
          if(csi->private){
               switch(*arg){
               default:
                    break;
               case 1:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_APPCURSOR);
                    break;
               case 5:
                    //mode = terminal->mode;
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_REVERSE);
                    // TODO if(mode != terminal->mode) redraw();
                    break;
               case 6:
                    CHANGE_BIT(terminal->cursor.state, set, CURSOR_STATE_ORIGIN);
                    terminal_move_cursor_to_absolute(terminal, 0, 0);
                    break;
               case 7:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_WRAP);
                    break;
               case 0:
               case 2:
               case 3:
               case 4:
               case 8:
               case 18:
               case 19:
               case 42:
               case 12:
                    // ignored
                    break;
               case 25:
                    CHANGE_BIT(terminal->mode, !set, TERMINAL_MODE_HIDE);
                    break;
               case 9:
                    // TODO: xsetpointermotion(0); ?
                    CHANGE_BIT(terminal->mode, 0, TERMINAL_MODE_MOUSE);
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_MOUSEEX10);
                    break;
               case 1000:
                    // TODO: xsetpointermotion(0); ?
                    CHANGE_BIT(terminal->mode, 0, TERMINAL_MODE_MOUSE);
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_MOUSEBTN);
                    break;
               case 1002:
                    // TODO: xsetpointermotion(0); ?
                    CHANGE_BIT(terminal->mode, 0, TERMINAL_MODE_MOUSE);
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_MOUSEMOTION);
                    break;
               case 1003:
                    // TODO: xsetpointermotion(0); ?
                    CHANGE_BIT(terminal->mode, 0, TERMINAL_MODE_MOUSE);
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_MOUSEEMANY);
                    break;
               case 1004:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_FOCUS);
                    break;
               case 1006:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_MOUSEGR);
                    break;
               case 1034:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_8BIT);
                    break;
               case 1049:
                    if(set){
                         terminal_cursor_save(terminal);
                    }else{
                         terminal_cursor_load(terminal);
                    }
                    // fallthrough
               case 47:
               case 1047:
                    // f this layout
                    alt = terminal->mode & TERMINAL_MODE_ALTSCREEN;
                    if(alt) terminal_clear_region(terminal, 0, 0, terminal->columns - 1, terminal->rows - 1);
                    if(set ^ alt) terminal_swap_screen(terminal);
                    if(*arg != 1049) break;
                    // fallthrough
               case 1048:
                    if(set){
                         terminal_cursor_save(terminal);
                    }else{
                         terminal_cursor_load(terminal);
                    }
                    break;
               case 2004:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_BRCKTPASTE);
                    break;
               case 1001:
               case 1005:
               case 1015:
                    // ignored
                    break;
               }
          }else{
               switch(*arg){
               default:
                    break;
               case 2:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_KBDLOCK);
                    break;
               case 4:
                    CHANGE_BIT(terminal->mode, set, TERMINAL_MODE_INSERT);
                    break;
               case 12:
                    CHANGE_BIT(terminal->mode, !set, TERMINAL_MODE_ECHO);
                    break;
               case 20:
                    CHANGE_BIT(terminal->mode, !set, TERMINAL_MODE_CRLF);
                    break;
               }
          }
     }
}

void terminal_set_attributes(Terminal_t* terminal)
{
     CSIEscape_t* csi = &terminal->csi_escape;

     for(int i = 0; i < csi->argument_count; ++i){
          switch(csi->arguments[i]){
          default:
               break;
          case 0:
               terminal->cursor.attributes.attributes &= ~(GLYPH_ATTRIBUTE_BOLD | GLYPH_ATTRIBUTE_FAINT |
                                                           GLYPH_ATTRIBUTE_ITALIC | GLYPH_ATTRIBUTE_UNDERLINE |
                                                           GLYPH_ATTRIBUTE_BLINK | GLYPH_ATTRIBUTE_REVERSE |
                                                           GLYPH_ATTRIBUTE_INVISIBLE | GLYPH_ATTRIBUTE_STRUCK);
               terminal->cursor.attributes.foreground = COLOR_FOREGROUND;
               terminal->cursor.attributes.background = COLOR_BACKGROUND;
               break;
          case 1:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_BOLD;
               break;
          case 2:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_FAINT;
               break;
          case 3:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_ITALIC;
               break;
          case 4:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_UNDERLINE;
               break;
          case 5: // fallthrough
          case 6:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_BLINK;
               break;
          case 7:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_REVERSE;
               break;
          case 8:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_INVISIBLE;
               break;
          case 9:
               terminal->cursor.attributes.attributes |= GLYPH_ATTRIBUTE_STRUCK;
               break;
          case 21:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_BOLD;
               break;
          case 22:
               terminal->cursor.attributes.attributes &= ~(GLYPH_ATTRIBUTE_BOLD | GLYPH_ATTRIBUTE_FAINT);
               break;
          case 23:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_ITALIC;
               break;
          case 24:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_UNDERLINE;
               break;
          case 25:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_BLINK;
               break;
          case 27:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_REVERSE;
               break;
          case 28:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_INVISIBLE;
               break;
          case 29:
               terminal->cursor.attributes.attributes &= ~GLYPH_ATTRIBUTE_STRUCK;
               break;
          case 30:
               terminal->cursor.attributes.foreground = COLOR_BLACK;
               break;
          case 31:
               terminal->cursor.attributes.foreground = COLOR_RED;
               break;
          case 32:
               terminal->cursor.attributes.foreground = COLOR_GREEN;
               break;
          case 33:
               terminal->cursor.attributes.foreground = COLOR_YELLOW;
               break;
          case 34:
               terminal->cursor.attributes.foreground = COLOR_BLUE;
               break;
          case 35:
               terminal->cursor.attributes.foreground = COLOR_MAGENTA;
               break;
          case 36:
               terminal->cursor.attributes.foreground = COLOR_CYAN;
               break;
          case 37:
               terminal->cursor.attributes.foreground = COLOR_WHITE;
               break;
          case 38: // TODO: reserved fg color
               break;
          case 39:
               terminal->cursor.attributes.foreground = COLOR_FOREGROUND;
               break;
          case 40:
               terminal->cursor.attributes.background = COLOR_BLACK;
               break;
          case 41:
               terminal->cursor.attributes.background = COLOR_RED;
               break;
          case 42:
               terminal->cursor.attributes.background = COLOR_GREEN;
               break;
          case 43:
               terminal->cursor.attributes.background = COLOR_YELLOW;
               break;
          case 44:
               terminal->cursor.attributes.background = COLOR_BLUE;
               break;
          case 45:
               terminal->cursor.attributes.background = COLOR_MAGENTA;
               break;
          case 46:
               terminal->cursor.attributes.background = COLOR_CYAN;
               break;
          case 47:
               terminal->cursor.attributes.background = COLOR_WHITE;
               break;
          case 48: // TODO: reserved bg color
               break;
          case 49:
               terminal->cursor.attributes.background = COLOR_BACKGROUND;
               break;
          case 90:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_BLACK;
               break;
          case 91:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_RED;
               break;
          case 92:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_GREEN;
               break;
          case 93:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_YELLOW;
               break;
          case 94:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_BLUE;
               break;
          case 95:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_MAGENTA;
               break;
          case 96:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_CYAN;
               break;
          case 97:
               terminal->cursor.attributes.foreground = COLOR_BRIGHT_WHITE;
               break;
          case 100:
               terminal->cursor.attributes.background = COLOR_BRIGHT_BLACK;
               break;
          case 101:
               terminal->cursor.attributes.background = COLOR_BRIGHT_RED;
               break;
          case 102:
               terminal->cursor.attributes.background = COLOR_BRIGHT_GREEN;
               break;
          case 103:
               terminal->cursor.attributes.background = COLOR_BRIGHT_YELLOW;
               break;
          case 104:
               terminal->cursor.attributes.background = COLOR_BRIGHT_BLUE;
               break;
          case 105:
               terminal->cursor.attributes.background = COLOR_BRIGHT_MAGENTA;
               break;
          case 106:
               terminal->cursor.attributes.background = COLOR_BRIGHT_CYAN;
               break;
          case 107:
               terminal->cursor.attributes.background = COLOR_BRIGHT_WHITE;
               break;
          }
     }
}

bool esc_handle(Terminal_t* terminal, Rune_t rune)
{
     switch(rune) {
     case '[':
          terminal->escape_state |= ESCAPE_STATE_CSI;
          return false;
     case '#':
          terminal->escape_state |= ESCAPE_STATE_TEST;
          return false;
     case '%':
          terminal->escape_state |= ESCAPE_STATE_UTF8;
          return false;
     case 'P': // DCS -- Device Control String
     case '_': // APC -- Application Program Command
     case '^': // PM -- Privacy Message
     case ']': // OSC -- Operating System Command
     case 'k': // old title set compatibility
          str_sequence(terminal, rune);
          return false;
     case 'n': // LS2 -- Locking shift 2
     case 'o': // LS3 -- Locking shift 3
          // TODO
          //term.charset = 2 + (ascii - 'n');
          break;
     case '(': // GZD4 -- set primary charset G0
     case ')': // G1D4 -- set secondary charset G1
     case '*': // G2D4 -- set tertiary charset G2
     case '+': // G3D4 -- set quaternary charset G3
          // TODO
          //term.icharset = ascii - '(';
          //term.esc |= ESC_ALTCHARSET;
          return false;
     case 'D': // IND -- Linefeed
          if(terminal->cursor.y == terminal->bottom){
               terminal_scroll_up(terminal, terminal->top, 1);
          }else{
               terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y + 1);
          }
          break;
     case 'E': // NEL -- Next line
          terminal_put_newline(terminal, 1); // always go to first col
          break;
     case 'H': // HTS -- Horizontal tab stop
          terminal->tabs[terminal->cursor.x] = 1;
          break;
     case 'M': // RI -- Reverse index
          if(terminal->cursor.y == terminal->top){
               terminal_scroll_down(terminal, terminal->top, 1);
          }else{
               terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y - 1);
          }
          break;
     case 'Z': // DECID -- Identify Terminal
          tty_write(terminal->file_descriptor, VT_IDENTIFIER, sizeof(VT_IDENTIFIER) - 1);
          break;
     case 'c': // RIS -- Reset to inital state
          terminal_reset(terminal);
          //resettitle();
          //xloadcols();
          break;
     case '=': // DECPAM -- Application keypad
          terminal->mode |= TERMINAL_MODE_APPKEYPAD;
          break;
     case '>': // DECPNM -- Normal keypad
          terminal->mode &= ~TERMINAL_MODE_APPKEYPAD;
          break;
     case '7': // DECSC -- Save Cursor
          terminal_cursor_save(terminal);
          break;
     case '8': // DECRC -- Restore Cursor
          terminal_cursor_load(terminal);
          break;
     case '\\': // ST -- String Terminator
          if(terminal->escape_state & ESCAPE_STATE_STR_END) str_handle(terminal);
          break;
     default:
          LOG("erresc: unknown sequence ESC 0x%02X '%c' in sequence: '%s'\n", (unsigned char)rune, isprint(rune) ? rune : '.', terminal->csi_escape.buffer);
          break;
     }

     return true;
}

void csi_handle(Terminal_t* terminal)
{
     CSIEscape_t* csi = &terminal->csi_escape;

	switch(csi->mode[0]){
	default:
          LOG("unhandled csi: '%c' in sequence: '%s'\n", csi->mode[0], csi->buffer);
          break;
     case '@':
          DEFAULT(csi->arguments[0], 1);
          terminal_insert_blank(terminal, csi->arguments[0]);
          break;
     case 'A':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y - csi->arguments[0]);
          break;
     case 'B':
     case 'e':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y + csi->arguments[0]);
          break;
     case 'i':
          // TODO
          switch(csi->arguments[0]){
          default:
               break;
          case 0:
               break;
          case 1:
               break;
          case 2:
               break;
          case 4:
               break;
          case 5:
               break;
          }
          break;
     case 'c':
		if (csi->arguments[0] == 0) tty_write(terminal->file_descriptor, VT_IDENTIFIER, sizeof(VT_IDENTIFIER) - 1);
          break;
     case 'C':
     case 'a':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, terminal->cursor.x + csi->arguments[0], terminal->cursor.y);
          break;
     case 'D':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, terminal->cursor.x - csi->arguments[0], terminal->cursor.y);
          break;
     case 'E':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, 0, terminal->cursor.y + csi->arguments[0]);
          break;
     case 'F':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, 0, terminal->cursor.y - csi->arguments[0]);
          break;
     case 'g':
          switch(csi->arguments[0]){
          default:
               break;
          case 0: // clear tab stop
               terminal->tabs[terminal->cursor.x] = 0;
               break;
          case 3: // clear all tabs
               memset(terminal->tabs, 0, terminal->columns * sizeof(*terminal->tabs));
               break;
          }
          break;
     case 'G':
     case '`':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to(terminal, csi->arguments[0] - 1, terminal->cursor.y);
          break;
     case 'H':
     case 'f':
          DEFAULT(csi->arguments[0], 1);
          DEFAULT(csi->arguments[1], 1);
          terminal_move_cursor_to_absolute(terminal, csi->arguments[1] - 1, csi->arguments[0] - 1);
          break;
     case 'I':
          DEFAULT(csi->arguments[0], 1);
          terminal_put_tab(terminal, csi->arguments[0]);
          break;
     case 'J': // clear region in relation to cursor
          switch(csi->arguments[0]){
          default:
               break;
          case 0: // below
               terminal_clear_region(terminal, terminal->cursor.x, terminal->cursor.y, terminal->columns - 1, terminal->cursor.y);
               if(terminal->cursor.y < (terminal->rows - 1)){
                    terminal_clear_region(terminal, 0, terminal->cursor.y + 1, terminal->columns - 1, terminal->rows - 1);
               }
               break;
          case 1: // above
               if(terminal->cursor.y > 1){
                    terminal_clear_region(terminal, 0, 0, terminal->columns - 1, terminal->cursor.y - 1);
               }
               terminal_clear_region(terminal, 0, terminal->cursor.y, terminal->cursor.x, terminal->cursor.y);
               break;
          case 2: // all
               terminal_clear_region(terminal, 0, 0, terminal->columns - 1, terminal->rows - 1);
               break;
          }
          break;
     case 'K': // clear line
          switch(csi->arguments[0]){
          default:
               break;
          case 0: // right of cursor
               terminal_clear_region(terminal, terminal->cursor.x, terminal->cursor.y, terminal->columns - 1, terminal->cursor.y);
               break;
          case 1: // left of cursor
               terminal_clear_region(terminal, 0, terminal->cursor.y, terminal->cursor.x, terminal->cursor.y);
               break;
          case 2: // all
               terminal_clear_region(terminal, 0, terminal->cursor.y, terminal->columns - 1, terminal->cursor.y);
               break;
          }
          break;
     case 'S':
          DEFAULT(csi->arguments[0], 1);
          terminal_scroll_up(terminal, terminal->top, csi->arguments[0]);
          break;
     case 'T':
          DEFAULT(csi->arguments[0], 1);
          terminal_scroll_down(terminal, terminal->top, csi->arguments[0]);
          break;
     case 'L':
          DEFAULT(csi->arguments[0], 1);
          terminal_insert_blank_line(terminal, csi->arguments[0]);
          break;
     case 'l':
          terminal_set_mode(terminal, false);
          break;
     case 'M':
          DEFAULT(csi->arguments[0], 1);
          terminal_delete_line(terminal, csi->arguments[0]);
          break;
     case 'X':
          DEFAULT(csi->arguments[0], 1);
          terminal_clear_region(terminal, terminal->cursor.x, terminal->cursor.y, terminal->cursor.x + csi->arguments[0] - 1, terminal->cursor.y);
          break;
     case 'P':
          DEFAULT(csi->arguments[0], 1);
          terminal_delete_char(terminal, csi->arguments[0]);
          break;
     case 'Z':
          DEFAULT(csi->arguments[0], 1);
          terminal_put_tab(terminal, -csi->arguments[0]);
          break;
     case 'd':
          DEFAULT(csi->arguments[0], 1);
          terminal_move_cursor_to_absolute(terminal, terminal->cursor.x, csi->arguments[0] - 1);
          break;
     case 'h':
          terminal_set_mode(terminal, true);
          break;
     case 'm':
          terminal_set_attributes(terminal);
          break;
     case 'n':
          if(csi->arguments[0] == 6){
               char buffer[BUFSIZ];
               int len = snprintf(buffer, BUFSIZ, "\033[%i;%iR",
                              terminal->cursor.x, terminal->cursor.y);
               tty_write(terminal->file_descriptor, buffer, len);
          }
          break;
     case 'r':
          if(!csi->private){
               DEFAULT(csi->arguments[0], 1);
               DEFAULT(csi->arguments[1], terminal->rows);
               terminal_set_scroll(terminal, csi->arguments[0] - 1, csi->arguments[1] - 1);
               terminal_move_cursor_to_absolute(terminal, 0, 0);
          }
          break;
     case 's':
          terminal_cursor_save(terminal);
          break;
     case 'u':
          terminal_cursor_load(terminal);
          break;
     case ' ': // cursor style
          break;
     }
}

void str_parse(Terminal_t* terminal)
{
     STREscape_t* str = &terminal->str_escape;
     int c;
     char *p = str->buffer;

     str->argument_count = 0;
     str->buffer[str->buffer_length] = 0;

     if(*p == 0) return;

     while(str->argument_count < ESCAPE_ARGUMENT_SIZE){
          str->arguments[str->argument_count] = p;
          str->argument_count++;

          while((c = *p) != ';' && c != 0){
               ++p;
          }

          if(c == 0) return;
          *p++ = 0;
     }
}

void str_sequence(Terminal_t* terminal, Rune_t rune)
{
     memset(&terminal->str_escape, 0, sizeof(terminal->str_escape));

     switch(rune){
     default:
          break;
     case 0x90:
          rune = 'P';
          terminal->escape_state |= ESCAPE_STATE_DCS;
          break;
     case 0x9f:
          rune = '_';
          break;
     case 0x9e:
          rune = '^';
          break;
     case 0x9d:
          rune = ']';
          break;
     }

     terminal->str_escape.type = rune;
     terminal->escape_state |= ESCAPE_STATE_STR;
}

void str_handle(Terminal_t* terminal)
{
     STREscape_t* str = &terminal->str_escape;

     terminal->escape_state &= ~(ESCAPE_STATE_STR_END | ESCAPE_STATE_STR);
     str_parse(terminal);
     int argument_count = str->argument_count;
     int param = argument_count ? atoi(str->arguments[0]) : 0;

     switch(str->type){
     default:
          break;
     case ']':
          switch(param){
          default:
               break;
          case 0:
          case 1:
          case 2:
               break;
          case 52:
               break;
          case 4:
          case 104:
               break;
          }
          break;
     case 'k':
          break;
     case 'P':
          terminal->mode |= ESCAPE_STATE_DCS;
          break;
     case '_':
          break;
     case '^':
          break;
     }
}

void terminal_put(Terminal_t* terminal, Rune_t rune)
{
     char characters[UTF8_SIZE];
     int width = 1;
     int len = 0;

     if(terminal->mode & TERMINAL_MODE_UTF8){
          utf8_encode(rune, characters, UTF8_SIZE, &len);
     }else{
          characters[0] = rune;
          width = 1;
     }

     if(terminal->escape_state & ESCAPE_STATE_STR){
          if(rune == '\a' || rune == 030 || rune == 032 || rune == 033 || is_controller_c1(rune)){
               terminal->escape_state &= ~(ESCAPE_STATE_START | ESCAPE_STATE_STR | ESCAPE_STATE_DCS);
               if(terminal->mode & TERMINAL_MODE_SIXEL){
                    CHANGE_BIT(terminal->mode, 0, TERMINAL_MODE_SIXEL);
                    return;
               }

               terminal->escape_state |= ESCAPE_STATE_STR_END;
          }else{
               if(terminal->mode & TERMINAL_MODE_SIXEL){
                    return;
               }

               if(terminal->escape_state & ESCAPE_STATE_DCS && terminal->str_escape.buffer_length == 0 && rune == 'q'){
                    terminal->mode |= TERMINAL_MODE_SIXEL;
               }

               if(terminal->str_escape.buffer_length + 1 >= (ESCAPE_BUFFER_SIZE - 1)){
                    return;
               }

               terminal->str_escape.buffer[terminal->str_escape.buffer_length] = rune;
               terminal->str_escape.buffer_length++;
               return;
          }
     }

     if(is_controller(rune)){
          terminal_control_code(terminal, rune);
          return;
     }

     if(terminal->escape_state & ESCAPE_STATE_START){
          if(terminal->escape_state & ESCAPE_STATE_CSI){
               CSIEscape_t* csi = &terminal->csi_escape;
               csi->buffer[csi->buffer_length] = rune;
               csi->buffer_length++;

               if(BETWEEN(rune, 0x40, 0x7E) || csi->buffer_length >= (ESCAPE_BUFFER_SIZE - 1)){
                    terminal->escape_state = 0;
                    csi_parse(csi);
                    csi_handle(terminal);
               }

               return;
          }else if(terminal->escape_state & ESCAPE_STATE_UTF8){
               if(rune == 'G'){
                    terminal->mode |= TERMINAL_MODE_UTF8;
               }else if(rune == '@'){
                    terminal->mode &= ~TERMINAL_MODE_UTF8;
               }
          }else if(terminal->escape_state & ESCAPE_STATE_ALTCHARSET){
               // TODO
          }else if(terminal->escape_state & ESCAPE_STATE_TEST){
               // TODO
          }else{
               if(!esc_handle(terminal, rune)) return;
          }

          terminal->escape_state = 0;
          return;
     }

     Glyph_t* current_glyph = terminal->lines[terminal->cursor.y] + terminal->cursor.x;
     if(terminal->mode & TERMINAL_MODE_WRAP && terminal->cursor.state & CURSOR_STATE_WRAPNEXT){
          current_glyph->attributes |= GLYPH_ATTRIBUTE_WRAP;
          terminal_put_newline(terminal, true);
          current_glyph = terminal->lines[terminal->cursor.y] + terminal->cursor.x;
     }

     if(terminal->mode & TERMINAL_MODE_INSERT && terminal->cursor.x + width < terminal->columns){
          memmove(current_glyph + width, current_glyph, (terminal->columns - terminal->cursor.x - width) * sizeof(*current_glyph));
     }

     if(terminal->cursor.x + width > terminal->columns){
          terminal_put_newline(terminal, true);
     }

     terminal_set_glyph(terminal, rune, &terminal->cursor.attributes, terminal->cursor.x, terminal->cursor.y);

     if(terminal->cursor.x + width < terminal->columns){
          terminal_move_cursor_to(terminal, terminal->cursor.x + width, terminal->cursor.y);
     }else{
          terminal->cursor.state |= CURSOR_STATE_WRAPNEXT;
     }
}

void terminal_echo(Terminal_t* terminal, Rune_t rune)
{
     if(is_controller(rune)){
          if(rune & 0x80){
               rune &= 0x7f;
               terminal_put(terminal, '^');
               terminal_put(terminal, '[');
          }else if(rune != '\n' && rune != '\r' && rune != '\t'){
               rune ^= 0x40;
               terminal_put(terminal, '^');
          }
     }

     terminal_put(terminal, rune);
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

bool tty_write(int file_descriptor, const char* string, size_t len)
{
     ssize_t written = 0;

     while(written < len){
          ssize_t rc = write(file_descriptor, string, len - written);

          if(rc < 0){
               printf("%s() write() to terminal failed: %s", __FUNCTION__, strerror(errno));
               return false;
          }

          written += rc;
          string += rc;
     }

     return true;
}

void* tty_reader(void* data)
{
     TTYThreadData_t* thread_data = (TTYThreadData_t*)(data);

     char buffer[BUFSIZ];
     int buffer_length = 0;
     Rune_t decoded;
     size_t decoded_length;

     while(true){
          int rc = read(thread_data->terminal->file_descriptor, buffer, ELEM_COUNT(buffer));

          if(rc < 0){
               LOG("%s() failed to read from tty file descriptor: '%s'\n", __FUNCTION__, strerror(errno));
               return NULL;
          }

          buffer_length = rc;

          for(int i = 0; i < buffer_length; ++i){
               if(utf8_decode(buffer + i, buffer_length, &decoded_length, &decoded)){
                    terminal_put(thread_data->terminal, decoded);
                    i += (decoded_length - 1);
               }
          }

          sleep(0);
     }
}

void* tty_write_keys(void* data)
{
     TTYThreadData_t* thread_data = (TTYThreadData_t*)(data);
     int rc, key;
     char character = 0;
     char* string = NULL;
     size_t len = 0;
     bool free_string = false;

     while(true){
          key = getch();
          string = keybound(key, 0);

          if(!string){
               free_string = false;
               len = 1;

               switch(key){
               default:
                    character = key;
                    break;
               // damnit curses
               case 10:
                    character = 13;
                    break;
               }

               string = (char*)(&character);
          }else{
               free_string = true;
               len = strlen(string);
          }

          switch(key){
          default:
               break;
          case 17:
               g_quit = true;
               break;
          }

          rc = write(thread_data->terminal->file_descriptor, string, len);
          if(rc < 0){
               printf("%s() write() to terminal failed: %s", __FUNCTION__, strerror(errno));
               return NULL;
          }

          if(thread_data->terminal->mode & TERMINAL_MODE_ECHO){
               for(int i = 0; i < len; i++){
                    terminal_echo(thread_data->terminal, string[i]);
               }
          }

          if(free_string) free(string);
     }

     return NULL;
}

bool utf8_decode(const char* buffer, size_t buffer_len, size_t* len, Rune_t* u)
{
     if(buffer_len < 1) return false;

     // 0xxxxxxx is just ascii
     if((buffer[0] & 0x80) == 0){
          *len = 1;
          *u = buffer[0];
     // 110xxxxx is a 2 byte utf8 string
     }else if((char)(buffer[0] & ~0x1F) == (char)(0xC0)){
          if(buffer_len < 2) return false;

          *len = 2;
          *u = buffer[0] & 0x1F;
          *u <<= 6;
          *u |= buffer[1] & 0x3F;
     // 1110xxxx is a 3 byte utf8 string
     }else if((char)(buffer[0] & ~0x0F) == (char)(0xE0)){
          if(buffer_len < 3) return false;

          *len = 3;
          *u = buffer[0] & 0x0F;
          *u <<= 6;
          *u |= buffer[1] & 0x3F;
          *u <<= 6;
          *u |= buffer[2] & 0x3F;
     // 11110xxx is a 4 byte utf8 string
     }else if((char)(buffer[0] & ~0x07) == (char)(0xF0)){
          if(buffer_len < 4) return false;

          *len = 4;
          *u = buffer[0] & 0x0F;
          *u <<= 6;
          *u |= buffer[1] & 0x3F;
          *u <<= 6;
          *u |= buffer[2] & 0x3F;
          *u <<= 6;
          *u |= buffer[3] & 0x3F;
     }else{
          return false;
     }

     return true;
}

bool utf8_encode(Rune_t u, char* buffer, size_t buffer_len, int* len)
{
     if(u < 0x80){
          if(buffer_len < 1) return false;
          *len = 1;

          // leave as-is
          buffer[0] = u;
     }else if(u < 0x0800){
          if(buffer_len < 2) return false;
          *len = 2;

          // u = 00000000 00000000 00000abc defghijk

          // 2 bytes
          // first byte:  110abcde
          buffer[0] = 0xC0 | ((u >> 6) & 0x1f);

          // second byte: 10fghijk
          buffer[1] = 0x80 | (u & 0x3f);
     }else if(u < 0x10000){
          if(buffer_len < 3) return false;
          *len = 3;

          // u = 00000000 00000000 abcdefgh ijklmnop

          // 3 bytes
          // first byte:  1110abcd
          buffer[0] = 0xE0 | ((u >> 12) & 0x0F);

          // second byte: 10efghij
          buffer[1] = 0x80 | ((u >> 6) & 0x3F);

          // third byte:  10klmnop
          buffer[2] = 0x80 | (u & 0x3F);
     }else if(u < 0x110000){
          if(buffer_len < 4) return false;
          *len = 4;

          // u = 00000000 000abcde fghijklm nopqrstu

          // 4 bytes
          // first byte:  11110abc
          buffer[0] = 0xF0 | ((u >> 18) & 0x07);
          // second byte: 10defghi
          buffer[1] = 0x80 | ((u >> 12) & 0x3F);
          // third byte:  10jklmno
          buffer[2] = 0x80 | ((u >> 6) & 0x3F);
          // fourth byte: 10pqrstu
          buffer[3] = 0x80 | (u & 0x3F);
     }

     return true;
}

int main(int argc, char** argv)
{
     setlocale(LC_ALL, "");

     // setup log
     {
          g_log = fopen(LOGFILE_NAME, "w");
          if(!g_log){
               fprintf(stderr, "failed to create log file: %s\n", LOGFILE_NAME);
               return 1;
          }
     }

     Terminal_t terminal = {};
     int tty_file_descriptor;
     pid_t tty_pid;

     // init terminal structure
     {
          terminal.columns = 80;
          terminal.rows = 24;
          terminal.bottom = terminal.rows - 1;

          // allocate lines
          terminal.lines = calloc(terminal.rows, sizeof(*terminal.lines));
          for(int r = 0; r < terminal.rows; ++r){
               terminal.lines[r] = calloc(terminal.columns, sizeof(*terminal.lines[r]));

               // default fg and bg
               for(int g = 0; g < terminal.columns; ++g){
                    terminal.lines[r][g].foreground = -1;
                    terminal.lines[r][g].background = -1;
               }
          }

          terminal.alternate_lines = calloc(terminal.rows, sizeof(*terminal.alternate_lines));
          for(int r = 0; r < terminal.rows; ++r){
               terminal.alternate_lines[r] = calloc(terminal.columns, sizeof(*terminal.alternate_lines[r]));
          }

          terminal.tabs = calloc(terminal.columns, sizeof(*terminal.tabs));
          terminal.dirty_lines = calloc(terminal.rows, sizeof(*terminal.dirty_lines));
          terminal_reset(&terminal);
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

     ColorDefs_t color_defs;
     color_defs.count = 0;

     pthread_t tty_read_thread;
     pthread_t tty_write_thread;

     // create terminal
     {
          if(!tty_create(terminal.rows, terminal.columns, &tty_pid, &tty_file_descriptor)){
               return 1;
          }

          terminal.file_descriptor = tty_file_descriptor;

          TTYThreadData_t* data = calloc(1, sizeof(*data));
          data->terminal = &terminal;

          int rc = pthread_create(&tty_read_thread, NULL, tty_reader, data);
          if(rc != 0){
               LOG("pthread_create() failed: '%s'\n", strerror(errno));
               return 1;
          }
     }

     // setup getch thread
     {
          TTYThreadData_t* data = calloc(1, sizeof(*data));
          data->terminal = &terminal;
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
     int32_t color_pair = 0;
     int32_t last_color_foreground = -1;
     int32_t last_color_background = -1;

     // main program loop
     while(!g_quit){
          gettimeofday(&previous_draw_time, NULL);

          do{
               gettimeofday(&current_draw_time, NULL);
               elapsed = (current_draw_time.tv_sec - previous_draw_time.tv_sec) * 1000000LL +
                         (current_draw_time.tv_usec - previous_draw_time.tv_usec);
          }while(elapsed < DRAW_USEC_LIMIT);

          wstandend(view);
          box(view, 0, 0);

          for(int r = 0; r < terminal.rows; ++r){
               if(terminal.dirty_lines[r]){
                    for(int c = 0; c < terminal.columns; ++c){
                         Glyph_t* glyph = terminal.lines[r] + c;

                         if(last_color_foreground != glyph->foreground || last_color_background != glyph->background){
                              wstandend(view);

                              if(glyph->foreground == COLOR_FOREGROUND && glyph->background == COLOR_BACKGROUND){
                                   // no need to create a new color pair for the default
                              }else{
                                   // does the new glyph match an existing definition?
                                   int32_t matched_pair = -1;
                                   for(int32_t i = 0; i < color_defs.count; ++i){
                                        if(glyph->foreground == color_defs.pairs[i].foreground &&
                                           glyph->background == color_defs.pairs[i].background){
                                             matched_pair = i;
                                             break;
                                        }
                                   }

                                   // if so use that color pair
                                   if(matched_pair >= 0){
                                        wattron(view, COLOR_PAIR(matched_pair));
                                   }else{
                                        // increment the color pair we are going to define, but make sure it wraps around to 0 at the max
                                        color_pair++;
                                        color_pair %= COLOR_PAIRS;
                                        if(color_pair == 0) color_pair++; // when we wrap around, start at 1, because curses doesn't like 0 index color pairs

                                        // create the pair definition
                                        init_pair(color_pair, glyph->foreground, glyph->background);

                                        // set our internal definition
                                        color_defs.pairs[color_pair].foreground = glyph->foreground;
                                        color_defs.pairs[color_pair].background = glyph->background;

                                        // override the count if we haven't wrapped around
                                        int32_t new_count = color_pair;
                                        if(new_count > color_defs.count) color_defs.count = new_count;

                                        wattron(view, COLOR_PAIR(color_pair));
                                   }
                              }

                              last_color_foreground = glyph->foreground;
                              last_color_background = glyph->background;
                         }

                         if(glyph->rune < 0x80){
                              mvwaddch(view, r + 1, c + 1, glyph->rune);
                         }else{
                              char utf8_string[UTF8_SIZE];
                              int len;
                              utf8_encode(glyph->rune, utf8_string, UTF8_SIZE, &len);
                              utf8_string[len] = 0;
                              mvwaddstr(view, r + 1, c + 1, utf8_string);
                         }
                    }

                    terminal.dirty_lines[r] = false;
               }
          }

          wmove(view, terminal.cursor.y + 1, terminal.cursor.x + 1);
          wrefresh(view);
     }

     pthread_cancel(tty_read_thread);
     pthread_join(tty_read_thread, NULL);
     pthread_cancel(tty_write_thread);
     pthread_join(tty_write_thread, NULL);

     // cleanup curses
     delwin(view);
     endwin();

     fclose(g_log);

     return 0;
}
