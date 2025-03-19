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
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>


/* Size for input/output buffer `buf'. */
#define IOBUF 4096
/* By how many lines the `lns' extended when it is not enough space. */
#define LNS_EXPAND 64
/* By how many lines the line's string is extended when it needs space. */
#define LN_EXPAND 64

/* String command for moving terminal cursor to the row `R' and column `C'. */
#define MV_CURS_CMD(R, C) "\x1b[" #R ";" #C "H"
/* Enter reverse video mode, i.e. swap fore- and background colors. */
#define REV_VID_CMD "\x1b[7m"
/* Reset video mode. */
#define VID_RST_CMD "\x1b[0m"
/* Erase the whole screen contents. */
#define ERASE_ALL_CMD "\x1b[2J"

/* Expand the string for line at `lns[I]'. */
#define EXPAND_LN(I) do {							\
	lns[I]->str = srealloc(lns[I]->str, lns[I]->sz += LN_EXPAND);		\
} while(0)

/*
 * Set `lns_l' to `V' and keep `lns_l_dig' synced with new value.
 */
#define LNS_L(V) do {			\
	lns_l = V;			\
	lns_l_dig = num_of_dig(lns_l);	\
} while (0);

/*
 * Write string `S' in reverse video mode and then exit it (mode).
 * `S' should be put in here _without_ double quotes.
 */
#define WR_REV_VID(S, ...) do {							\
	dprintf(STDOUT_FILENO, REV_VID_CMD #S VID_RST_CMD, __VA_ARGS__);	\
} while (0)

#define MV_CURS(R, C) do {					\
	dprintf(STDOUT_FILENO, MV_CURS_CMD(R, C));		\
	curs_x = C;						\
	curs_y = R;						\
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

/* The name of the file the buffer will be written to. */
char* filename;

/* A list of text lines. */
struct ln** lns;
/* Length of lines (actual number of lines). */
size_t lns_l;
/* Size (reserved space) for `lns' array.  Compare `lns_l'. */
size_t lns_sz;
/*
 * Number of digits in `lns_l'.
 * It is updated in sync with `lns_l' through `LNS_L' macro.
 * It's used to pad the line numbers.  See `print_ln'.
 */
int lns_l_dig;

/* Original termios(4) structure.  I.e. original terminal's settings. */
struct termios orig_tos;
/* Current termios(4) structure.  It is modified in order to enter raw mode. */
struct termios tos;

/* Terminal cursor column. */
unsigned short curs_x;
/* Terminal cursor row. */
unsigned short curs_y;

/* Number of terminal window rows. */
unsigned short ws_row;
/* Number of terminal window columns. */
unsigned short ws_col;

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
 * Get number of digits there are in the number.
 */
int
num_of_dig(size_t num)
{
	int dig_num;
	
	dig_num = 0;
	while (num != 0) {
		num /= 10;
		dig_num++;
	}
	
	return dig_num;
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
	free(filename);
}

/*
 * Terminate the program: free all the date and return to the
 * canonical terminal mode.
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
 * Obtain information about terminal window size.
 */
void
get_win_sz()
{
	struct winsize win_sz;
	
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &win_sz) == -1)
		die("can not obtain the terminal window size.\n");
	
	/*
	 * One line (at the top) is used for a filename,
	 * one line (at the bottom) is for entering commands.
	 */
	ws_row = win_sz.ws_row - 2;
	ws_col = win_sz.ws_col;
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
	sa.sa_handler = get_win_sz;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);
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
 * The `filename' to `name'.
 */
