/*
 * Copyright (c) 2021, Redis Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "functions.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "atomicvar.h"

static size_t engine_cache_memory = 0;

/* Forward declaration */
static void engineFunctionDispose(dict *d, void *obj);

struct librariesCtx {
    dict *libraries;    /* Function name -> Function object that can be used to run the function */
    dict *functions;    /* Function name -> Function object that can be used to run the function */
    size_t cache_memory /* Overhead memory (structs, dictionaries, ..) used by all the functions */;
};

dictType engineDictType = {
        dictSdsCaseHash,       /* hash function */
        dictSdsDup,            /* key dup */
        NULL,                  /* val dup */
        dictSdsKeyCaseCompare, /* key compare */
        dictSdsDestructor,     /* key destructor */
        NULL,                  /* val destructor */
        NULL                   /* allow to expand */
};

dictType functionDictType = {
        dictSdsHash,          /* hash function */
        dictSdsDup,           /* key dup */
        NULL,                 /* val dup */
        dictSdsKeyCompare,    /* key compare */
        dictSdsDestructor,    /* key destructor */
        NULL,                 /* val destructor */
        NULL                  /* allow to expand */
};

dictType libraryFunctionDictType = {
        dictSdsHash,          /* hash function */
        dictSdsDup,           /* key dup */
        NULL,                 /* val dup */
        dictSdsKeyCompare,    /* key compare */
        dictSdsDestructor,    /* key destructor */
        engineFunctionDispose,/* val destructor */
        NULL                  /* allow to expand */
};

dictType librariesDictType = {
        dictSdsHash,          /* hash function */
        dictSdsDup,           /* key dup */
        NULL,                 /* val dup */
        dictSdsKeyCompare,    /* key compare */
        dictSdsDestructor,    /* key destructor */
        NULL,                 /* val destructor */
        NULL                  /* allow to expand */
};

/* Dictionary of engines */
static dict *engines = NULL;

/* Functions Ctx.
 * Contains the dictionary that map a function name to
 * function object and the cache memory used by all the functions */
static librariesCtx *curr_lib_ctx = NULL;

static size_t functionMallocSize(functionInfo *fi) {
    return zmalloc_size(fi) + sdsZmallocSize(fi->name)
            + (fi->desc ? sdsZmallocSize(fi->desc) : 0)
            + fi->li->ei->engine->get_function_memory_overhead(fi->function);
}

/* Dispose function memory */
static void engineFunctionDispose(dict *d, void *obj) {
    UNUSED(d);
    functionInfo *fi = obj;
    sdsfree(fi->name);
    if (fi->desc) {
        sdsfree(fi->desc);
    }
    engine *engine = fi->li->ei->engine;
    engine->free_function(engine->engine_ctx, fi->function);
    zfree(fi);
}

static void engineLibraryFree(libraryInfo* li) {
    dictRelease(li->functions);
    sdsfree(li->name);
    sdsfree(li->code);
    if (li->desc) sdsfree(li->desc);
    zfree(li);
}

/* Clear all the functions from the given functions ctx */
void librariesCtxClear(librariesCtx *lib_ctx) {
    dictEmpty(curr_lib_ctx->functions, NULL);
    dictIterator *iter = dictGetIterator(lib_ctx->libraries);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        libraryInfo *li = dictGetVal(entry);
        engineLibraryFree(li);
    }
    dictReleaseIterator(iter);
    dictEmpty(curr_lib_ctx->libraries, NULL);
    curr_lib_ctx->cache_memory = 0;
}

/* Free the given functions ctx */
void librariesCtxFree(librariesCtx *lib_ctx) {
    librariesCtxClear(lib_ctx);
    dictRelease(lib_ctx->functions);
    dictRelease(lib_ctx->libraries);
    zfree(lib_ctx);
}

/* Swap the current functions ctx with the given one.
 * Free the old functions ctx. */
void librariesCtxSwapWithCurrent(librariesCtx *new_lib_ctx) {
    librariesCtxFree(curr_lib_ctx);
    curr_lib_ctx = new_lib_ctx;
}

/* return the current functions ctx */
librariesCtx* librariesCtxGetCurrent() {
    return curr_lib_ctx;
}

/* Create a new functions ctx */
librariesCtx* librariesCtxCreate() {
    librariesCtx *ret = zmalloc(sizeof(librariesCtx));
    ret->libraries = dictCreate(&librariesDictType);
    ret->functions = dictCreate(&functionDictType);
    ret->cache_memory = 0;
    return ret;
}

