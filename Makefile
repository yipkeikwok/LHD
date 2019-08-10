# CFLAGS = -ggdb3 -std=c++14 -Wall -Werror -fPIC -mcmodel=medium
CFLAGS = -march=native -funroll-loops -ffast-math -fPIC -Werror -Wall -mcmodel=medium

TARGET = ./bin/cache 

all : CFLAGS += -std=c++14 -g -O3
all : $(TARGET)

centos7 : CFLAGS += -std=c++1y -ggdb -O0
centos7 : $(TARGET)

HEADERS=$(wildcard *.hpp)

LDFLAGS = -lconfig++

.PHONY: clean
clean:
	rm obj/*.o bin/*

./obj:
	mkdir -p $@

./obj/%.o : %.cpp $(HEADERS) Makefile ./obj
	g++ $(CFLAGS) -c -o $@ $<

$(TARGET) : ./obj/cache.o ./obj/repl.o ./obj/lhd.o
	mkdir -p ./bin
	g++ $(CFLAGS) -o $@ $^ $(LDFLAGS)
