CC = gcc
CFLAGS = -Os -I.
OUTPUT = m68kmake

all: ${OUTPUT} exe

exe:
	./m68kmake

${OUTPUT}:${OBJS}
	gcc m68kmake.c 
	mv a.out m68kmake
	
clean:
	rm *.o
	rm ${OUTPUT}
