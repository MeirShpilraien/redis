FINAL_LUA_CFLAGS=$(LUA_CFLAGS)
FINAL_LUA_LDFLAGS=$(LUA_LDFLAGS)

uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

ifneq ($(findstring -m32,$(LUA_CFLAGS)),)
	FINAL_LUA_CFLAGS+= -fno-pic
	FINAL_LUA_LDFLAGS+= -melf_i386
endif

all:
	gcc $(FINAL_LUA_CFLAGS) -I../../deps/lua-5.4.2/src/ -fvisibility=hidden scripting.c -c -o scripting.o
	ld $(FINAL_LUA_LDFLAGS) scripting.o ../../deps/lua-5.4.2/src/liblua.a -r -o liblua_latest.o
ifneq ($(uname_S),Darwin)
	objcopy --localize-hidden liblua_latest.o liblua_latest.o
endif