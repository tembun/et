/*
 * et -- edit text.
 *
 * The visual text editor.
 */


#include <sys/ioctl.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>


typedef unsigned short US;


/* Size for input/output buffer `buf'. */
#define IOBUF 4096
/* By how many lines the `lns' extended when it is not enough space. */
#define LNS_EXPAND 64
/* By how many lines the line's string is extended when it needs space. */
#define LN_EXPAND 64
/* Which symbol indicates an empty lines. */
#define EMPT_LN_MARK "~"


/* The actual text starts to be printed at this screen row. */
#define BUF_ROW 1
/*
 * The number of columns between tab stops.
 * It makes sense to keep in in sync with your terminal emulator settings.
 */
#define TABSIZE 8
/* How many lines do we scroll down/up. */
#define SCRL_LN 8

/* The length of visual ``ruler'' to be printed in status line. */
#define RULER 80
/* The gap between mode name and cursor position reports in status line. */
#define STATUS_GAP 3

/* A list of `case's for word-separators.  No ":" after last `case'. */
#define CASE_SEPARATOR				\
	case ' ': case '\t': case '_': case '-':\
	case '.': case ':': case '=': case '+':	\
	case '{': case '[': case '(': case '}':	\
	case ']': case ')': case '*': case '|'	\

/* `ESC'. */
#define ESC 27
/* `Delete'. */
#define DEL 8
/* `Backspace'. */
#define BSP 127

/* ``cmd'' mode - when user inputs commands into the prompt. */
#define MOD_CMD 0
/* ``nav'' mode - when we move the cursor, i.e. navigate. */
#define MOD_NAV 1
/* ``edt'' mode - when we edit the text, i.e. insert/delete it. */
#define MOD_EDT 2

/*
 * String command for moving terminal cursor to the row `R' and column `C'.
 * It is not intended to be used "as is".  Rather, it's supposed to be used
 * in `MV_CURS' macro, where it's used as a format-string for `dprintf'.
 */
#define MV_CURS_CMD "\x1b[%d;%dH"
/* Enter reverse video mode, i.e. swap fore- and background colors. */
#define REV_VID_CMD "\x1b[7m"
/* Reset video mode. */
#define VID_RST_CMD "\x1b[0m"
/* Erase the whole screen contents. */
#define ERS_ALL_CMD "\x1b[2J"
/* Erase forward to the end of screen. */
#define ERS_FWD_CMD "\x1b[J"
/* Erase the entire line. */
#define ERS_LINE_ALL_CMD "\x1b[2K"
/* Erase line forward. */
#define ERS_LINE_FWD_CMD "\x1b[K"

/* Expand the string for line at `lns[I]' `B' bytes. */
#define EXPAND_LN(I, B) do {						\
	lns[I]->str = srealloc(lns[I]->str, lns[I]->sz += B);		\
} while(0)

/* Initialize object for line at index `I'. */
#define INIT_LN(I) do {				\
	lns[I] = scalloc(1, sizeof(struct ln));	\
	lns[I]->l = 0;				\
	lns[I]->sz = 0;				\
	lns[I]->str = NULL;			\
	EXPAND_LN(I, LN_EXPAND);		\
} while (0)

/*
 * Write argument (as arguments for `dprintf') in reverse video mode
 * and then exit it (mode).
 */
#define WR_REV_VID(...) do {			\
	dprintf(STDOUT_FILENO, REV_VID_CMD);	\
	dprintf(STDOUT_FILENO, __VA_ARGS__);	\
	dprintf(STDOUT_FILENO, VID_RST_CMD);	\
} while (0)

#define MV_CURS(R, C) do {				\
	dprintf(STDOUT_FILENO, MV_CURS_CMD, R, C);	\
	curs_x = C;					\
	curs_y = R;					\
} while (0)

/*
 * Set terminal cursor to `curs_y' and `curs_x' which are set,
 * _before_ this macro call.
 */
#define SYNC_CURS() dprintf(STDOUT_FILENO, MV_CURS_CMD, curs_y, curs_x)

/* Move cursor right `C' columns. */
#define MV_CURS_R(C) do {	\
	curs_x += C;		\
	SYNC_CURS();		\
} while (0)

/* Move cursor `C' columns left. */
#define MV_CURS_L(C) do {	\
	curs_x -= C;		\
	SYNC_CURS();\
} while (0)

/*
 * Move cursor safely: remember current cursor position before
 * changing it.  The further manual call of `RST_CURS' is expected.
 */
#define MV_CURS_SF(R, C) do {	\
	prev_curs_x = curs_x;	\
	prev_curs_y = curs_y;	\
	MV_CURS(R, C);		\
} while (0)

/*
 * Reset cursor position to the one that was saved by `MV_CURS_SF'.
 */
#define RST_CURS() MV_CURS(prev_curs_y, prev_curs_x)

#define ERS_ALL() dprintf(STDOUT_FILENO, ERS_ALL_CMD)
#define ERS_FWD() dprintf(STDOUT_FILENO, ERS_FWD_CMD)
#define ERS_LINE_ALL() dprintf(STDOUT_FILENO, ERS_LINE_ALL_CMD)
#define ERS_LINE_FWD() dprintf(STDOUT_FILENO, ERS_LINE_FWD_CMD)

/*
 * Actual current position within `lns'.  See `ln_x', `ln_y'.
 */
#define LN_X (off_x + ln_x)
#define LN_Y (off_y + ln_y)

/* Clamp value `A' to `M', if `A' greater than `M'. */
#define CLAMP_MAX(A, M) ((A) > (M) ? (M) : (A))

/*
 * Put a _printable_ character into screen and move cursor one
 * character right.
 */
#define PRINT_CHAR(S) do {		\
	dprintf(STDOUT_FILENO, S);	\
	MV_CURS_R(1);			\
} while (0)

/* Move cursor to the ``cmd'' prompt. */
#define MV_CMD() MV_CURS(ws_row + 2, 1)

/* Clean ``cmd'' line. */
#define CLN_CMD() do {	\
	MV_CMD();	\
	ERS_LINE_ALL();	\
} while (0)

#define SET_FILEPATH(N) do {				\
	filepath = srealloc(filepath, strlen(N)+1);	\
	strcpy(filepath, N);				\
} while (0)

/* Is `C' a valid name for a line mark. */
#define IS_MARK(C) ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z'))
/* Is `C' a printable character. */
#define IS_PRINTABLE(C) ((C) >= ' ' && (C) <= '~')

/* Free structure and string for line at index `I'. */
#define FREE_LN(I) do {		\
	free(lns[(I)]->str);	\
	free(lns[(I)]);		\
} while (0)

/* `dpl_pg' with offset of 0. */
#define DPL_PG() dpl_pg(0)


/* Main text line structure. */
struct ln {
	char*	str;
	size_t	l;
	size_t	sz;
	char	mark;
};


/* Input/output buffer. */
char buf[IOBUF];
/* Buffer for user commands.  Filled by `read_cmd'. */
char cmd[IOBUF];
/* Text that is right now displayed as a result of executing a command. */
char* cmd_txt;
/*
 * Current character index for `cmd'.
 * It is primarily used in `read_cmd', but we make it global because
 * we also need it as an indicator of an entered command length, to
 * redraw the prompt if we need (e.g. on window resize).
 */
int cmd_i;

/* The path of a file the buffer will be written to. */
char* filepath;

/* A list of text lines. */
struct ln** lns;
/* Length of lines (actual number of lines). */
size_t lns_l;
/* Size (reserved space) for `lns' array.  Compare `lns_l'. */
size_t lns_sz;

/* Original termios(4) structure.  I.e. original terminal's settings. */
struct termios orig_tos;
/* Current termios(4) structure.  It is modified in order to enter raw mode. */
struct termios tos;

