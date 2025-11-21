CC=gcc

client: src/main.c src/yxml.c
	$(CC) src/main.c src/yxml.c -o client -O3 -g -rdynamic -l bz2 -pthread -fanalyzer

static:
	scan-build clang main.c yxml.c -o client -l bz2
