/*
 * et -- edit text.
 *
 * The visual text editor.
 */


#include <sys/stat.h>

#include <fcntl.h>
#include <libgen.h>
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

/* Expand the string for line at `lns[I]'. */
#define EXPAND_LN(I) { \
	lns[I]->str = srealloc(lns[I]->str, lns[I]->sz += LN_EXPAND); \
}


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
size_t lns_l;
size_t lns_sz;

/* Original termios(4) structure.  I.e. original terminal's settings. */
struct termios orig_tos;
/* Current termios(4) structure.  It is modified in order to enter raw mode. */
struct termios tos;

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
		die("can not alloc %zu objects %zu bytes each.\n", n, size);
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
	if (tcsetattr(STDIN_FILENO, TCSANOW, &orig_tos) == -1)
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
	if (tcgetattr(STDIN_FILENO, &tos) == -1)
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
	if (tcsetattr(STDIN_FILENO, TCSANOW, &tos) == -1)
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
	free(lns[0]->str);
	
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
 * Run the visual editor.
 */
int
main(int argc, char** argv)
{
	lns = NULL;
	lns_l = 0;
	lns_sz = 0;
	filename = NULL;
	
	set_raw();
	
	expand_lns(0);
	
	if (argc > 2)
		die("I can edit only one thing at a time.");
	
	if (argc > 1) {
		handle_filepath(argv[1]);
	}
	else {
		dprintf(STDERR_FILENO, "no file is about to be read.\n");
	}
	
	terminate();
	
	return 0;
}
