.PHONY: all
all:
	gcc -O2 -w -Wno-error=int-conversion -o code main.c buddy.c
