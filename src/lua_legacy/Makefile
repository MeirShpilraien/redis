all:
	gcc $(CFLAGS) -I../../deps/lua/src/ -fvisibility=hidden scripting.c -c -o scripting.o
	ld scripting.o ../../deps/lua/src/liblua.a -r -o liblua_legacy.o
	objcopy --localize-hidden liblua_legacy.o liblua_legacy.o