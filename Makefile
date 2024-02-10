all: main

main: main.c
	gcc -o main main.c -std=gnu99

clean:
	rm main
