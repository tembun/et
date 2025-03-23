/*
 * et -- edit text.
 *
 * The visual text editor.
 */


#include <sys/ioctl.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <libgen.h>
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

/* The actual text starts to be printed at this screen row. */
#define BUF_ROW 1
/*
 * The number of columns between tab stops.
 * It makes sense to keep in in sync with your terminal emulator settings.
 */
#define TABSIZE 8
/* How many lines do we scroll down/up. */
#define SCRL_LN 8

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

/* Expand the string for line at `lns[I]'. */
#define EXPAND_LN(I) do {						\
	lns[I]->str = srealloc(lns[I]->str, lns[I]->sz += LN_EXPAND);	\
} while(0)

/*
 * Write string `S' in reverse video mode and then exit it (mode).
 * `S' should be put in here _without_ double quotes.
 */
#define WR_REV_VID(S, ...) do {						\
	dprintf(STDOUT_FILENO, REV_VID_CMD #S VID_RST_CMD, __VA_ARGS__);\
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
	filepath = realloc(filepath, strlen(N)+1);	\
	strcpy(filepath, N);				\
} while (0)


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
size_t ln_x;
size_t ln_y;

/*
 * Horizontal and vertical offsets of a lines in the screen.
 * They are used to compute `LN_X' and `LN_Y'.
 * --
 * Example: `off_y' of 5 means that the first (topmost) line
 * we see on the screen is `lns[5]' line.
 */
size_t off_x;
size_t off_y;

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
	
	for (i = 0; i < lns_l; ++i) {
		free(lns[i]->str);
		free(lns[i]);
	}
	
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
	 * `VMIN' is a minimum number of characters read(2) will read before
	 * it will be able to return.  Set it to `1' to be able to read
	 * input byte-by-byte.
	 */
	tos.c_cc[VMIN] = 1;
	
	/*
	 * `VTIME' is how much time should pass before read(2) will be able
	 * to return.  Set it to `0' for it to play no role in input read
	 * and for `VMIN' be significant.
	 */
	tos.c_cc[VTIME] = 0;
	
	/* Apply all changes we have just done. */
	if (tcsetattr(STDOUT_FILENO, TCSANOW, &tos) == -1)
		die("can not set terminal attributes.\n");
}

/*
 * Expand `lns' at index `off'.
 * Allocate space for:
 *     - `ln' pointer.
 *     - `ln' struct.
 *     - `ln->str' string.
 * 
 */
void
expand_lns(size_t off)
{
	size_t i;
	
	lns = srealloc(lns, (lns_sz += LNS_EXPAND) * sizeof(struct ln*));
	
	/*
	 * Initialize objects for new lines.
	 */
	for (i = 0; i < LNS_EXPAND; ++i) {
		lns[i+off] = scalloc(1, sizeof(struct ln));
		lns[i+off]->l = 0;
		lns[i+off]->sz = 0;
		lns[i+off]->str = NULL;
		EXPAND_LN(i+off);
	}
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
					expand_lns(lns_l);
				continue;
			}
			
			if (lns[lns_l]->l == lns[lns_l]->sz)
				EXPAND_LN(lns_l);
			
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
	WR_REV_VID(%s, mod == MOD_NAV ? "NAV" : "EDT");
	RST_CURS();
}

/*
 * Print the editor status-line, where editor mode dwells.
 * This is the same line that used for ``cmd'' prompt.
 */
