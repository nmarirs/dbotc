CC=gcc

client: src/main.c src/yxml.c
	$(CC) src/main.c src/yxml.c -o client -rdynamic -l bz2 -pthread

static:
	scan-build clang src/main.c src/yxml.c -o client -l bz2 -pthread
