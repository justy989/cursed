#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
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
//NOTE: used for testing #define TERM_NAME "dumb"
#define TERM_NAME "xterm"
#define UTF8_SIZE 4
#define ESCAPE_BUFFER_SIZE (128 * UTF8_SIZE)
#define ESCAPE_ARGUMENT_SIZE 16
// NOTE: 60 fps limit
#define DRAW_USEC_LIMIT 16666
#define VT_IDENTIFIER "\033[?6c"

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
     int arguments[ESCAPE_ARGUMENT_SIZE];
     uint32_t argument_count;
     char mode[2];
}CSIEscape_t;

typedef struct{
     char type;
     char buffer[ESCAPE_BUFFER_SIZE];
     uint32_t buffer_length;
     int arguments[ESCAPE_ARGUMENT_SIZE];
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
}Terminal_t;

typedef struct{
     Terminal_t* terminal;
}TTYReadData_t;

typedef struct{
     int file_descriptor;
}TTYWriteData_t;

FILE* g_log = NULL;
bool g_quit = false;

Cursor_t g_cursor[2];

bool tty_write(int file_descriptor, const char* string, size_t len);

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

     // clear the original line plus the scroll
     terminal_clear_region(terminal, 0, original, terminal->columns - 1, original + n - 1);
     terminal_set_dirt(terminal, original, terminal->bottom);

     // swap lines to move them all up
     // the cleared lines will end up at the bottom
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

	terminal_set_dirt(terminal, original, terminal->bottom - n);
	terminal_clear_region(terminal, 0, terminal->bottom - n + 1, terminal->columns - 1, terminal->bottom);

	for (int i = terminal->bottom; i >= original + n; i--) {
		temp_line = terminal->lines[i];
		terminal->lines[i] = terminal->lines[i - n];
          terminal->lines[i - n] = temp_line;
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

void terminal_control_code(Terminal_t* terminal, Rune_t rune)
{
     assert(is_controller(rune));

     switch(rune){
     default:
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
          // TODO
		//tstrsequence(ascii);
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

bool esc_handle(Terminal_t* terminal, Rune_t rune)
{
     switch(rune) {
     case '[':
          terminal->escape_state |= ESCAPE_STATE_CSI;
          return 0;
     case '#':
          terminal->escape_state |= ESCAPE_STATE_TEST;
          return 0;
     case '%':
          terminal->escape_state |= ESCAPE_STATE_UTF8;
          return 0;
     case 'P': /* DCS -- Device Control String */
     case '_': /* APC -- Application Program Command */
     case '^': /* PM -- Privacy Message */
     case ']': /* OSC -- Operating System Command */
     case 'k': /* old title set compatibility */
          // TODO
          //tstrsequence(ascii);
          return 0;
     case 'n': /* LS2 -- Locking shift 2 */
     case 'o': /* LS3 -- Locking shift 3 */
          // TODO
          //term.charset = 2 + (ascii - 'n');
          break;
     case '(': /* GZD4 -- set primary charset G0 */
     case ')': /* G1D4 -- set secondary charset G1 */
     case '*': /* G2D4 -- set tertiary charset G2 */
     case '+': /* G3D4 -- set quaternary charset G3 */
          // TODO
          //term.icharset = ascii - '(';
          //term.esc |= ESC_ALTCHARSET;
          return 0;
     case 'D': /* IND -- Linefeed */
          if (terminal->cursor.y == terminal->bottom) {
               terminal_scroll_up(terminal, terminal->top, 1);
          } else {
               terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y + 1);
          }
          break;
     case 'E': /* NEL -- Next line */
          terminal_put_newline(terminal, 1); /* always go to first col */
          break;
     case 'H': /* HTS -- Horizontal tab stop */
          terminal->tabs[terminal->cursor.x] = 1;
          break;
     case 'M': /* RI -- Reverse index */
          if (terminal->cursor.y == terminal->top) {
               terminal_scroll_down(terminal, terminal->top, 1);
          } else {
               terminal_move_cursor_to(terminal, terminal->cursor.x, terminal->cursor.y - 1);
          }
          break;
     case 'Z': /* DECID -- Identify Terminal */
          tty_write(terminal->file_descriptor, VT_IDENTIFIER, sizeof(VT_IDENTIFIER) - 1);
          break;
     case 'c': /* RIS -- Reset to inital state */
          // TODO
          //treset();
          //resettitle();
          //xloadcols();
          break;
     case '=': /* DECPAM -- Application keypad */
          terminal->mode |= TERMINAL_MODE_APPKEYPAD;
          break;
     case '>': /* DECPNM -- Normal keypad */
          terminal->mode &= ~TERMINAL_MODE_APPKEYPAD;
          break;
     case '7': /* DECSC -- Save Cursor */
          // TODO:
          //tcursor(CURSOR_SAVE);
          break;
     case '8': /* DECRC -- Restore Cursor */
          // TODO:
          //tcursor(CURSOR_LOAD);
          break;
     case '\\': /* ST -- String Terminator */
          // TODO:
          //if(terminal->escape_mode & ESCAPE_STATE_STR_END) strhandle();
          break;
     default:
          LOG("erresc: unknown sequence ESC 0x%02X '%c'\n", (unsigned char)rune, isprint(rune) ? rune : '.');
          break;
     }

     return true;
}

void csi_handle(Terminal_t* terminal)
{
     CSIEscape_t* csi = &terminal->csi_escape;

	switch (csi->mode[0]) {
	default:
          break;
     case '@':
          // TODO
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
          // TODO
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
          // TODO: set mode
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
          // TODO
          break;
     case 'm':
          // TODO
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
     case ' ':
          break;
     }
}


void terminal_put(Terminal_t* terminal, Rune_t rune)
{
     if(is_controller(rune)){
          terminal_control_code(terminal, rune);
          return;
     }

     if(terminal->escape_state & ESCAPE_STATE_STR){
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
               // TODO
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
     TTYReadData_t* thread_data = (TTYReadData_t*)(data);

     char buffer[BUFSIZ];
     int buffer_length = 0;

     while(true){
          int rc = read(thread_data->terminal->file_descriptor, buffer, ELEM_COUNT(buffer));

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
     int rc, key;
     char character = 0;
     char* string = NULL;
     size_t len = 0;
     bool free_string = false;

     while(true){
          key = getch();
          string = keybound(key, 0);

          if(!string){
               character = key;
               free_string = false;
               string = (char*)(&character);
               len = 1;
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

          rc = write(thread_data->file_descriptor, string, len);
          if(rc < 0){
               printf("%s() write() to terminal failed: %s", __FUNCTION__, strerror(errno));
               return NULL;
          }

          if(free_string) free(string);
     }

     return NULL;
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
          terminal.bottom = terminal.rows - 1;

          // allocate lines
          terminal.lines = calloc(terminal.rows, sizeof(*terminal.lines));
          for(int r = 0; r < terminal.rows; ++r){
               terminal.lines[r] = calloc(terminal.columns, sizeof(*terminal.lines[r]));
          }

          terminal.alternate_lines = calloc(terminal.rows, sizeof(*terminal.alternate_lines));
          for(int r = 0; r < terminal.rows; ++r){
               terminal.alternate_lines[r] = calloc(terminal.columns, sizeof(*terminal.alternate_lines[r]));
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

          terminal.file_descriptor = tty_file_descriptor;

          TTYReadData_t* data = calloc(1, sizeof(*data));
          data->terminal = &terminal;

          int rc = pthread_create(&tty_read_thread, NULL, tty_reader, data);
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
