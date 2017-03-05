all:
	gcc src/combiner.c -c -std=gnu99 -g -O3 -o link.o
	g++ src/main.cpp -O3 -o main.o -c -std=c++11
	g++ main.o link.o -lpthread