/*
 * Creating a function inside the given library.
 * On success, return C_OK.
 * On error, return C_ERR and set err output parameter with a relevant error message.
 */
int libraryCreateFunction(const char *name, void *function, libraryInfo *li, const char *desc, sds *err) {
    if (dictFetchValue(li->functions, name)) {
        *err = sdsnew("Function already exists in the library");
        return C_ERR;
    }

    functionInfo *fi = zmalloc(sizeof(*fi));
    *fi = (functionInfo) {
        .name = sdsnew(name),
        .function = function,
        .li = li,
        .desc = desc ? sdsnew(desc) : NULL,
    };

    int res = dictAdd(li->functions, fi->name, fi);
    serverAssert(res == DICT_OK);

    return C_OK;
}

static libraryInfo* engineLibraryCreate(sds name, engineInfo *ei, sds desc, sds code) {
    libraryInfo *li = zmalloc(sizeof(*li));
    *li = (libraryInfo) {
        .name = sdsdup(name),
        .functions = dictCreate(&libraryFunctionDictType),
        .ei = ei,
        .code = sdsdup(code),
        .desc = desc ? sdsdup(desc) : NULL,
    };
    return li;
}

static void functionLibraryUnlink(librariesCtx *lib_ctx, libraryInfo* li) {
    dictIterator *iter = dictGetIterator(li->functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionInfo *fi = dictGetVal(entry);
        int ret = dictDelete(lib_ctx->functions, fi->name);
        serverAssert(ret == DICT_OK);
        lib_ctx->cache_memory -= functionMallocSize(fi);
    }
    dictReleaseIterator(iter);
    int ret = dictDelete(lib_ctx->libraries, li->name);
    serverAssert(ret == DICT_OK);
    // todo: decr libraryInfo struct overhead
}

static void functionLibraryLink(librariesCtx *lib_ctx, libraryInfo* li) {
    dictIterator *iter = dictGetIterator(li->functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionInfo *fi = dictGetVal(entry);
        serverAssert(dictFetchValue(lib_ctx->functions, fi->name) == NULL);
        dictAdd(lib_ctx->functions, fi->name, fi);
        lib_ctx->cache_memory += functionMallocSize(fi);
    }
    dictReleaseIterator(iter);
    dictAdd(lib_ctx->libraries, li->name, li);
    // todo: add libraryInfo struct overhead
}

/* Register an engine, should be called once by the engine on startup and give the following:
 *
 * - engine_name - name of the engine to register
 * - engine_ctx - the engine ctx that should be used by Redis to interact with the engine */
int functionsRegisterEngine(const char *engine_name, engine *engine) {
    sds engine_name_sds = sdsnew(engine_name);
    if (dictFetchValue(engines, engine_name_sds)) {
        serverLog(LL_WARNING, "Same engine was registered twice");
        sdsfree(engine_name_sds);
        return C_ERR;
    }

    client *c = createClient(NULL);
    c->flags |= (CLIENT_DENY_BLOCKING | CLIENT_SCRIPT);
    engineInfo *ei = zmalloc(sizeof(*ei));
    *ei = (engineInfo ) { .name = engine_name_sds, .engine = engine, .c = c,};

    dictAdd(engines, engine_name_sds, ei);

    engine_cache_memory += zmalloc_size(ei) + sdsZmallocSize(ei->name) +
            zmalloc_size(engine) +
            engine->get_engine_memory_overhead(engine->engine_ctx);

    return C_OK;
}

/*
 * FUNCTION STATS
 */
void functionStatsCommand(client *c) {
    if (scriptIsRunning() && scriptIsEval()) {
        addReplyErrorObject(c, shared.slowevalerr);
        return;
    }

    addReplyMapLen(c, 2);

    addReplyBulkCString(c, "running_script");
    if (!scriptIsRunning()) {
        addReplyNull(c);
    } else {
        addReplyMapLen(c, 3);
        addReplyBulkCString(c, "name");
        addReplyBulkCString(c, scriptCurrFunction());
        addReplyBulkCString(c, "command");
        client *script_client = scriptGetCaller();
        addReplyArrayLen(c, script_client->argc);
        for (int i = 0 ; i < script_client->argc ; ++i) {
            addReplyBulkCBuffer(c, script_client->argv[i]->ptr, sdslen(script_client->argv[i]->ptr));
        }
        addReplyBulkCString(c, "duration_ms");
        addReplyLongLong(c, scriptRunDuration());
    }

    addReplyBulkCString(c, "engines");
    addReplyArrayLen(c, dictSize(engines));
    dictIterator *iter = dictGetIterator(engines);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        engineInfo *ei = dictGetVal(entry);
        addReplyBulkCString(c, ei->name);
    }
    dictReleaseIterator(iter);
}