/*
 * The current mode of the program.  There are three of them:
 *     ``cmd'' - command-prompt; see `MOD_CMD'.
 *     ``nav'' - navigate; see `MOD_NAV'.
 *     ``edt'' - edit; see `MOD_ED'.
 * We start in the ``nav'' one.
 */
char mod;

/*
 * Terminal cursor position (row and column).
 */
US curs_x;
US curs_y;

/*
 * Previous cursor position.  Used for updating some parts of a
 * screen and then restoring the cursor position.
 */
US prev_curs_x;
US prev_curs_y;

/*
 * The terminal cursor position used to be _before_ we escaped to
 * the ``cmd'' prompt (see `esc_cmd').  We keep it to restore it
 * later, when we quit the prompt.
 */
US nav_curs_x;
US nav_curs_y;

/*
 * Number of terminal window rows and columns.
 */
US ws_row;
US ws_col;

/*
 * Current line index (`ln_y') and current offset within its string (`ln_x').
 * These values are in range [0, `ws_row'-1] (`ln_y') and [0, `ws_col'-1]
 * (`ln_x').  Thus, they are limited to the screen.  To get actual offsets
 * within the `lns', use `LN_X' and `LN_Y'.
 */
US ln_x;
US ln_y;

/*
 * Horizontal and vertical offsets of a lines in the screen.
 * They are used to compute `LN_X' and `LN_Y'.
 * --
 * Example: `off_y' of 5 means that the first (topmost) line
 * we see on the screen is `lns[5]' line.
 */
size_t off_x;
size_t off_y;

/* Is buffer dirty. */
char dirty;

/*
 * Set this flag up when an action that changes the cursor
 * position has been done.
 */
char need_print_pos;


/*
 * Print the error message with program's name prefix and exit.
 */
void
die(char* err, ...)
{
	va_list ap;
	va_start(ap, err);
	dprintf(STDERR_FILENO, "[et]: ");
	vdprintf(STDERR_FILENO, err, ap);
	va_end(ap);
	exit(1);
}

/*
 * Safe malloc(3).
 */
void*
smalloc(size_t size)
{
	void* ret;	
	ret = malloc(size);
	if (ret == NULL)
		die("can not allocate %zu bytes.\n", size);
	return ret;
}

/*
 * Safe realloc(3).
 */
void*
srealloc(void* p, size_t size)
{
	void* ret;
	ret = realloc(p, size);
	if (ret == NULL)
		die("can not reallocate %zu bytes.\n", size);	
	return ret;
}

/*
 * Safe calloc(3).
 */
void*
scalloc(size_t n, size_t size)
{
	void* ret;
	ret = calloc(n, size);
	if (ret == NULL)
		die(
"can not allocate %zu objects %zu bytes each.\n", n, size);
	return ret;
}

/*
 * Free `lns', every `ln' and `ln->str' within it.
 */
void
free_lns()
{
	size_t i;
	
	for (i = 0; i < lns_l; ++i)
		FREE_LN(i);
	
	free(lns);
}

/*
 * Free all global pointers.
 */
void
free_all()
{
	free_lns();
	free(filepath);
	free(cmd_txt);
}

/*
 * Terminate the program: free all the data, return to the
 * canonical terminal mode and exit.
 */
void
terminate()
{
	free_all();
	/*
	 * Restore original terminal settings.
	 * --
	 * `TCSANOW' flag is for changes to be applied immediately.
	 */
	if (tcsetattr(STDOUT_FILENO, TCSANOW, &orig_tos) == -1)
		die("can not restore original terminal attributes.\n");
	exit(0);
}

/*
 * Set raw (non-canonical) terminal mode.
 * See termios(4).
 * --
 * It is important to save the terminal's current settings into memory
 * _before_ we alter them, to restore them after editor's closed.
 */
void
set_raw()
{
	/* Save current settings in `tos'. */
	if (tcgetattr(STDOUT_FILENO, &tos) == -1)
		die("can not get terminal attributes.\n");
	
	/* Save (copy) current settings to be able to restore them later. */
	orig_tos = tos;
	
	/* Schedule restoring terminal settings at program's exit. */
	if (atexit(&terminate) == -1)
		die("can not register the exit-function.\n");
	
	/*
	 * The following flags are set to enter non-canonical mode.
	 * More detailed meanings of these are in termios(4).
	 */
	
	/*
	 * Disable:
	 *     `ECHO' and `ECHONL' - to not to echo things back.
	 *     `ICANON' - read byte-by-byte instead of default line-by-line.
	 *     `ISIG' - to turn off default keybinding for signals.
	 *              I.e. `CTRL+C' for `SIGINT'.
	 */
	tos.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG);
	
	/*
	 * Disable:
	 *     `IXON' - prevent software flow control.  These are the things
	 *              like `CTRL+S' (freeze) and `CTRL+Q' (unfreeze).
	 *     `ICRNL' - to not to map `CR' (`\r') to `NL' (`\n').
	 */
	tos.c_iflag &= ~(IXON | ICRNL);
	
	/*
	 * Disable:
	 *     `OPOST' - to turn off output processing, so we need to manually
	 *               send `\r\n' instead of just `\n' every time we want
	 *               to print a new line.
	 */
	tos.c_oflag &= ~OPOST;
	
	/*
	 * read(2) satisfies when either 1 byte has been read or
	 * 100 ms have passed.  It will be very helpful for
	 * handling (in our case, detecting and ignoring) ESCape
	 * sequences (see `input_loop').
	 */
	tos.c_cc[VMIN] = 1;
	tos.c_cc[VTIME] = 1;
	
	/* Apply all changes we have just done. */
	if (tcsetattr(STDOUT_FILENO, TCSANOW, &tos) == -1)
		die("can not set terminal attributes.\n");
}

/*
 * Expand `lns' at the end.
 * Allocate space for:
 *     - `ln' pointer.
 *     - `ln' struct.
 *     - `ln->str' string.
 * 
 */
void
expand_lns()
{
	size_t i;
	
	lns = srealloc(lns, (lns_sz += LNS_EXPAND) * sizeof(struct ln*));
	
	/*
	 * Initialize objects for new lines.
	 */
	for (i = 0; i < LNS_EXPAND; ++i)
		INIT_LN(i+lns_l);
}

/*
 * Check if file at `path' does exist.
 */
char
check_exists(char* path)
{
	struct stat st;
		
	return stat(path, &st) != -1;
}

/*
 * Read contents of a file at file descriptor `fd' into buffer.
 * --
 * The `fd' is _not_ closed in this function.
 * --
 * This function is ought to be called only when we're sure
 * that `lns' has at least been initialized.
 */
void
read_fd(int fd)
{
	/* Actually read bytes. */
	ssize_t arb;
	int i;
	
	while ((arb = read(fd, &buf, IOBUF)) > 0) {
		for (i = 0; i < arb; ++i) {
			if (buf[i] == '\n') {
				lns_l++;
				if (lns_l == lns_sz)
					expand_lns();
				continue;
			}
			
			if (lns[lns_l]->l == lns[lns_l]->sz)
				EXPAND_LN(lns_l, LN_EXPAND);
			
			lns[lns_l]->str[lns[lns_l]->l] = buf[i];
			lns[lns_l]->l++;
		}
	}
	
	if (arb == -1)
		die("error during reading a file.\n");
}

/*
 * If the editor's been invoked with a file path, then
 * we need to read the contents of the file at this
 * path.  If there's no file exists at this path, we
 * just open an empty buffer (do not create a file now).
 */
void
handle_filepath(char* path)
{
	/* File descriptor for a target file. */
	int fd;
	
	/*
	 * If file already exists, we open it for reading only,
	 * and the read it, but if it doesn't we create it for
	 * both reading and writing and do _not_ read it,
	 * because it's empty and `lns' has already been
	 * initialized.
	 */
	if (check_exists(path)) {
		fd = open(path, O_RDONLY);
		if (fd == -1)
			die("can not open file at %s.\n", path);
		read_fd(fd);
	}
	else
		return;
	
	if (close(fd) == -1)
		die("can not close the file.\n");
	
	SET_FILEPATH(path);
}

