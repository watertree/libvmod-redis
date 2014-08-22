#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <hiredis/hiredis.h>

/**
 * mhash.h has a habit of pulling in assert(). Let's hope it's a define,
 * and that we can undef it, since Varnish has a better one.
 */
#include <mhash.h>
#ifdef assert
#   undef assert
#endif

#include "vrt.h"
#include "bin/varnishd/cache.h"
#include "vcc_if.h"

typedef struct vcl_priv {
#define VCL_PRIV_MAGIC 0x77feec11
    unsigned magic;
    // Redis connection.
    const char *host;
    unsigned port;
    struct timeval timeout;
} vcl_priv_t;

typedef struct thread_state {
#define THREAD_STATE_MAGIC 0xa6bc103e
    unsigned magic;
    // Current request XID & ID.
    unsigned xid;
    int id;

    // Redis context.
    redisContext *context;

    // Redis command: arguments + reply.
#define MAX_REDIS_COMMAND_ARGS 128
    unsigned argc;
    const char *argv[MAX_REDIS_COMMAND_ARGS];
    redisReply *reply;
} thread_state_t;

#define REDIS_LOG(sp, message, ...) \
    do { \
        char _buffer[512]; \
        snprintf( \
            _buffer, sizeof(_buffer), \
            "[REDIS][%s] %s", __func__, message); \
        WSP(sp, SLT_Error, _buffer, ##__VA_ARGS__); \
    } while (0)

static pthread_once_t thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;

static vcl_priv_t * new_vcl_priv();
static void free_vcl_priv(vcl_priv_t *priv);

static thread_state_t *get_thread_state(struct sess *sp, struct vmod_priv *vcl_priv, unsigned drop_reply);
static void make_thread_key();

static const char *get_reply(struct sess *sp, redisReply *reply);

static const char *sha1(const char *script);

/******************************************************************************
 * VMOD INITIALIZATION.
 *****************************************************************************/

int
init_function(struct vmod_priv *vcl_priv, const struct VCL_conf *conf)
{
    // Initialize global state shared with all VCLs. This code *is
    // required* to be thread safe.
    AZ(pthread_once(&thread_once, make_thread_key));

    // Initialize the local VCL data structure and set its free function.
    // Code initializing / freeing the VCL private data structure *is
    // not required* to be thread safe.
    if (vcl_priv->priv == NULL) {
        vcl_priv->priv = new_vcl_priv("127.0.0.1", 6379, 500);
        vcl_priv->free = (vmod_priv_free_f *)free_vcl_priv;
    }

    // Done!
    return 0;
}

/******************************************************************************
 * redis.init();
 *****************************************************************************/

void
vmod_init(
    struct sess *sp, struct vmod_priv *vcl_priv,
    const char *host, int port, int timeout)
{
    vcl_priv_t *old = vcl_priv->priv;
    vcl_priv->priv = new_vcl_priv(host, port, timeout);
    if (old != NULL) {
        free_vcl_priv(old);
    }
}

/******************************************************************************
 * redis.call();
 *****************************************************************************/

void
vmod_call(struct sess *sp, struct vmod_priv *vcl_priv, const char *command)
{
    // Check input.
    if (command != NULL) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(sp, vcl_priv, 1);

        // Do not continue if a Redis context is not available.
        if (state->context != NULL) {
            // Send command.
            state->reply = redisCommand(state->context, command);

            // Check reply.
            if (state->context->err) {
                REDIS_LOG(sp,
                    "Failed to execute Redis command (%s): [%d] %s",
                    command,
                    state->context->err,
                    state->context->errstr);
            } else if (state->reply == NULL) {
                REDIS_LOG(sp,
                    "Failed to execute Redis command (%s)",
                    command);
            } else if (state->reply->type == REDIS_REPLY_ERROR) {
                REDIS_LOG(sp,
                    "Got error reply while executing Redis command (%s): %s",
                    command,
                    state->reply->str);
            }
        }
    }
}

/******************************************************************************
 * redis.command();
 *****************************************************************************/

void
vmod_command(struct sess *sp, struct vmod_priv *vcl_priv, const char *name)
{
    // Check input.
    if ((name != NULL) && (strlen(name) > 0)) {
        // Fetch local thread state & flush previous command.
        thread_state_t *state = get_thread_state(sp, vcl_priv, 1);

        // Convert command name to uppercase (for later comparison with the
        // 'EVAL' string).
        char *command = strdup(name);
        AN(command);
        char *ptr = command;
        while (*ptr++ = toupper(*ptr));

        // Initialize.
        state->argc = 1;
        state->argv[0] = command;
    }
}

/******************************************************************************
 * redis.push();
 *****************************************************************************/

void
vmod_push(struct sess *sp, struct vmod_priv *vcl_priv, const char *arg)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0);

    // Do not continue if the maximum number of allowed arguments has been
    // reached or if the initial call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->argc < MAX_REDIS_COMMAND_ARGS)) {
        // Handle NULL arguments as empty strings.
        if (arg != NULL) {
            state->argv[state->argc++] = strdup(arg);
        } else {
            state->argv[state->argc++] = strdup("");
        }
        AN(state->argv[state->argc - 1]);
    } else {
        REDIS_LOG(sp, "Failed to push Redis argument");
    }
}

