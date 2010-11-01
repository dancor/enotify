all: enotify

clean:
	rm enotify

install: all
	cp enotify /usr/local/bin/enotify

enotify: enotify.cpp
	g++ -ggdb -Wall enotify.cpp -o enotify