/*
 * Print current editor mode: ``NAV'' or ``EDT''.  There's no need to
 * print the ``CMD'' mode, because it uses the same line as status does.
 */
void
print_mod()
{
	MV_CURS_SF(ws_row+1, 1);
	/* Erase the current mode. */
	dprintf(STDOUT_FILENO, "   \r");
	WR_REV_VID("%s", mod == MOD_NAV ? "NAV" : "EDT");
	RST_CURS();
}

/*
 * Display a current cursor coordinates.
 * This function is the last one called in the `print_status',
 * so it prints the trailing whitespaces to form an `RULER'
 * characters long ruler.
 */
void
print_pos()
{
	/*
	 * Skip 3-characters long mode name.
	 */
	MV_CURS_SF(ws_row+1, 4);
	ERS_LINE_FWD();
	WR_REV_VID("%*s", RULER-3-STATUS_GAP, "");
	RST_CURS();
	MV_CURS_SF(ws_row+1, 4);
	WR_REV_VID("%*s", STATUS_GAP, "");
	WR_REV_VID("%zu, %zu", LN_Y+1, LN_X+1);
	RST_CURS();
}

/*
 * Print the editor status-line, where editor mode dwells.
 * Every function is called here writes the text in reverse
 * video mode and the last function prints an empty tail of
 * whitespaces (in reverse mode too) to form an `RULER'
 * characters long ruler.
 */
void
print_status()
{	
	print_mod();
	print_pos();
}

/*
 * Redraw a ``cmd'' line with its current state (command that is
 * currently typed here or a result of a command, that is displayed
 * on the screen.
 */
void
print_cmd()
{
	CLN_CMD();
	/*
	 * If right now there is a text printed as a result of a
	 * previous command, we re-print that again, otherwise,
	 * we print the command (not completed yet) that was in
	 * the process of typing before we called this function.
	 */
	if (cmd_txt == NULL) {		
		PRINT_CHAR(":");
		write(STDOUT_FILENO, &cmd, cmd_i);
	}
	else
		WR_REV_VID("%s", cmd_txt);
}

/*
 * Initial terminal setup before starting printing the text out:
 *     - Clear the screen.
 *     - Print the headline out.
 */
void
setup_terminal()
{
	ERS_ALL();
	print_status();
}

/*
 * Print buffer line at index `idx' [`start', `end').
 */
void
print_ln(size_t idx, size_t start, size_t end)
{
	char* tmp;
	size_t len;
	
	len = end - start;
	
	tmp = smalloc(len);
	strncpy(tmp, lns[idx]->str, len);
	
	write(STDOUT_FILENO, tmp, len);
	
	free(tmp);
}

/*
 * Display the text so that it fits in one screen.
 * The print starts from the current vertical line offset `from'.
 */
void
dpl_pg(US from)
{
	size_t i;
	size_t end;
	/* Number of printable lines. */
	size_t ln_num;
	/* Number of trailing empty lines. */
	US empt_num;
	size_t off;
	
	off = off_y + from;
	ln_num = lns_l - off;
	
	MV_CURS_SF(from+1, 1);
	
	ERS_FWD();

	/* If lines can not fit the screen. */
	if (ln_num > ws_row-from-1) {
		end = off_y + ws_row;
		empt_num = 0;
	}
	else {
		end = lns_l;
		empt_num = ws_row - ln_num - from;
	}
	
	for (i = off; i < end; ++i) {
		print_ln(i, 0, lns[i]->l);
		write(STDOUT_FILENO, "\n\r", 2);
	}
	
	/*
	 * In case of empty buffer we don't want to print
	 * the empty line marker right on the line the
	 * cursor currently at.
	 */
	if (lns_l == 0) {
		empt_num--;
		curs_y++;
		SYNC_CURS();
	}
	
	/*
	 * Print empty lines, if any.
	 */
	for (i = 0; i < empt_num; ++i)
		dprintf(STDOUT_FILENO, "%s\n\r", EMPT_LN_MARK);
	
	RST_CURS();
	
	if (mod != MOD_CMD)
		print_status();
}

/*
 * Set current mode and update its name in the head line.
 */
void
set_mod(char m)
{
	mod = m;
	if (mod != MOD_CMD)
		print_mod();
}

/*
 * Get next tab stop from the column `col'.
 */
US
nx_tab(US col)
{
	return TABSIZE * ((col-1)/(TABSIZE) + 1) + 1;
}

/*
 * On which column does this (`l_x') character in
 * this (`l_y') line resides.
 * --
 * The reverse of `col2char'.
 */
US
char2col(size_t l_y, size_t l_x)
{
	/* Imaginary column. */
	US curs_tmp;
	/* The index of a character within current `ln'. */
	size_t x;
	
	x = 0;
	curs_tmp = 1;
	while (x != l_x) {
		if (lns[l_y]->str[x] != '\t')
			++curs_tmp;
		else
			curs_tmp = nx_tab(curs_tmp);
		++x;
	}
	
	return curs_tmp;
}

/*
 * What character in `l_y' line is at `col' screen column.
 * If column `col' the actual length of a string, then
 * assume the request is for the last character string.
 * Due to this, it also puts the actual column number
 * (because it doesn't always match the passed `col')
 * in the `res_col'.
 * --
 * The reverse of `char2col'.
 */
size_t
col2char(size_t l_y, US col, US* res_col)
{
	US curs_tmp;
	US nx_tab_col;
	size_t x;
	
	x = 0;
	curs_tmp = 1;
	while (curs_tmp < col && x < lns[l_y]->l) {
		if (lns[l_y]->str[x] != '\t')
			++curs_tmp;
		else {
			nx_tab_col = nx_tab(curs_tmp);
			/*
			 * Handle the case, when there is a tab-stop-gap
			 * _below_ our cursor position.  We decide where
			 * to go: to the begining of a tab stop or to the
			 * end of it depending on which is closer to us.
			 */
			if (nx_tab_col > col) {
				/* Go to the tab stop start. */
				if (col-curs_tmp < nx_tab_col-col) {
					*res_col = curs_tmp;
					return x;
				}
				/* Go to the tab stop end. */
				else {
					*res_col = nx_tab_col;
					return x+1;
				}
			}
			curs_tmp = nx_tab_col;
		}
		++x;
	}
	
	*res_col = curs_tmp;
	return x;
}

/*
 * Navigate text cursor to the right.
 */
void
nav_right()
{
	/*
	 * How many column should we move cursor to the
	 * right.  It is used when we handle tab stops.
	 */
	US step;
	
	if (lns_l == 0)
		return;
	
	/* If it's not the last line character, just move right. */
	if (LN_X != lns[LN_Y]->l) {
		if (lns[LN_Y]->str[LN_X] != '\t')
			step = 1;
		else
			step = nx_tab(curs_x) - curs_x;
		MV_CURS_R(step);
		ln_x++;
	}
	/*
	 * If this is the last character on the _not_
	 * last line in the buffer.
	 */
	else if (LN_Y != lns_l - 1) {
		/* Flag is screen scrolling needed. */
		char scrl;
		
		/*
		 * Scroll is needed only if current line is
		 * the last visible on the screen.
		 */
		scrl = ln_y == ws_row-1;
		
		ln_x = 0;
		curs_x = 1;
		
		if (scrl) {
			off_y++;
			DPL_PG();
		}
		else {
			ln_y++;
			curs_y++;
		}
		SYNC_CURS();
	}
	/* Don't move cursor otherwise. */
	else
		return;
	
	need_print_pos = 1;
}

