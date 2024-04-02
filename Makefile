run:build
	./a.out
debug:build
	gdb -x a.gdb a.out
build:stackful.c stackful.s
	gcc -g -m32 -std=c99 $^
