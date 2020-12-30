all:
	gcc $(CFLAGS) -I../../deps/lua-5.4.2/src/ -fvisibility=hidden scripting.c -c -o scripting.o
	ld scripting.o ../../deps/lua-5.4.2/src/liblua.a -r -o liblua_latest.o
	objcopy --localize-hidden liblua_latest.o liblua_latest.o