/*
 * FUNCTION LIST
 */
void functionListCommand(client *c) {
    /* general information on all the libraries */
    addReplyArrayLen(c, dictSize(curr_lib_ctx->libraries));
    dictIterator *iter = dictGetIterator(curr_lib_ctx->libraries);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        libraryInfo *li = dictGetVal(entry);
        addReplyMapLen(c, 4);
        addReplyBulkCString(c, "library_name");
        addReplyBulkCBuffer(c, li->name, sdslen(li->name));
        addReplyBulkCString(c, "engine");
        addReplyBulkCBuffer(c, li->ei->name, sdslen(li->ei->name));
        addReplyBulkCString(c, "description");
        if (li->desc) {
            addReplyBulkCBuffer(c, li->desc, sdslen(li->desc));
        } else {
            addReplyNull(c);
        }

        addReplyBulkCString(c, "functions");
        addReplyArrayLen(c, dictSize(li->functions));
        dictIterator *functions_iter = dictGetIterator(li->functions);
        dictEntry *function_entry = NULL;
        while ((function_entry = dictNext(functions_iter))) {
            functionInfo *fi = dictGetVal(function_entry);
            addReplyMapLen(c, 2);
            addReplyBulkCString(c, "name");
            addReplyBulkCBuffer(c, fi->name, sdslen(fi->name));
            addReplyBulkCString(c, "description");
            if (fi->desc) {
                addReplyBulkCBuffer(c, fi->desc, sdslen(fi->desc));
            } else {
                addReplyNull(c);
            }
        }
        dictReleaseIterator(functions_iter);
    }
    dictReleaseIterator(iter);
}

/*
 * FUNCTION INFO <LIBRARY NAME> [WITHCODE]
 */
void functionInfoCommand(client *c) {
    if (c->argc > 4) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command or subcommand", c->cmd->name);
        return;
    }
    /* dedicated information on specific function */
    robj *lib_name = c->argv[2];
    int with_code = 0;
    if (c->argc == 4) {
        robj *with_code_arg = c->argv[3];
        if (!strcasecmp(with_code_arg->ptr, "withcode")) {
            with_code = 1;
        }
    }

    libraryInfo *li = dictFetchValue(curr_lib_ctx->libraries, lib_name->ptr);
    if (!li) {
        addReplyError(c, "Function does not exists");
        return;
    }
    addReplyMapLen(c, with_code? 4 : 3);
    addReplyBulkCString(c, "name");
    addReplyBulkCBuffer(c, li->name, sdslen(li->name));
    addReplyBulkCString(c, "engine");
    addReplyBulkCBuffer(c, li->ei->name, sdslen(li->ei->name));
    addReplyBulkCString(c, "description");
    if (li->desc) {
        addReplyBulkCBuffer(c, li->desc, sdslen(li->desc));
    } else {
        addReplyNull(c);
    }
    if (with_code) {
        addReplyBulkCString(c, "code");
        addReplyBulkCBuffer(c, li->code, sdslen(li->code));
    }
}

/*
 * FUNCTION DELETE <LIBRARY NAME>
 */
void functionDeleteCommand(client *c) {
    if (server.masterhost && server.repl_slave_ro && !(c->flags & CLIENT_MASTER)) {
        addReplyError(c, "Can not delete a function on a read only replica");
        return;
    }

    robj *function_name = c->argv[2];
    libraryInfo *li = dictFetchValue(curr_lib_ctx->libraries, function_name->ptr);
    if (!li) {
        addReplyError(c, "Function not found");
        return;
    }

    functionLibraryUnlink(curr_lib_ctx, li);
    engineLibraryFree(li);
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c, shared.ok);
}

void functionKillCommand(client *c) {
    scriptKill(c, 0);
}