void
set_filename(char* name)
{
	filename = srealloc(filename, strlen(name)+1);
	strcpy(filename, name);
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
				LNS_L(lns_l+1);
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
 * path (if it doesn't exist - create it), extract the
 * file _name_ from the path and remember it as a name
 * for a file we will write the buffer back later.
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
	else {
		fd = open(path, O_CREAT | O_RDWR);
		if (fd == -1)
			die("can not open file at %s.\n", path);
	}
	
	if (close(fd) == -1)
		die("can not close the file.\n");
	
	set_filename(basename(path));
}

/*
 * Convert line number (in string representation) to index.
 * It's used to handle start line number (2'd program argument).
 * Returns `-1' if wrong line number is specified.
 */
ssize_t
get_ln_idx(char* num_str)
{
	ssize_t num;
	
	num = strtol(num_str, NULL, 10);
	if (num <= 0)
		return -1;
	
	/*
	 * From this point, `num' is converted to index.
	 */
	num--;
	if ((size_t)num >= lns_l)
		num = lns_l - 1;
	
	return num;
}

/*
 * In the first screen row we print a name of edited
 * file in the reverse video mode.
 */
void
print_filename()
{
	MV_CURS(1, 1);
	if (filename != NULL)
		WR_REV_VID(%s, filename);
	else
		/*
		 * If no target file is provided as argument, then
		 * anonymous buffer is used.
		 */
		WR_REV_VID(%s, "<anon>");
}

/*
 * Initial terminal setup before starting printing the text out:
 *     - Clear the screen.
 *     - In the first row print the filename.
 */
void
setup_terminal()
{
	dprintf(STDOUT_FILENO, ERASE_ALL_CMD);
	print_filename();
	MV_CURS(2, 1);
}

/*
 * Print buffer line at index `idx' [`start', `end') and prepend
 * it with the line number.
 */
void
print_ln(size_t idx, size_t start, size_t end)
{
	char* tmp;
	size_t len;
	
	len = end - start;
	
	tmp = smalloc(len);
	strncpy(tmp, lns[idx]->str, len);
	
	/*
	 * Pad the line number for it to line up with
	 * other line numbers.
	 */
	dprintf(STDOUT_FILENO, "%*zu  ", lns_l_dig, idx+1);
	write(STDOUT_FILENO, tmp, len);
	
	free(tmp);
}

/*
 * Display the text so that it fits in one screen.
 * The print starts from the line at index `from' (the topmost line).
 */
void
dpl_pg(size_t from)
{
	size_t i;
	size_t end;
	/* Number of printable lines. */
	size_t ln_num;
	/* Number of trailing empty lines. */
	size_t empt_num;
	
	ln_num = lns_l - from;
	
	MV_CURS(2, 1);
	
	/*
	 * If lines can not fit the screen.
	 */
	if (ln_num > ws_row) {
		end = from + ws_row;
		empt_num = 0;
	}
	else {
		end = lns_l;
		empt_num = ws_row - ln_num;
	}
	
	for (i = from; i < end; ++i) {
		print_ln(i, 0, lns[i]->l);
		dprintf(STDOUT_FILENO, "\n\r");
	}
	
	/*
	 * Print empty lines, if any.
	 */
	for (i = 0; i < empt_num; ++i)
		dprintf(STDOUT_FILENO, "~\n\r");
}

/*
 * Run the visual editor.
 *
 * If no arguments are provided, then an empty anonymous buffer
 * is created and opened.
 *
 * First argument is a file path.  If there's no filt at this path,
 * then then file will be created for reading and writing and then
 * opened.
 *
 * Second argument is optional and it tells the line number to open
 * file at (i.e. which line will be at the top of the screen at
 * first file open).
 */
int
main(int argc, char** argv)
{
	/* Start line index. */
	size_t st_ln_idx;
	
	lns = NULL;
	LNS_L(0);
	lns_sz = 0;
	filename = NULL;
	st_ln_idx = 0;
	
	expand_lns(0);
	
	if (argc > 3)
		die("I can edit only one thing at a time.");
	if (argc > 1) {
		handle_filepath(argv[1]);
		if (argc == 3) {
			st_ln_idx = get_ln_idx(argv[2]);
			if ((ssize_t)st_ln_idx == -1)
				die("invalid start line number.\n");
		}
	}

	set_raw();
	setup_terminal();
	init_win_sz();
	
	dpl_pg(st_ln_idx);
	
	terminate();
	
	return 0;
}