/******************************************************************************
 * redis.execute();
 *****************************************************************************/

void
vmod_execute(struct sess *sp, struct vmod_priv *vcl_priv)
{
    // Fetch local thread state.
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0);

    // Do not continue if a Redis context is not available or if the initial
    // call to redis.command() was not executed.
    if ((state->argc >= 1) && (state->context != NULL)) {
        // When executing EVAL commands, first try with EVALSHA.
        unsigned done = 0;
        if ((strcmp(state->argv[0], "EVAL") == 0) && (state->argc >= 2)) {
            // Replace EVAL with EVALSHA.
            free((void *) state->argv[0]);
            state->argv[0] = strdup("EVALSHA");
            AN(state->argv[0]);
            const char *script = state->argv[1];
            state->argv[1] = sha1(script);

            // Execute the EVALSHA command.
            state->reply = redisCommandArgv(
                state->context,
                state->argc,
                state->argv,
                NULL);

            // Check reply. If Redis replies with a NOSCRIPT, the original
            // EVAL command should be executed to register the script for
            // the first time in the Redis server.
            if (!state->context->err &&
                (state->reply != NULL) &&
                (state->reply->type == REDIS_REPLY_ERROR) &&
                (strncmp(state->reply->str, "NOSCRIPT", 8) == 0)) {
                // Replace EVALSHA with EVAL.
                free((void *) state->argv[0]);
                state->argv[0] = strdup("EVAL");
                AN(state->argv[0]);
                free((void *) state->argv[1]);
                state->argv[1] = script;

            // The command execution is completed.
            } else {
                free((void *) script);
                done = 1;
            }
        }

        // Send command, unless it was originally an EVAL command and it
        // was already executed using EVALSHA.
        if (!done) {
            state->reply = redisCommandArgv(
                state->context,
                state->argc,
                state->argv,
                NULL);
        }

        // Check reply.
        if (state->context->err) {
            REDIS_LOG(sp,
                "Failed to execute Redis command (%s): [%d] %s",
                state->argv[0],
                state->context->err,
                state->context->errstr);
        } else if (state->reply == NULL) {
            REDIS_LOG(sp,
                "Failed to execute Redis command (%s)",
                state->argv[0]);
        } else if (state->reply->type == REDIS_REPLY_ERROR) {
            REDIS_LOG(sp,
                "Got error reply while executing Redis command (%s): %s",
                state->argv[0],
                state->reply->str);
        }
    }
}

/******************************************************************************
 * redis.reply_is_error();
 * redis.reply_is_nil();
 * redis.reply_is_status();
 * redis.reply_is_integer();
 * redis.reply_is_string();
 * redis.reply_is_array();
 *****************************************************************************/

#define VMOD_REPLY_IS_FOO(lower, upper) \
unsigned \
vmod_reply_is_ ## lower(struct sess *sp, struct vmod_priv *vcl_priv) \
{ \
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0); \
    return \
        (state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper); \
}

VMOD_REPLY_IS_FOO(error, ERROR)
VMOD_REPLY_IS_FOO(nil, NIL)
VMOD_REPLY_IS_FOO(status, STATUS)
VMOD_REPLY_IS_FOO(integer, INTEGER)
VMOD_REPLY_IS_FOO(string, STRING)
VMOD_REPLY_IS_FOO(array, ARRAY)

/******************************************************************************
 * redis.get_reply();
 *****************************************************************************/

const char *
vmod_get_reply(struct sess *sp, struct vmod_priv *vcl_priv)
{
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0);
    if (state->reply != NULL) {
        return get_reply(sp, state->reply);
    } else {
        return 0;
    }
}

/******************************************************************************
 * redis.get_error_reply();
 * redis.get_status_reply();
 * redis.get_integer_reply();
 * redis.get_string_reply();
 *****************************************************************************/

#define VMOD_GET_FOO_REPLY(lower, upper, return_type, reply_field, fallback_value) \
return_type \
vmod_get_ ## lower ## _reply(struct sess *sp, struct vmod_priv *vcl_priv) \
{ \
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0); \
    if ((state->reply != NULL) && \
        (state->reply->type == REDIS_REPLY_ ## upper)) { \
        return state->reply->reply_field; \
    } else { \
        return fallback_value; \
    } \
}

VMOD_GET_FOO_REPLY(error, ERROR, const char *, str, NULL)
VMOD_GET_FOO_REPLY(status, STATUS, const char *, str, NULL)
VMOD_GET_FOO_REPLY(integer, INTEGER, int, integer, 0)
VMOD_GET_FOO_REPLY(string, STRING, const char *, str, NULL)

/******************************************************************************
 * redis.get_array_reply_length();
 *****************************************************************************/

int
vmod_get_array_reply_length(struct sess *sp, struct vmod_priv *vcl_priv)
{
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY)) {
        return state->reply->elements;
    } else {
        return 0;
    }
}

/******************************************************************************
 * redis.get_array_reply_value();
 *****************************************************************************/