/*
 * Navigate text cursor to the left.
 */
void
nav_left()
{
	/* If it is not the first line character, then move left. */
	if (LN_X != 0) {
		/* See at `nav_right'. */
		US step;
		
		ln_x--;
		
		if (lns[LN_Y]->str[LN_X] != '\t')
			step = 1;
		else
			step = curs_x - char2col(LN_Y, LN_X);
		MV_CURS_L(step);
	}
	/* If the line is not the first in the buffer. */
	else if (LN_Y != 0) {
		/* Will we do scroll. */
		char scrl;
		
		/*
		 * We do scroll if it's the first visible
		 * line on the screen.
		 */
		scrl = ln_y == 0;
		
		if (scrl) {
			off_y--;
			DPL_PG();
		}
		else {
			ln_y--;
			curs_y--;
		}
		
		ln_x = lns[LN_Y]->l;
		curs_x = char2col(LN_Y, LN_X);
		SYNC_CURS();
	}
	/* Don't move the cursor otherwise. */
	else
		return;
	
	need_print_pos = 1;
}

/*
 * Navigate text cursor down.
 */
void
nav_dwn()
{
	/* Is using scroll. */
	char scrl;
	US nw_curs_x;
	
	/* If last line of a _text_. */
	if (lns_l == 0 || LN_Y == lns_l-1)
		return;
	
	scrl = ln_y == ws_row-1;
	
	if (scrl)
		off_y++;
	else {
		ln_y++;
		curs_y++;
	}
	ln_x = col2char(LN_Y, curs_x, &nw_curs_x);
	curs_x = nw_curs_x;
		
	if (scrl)
		DPL_PG();	
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Navigate text cursor up.
 */
void
nav_up()
{
	/* Does scroll need to be used. */
	char scrl;
	
	/* If first line of a _text_. */
	if (LN_Y == 0)
		return;
	
	scrl = ln_y == 0;
	
	US nw_curs_x;
	
	if (scrl)
		off_y--;
	else {
		curs_y--;
		ln_y--;
	}
	ln_x = col2char(LN_Y, curs_x, &nw_curs_x);
	curs_x = nw_curs_x;
	
	if (scrl)
		DPL_PG();
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Scroll screen `scrl_ln' lines down.
 */
void
scrl_dwn(size_t scrl_ln)
{
	/* Last visible line index of the screen. */
	size_t last_ln = off_y+ws_row-1;
	/* How many lines to _actually_ scroll. */
	size_t scrl_n;
	
	/*
	 * If we're already on the last screen - we have nothing
	 * to scroll.
	 */
	if (off_y+ws_row >= lns_l)
		return;
	
	/*
	 * If we want to scroll out of screen scroll until
	 * the last text line will be the last screen line.
	 */
	if (last_ln+scrl_ln >= lns_l)
		scrl_n = lns_l-1 - last_ln;
	else
		scrl_n = scrl_ln;
	
	/*
	 * If current cursor position will not survive scrolling
	 * (will linger in the top and be lost), then we manually
	 * put it in the begining of a first visible line after
	 * scrolling.
	 */
	if (scrl_n > ln_y || curs_y-scrl_n < BUF_ROW) {
		curs_x = 1;
		curs_y = BUF_ROW;
		ln_x = 0;
		ln_y = 0;
	}
	/*
	 * Otherwise, keep cursor at the same position (but
	 * compensate scrolling down).
	 */
	else {
		curs_y -= scrl_n;
		ln_y -= scrl_n;
	}
	
	off_y += scrl_n;	
	DPL_PG();
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Scroll `scrl_ln' lines up.
 */
void
scrl_up(size_t scrl_ln)
{
	/* How many lines to actually scroll up. */
	size_t scrl_n;
	
	/*
	 * If we're on the first possible screen, then we
	 * physically can not scroll up.
	 */
	if (off_y == 0)
		return;
	
	/*
	 * If we're going to scroll too much up (and cross the
	 * first line), then scroll until first line is the
	 * first on the screen and stop.
	 */
	if (off_y < scrl_ln)
		scrl_n = off_y;
	else
		scrl_n = scrl_ln;
	
	/*
	 * If cursor is about to linger in the bottom (being
	 * not visible), then manually put in at the first
	 * character of a last visible line after scrolling.
	 */
	if (curs_y+scrl_n > ws_row-1) {
		curs_x = 1;
		curs_y = ws_row;
		ln_x = 0;
		ln_y = ws_row-1;
	}
	/*
	 * Otherwise, keep the cursor at the same position
	 * (compensating the scrolling).
	 */
	else {
		curs_y += scrl_n;
		ln_y += scrl_n;
	}
	
	off_y -= scrl_n;
	DPL_PG();
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Scroll to the end of text (so that the last text line is
 * the last visible one) and also set the cursor to the last
 * character in the text.
 */
void
scrl_end()
{
	US last_row;
	
	curs_x = char2col(lns_l-1, lns[lns_l-1]->l);
	ln_x = lns[lns_l-1]->l;
	last_row = lns_l - off_y;
	
	/* If need to scroll the screen. */
	if (last_row > ws_row) {
		off_y = lns_l - ws_row;
		ln_y = ws_row - 1;
		curs_y = ws_row;
		DPL_PG();
	}
	/*
	 * This is the case where the last _text_ line is
	 * seen on the screen, but it is not the last _screen_
	 * line.  It happens when text itself has a few lines
	 * in general, or if we've scrolled out of the screen.
	 * In this case we do not do scroll, just move the
	 * cursor to the end.
	 */
	else {
		ln_y = last_row-1;
		curs_y = last_row;
		SYNC_CURS();
	}
	
	need_print_pos = 1;
}

/*
 * Scroll to the start of text and set the cursor to the first
 * text character.
 */
void
scrl_start()
{
	/* Skip if cursor's already on the first text line and character. */
	if (LN_Y == 0 && LN_X == 0)
		return;
	
	ln_x = 0;
	ln_y = 0;
	curs_x = 1;
	curs_y = 1;
	
	/* If we need to do actual scroll and redraw page. */
	if (off_y != 0) {
		off_y = 0;
		DPL_PG();
	}
	/*
	 * Synchronize cursor even in case it's not needed (if
	 * cursor is already at (1,1)), but it keeps simple.
	 */
	else
		SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Navigate to the line start.
 */
void
nav_ln_start()
{
	/* Skip if we're already there. */
	if (LN_X == 0)
		return;
	
	off_x = 0;
	ln_x = 0;
	curs_x = 1;
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Navigate to the line end.
 */
void
nav_ln_end()
{
	/* Skip if we're already there. */
	if (LN_X == lns[LN_Y]->l)
		return;
	
	ln_x = lns[LN_Y]->l;
	curs_x = char2col(LN_Y, lns[LN_Y]->l);
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Navigate to the next word-boundary..
 * The ``word'' is defined as a sequence of characters
 * that doesn't contain separators (see `CASE_SEPARATOR').
 */
void
nav_word_nx()
{
	size_t i;
	size_t nav_char;
	US nav_col;
	char first;
	
	nav_char = 0;
	first = 1;
	
	/* If we're at the end of line. */
	if (LN_X == lns[LN_Y]->l) {
		/* We're at the end of text - can't move further. */
		if (LN_Y == lns_l - 1)
			return;
		/*
		 * Otherwise navigating to the next word is equivalent
		 * to plain navigating right.
		 */
		nav_right();
		return;
	}
	
	for (i = LN_X; i <= lns[LN_Y]->l; (++i, first = 0)) {
		if (i == lns[LN_Y]->l) {
			nav_char = i;
			break;
		}
		switch (lns[LN_Y]->str[i]) {
		CASE_SEPARATOR:
			/*
			 * Implement navigating to the word boundaries.
			 */
			if (!first)
				nav_char = i;
			else {
				/* Jump through the repeated separators. */
				while (i != lns[LN_Y]->l-1 &&
				    lns[LN_Y]->str[i+1] == lns[LN_Y]->str[i])
					++i;
				nav_char = i+1;
			}
			goto out;
		}
	}
out:
	nav_col = char2col(LN_Y, nav_char);
	ln_x = nav_char;
	curs_x = nav_col;
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Navigate to the previous word-boundary.
 * See the definition of ``word'' in `nav_word_nx'.
 */
void
nav_word_pr()
{
	size_t i;
	size_t nav_char;
	US nav_col;
	char first;
	
	nav_char = 0;
	first = 1;
	
	/* If we're at the first line character. */
	if (LN_X == 0) {
		/* If we're in the first text line - skip. */
		if (LN_Y == 0)
			return;
		/* Otherwise, it's the same as navigate to the left. */
		nav_left();
		return;
	}
	
	for (i = LN_X-1; i >= 0; (--i, first = 0)) {
		if (i == 0) {
			nav_char = 0;
			break;
		}
		switch (lns[LN_Y]->str[i]) {
		CASE_SEPARATOR:
			/*
			 * Implement navigating to the word boundaries.
			 */
			if (!first)
				nav_char = i+1;
			else {
				/* Jump through the repeated separators. */
				while (i != 0 &&
				    lns[LN_Y]->str[i-1] == lns[LN_Y]->str[i])
					--i;
				nav_char = i;
			}
			goto out;
		}
	}
out:
	nav_col = char2col(LN_Y, nav_char);
	ln_x = nav_char;
	curs_x = nav_col;
	SYNC_CURS();
	
	need_print_pos = 1;
}

/*
 * Delete everything in this line that is after current cursor position.
 */
void
del_ln_fwd()
{
	/* If we're on the last _text_ line. */
	char last;
	
	/*
	 * If the cursor is in the middle of line, then
	 * we erase the rest part of this line.  Cursor
	 * stays at the same position.
	 */
	if (lns[LN_Y]->l != 0 || lns_l == 1) {
		lns[LN_Y]->l = LN_X;
		ERS_LINE_FWD();
		return;
	}
	
	/*
	 * If we've reached this, it means we're in the
	 * begining of line and the line itself is empty.
	 * In this case, we delete this whole line (i.e.
	 * remove it from `lns') and move all lines that
	 * were _below_ this line up.  The cursor stays
	 * at same place.  But there are two edge cases:
	 *     1) We've deleted the last _text_ line.  In
	 *        this case we move cursor up so that it
	 *        points to the _current_ last line.  In
	 *        case it was the last _visible_ line we
	 *        also move the _screen_ one line up.
	 *     2) We've deleted the topmost _visible_ line.
	 *        In this case, we move one _screen_ up and
	 *        put the cursor on the last line possible.
	 */
	
	last = LN_Y == lns_l - 1;
	
	FREE_LN(LN_Y);
	
	/* Move all lines that are after the deleted one up. */
	memcpy(&lns[LN_Y], &lns[LN_Y+1],
		(lns_sz-LN_Y-1) * (sizeof(struct ln*)));
	
	lns_l--;
	lns_sz--;
	
	if (last) {
		/* Case #1 (subcase 2). */
		if (ln_y == ws_row - 1 && off_y != 0) {
			/*
			 * Move screen one line up to not render empty
			 * line markers.
			 */
			off_y--;
			DPL_PG();
		}
		/* Case #2. */
		else if (ln_y == 0) {
			/* If previous screen _will_ fit the screen. */
			if (off_y > ws_row)
				off_y -= ws_row;
			else
				off_y = 0;
			
			/* Put cursor on the last possible line. */
			ln_y = CLAMP_MAX(lns_l-off_y, ws_row-1);
			curs_y = ln_y+1;
			
			DPL_PG();
			SYNC_CURS();
		}
		/* Case #1 (subcase 1). */
		else {
			ERS_LINE_FWD();
			/*
			 * A trick to not `dpl_pg'.  In fact, what
			 * we really need is to move cursor one line
			 * up and put an empty line marker below.
			 */
			dprintf(STDOUT_FILENO, EMPT_LN_MARK);
			
			/* Move cursor one line up. */
			ln_y--;
			curs_y--;
			SYNC_CURS();
		}
	}
	/* Redraw all the lines below (not entire page). */
	else
		dpl_pg(ln_y);
	
	dirty = 1;
	need_print_pos = 1;
}

/*
 * Escape to the ``cmd'' prompt: move cursor to the ``cmd'',
 * line, erase it, save the previous cursor position for
 * ``nav'' mode (to restore it later) and print the ":" -
 * the prompt itself.
 */
void
esc_cmd()
{
	/*
	 * At this moment we don't need to store previous
	 * command result text (if it was) anymore.
	 */
	free(cmd_txt);
	cmd_txt = NULL;
	
	/*
	 * We zero the first `cmd' character in case we receive a
	 * `SIGWINCH' before starting entering a new command.  This
	 * will lead to the situation where _previous_ `cmd' will be
	 * written to the ``cmd'' line instead of an empty prompt.
	 */
	cmd[0] = '\0';
	
	/*
	 * Save current text-cursor position to restore it when we
	 * quit the ``cmd''.
	 */
	if (mod == MOD_NAV) {
		nav_curs_x = curs_x;
		nav_curs_y = curs_y;
	}
	CLN_CMD();
	PRINT_CHAR(":");
	set_mod(MOD_CMD);
}

/*
 * Quit the ``cmd'' prompt and return to the last cursor position
 * in the ``nav'' mode, from which we escaped to the prompt.
 */
void
quit_cmd()
{
	CLN_CMD();
	mod = MOD_NAV;
	MV_CURS(nav_curs_y, nav_curs_x);
	print_status();
}

/*
 * Display text message on the bottommost line of the screen,
 * i.e. the line, where user can enter the commands.  The text
 * is printed in reverse-video mode.
 */
void
dpl_cmd_txt(char* msg)
{
	/*
	 * We need to save the printed message in case we'll
	 * need to redraw it (e.g. in case of `SIGWINCH').
	 */
	cmd_txt = srealloc(cmd_txt, strlen(msg)+1);
	strcpy(cmd_txt, msg);
	
	CLN_CMD();
	WR_REV_VID("%s", msg);
	
	/*
	 * For cursor not to hang about in the end.
	 * It makes an illusion of trailing whitespace.
	 */
	curs_x = 0;
	SYNC_CURS();
}

/*
 * Read command from user input.
 * Returns `0' if command has been read and it can be passed
 * for executing, `1' - otherwise.
 * --
 * It reads the input byte-by-byte and populates `cmd' with only
 * _printable_ characters from there.  Then `cmd' buffer is
 * ready to be passed to `do_cmd' for execution.
 * --
 * There are several ways how we can cancel the input:
 *     1) `\n' do that if it is a first character.
 *     2) So do `Backspace' and `Delete'.
 *     3) `ESC' cancels the entire input no matter where it's met.
 */
int
read_cmd()
{
	/*
	 * Flag if it is a first read _printable_ character.  I.e. the
	 * first character that will appear in `cmd'.
	 */
	int first;
	
	cmd_i = 0;
	first = 1;
	
	/*
	 * Read and accumulate the command within `buf'.
	 * `\n' indicates the end of a command.
	 */
	while (1) {
		if (read(STDIN_FILENO, &buf, 1) > 0) {
			switch (*buf) {
			/*
			 * If we meet `ESC' or `Backspace' or `DEL' in the
			 * input there are two cases:
			 *     1) It is the first character.
			 *     2) It is in the middle of an input.
			 * In case 1) we quit the prompt, and in case 2) we
			 * discard the text in the prompt that we've entered.
			 */
			case ESC:
			case BSP:
			case DEL:
				if (first)
					return 1;
				else {
					/*
					 * Erase everything after ":" prompt.
					 */
					MV_CURS(ws_row + 2, 2);
					ERS_LINE_FWD();
					
					/*
					 * Pretend that this loop has just
					 * begun.
					 * --
					 * See comments in `esc_cmd' regarding
					 * zeroing the `cmd'.
					 */
					cmd[0] = '\0';
					cmd_i = 0;
					first = 1;
					continue;
				}
			/*
			 * `\r' is mapped to `\n' manually.
			 */
			case '\r':
			case '\n':
				cmd[cmd_i++] = '\n';
				/*
				 * If `\n' is a first character, then the
				 * command in fact empty and no further
				 * processing is needed.
				 */
				return first;
			default:
				/* Handle only printable characters. */
				if (IS_PRINTABLE(*buf)) {
					/*
					 * Update `first' flag only here,
					 * because we _don't_ count any
					 * unprintable characters.
					 */
					first = 0;
					cmd[cmd_i++] = *buf;
					write(STDOUT_FILENO, &buf, 1);
				}
			}
		}
	}
	
	/*
	 * Actually we can reach this place only if we've entered
	 * `IOBUF' characters and _didn't_ put a `\n' there.  So
	 * it means that input is invalid.
	 */
	return 1;
}

/*
 * ``cmd'' `f' without arguments prints the current filepath,
 * to which the buffer will be saved in case of writing (`w') it.
 * If it's invoked with argument like: `f <path>', then it sets
 * current filepath to `path'.
 * --
 * Return format the same as for `do_cmd'.
 */
int
do_filepath()
{
	char* cmdp;
	
	cmdp = &cmd[1];
	
	switch (*cmdp) {
	case '\n':
		if (filepath == NULL)
			dpl_cmd_txt("<Anonymous>");
		else
			dpl_cmd_txt(filepath);
		return 1;
	case ' ': {
		char* path;
		int i;
		
		cmdp++;
		path = smalloc(PATH_MAX+1);
		
		for (i = 0; i < PATH_MAX; ++i) {
			if (*(cmdp+i) == '\n')
				break;
			path[i] = *(cmdp+i);
		}
		if (i == 0)
			return -1;
		path[i+1] = '\0';
		
		SET_FILEPATH(path);
		free(path);
		return 0;
	}
	default:
		return -1;
	}
}

/*
 * Parse and execute ``mark-line'' command.
 * Format of a return value the same as for `do_cmd'.
 * --
 * A mark itself is expected to be in `cmd[1]'.
 */
int
do_mark_ln()
{
	size_t i;
	
	if (!(IS_MARK(cmd[1])))
		return -1;
	
	/*
	 * If another line already has this mark, remove it
	 * from it (i.e. reassign mark to current line).
	 */
	for (i = 0; i < lns_l; ++i) {
		if (lns[i]->mark == cmd[1]) {
			lns[i]->mark = 0;
			break;
		}
	}
	
	lns[LN_Y]->mark = cmd[1];
	return 0;
}

/*
 * Get line number by mark.
 */
ssize_t
mark2ln(char mark)
{
	size_t i;
	
	for (i = 0; i < lns_l; ++i) {
		if (lns[i]->mark == mark)
			return i;
	}
	
	return -1;
}

/*
 * Jump (i.e. go) to the line number (counting from 1) so that
 * it is situated in the center of a screen (if it can be).
 */
void
jmp_ln(size_t ln_num)
{
	US top_off;
	
	/* Don't do anything if we're already there. */
	if (LN_Y == ln_num - 1)
		return;
	
	top_off = ws_row / 2;
	/*
	 * If line can't be centered (somewhat line in the begining
	 * of the text), the do not center it and display first
	 * screen possible.
	 */
	if (ln_num <= top_off) {
		off_y = 0;
		ln_y = ln_num - 1;
		nav_curs_y = ln_num;
	}
	else {
		off_y = ln_num - top_off;
		ln_y = top_off - 1;
		nav_curs_y = top_off;
	}
	
	ln_x = 0;
	/*
	 * Set `nav_curs_x', not `curs_x', because this is a
	 * ``cmd'' command and after `nav_curs_x' tells where
	 * to put cursor after quitting the prompt.
	 */
	nav_curs_x = 1;
	
	DPL_PG();
}

/*
 * Parse and execute the ``jump-to-line'' command.
 * Return format is the same as for `do_cmd'.
 */
int
do_jmp_ln()
{
	size_t ln_num;
	char* cmdp = &cmd[1];
	
	ln_num = strtol(cmdp, &cmdp, 10);
	if (ln_num == 0) {
		if (IS_MARK(*cmdp) && *(cmdp+1) == '\n')
			ln_num = mark2ln(*cmdp);
		else
			return -1;
	}
	
	/*
	 * Here we can be sure that there is _not_ a negative
	 * or zero value in `ln_num', so we need to check only
	 * for out-of-last-line overflow.
	 */
	if (ln_num > lns_l)
		return -1;
	
	jmp_ln(ln_num);
	return 0;
}

/*
 * Quit the editor.
 */
void
quit()
{
	CLN_CMD();
	terminate();
}

/*
 * Write buffer contents to the file.
 * --
 * Return format obeys to `do_cmd'.
 */
int
do_write_file()
{
	char* cmdp = &cmd[1];
	/* Do we need to quit editor after write. */
	char q;
	/* A filepath the buffer will be written to. */
	char* path;
	/* Did we allocate memory for pathname. */
	char alc_path;
	/* A file descriptor for a target file. */
	int fd;
	/* General purpose iterator. */
	size_t i;
	/* Buffer that will be written to the file. */
	char* wbuf;
	/* Length of a `wbuf'. */
	size_t wbufl;
	/* Iterator of a `wbuf'. */
	size_t wbufi;
	
	q = *cmdp == 'q';
	
	if (q)
		cmdp++;
	
	switch (*cmdp) {
	case '\n':
		alc_path = 0;
		
		if (filepath == NULL) {
			dpl_cmd_txt(
"Which filepath?  Do either `w[q] <path>' or `f <path>'.");
			return 1;
		}
		path = filepath;
		break;
	case ' ':		
		alc_path = 1;
		
		cmdp++;
		path = smalloc(PATH_MAX+1);
		for (i = 0; i < PATH_MAX; ++i) {
			if (*(cmdp+i) == '\n')
				break;
			path[i] = *(cmdp+i);
		}
		if (i == 0)
			return -1;
		path[i+1] = '\0';
		break;
	default:
		return -1;
	}
	
	if (check_exists(path))
		fd = open(path, O_WRONLY | O_TRUNC);
	else
		fd = open(path, O_CREAT | O_RDWR);
	if (fd < 0) {
		dpl_cmd_txt("Can not open the file.");
		return 1;
	}
	/*
	 * Do not free the path if we've used `filepath' for it.
	 */
	if (alc_path)
		free(path);
	
	wbufl = 0;
	for (i = 0; i < lns_l; ++i) {
		wbufl += lns[i]->l+1;
	}
	
	wbuf = smalloc(wbufl);
	wbufi = 0;
	for (i = 0; i < lns_l; ++i) {
		strncpy(wbuf+wbufi, lns[i]->str, lns[i]->l);
		wbufi += lns[i]->l;
		wbuf[wbufi++] = '\n';
	}
	
	if (write(fd, wbuf, wbufl) < 0) {
		free(wbuf);
		dpl_cmd_txt("Error writing file.");
		return 1;
	}
	free(wbuf);
	close(fd);
	
	dirty = 0;
	
	if (q)
		quit();
	
	return 0;
}

/*
 * Execute the command, read by `read_cmd' into `cmd' buffer.
 * Returns `0' if command is successfull and we need to immediately
 * quit the ``cmd'' prompt.
 * Returns `1' if the command is OK,
 * but we do _not_ need to clean things up.  It is for commands
 * which outputs the text to the ``cmd'' as a result of execution.
 * Returns `-1' if command if failed and we need to print an error
 * message on the ``cmd'' line.
 */
int
do_cmd()
{	
	while (*cmd != '\n') {
		switch (*cmd) {
		/*
		 * Quit the editor.
		 */
		case 'q':
		case 'Q':
			if (*(cmd+1) != '\n')
				return 1;
			if (*cmd == 'q' && dirty == 1) {
				dpl_cmd_txt("Can't - the buffer is dirty.");
				return 1;
			}
			quit();
			return 0;
		case 'f':
			return do_filepath();
		case 'j':
			return do_jmp_ln();
		case 'k':
			return do_mark_ln();
		case 'w':
			return do_write_file();
		default:
			return -1;
		}
	}
	
	/* NOTREACHED. */
	return 1;
}

/*
 * Insert one _printable_ character (and tab) under current
 * cursor position.
 */
void
ins_char(char c)
{
	/* Check if we have enough space for this character. */
	if (lns[LN_Y]->l + 1 > lns[LN_Y]->sz)
		EXPAND_LN(LN_Y, LN_EXPAND);
	
	/*
	 * Shift stirng characters one character to the right,
	 * then insert the character in the empty space and
	 * redraw the rest of the line.
	 */
	memmove(lns[LN_Y]->str+LN_X+1, lns[LN_Y]->str+LN_X,
	    lns[LN_Y]->sz-LN_X);
	ERS_LINE_FWD();
	lns[LN_Y]->str[LN_X] = c;
	lns[LN_Y]->l++;
	/*
	 * In combination with `ERS_LINE_FWD' above it reprints
	 * the rest part of this line only.
	 */
	write(STDOUT_FILENO, lns[LN_Y]->str+LN_X, lns[LN_Y]->l-LN_X);
	/*
	 * We could just inserted tabulation character, that's why
	 * we need to perform a complete navigation routine to keep
	 * cursor in sync with buffer.
	 */
	nav_right();
	SYNC_CURS();
	
	dirty = 1;
	need_print_pos = 1;
}

/*
 * Insert a line break at current cursor position.
 */
void
ins_ln_brk()
{
	/* Pointer to last unused line, that has been allocated. */
	struct ln* last_unus;
	
	if (lns_l + 1 > lns_sz)
		expand_lns();
	
	/*
	 * If we're inserting line break on the last _screen_ line,
	 * but _not_ on the last _text_ line, we firstly scroll down
	 * and then do the same routines we do for general case.
	 */
	if (ln_y == ws_row - 1 && LN_Y != lns_l)
		scrl_dwn(1);
	
	/*
	 * As far as we already have some extra dummy line
	 * structures, let's not waste and lose them, but
	 * use them in the actual text.  But due to their
	 * layout (linear, array-like), we need to first
	 * remember the last line of these: we're going to
	 * shift all the below-lying lines down and insert
	 * this dummy line after current line; in essense,
	 * it breaks down to the task similar to swapping
	 * two variables, which involves a third temporary
	 * variable, which is, in our case, `last_unus'.
	 */
	last_unus = lns[lns_sz-1];
	/* Shift all the lines below one position down. */
	memmove(lns+LN_Y+2, lns+LN_Y+1,
	    (lns_sz-LN_Y) * sizeof(struct ln*));
	
	/*
	 * Insert early-pre-initialized line structure
	 * after current line.
	 */
	lns[LN_Y+1] = last_unus;
	lns_l++;
	
	/*
	 * The hunk of a line, that used to be after cursor,
	 * we now transfer to the just-inserted line structure.
	 */
	lns[LN_Y+1]->sz = lns[LN_Y+1]->l = lns[LN_Y]->l - LN_X;
	memcpy(lns[LN_Y+1]->str, lns[LN_Y]->str+LN_X,
	    lns[LN_Y]->sz-LN_X);
	
	/* Trim the current line to its present length. */
	lns[LN_Y]->l = LN_X;
	
	/*
	 * Visually clean up the rest of the current line
	 * and move the cursor to the newly inserted line.
	 */
	ERS_FWD();
	ln_x = 0;
	ln_y++;
	curs_x = 1;
	/*
	 * Important: we _do_ increment `curs_y' even in case we're
	 * at the last _text_ line and at the last _screen_ line.
	 * We do that because in this case we will `scrl_dwn' one line
	 * and its implementation does decrease the `curs_y' to keep
	 * cursor at the same _text_ line.  But this is an edge case:
	 * we need to scroll down meanwhile staying at the same _visual_
	 * line.  If we increment `curs_y' first, then it'll be decreased
	 * in `scrl_dwn', then it will stay the same.
	 */
	curs_y++;
	if (ln_y == ws_row && LN_Y == lns_l-1) {
		SYNC_CURS();
		scrl_dwn(1);
	}
	else
		dpl_pg(ln_y);
	
	dirty = 1;
	need_print_pos = 1;
}

/*
 * Delete one character backward at current cursor position.
 */
void
del_char_back()
{
	/*
	 * If we're about to delete first character in the line,
	 * then, if there is a line above, we want to append
	 * current line to the end of it and move all the lines
	 * below up.
	 */
	if (LN_X == 0) {
		/*
		 * Length of current line.
		 * --
		 * We need it as separate variable, because due
		 * to the order of actions, we need to free this
		 * line first, and then use its length.
		 */
		size_t pr_len;
		
		if (LN_Y == 0)
			return;
		if (ln_y == 0)
			scrl_up(1);
		
		/* Check, if line above has a room for current line. */
		if (lns[LN_Y-1]->l + lns[LN_Y]->l > lns[LN_Y-1]->sz)
			EXPAND_LN(LN_Y-1, lns[LN_Y-1]->l + lns[LN_Y]->l -
			    lns[LN_Y-1]->sz);
		/*
		 * Append current line to the end of line above (in the
		 * data structure).
		 */
		memcpy(lns[LN_Y-1]->str+lns[LN_Y-1]->l, lns[LN_Y]->str,
		    lns[LN_Y]->l);
		
		pr_len = lns[LN_Y]->l;
		FREE_LN(LN_Y);
		/* Move all the lines that were below current line, up. */
		memcpy(lns+LN_Y, lns+LN_Y+1,
		    (lns_sz-LN_Y) * sizeof(struct ln*));
		
		/*
		 * Due to the way the `dpl_pg' works (will call it in
		 * the end of this branch), it will not do anything if
		 * we pass the last text line index into it.  Hence,
		 * in case we've just taken action on the last line,
		 * we manually patch the visual buffer, to update only
		 * the parts we need.  We need to put an empty line
		 * marker on current line, because it was the last one.
		 */
		if (LN_Y == lns_l - 1) {
			ERS_LINE_FWD();
			dprintf(STDOUT_FILENO, EMPT_LN_MARK);
		}
		
		/*
		 * Move cursor the the end of previous line (that end
		 * that was _before_ appending the current line).
		 */
		lns_l--;
		lns_sz--;
		curs_y--;
		curs_x = char2col(LN_Y-1, lns[LN_Y-1]->l);
		ln_y--;
		ln_x = lns[LN_Y]->l;
		SYNC_CURS();
		
		/*
		 * Visually append current line to the end of
		 * the previous one.
		 */
		write(STDOUT_FILENO, lns[LN_Y]->str+lns[LN_Y]->l, pr_len);
		/*
		 * So far we've being referring to previous line initial
		 * length, but from now on, we are not going to do this
		 * anymore and we can alter it.
		 */
		lns[LN_Y]->l += pr_len;
		
		/*
		 * `dpl_pg' makes sense only in case of a not-last line
		 * (see few comments above).
		 */
		if (LN_Y != lns_l-1)
			dpl_pg(ln_y+1);
		
		dirty = 1;
		need_print_pos = 1;
		
		return;
	}
	
	/*
	 * Plain deleting one character back.
	 * Since we still need to handle tab stops, we employ
	 * the `nav_left', which already includes this logic.
	 */
	
	nav_left();
	
	/*
	 * Shift the entire string one character left.
	 * Bear in mind, that due to the prior call of `nav_left',
	 * we now assume that ``current'' `LN_X' is that one that was
	 * before the original one.
	 */
	memcpy(lns[LN_Y]->str+LN_X, lns[LN_Y]->str+LN_X+1,
	    lns[LN_Y]->sz-LN_X);
	lns[LN_Y]->l--;
	lns[LN_Y]->sz--;
	
	/*
	 * Redraw everything in this line after the cursor.
	 */
	ERS_LINE_FWD();
	write(STDOUT_FILENO, lns[LN_Y]->str+LN_X, lns[LN_Y]->l-LN_X);
}

/*
 * Routine for handling character that user's just entered.
 */
void
handle_char(char c)
{
	if (mod == MOD_EDT && c != CTRL('j') && c != BSP && c != DEL)
		goto put_char;
	
	switch (c) {
	case CTRL('j'):
		if (mod == MOD_NAV) {
			set_mod(MOD_EDT);
			break;
		}
		else if (mod == MOD_EDT) {
			set_mod(MOD_NAV);
			break;
		}
	case '\r':
	case ESC:
	case DEL:
	case BSP:
		/*
		 * If we are here _and_ we _are_ in ``cmd'' mode,
		 * it means that we've just entered invalid
		 * command and see an error on the prompt line.
		 * In this case, if we press either `ESC', `Delete',
		 * `Backspace' or newline, we want to continue being
		 * in ``cmd'' mode, but just remove the error alert.
		 * So that means we want to enter cmd-loop again
		 * and it is done by means of falling through
		 * to the ":" label.
		 */
		if (mod == MOD_EDT)
			goto put_char;
		if (mod == MOD_NAV)
			break;
		/* FALLTHROUGH. */
	case ':':
		esc_cmd();
		if (read_cmd() != 0) {
			quit_cmd();
			break;
		}
		
		/*
		 * `read_cmd' has just read command into `cmd'.
		 */
		switch (do_cmd()) {
		case -1:
			dpl_cmd_txt("Sorry.");
			break;
		case 0:
			quit_cmd();
			break;
		/* `1' means we don't need to do anything. */
		}
		break;
	case ';':
		if (mod == MOD_NAV)
			nav_right();
		break;
	case 'j':
		if (mod == MOD_NAV)
			nav_left();
		break;
	case 'l':
		if (mod == MOD_NAV)
			nav_dwn();
		break;
	case 'k':
		if (mod == MOD_NAV)
			nav_up();
		break;
	case CTRL('l'):
		if (mod == MOD_NAV)
			scrl_dwn(SCRL_LN);
		break;
	case CTRL('k'):
		if (mod == MOD_NAV)
			scrl_up(SCRL_LN);
		break;
	case '>':
		if (mod == MOD_NAV)
			scrl_end();
		break;
	case '<':
		if (mod == MOD_NAV)
			scrl_start();
		break;
	case CTRL('a'):
		if (mod == MOD_NAV)
			nav_ln_start();
		break;
	case CTRL('d'):
		if (mod == MOD_NAV)
			nav_ln_end();
		break;
	case 'd':
		if (mod == MOD_NAV)
			nav_word_nx();
		break;
	case 'a':
		if (mod == MOD_NAV)
			nav_word_pr();
		break;
	case CTRL('e'):
		if (mod == MOD_NAV)
			del_ln_fwd();
		break;
	default:
		if (mod == MOD_EDT) {
put_char:
			if (IS_PRINTABLE(c) || c == '\t')
				ins_char(c);
			/* `Enter' key generates it. */
			else if (c == '\r')
				ins_ln_brk();
			else if (c == BSP)
				del_char_back();
		}
	}
}

/*
 * Infinite loop that handles user input byte-by-byte.
 */
void
input_loop()
{
	while (1) {
		while (read(STDIN_FILENO, &buf, 1) == 1) {
			/*
			 * If we have just read an `ESC' character it either
			 * means that we've just literally hit `ESC' key or
			 * we stroke a key that generates an escape sequence.
			 * We want to completely throw the later out (i.e
			 * ignore it).  We don't need them, 'cause this editor
			 * doesn't make use of any of these.
			 */
			if (buf[0] == ESC) {
				char dummy;
				
				/*
				 * Make use of the `VTIME' flag that we've set
				 * in `set_raw'.  It means, that read(2) will
				 * return if nary characters have been read
				 * within 100 ms.  I.e. it will not hang here.
				 */
				if (read(STDIN_FILENO, &dummy, 1) == 1 &&
				    read(STDIN_FILENO, &dummy, 1) == 1)
				    	continue;
			}
			
			/*
			 * In case it's an ordinary key or a _single_ `ESC',
			 * we do handle that character.
			 */
			handle_char(*buf);
			
			/*
			 * Different actions in `handle_char' can set
			 * `need_print_pos' flag if they adjust the cursor
			 * position.
			 */
			if (need_print_pos) {
				print_pos();
				need_print_pos = 0;
			}
		}
	}
}

/*
 * Obtain information about terminal window size.
 */
void
get_win_sz()
{
	struct winsize win_sz;
	
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &win_sz) == -1)
		die("can not obtain the terminal window size.\n");
	
	/*
	 * One line (at the bottom) is for entering commands and
	 * status bar.
	 */
	ws_row = win_sz.ws_row - 1;
	ws_col = win_sz.ws_col;
}

/*
 * Take an action when window resizes.
 */
void
handle_sigwinch()
{
	get_win_sz();
	/*
	 * If current cursor position will not be visible after
	 * resizing (more accurately, shrinking) the window, we
	 * scroll so that the current line on which the cursor
	 * is is on the last visible line.
	 */
	if (ln_y >= ws_row) {
		off_y += (ln_y - ws_row+1);
		ln_y = ws_row-1;
		curs_y = ws_row;
	}
	DPL_PG();
	if (mod == MOD_CMD)
		print_cmd();
}

/*
 * Set up a routine that will update window size information every
 * time it changes.
 */
void
init_win_sz()
{
	struct sigaction sa;
	
	get_win_sz();
	
	/*
	 * It is important to initialize all fields for `sa',
	 * because otherwise they are filled with random
	 * values and it may (and will) lead to `EINVAL'.
	 */
	sa.sa_handler = handle_sigwinch;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);
}

/*
 * Run the visual editor.
 *
 * If no arguments are provided, then an empty anonymous buffer
 * is created and opened.  You will be prompted to specify a
 * name to write the buffer out to when you make an attempt for
 * ``write'' command.
 *
 * First and the only one available argument is a file path. If
 * there's no filt at this path, then then file will be created
 * for reading and writing and then opened.
 */
int
main(int argc, char** argv)
{
	lns = NULL;
	lns_l = 0;
	lns_sz = 0;
	mod = MOD_NAV;
	off_x = 0;
	off_y = 0;
	ln_x = 0;
	ln_y = 0;
	filepath = NULL;
	need_print_pos = 0;
	
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		die("Both input and output should go to the terminal.\n");
	
	expand_lns();
	
	if (argc > 2)
		die("I can edit only one thing at a time.\n");
	if (argc > 1)
		handle_filepath(argv[1]);
	else
		lns_l = 1;
	
	set_raw();
	setup_terminal();
	init_win_sz();
	
	DPL_PG();
	/* Move cursor to the first visible character. */
	MV_CURS(BUF_ROW, 1);
	input_loop();
	
	terminate();
	
	return 0;
}
