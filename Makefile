all: et.c
	cc -o et et.c -Oz && llvm-strip et

clean:
	rm -f et et.core