const char *
vmod_get_array_reply_value(
    struct sess *sp, struct vmod_priv *vcl_priv,
    int index)
{
    thread_state_t *state = get_thread_state(sp, vcl_priv, 0);
    if ((state->reply != NULL) &&
        (state->reply->type == REDIS_REPLY_ARRAY) &&
        (index < state->reply->elements)) {
        return get_reply(sp, state->reply->element[index]);
    } else {
        return NULL;
    }
}

/******************************************************************************
 * redis.free();
 *****************************************************************************/

void
vmod_free(struct sess *sp, struct vmod_priv *vcl_priv)
{
    get_thread_state(sp, vcl_priv, 1);
}

/******************************************************************************
 * UTILITIES.
 *****************************************************************************/

static vcl_priv_t *
new_vcl_priv(const char *host, int port, int timeout)
{
    vcl_priv_t *result;
    ALLOC_OBJ(result, VCL_PRIV_MAGIC);
    AN(result);

    result->host = strdup(host);
    AN(result->host);
    result->port = port;
    result->timeout.tv_sec = timeout / 1000;;
    result->timeout.tv_usec = (timeout % 1000) * 1000;

    return result;
}

static void
free_vcl_priv(vcl_priv_t *priv)
{
    free((void *) priv->host);

    FREE_OBJ(priv);
}

static thread_state_t *
get_thread_state(
    struct sess *sp, struct vmod_priv *vcl_priv,
    unsigned flush)
{
    // Initializations.
    vcl_priv_t *config = vcl_priv->priv;
    thread_state_t *result = pthread_getspecific(thread_key);

    // Create thread state if not created yet, and flush Redis context
    // if found in an error state.
    if (result == NULL) {
        ALLOC_OBJ(result, THREAD_STATE_MAGIC);
        AN(result);

        result->xid = sp->xid;
        result->id = sp->id;
        result->context = NULL;
        result->argc = 0;
        result->reply = NULL;

        pthread_setspecific(thread_key, result);
    } else {
        CHECK_OBJ(result, THREAD_STATE_MAGIC);

        if ((result->context != NULL) && result->context->err) {
            redisFree(result->context);
            result->context = NULL;
        }
    }

    // Create Redis context. If any error arises discard the context and
    // continue.
    if (result->context == NULL) {
        result->context = redisConnectWithTimeout(config->host, config->port, config->timeout);
        AN(result->context);
        if (result->context->err) {
            REDIS_LOG(sp,
                "Failed to establish Redis connection (%d): %s",
                result->context->err,
                result->context->errstr);
            redisFree(result->context);
            result->context = NULL;
        }
    }

    // Is this a new request? Check the XID (and the ID just in case we have
    // non-unique XIDs, as in some versions of Varnish 3).
    if ((result->xid != sp->xid) || (result->id != sp->id)) {
        // Update XID & ID.
        result->xid = sp->xid;
        result->id = sp->id;

        // Drop previous Redis command.
        flush = 1;
    }

    // Drop previously stored Redis command.
    if (flush) {
        while (result->argc > 0) {
            free((void *) result->argv[--(result->argc)]);
        }
        if (result->reply != NULL) {
            freeReplyObject(result->reply);
            result->reply = NULL;
        }
    }

    // Done!
    return result;
}

static void
free_thread_state(thread_state_t *state)
{
    if (state->context != NULL) {
        redisFree(state->context);
    }

    while (state->argc > 0) {
        free((void *) state->argv[--(state->argc)]);
    }

    if (state->reply != NULL) {
        freeReplyObject(state->reply);
    }

    FREE_OBJ(state);
}

static void
make_thread_key()
{
    AZ(pthread_key_create(&thread_key, (void *) free_thread_state));
}

static const char *
get_reply(struct sess *sp, redisReply *reply)
{
    // Default result.
    const char *result = NULL;

    // Check type of Redis reply.
    // XXX: array replies are *not* supported.
    char buffer[64];
    switch (reply->type) {
        case REDIS_REPLY_ERROR:
        case REDIS_REPLY_STATUS:
        case REDIS_REPLY_STRING:
            result = WS_Dup(sp->ws, reply->str);
            AN(result);
            break;

        case REDIS_REPLY_INTEGER:
            sprintf(buffer, "%lld", reply->integer);
            result = WS_Dup(sp->ws, buffer);
            AN(result);
            break;

        case REDIS_REPLY_ARRAY:
            result = WS_Dup(sp->ws, "array");
            AN(result);
            break;

        default:
            return NULL;
    }

    // Done!
    return result;
}

static const char *
sha1(const char *script)
{
    // Hash.
    unsigned block_size = mhash_get_block_size(MHASH_SHA1);
    unsigned char buffer[block_size];
    MHASH td = mhash_init(MHASH_SHA1);
    mhash(td, script, strlen(script));
    mhash_deinit(td, buffer);

    // Encode.
    char *result = malloc(block_size * 2);
    AN(result);
    char *ptr = result;
    for (int i = 0; i < block_size; i++) {
        sprintf(ptr, "%.2x", buffer[i]);
        ptr += 2;
    }

    // Done!
    return result;
}