static void fcallCommandGeneric(client *c, int ro) {
    robj *function_name = c->argv[1];
    functionInfo *fi = dictFetchValue(curr_lib_ctx->functions, function_name->ptr);
    if (!fi) {
        addReplyError(c, "Function not found");
        return;
    }
    engine *engine = fi->li->ei->engine;

    long long numkeys;
    /* Get the number of arguments that are keys */
    if (getLongLongFromObject(c->argv[2], &numkeys) != C_OK) {
        addReplyError(c, "Bad number of keys provided");
        return;
    }
    if (numkeys > (c->argc - 3)) {
        addReplyError(c, "Number of keys can't be greater than number of args");
        return;
    } else if (numkeys < 0) {
        addReplyError(c, "Number of keys can't be negative");
        return;
    }

    scriptRunCtx run_ctx;

    scriptPrepareForRun(&run_ctx, fi->li->ei->c, c, fi->name);
    if (ro) {
        run_ctx.flags |= SCRIPT_READ_ONLY;
    }
    engine->call(&run_ctx, engine->engine_ctx, fi->function, c->argv + 3, numkeys,
                 c->argv + 3 + numkeys, c->argc - 3 - numkeys);
    scriptResetRun(&run_ctx);
}

/*
 * FCALL <FUNCTION NAME> nkeys <key1 .. keyn> <arg1 .. argn>
 */
void fcallCommand(client *c) {
    fcallCommandGeneric(c, 0);
}

/*
 * FCALL_RO <FUNCTION NAME> nkeys <key1 .. keyn> <arg1 .. argn>
 */
void fcallroCommand(client *c) {
    fcallCommandGeneric(c, 1);
}

void functionFlushCommand(client *c) {
    if (c->argc > 3) {
        addReplySubcommandSyntaxError(c);
        return;
    }
    int async = 0;
    if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"sync")) {
        async = 0;
    } else if (c->argc == 3 && !strcasecmp(c->argv[2]->ptr,"async")) {
        async = 1;
    } else if (c->argc == 2) {
        async = server.lazyfree_lazy_user_flush ? 1 : 0;
    } else {
        addReplyError(c,"FUNCTION FLUSH only supports SYNC|ASYNC option");
        return;
    }

    if (async) {
        librariesCtx *old_f_ctx = curr_lib_ctx;
        curr_lib_ctx = librariesCtxCreate();
        freeFunctionsAsync(old_f_ctx);
    } else {
        librariesCtxClear(curr_lib_ctx);
    }
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c,shared.ok);
}

void functionHelpCommand(client *c) {
    const char *help[] = {
"CREATE <ENGINE NAME> <LIBRARY NAME> [REPLACE] [DESC <LIBRARY DESCRIPTION>] <LIBRARY CODE>",
"    Create a new library with the given library name and code.",
"DELETE <LIBRARY NAME>",
"    Delete the given library.",
"INFO <LIBRARY NAME> [WITHCODE]",
"    Print the following information about the library:",
"    * Library name",
"    * The engine used to run the library",
"    * Library description",
"    * Library code (only if WITHCODE is given)",
"LIST",
"    Return general information on all the libraries:",
"    * Library name",
"    * The engine used to run the Library",
"    * Library description",
"STATS",
"    Return information about the current function running:",
"    * Function name",
"    * Command used to run the function",
"    * Duration in MS that the function is running",
"    If not function is running, return nil",
"    In addition, returns a list of available engines.",
"KILL",
"    Kill the current running function.",
"FLUSH [ASYNC|SYNC]",
"    Delete all the libraries.",
"    When called without the optional mode argument, the behavior is determined by the",
"    lazyfree-lazy-user-flush configuration directive. Valid modes are:",
"    * ASYNC: Asynchronously flush the libraries.",
"    * SYNC: Synchronously flush the libraries.",
NULL };
    addReplyHelp(c, help);
}

/* Compile and save the given library, return C_OK on success and C_ERR on failure.
 * In case on failure the err out param is set with relevant error message */
int functionsCreateWithLibraryCtx(sds lib_name,sds engine_name, sds desc, sds code,
                                  int replace, sds* err, librariesCtx *lib_ctx) {
    engineInfo *ei = dictFetchValue(engines, engine_name);
    if (!ei) {
        *err = sdsnew("Engine not found");
        return C_ERR;
    }
    engine *engine = ei->engine;

    libraryInfo *li = dictFetchValue(lib_ctx->libraries, lib_name);
    if (li && !replace) {
        *err = sdsnew("Library already exists");
        return C_ERR;
    }

    if (li) {
        functionLibraryUnlink(lib_ctx, li);
    }

    libraryInfo *new_li = engineLibraryCreate(lib_name, ei, desc, code);
    if (engine->create(engine->engine_ctx, new_li, code, err) != C_OK) {
        goto error;
    }

    if (dictSize(new_li->functions) == 0) {
        *err = sdsnew("No libraries registered");
        goto error;
    }

    dictIterator *iter = dictGetIterator(new_li->functions);
    dictEntry *entry = NULL;
    while ((entry = dictNext(iter))) {
        functionInfo *fi = dictGetVal(entry);
        if (dictFetchValue(lib_ctx->functions, fi->name)) {
            /* functions name collision, abort. */
            *err = sdscatfmt(sdsempty(), "Function %s already exists", fi->name);
            dictReleaseIterator(iter);
            goto error;
        }
    }
    dictReleaseIterator(iter);

    if (li) {
        engineLibraryFree(li);
    }

    functionLibraryLink(lib_ctx, new_li);

    return C_OK;

error:
    engineLibraryFree(new_li);
    if (li) functionLibraryLink(lib_ctx, li);
    return C_ERR;
}