void
print_status()
{
	print_mod();
	/*
	 * Put the cursor _after_ the mode name (3 characters).
	 */
	MV_CURS_SF(ws_row+1, 4);
	/*
	 * Print an 80-characters (3 characters are for mode name)
	 * long line (in reverse mode) as a ruler.
	 */
	WR_REV_VID(%*s, 77, "");
	RST_CURS();
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
		WR_REV_VID(%s, cmd_txt);
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
 * The print starts from the current vertical line offset `off_y'.
 */
void
dpl_pg()
{
	size_t i;
	size_t end;
	/* Number of printable lines. */
	size_t ln_num;
	/* Number of trailing empty lines. */
	size_t empt_num;
	
	ln_num = lns_l - off_y;
	
	MV_CURS_SF(BUF_ROW, 1);
	ERS_FWD();
	
	/*
	 * If lines can not fit the screen.
	 */
	if (ln_num > ws_row) {
		end = off_y + ws_row;
		empt_num = 0;
	}
	else {
		end = lns_l;
		empt_num = ws_row - ln_num;
	}
	
	for (i = off_y; i < end; ++i) {
		print_ln(i, 0, lns[i]->l);
		dprintf(STDOUT_FILENO, "\n\r");
	}
	
	/*
	 * Print empty lines, if any.
	 */
	for (i = 0; i < empt_num; ++i)
		dprintf(STDOUT_FILENO, "~\n\r");
	
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
			dpl_pg();
		}
		else {
			ln_y++;
			curs_y++;
		}
		SYNC_CURS();
	}
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
			dpl_pg();
		}
		else {
			ln_y--;
			curs_y--;
		}
		
		ln_x = lns[LN_Y]->l;
		curs_x = char2col(LN_Y, LN_X);
		SYNC_CURS();
	}
}

/*
 * Navigate text cursor down.
 */
void
nav_dwn()
{
	/* Is using scroll. */
	char scrl;
	
	scrl = ln_y == ws_row-1 && LN_Y != lns_l-1;
	
	if (scrl)
		off_y++;
	
	/* Not last line of a _text_. */
	if (LN_Y != lns_l-1) {
		US nw_curs_x;
		
		if (!scrl) {
			ln_y++;
			curs_y++;
		}
		ln_x = col2char(LN_Y, curs_x, &nw_curs_x);
		curs_x = nw_curs_x;
		
		if (!scrl)
			SYNC_CURS();
	}
	
	if (scrl) {
		dpl_pg();
		SYNC_CURS();
	}
}

/*
 * Navigate text cursor up.
 */
void
nav_up()
{
	/* Does scroll need to be used. */
	char scrl;
	
	scrl = ln_y == 0 && LN_Y != 0;
	
	if (scrl)
		off_y--;
	
	/* Not a first line in the _text_. */
	if (LN_Y != 0) {
		US nw_curs_x;
		
		if (!scrl) {
			curs_y--;
			ln_y--;
		}
		ln_x = col2char(LN_Y, curs_x, &nw_curs_x);
		curs_x = nw_curs_x;
		if (!scrl)
			SYNC_CURS();
	}
	
	if (scrl) {
		dpl_pg();
		SYNC_CURS();
	}
}

/*
 * Scroll screen `SCRL_LN' lines down.
 */
void
scrl_dwn()
{
	/* Last visible line index of the screen. */
	size_t last_ln = off_y+ws_row-1;
	/* How many lines to _actually_ scroll. */
	size_t scrl_n;
	
	/*
	 * If we're already on the last screen - we have nothing
	 * to scroll.
	 */
	if (off_y+ws_row == lns_l)
		return;
	
	/*
	 * If we want to scroll out of screen scroll until
	 * the last text line will be the last screen line.
	 */
	if (last_ln+SCRL_LN >= lns_l)
		scrl_n = lns_l-1 - last_ln;
	else
		scrl_n = SCRL_LN;
	
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
	dpl_pg();
	SYNC_CURS();
}

/*
 * Scroll `SCRL_LN' lines up.
 */
void
scrl_up()
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
	if (off_y < SCRL_LN)
		scrl_n = off_y;
	else
		scrl_n = SCRL_LN;
	
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
	dpl_pg();
	SYNC_CURS();
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
	cmd_txt = realloc(cmd_txt, strlen(msg)+1);
	strcpy(cmd_txt, msg);
	
	CLN_CMD();
	WR_REV_VID(%s, msg);
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
			 * If we meet `ESC' in the input there are two cases:
			 *     1) It is the first character.
			 *     2) It is in the middle of an input.
			 * In case 1) we quit the prompt, and in case 2) we
			 * clean the ``cmd'' input up but keep staying there.
			 */
			case ESC:
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
			 * These decline the input only if they're first.
			 */
			case DEL:
			case BSP:
				if (first)
					return 1;
				break;
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
				if (*buf >= ' ' && *buf <= '~') {
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
 * Execute the command, read by `read_cmd' into `cmd' buffer.
 * Returns `0' if command is successfull and we need to immediately
 * clean the ``cmd'' line up.
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
			if (*(cmd+1) != '\n')
				return 1;
			CLN_CMD();
			terminate();
			return 0;
		case 'f':
			dpl_cmd_txt(filepath);
			return 1;
		default:
			return -1;
		}
	}
	
	/* NOTREACHED. */
	return 1;
}

/*
 * Routine for handling character that user's just entered.
 */
void
handle_char(char c)
{
	switch (c) {
	case '\r':
	case '\n':
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
		if (mod != MOD_CMD)
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
			CLN_CMD();
			break;
		/* `1' means we don't need to do anything. */
		}
		break;
	case ';':
		if (mod == MOD_NAV) {
			nav_right();
			break;
		}
		break;
	case 'j':
		if (mod == MOD_NAV) {
			nav_left();
			break;
		}
		break;
	case 'l':
		if (mod == MOD_NAV) {
			nav_dwn();
			break;
		}
		break;
	case 'k':
		if (mod == MOD_NAV) {
			nav_up();
			break;
		}
		break;
	case CTRL('l'):
		if (mod == MOD_NAV) {
			scrl_dwn();
			break;
		}
		break;
	case CTRL('k'):
		if (mod == MOD_NAV) {
			scrl_up();
			break;
		}
		break;
	}
}

/*
 * Infinite loop that handles user input byte-by-byte.
 */
void
input_loop()
{
	while (1) {
		while (read(STDIN_FILENO, &buf, 1) > 0) {
			handle_char(*buf);
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
	dpl_pg();
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
	
	expand_lns(0);
	
	if (argc > 2)
		die("I can edit only one thing at a time.\n");
	if (argc > 1)
		handle_filepath(argv[1]);
	
	set_raw();
	setup_terminal();
	init_win_sz();
	
	dpl_pg();
	/* Move cursor to the first visible character. */
	MV_CURS(BUF_ROW, 1);
	input_loop();
	
	terminate();
	
	return 0;
}
