all:
	@gcc -std=c11 -pedantic -D_DEFAULT_SOURCE -o barstatus `pkg-config --libs --cflags xcb alsa libmpdclient libnotify` \
	-lpthread -lxcb-ewmh -lxcb-icccm barstatus.c