/*
 * FUNCTION LOAD <ENGINE NAME> <LIBRARY NAME>
 *             [REPLACE] [DESC <LIBRARY DESCRIPTION>] <LIBRARY CODE>
 *
 * ENGINE NAME     - name of the engine to use the run the function
 * LIBRARY NAME   - name to use to invoke the function
 * REPLACE         - optional, replace existing function
 * DESCRIPTION     - optional, function description
 * LIBRARY CODE   - function code to pass to the engine
 */
void functionLoadCommand(client *c) {

    if (server.masterhost && server.repl_slave_ro && !(c->flags & CLIENT_MASTER)) {
        addReplyError(c, "Can not create a function on a read only replica");
        return;
    }

    robj *engine_name = c->argv[2];
    robj *library_name = c->argv[3];

    int replace = 0;
    int argc_pos = 4;
    sds desc = NULL;
    while (argc_pos < c->argc - 1) {
        robj *next_arg = c->argv[argc_pos++];
        if (!strcasecmp(next_arg->ptr, "replace")) {
            replace = 1;
            continue;
        }
        if (!strcasecmp(next_arg->ptr, "description")) {
            if (argc_pos >= c->argc) {
                addReplyError(c, "Bad function description");
                return;
            }
            desc = c->argv[argc_pos++]->ptr;
            continue;
        }
    }

    if (argc_pos >= c->argc) {
        addReplyError(c, "Function code is missing");
        return;
    }

    robj *code = c->argv[argc_pos];
    sds err = NULL;
    if (functionsCreateWithLibraryCtx(library_name->ptr, engine_name->ptr,
                                      desc, code->ptr, replace, &err, curr_lib_ctx) != C_OK)
    {
        addReplyErrorSds(c, err);
        return;
    }
    /* Indicate that the command changed the data so it will be replicated and
     * counted as a data change (for persistence configuration) */
    server.dirty++;
    addReply(c, shared.ok);
}

/* Return memory usage of all the engines combine */
unsigned long functionsMemory() {
    dictIterator *iter = dictGetIterator(engines);
    dictEntry *entry = NULL;
    size_t engines_nemory = 0;
    while ((entry = dictNext(iter))) {
        engineInfo *ei = dictGetVal(entry);
        engine *engine = ei->engine;
        engines_nemory += engine->get_used_memory(engine->engine_ctx);
    }
    dictReleaseIterator(iter);

    return engines_nemory;
}

/* Return memory overhead of all the engines combine */
unsigned long functionsMemoryOverhead() {
    size_t memory_overhead = dictSize(engines) * sizeof(dictEntry) +
            dictSlots(engines) * sizeof(dictEntry*);
    memory_overhead += dictSize(curr_lib_ctx->functions) * sizeof(dictEntry) +
            dictSlots(curr_lib_ctx->functions) * sizeof(dictEntry*) + sizeof(librariesCtx);
    memory_overhead += curr_lib_ctx->cache_memory;
    memory_overhead += engine_cache_memory;

    return memory_overhead;
}

/* Returns the number of functions */
unsigned long functionsNum() {
    return dictSize(curr_lib_ctx->functions);
}

dict* librariesGet() {
    return curr_lib_ctx->libraries;
}

size_t functionsLen(librariesCtx *functions_ctx) {
    return dictSize(functions_ctx->functions);
}

/* Initialize engine data structures.
 * Should be called once on server initialization */
int functionsInit() {
    engines = dictCreate(&engineDictType);
    curr_lib_ctx = librariesCtxCreate();

    if (luaEngineInitEngine() != C_OK) {
        return C_ERR;
    }

    return C_OK;
}
