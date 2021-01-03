#define LUA_LATEST
#define lua_strlen lua_rawlen
#define scriptingInit scriptingInitLatest
#define lua_open luaL_newstate
#define luaL_checkint luaL_checkinteger

#include "scripting.c"

LUALIB_API int (luaopen_cjson) (lua_State *L);
LUALIB_API int (luaopen_struct) (lua_State *L);
LUALIB_API int (luaopen_cmsgpack) (lua_State *L);

static const luaL_Reg libs[] = {
  {LUA_GNAME, luaopen_base},
//  {LUA_LOADLIBNAME, luaopen_package},
//  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
//  {LUA_IOLIBNAME, luaopen_io},
//  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_MATHLIBNAME, luaopen_math},
//  {LUA_UTF8LIBNAME, luaopen_utf8},
  {LUA_DBLIBNAME, luaopen_debug},
  {"cjson", luaopen_cjson},
  {"struct", luaopen_struct},
  {"cmsgpack", luaopen_cmsgpack},
//  {"bit", luaopen_bit},
  {NULL, NULL}
};

void luaLoadLibraries(lua_State *lua) {
    const luaL_Reg *currLib;
    /* "require" functions from 'loadedlibs' and set results to global table */
    for (currLib = libs; currLib->func; currLib++) {
        luaL_requiref(lua, currLib->name, currLib->func, 1);
        lua_pop(lua, 1);  /* remove lib */
    }
}

#undef lua_strlen
#undef scriptingInit
#undef lua_open
#undef luaL_checkint
#undef LUA_LATEST

