/*
 * Copyright 2010-2012 self.disconnect
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This file has been modified from its original upstream version. It is
 * forked from commit cfaef071 of the (now abandoned) repository at
 *
 *     https://github.com/disconnect/apache-websocket
 */

/*                       _                _                     _         _
 *   _ __ ___   ___   __| | __      __ _ | |__  ___  ___   ___ | | __ _ _| |_   mod_websocket
 *  | '_ ` _ \ / _ \ / _` | \ \ /\ / / _ \ '_ \/ __// _ \ / __\| |/ / _ \_  _|  Apache Interface to WebSocket
 *  | | | | | | (_) | (_| |  \ V  V /  __/ |_) )__ \ (_) | (___|   (  __/| |__
 *  |_| |_| |_|\___/ \__,_|___\_/\_/ \___|_,__/|___/\___/ \___/|_|\_\___| \__/
 *                       |_____|
 *   mod_websocket.c
 *   Apache API inteface structures
 */

#include "apr_base64.h"
#include "apr_lib.h"
#include "apr_queue.h"
#include "apr_sha1.h"
#include "apr_strings.h"
#include "apr_thread_cond.h"

#include "httpd.h"
#include "http_config.h"
#include "http_log.h"
#include "http_protocol.h"
#include "util_varbuf.h"

#include "websocket_plugin.h"
#include "mod_websocket_hook_export.h"
#include "validate_utf8.h"

#define CORE_PRIVATE
#include "http_core.h"
#include "http_connection.h"

#if !defined(APR_ARRAY_IDX)
#define APR_ARRAY_IDX(ary,i,type) (((type *)(ary)->elts)[i])
#endif
#if !defined(APR_ARRAY_PUSH)
#define APR_ARRAY_PUSH(ary,type) (*((type *)apr_array_push(ary)))
#endif

module AP_MODULE_DECLARE_DATA websocket_module;

#ifdef APLOG_USE_MODULE /* only in Apache 2.4 */
APLOG_USE_MODULE(websocket);
#endif

#ifndef APLOG_TRACE1 /* not defined in Apache 2.2 */
#define APLOG_TRACE1 APLOG_DEBUG
#endif

typedef struct
{
    char *location;
    apr_dso_handle_t *res_handle;
    WebSocketPlugin *plugin;
    apr_int64_t message_limit;
    int allow_reserved; /* whether to allow reserved status codes */
    int origin_check;   /* how to check the Origin during a handshake */
    apr_hash_t *trusted_origins; /* allowlist for ORIGIN_CHECK_TRUSTED */
} websocket_config_rec;

/* Possible config values for websocket_config_rec->origin_check */
#define ORIGIN_CHECK_OFF     0 /* No checks whatsoever */
#define ORIGIN_CHECK_SAME    1 /* Origin must match that of the request target */
#define ORIGIN_CHECK_TRUSTED 2 /* Origin must be on the WebSocketTrustedOrigin list */

#define BLOCK_DATA_SIZE              4096

#define QUEUE_CAPACITY                 16

#define DATA_FRAMING_MASK               0
#define DATA_FRAMING_START              1
#define DATA_FRAMING_PAYLOAD_LENGTH     2
#define DATA_FRAMING_PAYLOAD_LENGTH_EXT 3
#define DATA_FRAMING_EXTENSION_DATA     4
#define DATA_FRAMING_APPLICATION_DATA   5

#define FRAME_GET_FIN(BYTE)         (((BYTE) >> 7) & 0x01)
#define FRAME_GET_RSV1(BYTE)        (((BYTE) >> 6) & 0x01)
#define FRAME_GET_RSV2(BYTE)        (((BYTE) >> 5) & 0x01)
#define FRAME_GET_RSV3(BYTE)        (((BYTE) >> 4) & 0x01)
#define FRAME_GET_OPCODE(BYTE)      ( (BYTE)       & 0x0F)
#define FRAME_GET_MASK(BYTE)        (((BYTE) >> 7) & 0x01)
#define FRAME_GET_PAYLOAD_LEN(BYTE) ( (BYTE)       & 0x7F)

#define FRAME_SET_FIN(BYTE)         (((BYTE) & 0x01) << 7)
#define FRAME_SET_OPCODE(BYTE)       ((BYTE) & 0x0F)
#define FRAME_SET_MASK(BYTE)        (((BYTE) & 0x01) << 7)
#define FRAME_SET_LENGTH(X64, IDX)  (unsigned char)(((X64) >> ((IDX)*8)) & 0xFF)

#define OPCODE_CONTINUATION 0x0
#define OPCODE_TEXT         0x1
#define OPCODE_BINARY       0x2
#define OPCODE_CLOSE        0x8
#define OPCODE_PING         0x9
#define OPCODE_PONG         0xA

#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBSOCKET_GUID_LEN 36

#define STATUS_CODE_OK                1000
#define STATUS_CODE_GOING_AWAY        1001
#define STATUS_CODE_PROTOCOL_ERROR    1002
#define STATUS_CODE_RESERVED          1004 /* Protocol 8: frame too large */
#define STATUS_CODE_INVALID_UTF8      1007
#define STATUS_CODE_POLICY_VIOLATION  1008
#define STATUS_CODE_MESSAGE_TOO_LARGE 1009
#define STATUS_CODE_INTERNAL_ERROR    1011

/* The supported WebSocket protocol versions. */
static int supported_versions[] = { 13, 8, 7 };
static int supported_versions_len = sizeof(supported_versions) /
                                    sizeof(supported_versions[0]);

/* Provided hooks */
/* We want to stop if one plugin accepts, but *_RUN_FIRST does not exist for optional hooks, so set both ok and declined to DECLINED */
AP_IMPLEMENT_OPTIONAL_HOOK_RUN_ALL(int,websocket_plugin_init,(const char* name, struct _WebSocketPlugin** plugin),(name, plugin),DECLINED,DECLINED)

/*
 * Configuration
 */

static void *mod_websocket_create_dir_config(apr_pool_t *p, char *path)
{
    websocket_config_rec *conf = NULL;

    if (path != NULL) {
        conf = apr_pcalloc(p, sizeof(websocket_config_rec));
        if (conf != NULL) {
            conf->location = apr_pstrdup(p, path);
            conf->message_limit = 32 * 1024 * 1024;
            conf->origin_check = ORIGIN_CHECK_SAME;
            conf->trusted_origins = apr_hash_make(p);
        }
    }
    return (void *)conf;
}

static apr_status_t mod_websocket_cleanup_config(void *data)
{
    if (data != NULL) {
        websocket_config_rec *conf = (websocket_config_rec *)data;

        if (conf != NULL) {
            if ((conf->plugin != NULL) && (conf->plugin->destroy != NULL)) {
                conf->plugin->destroy(conf->plugin);
            }
            conf->plugin = NULL;
            if (conf->res_handle != NULL) {
                apr_dso_unload(conf->res_handle);
                conf->res_handle = NULL;
            }
        }
    }
    return APR_SUCCESS;
}

static const char *mod_websocket_conf_allow_reserved(cmd_parms *cmd,
                                                     void *confv, int on)
{
    websocket_config_rec *conf = (websocket_config_rec *)confv;

    if (conf != NULL) {
        conf->allow_reserved = on;
    }

    return NULL;
}

static const char *mod_websocket_conf_handler(cmd_parms *cmd, void *confv,
                                              const char *path,
                                              const char *name)
{
    websocket_config_rec *conf = (websocket_config_rec *)confv;
    apr_dso_handle_t *res_handle = NULL;
    apr_dso_handle_sym_t sym;
    WebSocketPlugin *plugin;
    const char *errmsg = NULL;

    if (!conf || !path || !name) {
        return "Invalid parameters for WebSocketHandler";
    }

    if (apr_dso_load(&res_handle, ap_server_root_relative(cmd->pool, path),
                     cmd->pool) == APR_SUCCESS) {
        if ((apr_dso_sym(&sym, res_handle, name) != APR_SUCCESS) || !sym) {
            apr_dso_unload(res_handle);
            return apr_psprintf(cmd->pool, "Could not find initialization function "
                                             "\"%s\" for WebSocketHandler %s", name, path);
        } else {
            /* Get the plugin struct from the module. */
            plugin = ((WS_Init) sym) ();
        }
    } else {
        /* dso could not be loaded, check if it is a apache plugin */
        if(strcmp(path, "internal")
            || ap_run_websocket_plugin_init(name, &plugin) != OK) {
            char err[256];
            return apr_pstrcat(cmd->pool, "Could not load WebSocketHandler ", path,
                            ": ", apr_dso_error(res_handle, err, sizeof(err)),
                            NULL);
        }
    }

    /* Do a sanity check on the returned plugin struct */
    if (!plugin) {
        errmsg = "returned NULL";
    }
    else if (plugin->version != WEBSOCKET_PLUGIN_VERSION_0) {
        errmsg = apr_psprintf(cmd->pool, "unsupported plugin version %u",
                              plugin->version);
    }
    else if (plugin->size < sizeof(WebSocketPlugin)) {
        errmsg = "invalid plugin size; check plugin version and compiler";
    }
    else if (!plugin->on_message) {
        errmsg = "on_message handler is NULL";
    }

    if (errmsg) {
        apr_dso_unload(res_handle);
        return apr_pstrcat(cmd->pool, "Invalid response from WebSocketHandler "
                           "initialization function ", name, " (", errmsg, ")",
                           NULL);
    }

    conf->res_handle = res_handle;
    conf->plugin = plugin;
    apr_pool_cleanup_register(cmd->pool, conf, mod_websocket_cleanup_config,
                              apr_pool_cleanup_null);

    return NULL;
}

static const char *mod_websocket_conf_origin_check(cmd_parms *cmd, void *confv,
                                                   const char *mode)
{
    websocket_config_rec *conf = (websocket_config_rec *)confv;

    if (conf) {
        if (!strcasecmp(mode, "Off")) {
            conf->origin_check = ORIGIN_CHECK_OFF;
        } else if (!strcasecmp(mode, "Same")) {
            conf->origin_check = ORIGIN_CHECK_SAME;
        } else if (!strcasecmp(mode, "Trusted")) {
            conf->origin_check = ORIGIN_CHECK_TRUSTED;
        } else {
            return "WebSocketOriginCheck must be Off, Same, or Trusted";
        }
    }

    return NULL;
}

static const char *mod_websocket_conf_add_origin(cmd_parms *cmd, void *confv,
                                                 const char *arg)
{
    websocket_config_rec *conf = (websocket_config_rec *)confv;

    if (conf) {
        const char *origin = apr_pstrdup(cmd->pool, arg);
        apr_hash_set(conf->trusted_origins, origin, APR_HASH_KEY_STRING,
                     origin);

        ap_log_error(APLOG_MARK, APLOG_DEBUG, APR_SUCCESS, cmd->server,
                     "added Origin '%s' to the Trusted list for %s",
                     origin, (cmd->path ? cmd->path : "null"));
    }

    return NULL;
}

static const char *mod_websocket_conf_max_message_size(cmd_parms *cmd,
                                                       void *confv,
                                                       const char *size)
{
    websocket_config_rec *conf = (websocket_config_rec *)confv;
    char *response;

    /* Warn about the name change from MaxMessageSize, if appropriate. */
    if (cmd->cmd->name[0] == 'M') {
        ap_log_error(APLOG_MARK, APLOG_WARNING, APR_SUCCESS, cmd->server,
                     "The MaxMessageSize directive (%s:%d) is deprecated; use "
                     "WebSocketMaxMessageSize instead",
                     cmd->directive->filename, cmd->directive->line_num);
    }

    if ((conf != NULL) && (size != NULL)) {
        apr_int64_t message_limit = apr_atoi64(size);
        if (message_limit > 0) {
            if (message_limit > APR_SIZE_MAX) {
                /*
                 * Since we pass messages as a single contiguous buffer to the
                 * plugin, they can't exceed the max size_t.
                 *
                 * FIXME: a streaming API could allow plugins to get around this
                 * restriction.
                 */
                response = apr_psprintf(cmd->pool,
                                        "Max message size is too large for your"
                                        " system (reduce to %"APR_SIZE_T_FMT")",
                                        APR_SIZE_MAX);
            } else {
                conf->message_limit = message_limit;
                response = NULL;
            }
        }
        else {
            response = "Invalid maximum message size";
        }
    }
    else {
        response = "Invalid parameter";
    }
    return response;
}

/*
 * Functions available to plugins.
 */

typedef struct _WebSocketState
{
    request_rec *r;
    apr_bucket_brigade *obb;
    apr_os_thread_t main_thread;
    apr_thread_mutex_t *mutex;
    apr_thread_cond_t *cond;
    apr_array_header_t *protocols;
    int closing;
    apr_int64_t protocol_version;
    apr_pollset_t *pollset;
    apr_queue_t *queue;
} WebSocketState;

static request_rec *CALLBACK mod_websocket_request(const WebSocketServer *server)
{
    if ((server != NULL) && (server->state != NULL)) {
        return server->state->r;
    }
    return NULL;
}

static const char *CALLBACK mod_websocket_header_get(const WebSocketServer *server,
                                                     const char *key)
{
    if ((server != NULL) && (key != NULL)) {
        WebSocketState *state = server->state;

        if ((state != NULL) && (state->r != NULL)) {
            return apr_table_get(state->r->headers_in, key);
        }
    }
    return NULL;
}

static void CALLBACK mod_websocket_header_set(const WebSocketServer *server,
                                              const char *key,
                                              const char *value)
{
    if ((server != NULL) && (key != NULL) && (value != NULL)) {
        WebSocketState *state = server->state;

        if ((state != NULL) && (state->r != NULL)) {
            apr_table_setn(state->r->headers_out,
                           apr_pstrdup(state->r->pool, key),
                           apr_pstrdup(state->r->pool, value));
        }
    }
}

static size_t CALLBACK mod_websocket_protocol_count(const WebSocketServer *server)
{
    size_t count = 0;

    if ((server != NULL) && (server->state != NULL) &&
        (server->state->protocols != NULL) &&
        !apr_is_empty_array(server->state->protocols)) {
        count = (size_t) server->state->protocols->nelts;
    }
    return count;
}

static const char *CALLBACK mod_websocket_protocol_index(const WebSocketServer *server,
                                                         const size_t index)
{
    if ((index >= 0) && (index < mod_websocket_protocol_count(server))) {
        return APR_ARRAY_IDX(server->state->protocols, index, char *);
    }
    return NULL;
}

static void CALLBACK mod_websocket_protocol_set(const WebSocketServer *server,
                                                const char *protocol)
{
    if ((server != NULL) && (protocol != NULL)) {
        WebSocketState *state = server->state;

        if ((state != NULL) && (state->r != NULL)) {
            apr_table_setn(state->r->headers_out, "Sec-WebSocket-Protocol",
                           apr_pstrdup(state->r->pool, protocol));
        }
    }
}

/*
 * Sends data to the WebSocket connection using the given server state. The
 * server state must be locked upon entering this function. buffer_size is
 * assumed to be within the limits defined by the WebSocket protocol (i.e. fits
 * in 63 bits).
 */
static size_t mod_websocket_send_internal(WebSocketState *state,
                                          const int type,
                                          const unsigned char *buffer,
                                          const size_t buffer_size)
{
    apr_uint64_t payload_length =
        (apr_uint64_t) ((buffer != NULL) ? buffer_size : 0);
    size_t written = 0;

    if ((state->r != NULL) && (state->obb != NULL) && !state->closing) {
        unsigned char header[32];
        ap_filter_t *of = state->r->connection->output_filters;
        apr_size_t pos = 0;
        unsigned char opcode;

        switch (type) {
        case MESSAGE_TYPE_TEXT:
            opcode = OPCODE_TEXT;
            break;
        case MESSAGE_TYPE_BINARY:
            opcode = OPCODE_BINARY;
            break;
        case MESSAGE_TYPE_PING:
            opcode = OPCODE_PING;
            break;
        case MESSAGE_TYPE_PONG:
            opcode = OPCODE_PONG;
            break;
        case MESSAGE_TYPE_CLOSE:
        default:
            state->closing = 1;
            opcode = OPCODE_CLOSE;
            break;
        }
        header[pos++] = FRAME_SET_FIN(1) | FRAME_SET_OPCODE(opcode);
        if (payload_length < 126) {
            header[pos++] =
                FRAME_SET_MASK(0) | FRAME_SET_LENGTH(payload_length, 0);
        }
        else {
            if (payload_length < 65536) {
                header[pos++] = FRAME_SET_MASK(0) | 126;
            }
            else {
                header[pos++] = FRAME_SET_MASK(0) | 127;
                header[pos++] = FRAME_SET_LENGTH(payload_length, 7);
                header[pos++] = FRAME_SET_LENGTH(payload_length, 6);
                header[pos++] = FRAME_SET_LENGTH(payload_length, 5);
                header[pos++] = FRAME_SET_LENGTH(payload_length, 4);
                header[pos++] = FRAME_SET_LENGTH(payload_length, 3);
                header[pos++] = FRAME_SET_LENGTH(payload_length, 2);
            }
            header[pos++] = FRAME_SET_LENGTH(payload_length, 1);
            header[pos++] = FRAME_SET_LENGTH(payload_length, 0);
        }
        ap_fwrite(of, state->obb, (const char *)header, pos); /* Header */
        if (payload_length > 0) {
            if (ap_fwrite(of, state->obb,
                          (const char *)buffer,
                          buffer_size) == APR_SUCCESS) { /* Payload Data */
                written = buffer_size;
            }
        }
        if (ap_fflush(of, state->obb) != APR_SUCCESS) {
            written = 0;
        }
    }

    return written;
}

typedef struct
{
    int type;
    const unsigned char * buffer;
    size_t buffer_size;
    int done;
    size_t written;
} WebSocketMessageData;

/*
 * Sends a buffer of data via the WebSocket. Returns the number of bytes that
 * are actually written.
 *
 * If this function is called from a different thread than the one running the
 * main framing loop, the message will be queued and the calling thread will
 * block until the data is written by the main thread.
 */
static size_t CALLBACK mod_websocket_plugin_send(const WebSocketServer *server,
                                                 const int type,
                                                 const unsigned char *buffer,
                                                 const size_t buffer_size)
{
    size_t written = 0;

    /* Deal with size more that 63 bits - FIXME */
    /* FIXME - if sending a zero-length message, the API cannot distinguish
     * between success and failure */
    if ((server != NULL) && (server->state != NULL)) {
        WebSocketState *state = server->state;

        apr_thread_mutex_lock(state->mutex);

        if (apr_os_thread_equal(apr_os_thread_current(), state->main_thread)) {
            /* This is the main thread. It's safe to write messages directly. */
            written = mod_websocket_send_internal(state, type, buffer, buffer_size);
        }
        else if ((state->pollset != NULL) && (state->queue != NULL) &&
                 !state->closing) {
            /* Dispatch this message to the main thread. */
            apr_status_t rv;
            WebSocketMessageData msg = { 0 };

            /* Populate the message data. */
            msg.type = type;
            msg.buffer = buffer;
            msg.buffer_size = buffer_size;

            /* Queue the message. */
            do {
                rv = apr_queue_push(state->queue, &msg);
            } while (APR_STATUS_IS_EINTR(rv));

            if (rv != APR_SUCCESS) {
                /* Couldn't push the message onto the queue. */
                goto send_unlock;
            }

            /* Interrupt the pollset. */
            rv = apr_pollset_wakeup(state->pollset);

            if (rv != APR_SUCCESS) {
                /*
                 * Couldn't wake up poll...? We can't return zero since we've
                 * already pushed the message, and it might actually be sent...
                 */
                /* TODO: log. */
            }

            /* Wait for the message to be written. */
            while (!msg.done && !state->closing) {
                apr_thread_cond_wait(state->cond, state->mutex);
            }

            if (msg.done) {
                written = msg.written;
            }
        }

send_unlock:
        apr_thread_mutex_unlock(state->mutex);
    }

    return written;
}


static void CALLBACK mod_websocket_plugin_close(const WebSocketServer *
                                                server)
{
    if (server != NULL) {
        /* Send closing handshake */
        mod_websocket_plugin_send(server, MESSAGE_TYPE_CLOSE, NULL, 0);
    }
}

/*
 * Read a buffer of data from the input stream.
 */
static apr_status_t mod_websocket_read_nonblock(request_rec *r,
                                                apr_bucket_brigade *bb,
                                                char *buffer,
                                                apr_size_t *bufsiz)
{
    apr_status_t rv;

    if ((rv = ap_get_brigade(r->input_filters, bb, AP_MODE_READBYTES,
                             APR_NONBLOCK_READ, *bufsiz)) == APR_SUCCESS) {
        rv = apr_brigade_flatten(bb, buffer, bufsiz);
        ap_log_rerror(APLOG_MARK, APLOG_TRACE1, rv, r,
                      "read %ld bytes from brigade", (long) *bufsiz);

        apr_brigade_cleanup(bb);
    }

    if ((rv == APR_SUCCESS) && (*bufsiz == 0)) {
        /*
         * For some reason, nonblocking reads can return APR_SUCCESS even when
         * there was nothing actually read. Treat this as an EAGAIN.
         */
        rv = APR_EAGAIN;
    }

    return rv;
}

/*
 * Base64-encode the SHA-1 hash of the client-supplied key with the WebSocket
 * GUID appended to it.
 */
static void mod_websocket_handshake(request_rec *r, const char *key)
{
    apr_byte_t response[32];
    apr_byte_t digest[APR_SHA1_DIGESTSIZE];
    apr_sha1_ctx_t context;
    int len;

    apr_sha1_init(&context);
    apr_sha1_update(&context, key, strlen(key));
    apr_sha1_update(&context, WEBSOCKET_GUID, WEBSOCKET_GUID_LEN);
    apr_sha1_final(digest, &context);

    len = apr_base64_encode_binary((char *)response, digest, sizeof(digest));
    response[len] = '\0';

    apr_table_setn(r->headers_out, "Sec-WebSocket-Accept",
                   apr_pstrdup(r->pool, (const char *)response));
}

/*
 * Compatibility wrapper for ap_parse_token_list_strict(), which doesn't exist
 * until Apache 2.4.17. We remove the skip_invalid flag since we never ignore
 * invalid separators, and we assume the tokens array is always preinitialized.
 */
static const char* parse_token_list_strict(apr_pool_t *p, const char *tok,
                                           apr_array_header_t *tokens)
{
#if AP_MODULE_MAGIC_AT_LEAST(20120211,51)
    return ap_parse_token_list_strict(p, tok, &tokens,
                                      0 /* don't ignore invalid separators */);
#else
    /*
     * ap_get_token() allows a bunch of stuff we don't want, so we have to
     * perform more validity checks.
     */

    while (*tok) {
        const char *token;

        token = ap_get_token(p, &tok, 0);

        /*
         * Check that the token is valid before putting it into the array. Empty
         * tokens are fine (we must accept them per RFC 7230); we'll just skip
         * them.
         */
        if (token && *token) {
            const char *c;

            for (c = token; *c; ++c) {
                /*
                 * This is the T_HTTP_TOKEN_STOP check from gen_test_char.
                 * Disallow control characters and separators in tokens.
                 */
                if (apr_iscntrl(*c) || strchr(" \t()<>@,;:\\\"/[]?={}", *c)) {
                    return apr_psprintf(p, "Encountered illegal separator "
                                        "'\\x%.2x'", (unsigned int) *c);
                }
            }

            *((const char **) apr_array_push(tokens)) = token;
        }

        /*
         * ap_get_token() breaks tokens on whitespace, semicolons, and commas.
         * Make sure that we only get commas after a token.
         */
        if (*tok == ',') {
            ++tok;
        } else if (*tok) {
            return apr_psprintf(p, "Encountered illegal separator '\\x%.2x'",
                                (unsigned int) *tok);
        }
    }

    return NULL;
#endif
}

/*
 * The client-supplied WebSocket protocol entry consists of a list of
 * client-side supported protocols. Parse the list, and populate an array with
 * those protocol names.
 */
static apr_status_t parse_protocol(request_rec *r,
                                   apr_array_header_t *protocols)
{
    const char *sec_websocket_protocol;
    const char *error;

    sec_websocket_protocol = apr_table_get(r->headers_in,
                                           "Sec-WebSocket-Protocol");

    if (!sec_websocket_protocol) {
        /* A missing header means we have no requested subprotocols. */
        return APR_SUCCESS;
    }

    error = parse_token_list_strict(r->pool, sec_websocket_protocol, protocols);

    if (!error && apr_is_empty_array(protocols)) {
        /* Sec-WebSocket-Protocol must contain at least one valid token. */
        error = "Header contains no subprotocols";
    }

    if (error) {
        ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r,
                      "Client sent invalid Sec-WebSocket-Protocol: %s", error);
        return APR_EINVAL;
    }

    return APR_SUCCESS;
}

/*
 * Parses the protocol version from a Sec-WebSocket-Version header. Returns -1
 * if the version is invalid or prohibited by the RFC.
 */
static apr_int64_t parse_protocol_version(const char *version)
{
    size_t len;
    apr_int64_t result;

    if (!version) {
        return -1;
    }

    len = strlen(version);

    /*
     * We perform our checks up front because apr_atoi64() is rather permissive
     * in what it allows.
     *
     * The rules:
     * - No empty strings.
     * - All characters must be digits 0-9.
     * - No leading zeroes ("0" is okay, but not "013").
     * - Only values 0-255 are allowed.
     */
    if ((len < 1) || !apr_isdigit(version[0]) ||
        ((len > 1) && (!apr_isdigit(version[1]) || (version[0] == '0'))) ||
        ((len > 2) && !apr_isdigit(version[2])) ||
        (len > 3)) {
        return -1;
    }

    result = apr_atoi64(version);

    if (result < 0 || result > 255) {
        return -1;
    }

    return result;
}

/* Checks whether a string is a valid Sec-WebSocket-Key. */
static int is_valid_key(const char *key)
{
    /*
     * The key has to be a Base64-encoded value that decodes to 16 bytes long.
     * That means a valid encoded value is exactly 24 bytes long, with one of
     * the values 'A', 'Q', 'g', or 'w' for the final encoded character, and two
     * bytes of padding ("==") at the end.
     */
    static const char * const base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char * const final_chars = "AQgw";

    if (!key) {
        return 0;
    }

    return ((strlen(key) == 24) &&
            (strspn(key, base64_chars) == 22) &&
            (strspn(key + 21, final_chars) == 1) &&
            (key[22] == '=') && (key[23] == '='));
}

/*
 * Checks whether or not a parsed protocol version is supported by this module.
 */
static int is_supported_version(apr_int64_t version)
{
    int i;

    for (i = 0; i < supported_versions_len; ++i) {
        if (version == supported_versions[i]) {
            return 1;
        }
    }

    return 0;
}

/*
 * Creates a Sec-WebSocket-Version header value containing the supported
 * protocol versions.
 */
static const char *make_supported_version_header(apr_pool_t *pool)
{
    apr_array_header_t *versions = apr_array_make(pool, supported_versions_len,
                                                  sizeof(const char*));
    int i;

    /* Convert our supported versions to strings. */
    for (i = 0; i < supported_versions_len; ++i) {
        const char **new = (const char **) apr_array_push(versions);
        *new = apr_itoa(pool, supported_versions[i]);
    }

    /* Concatenate the version strings into a single header value. */
    return apr_array_pstrcat(pool, versions, ',');
}

typedef struct _WebSocketFrameData
{
    struct ap_varbuf message_buf;
    unsigned char fin;
    unsigned char opcode;
    unsigned int utf8_state;
    apr_int64_t message_length; /* length of the current message so far */
} WebSocketFrameData;

/* Variables that need to persist across calls to mod_websocket_handle_incoming */
typedef struct
{
    int framing_state;
    int closing; /* should the connection be closed due to incoming data? */
    unsigned short status_code;
    /* XXX fin and opcode appear to be duplicated with frame; can they be removed? */
    unsigned char fin;
    unsigned char opcode;
    WebSocketFrameData control_frame;
    WebSocketFrameData message_frame;
    WebSocketFrameData *frame;
    apr_int64_t payload_length; /* length of the current frame */
    apr_int64_t mask_offset;
    apr_int64_t extension_bytes_remaining;
    int payload_length_bytes_remaining;
    int masking;
    int mask_index;
    unsigned char mask[4];
} WebSocketReadState;

/*
 * Returns 1 if the given status code is prohibited from being sent by an
 * endpoint.
 */
static int is_prohibited_status_code(unsigned short status)
{
    return (
            /* 0-999 are not used. */
            (status < STATUS_CODE_OK) ||

            /* These three codes are reserved for client-side use. */
            (status == 1005) ||
            (status == 1006) ||
            (status == 1015) ||

            /* The spec only defines up to 4999. Be conservative and reject. */
            (status >= 5000)
           );
}

/*
 * Returns 1 if the given status code is currently marked reserved/unassigned by
 * the RFC and the close code registry, but it is not explicitly prohibited from
 * being sent by an endpoint.
 */
static int is_reserved_status_code(unsigned short status)
{
    return (
            /* Reserved -- old code from protocol version 8 */
            (status == STATUS_CODE_RESERVED) ||

            /* Unassigned. */
            (status == 1014) ||
            ((status >= 1016) && (status < 3000))
           );
}

/*
 * Checks the reason buffer for a Close frame and verifies that the status code
 * (contained in the first two bytes) is not prohibited by RFC 6455.
 *
 * The reject_reserved parameter controls whether reserved/unassigned codes are
 * rejected.
 */
static int is_valid_status_code(const unsigned char *buffer, size_t buffer_size,
                                int reject_reserved)
{
    unsigned short status;

    if (buffer_size < 2) {
        /* There is no status code; consider it valid. */
        return 1;
    }

    /* The status code is in the first two bytes of the reason buffer. */
    status  = buffer[0] << 8;
    status |= buffer[1];

    return !is_prohibited_status_code(status) &&
           !(reject_reserved && is_reserved_status_code(status));
}

/*
 * Constructs a serialized Origin string for the request URI. See RFC 6454.
 */
static const char *construct_request_origin(request_rec *r)
{
    const char *hostname = r->hostname;
    apr_port_t port = ap_get_server_port(r);

    if (ap_strchr_c(hostname, ':')) {
        /* IPv6 hostnames need to be bracketed. */
        hostname = apr_pstrcat(r->pool, "[", hostname, "]", NULL);
    }

    return apr_pstrcat(r->pool, ap_http_scheme(r), "://", hostname,
                       (ap_is_default_port(port, r) ?
                           NULL : apr_psprintf(r->pool, ":%d", port)),
                       NULL);
}

/*
 * Checks the origin of the incoming handshake and determines whether it's one
 * we trust.
 */
static int is_trusted_origin(request_rec *r, websocket_config_rec *conf,
                             apr_int64_t version) {
    const char *request_origin;
    const char *origin;
    int mode = conf->origin_check;

    if (mode == ORIGIN_CHECK_OFF) {
        /* No checks; trust everything. */
        return 1;
    }

    /*
     * Versions 8 (HyBi draft 12) and 13 use the Origin header; protocol 7 uses
     * Sec-WebSocket-Origin.
     */
    origin = apr_table_get(r->headers_in, (version < 8) ? "Sec-WebSocket-Origin"
                                                        : "Origin");

    if (!origin) {
        /*
         * A request without an Origin is not made on behalf of a user by a
         * user-agent, so we don't need to apply same-origin protection.
         */
        return 1;
    }

    if (mode == ORIGIN_CHECK_SAME) {
        /*
         * The origin of the request and the Origin sent by the user-agent must
         * match exactly.
         */
        request_origin = construct_request_origin(r);
        if (!request_origin) {
            return 0;
        }

        if (strcmp(origin, request_origin)) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r,
                          "Origin header '%s' sent by user-agent does not match "
                          "request origin '%s'; rejecting WebSocket upgrade",
                          origin, request_origin);
            return 0;
        }

        return 1;
    } else if (mode == ORIGIN_CHECK_TRUSTED) {
        /*
         * See if the Origin is in our allowlist.
         */
        void *val = apr_hash_get(conf->trusted_origins, origin,
                                 APR_HASH_KEY_STRING);

        if (!val) {
            ap_log_rerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS, r,
                          "Origin header '%s' sent by user-agent is not in the "
                          "Trusted list; rejecting WebSocket upgrade",
                          origin);
            return 0;
        }

        return 1;
    }

    ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, r,
                  "conf->origin_check is somehow an unexpected value");
    return 0;
}

/**
 * Reads from the given data block until the end of the block or a frame
 * boundary is encountered, handling plugin callbacks as messages are received.
 *
 * Returns the number of bytes parsed from the block. (At least one byte is
 * guaranteed to be handled per call, so the block must not be empty.) A return
 * value of zero indicates that the rest of the block is to be discarded and the
 * connection closed; state->status_code will be set accordingly.
 */
static apr_size_t mod_websocket_handle_frame(const WebSocketServer *server,
                                             unsigned char *block,
                                             apr_size_t block_size,
                                             WebSocketReadState *state,
                                             websocket_config_rec *conf,
                                             void *plugin_private)
{
    apr_size_t block_offset = 0;

    if (!block_size || state->closing) {
        /* handled_incoming() shouldn't be calling us in this case */
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, server->state->r,
                      "mod_websocket_handle_frame called %s",
                      (block_size ? "after close" : "with empty block"));

        state->status_code = STATUS_CODE_INTERNAL_ERROR;
        return 0;
    }

    switch (state->framing_state) {
    case DATA_FRAMING_START:
        /*
         * Since we don't currently support any extensions,
         * the reserve bits must be 0
         */
        if ((FRAME_GET_RSV1(block[block_offset]) != 0) ||
            (FRAME_GET_RSV2(block[block_offset]) != 0) ||
            (FRAME_GET_RSV3(block[block_offset]) != 0)) {
            state->status_code = STATUS_CODE_PROTOCOL_ERROR;
            return 0;
        }
        state->fin = FRAME_GET_FIN(block[block_offset]);
        state->opcode = FRAME_GET_OPCODE(block[block_offset++]);

        state->framing_state = DATA_FRAMING_PAYLOAD_LENGTH;

        if (state->opcode >= 0x8) { /* Control frame */
            if (state->fin) {
                state->frame = &state->control_frame;
                state->frame->opcode = state->opcode;
                state->frame->utf8_state = UTF8_VALID;
            }
            else {
                state->status_code = STATUS_CODE_PROTOCOL_ERROR;
                return 0;
            }
        }
        else { /* Message frame */
            state->frame = &state->message_frame;
            if (state->opcode) {
                if (state->frame->fin) {
                    state->frame->opcode = state->opcode;
                    state->frame->utf8_state = UTF8_VALID;
                }
                else {
                    state->status_code = STATUS_CODE_PROTOCOL_ERROR;
                    return 0;
                }
            }
            else if (state->frame->fin ||
                     ((state->opcode = state->frame->opcode) == 0)) {
                state->status_code = STATUS_CODE_PROTOCOL_ERROR;
                return 0;
            }
            state->frame->fin = state->fin;
        }
        state->payload_length = 0;
        state->payload_length_bytes_remaining = 0;

        if (block_offset >= block_size) {
            break; /* Only break if we need more data */
        }

    case DATA_FRAMING_PAYLOAD_LENGTH:
        state->payload_length = (apr_int64_t)
            FRAME_GET_PAYLOAD_LEN(block[block_offset]);
        state->masking = FRAME_GET_MASK(block[block_offset++]);

        if (state->payload_length == 126) {
            state->payload_length = 0;
            state->payload_length_bytes_remaining = 2;
        }
        else if (state->payload_length == 127) {
            state->payload_length = 0;
            state->payload_length_bytes_remaining = 8;
        }
        else {
            state->payload_length_bytes_remaining = 0;
        }
        if ((state->masking == 0) ||   /* Client-side mask is required */
            ((state->opcode >= 0x8) && /* Control opcodes cannot have a payload larger than 125 bytes */
             (state->payload_length_bytes_remaining != 0)) ||
            ((state->opcode == OPCODE_CLOSE) && /* Close payloads must be at least two bytes if not empty */
             (state->payload_length == 1))) {
            state->status_code = STATUS_CODE_PROTOCOL_ERROR;
            return 0;
        }
        else {
            state->framing_state = DATA_FRAMING_PAYLOAD_LENGTH_EXT;
        }
        if (block_offset >= block_size) {
            break;  /* Only break if we need more data */
        }

    case DATA_FRAMING_PAYLOAD_LENGTH_EXT:
        while ((state->payload_length_bytes_remaining > 0) &&
               (block_offset < block_size)) {
            state->payload_length *= 256;
            state->payload_length += block[block_offset++];
            state->payload_length_bytes_remaining--;
        }
        if (state->payload_length_bytes_remaining == 0) {
            state->frame->message_length += state->payload_length;

            if ((state->payload_length < 0) ||
                (state->frame->message_length < 0) ||
                (state->frame->message_length > conf->message_limit)) {
                /* Invalid message length */
                state->status_code = (server->state->protocol_version >= 13) ?
                                      STATUS_CODE_MESSAGE_TOO_LARGE :
                                      STATUS_CODE_RESERVED;
                return 0;
            }
            else if (state->masking != 0) {
                state->framing_state = DATA_FRAMING_MASK;
            }
            else {
                state->framing_state = DATA_FRAMING_EXTENSION_DATA;
                break;
            }
        }
        if (block_offset >= block_size) {
            break;  /* Only break if we need more data */
        }

    case DATA_FRAMING_MASK:
        while ((state->mask_index < 4) && (block_offset < block_size)) {
            state->mask[state->mask_index++] = block[block_offset++];
        }
        if (state->mask_index == 4) {
            state->framing_state = DATA_FRAMING_EXTENSION_DATA;
            state->mask_offset = 0;
            state->mask_index = 0;
            if ((state->mask[0] == 0) && (state->mask[1] == 0) &&
                (state->mask[2] == 0) && (state->mask[3] == 0)) {
                state->masking = 0;
            }
        }
        else {
            break;
        }
        /* Fall through */

    case DATA_FRAMING_EXTENSION_DATA:
        /* Deal with extension data when we support them -- FIXME */
        if (state->extension_bytes_remaining == 0) {
            if (state->payload_length > 0) {
                ap_varbuf_grow(&state->frame->message_buf,
                               state->frame->message_buf.strlen +
                                   state->payload_length);
            }
            state->framing_state = DATA_FRAMING_APPLICATION_DATA;
        }
        /* Fall through */

    case DATA_FRAMING_APPLICATION_DATA:
    {
        apr_int64_t block_data_length;
        apr_int64_t block_length = 0;
        apr_size_t message_len = state->frame->message_buf.strlen;
        unsigned char *message_data =
            (unsigned char*) state->frame->message_buf.buf;

        block_length = block_size - block_offset;
        block_data_length = (state->payload_length > block_length) ?
                            block_length : state->payload_length;

        if (state->masking) {
            apr_int64_t i;
            int validate = 0; /* whether we need to validate UTF-8 */
            apr_int64_t skip_bytes = 0; /* number of bytes to skip during validation */

            if (state->opcode == OPCODE_TEXT) {
                validate = 1;
            } else if (state->opcode == OPCODE_CLOSE) {
                /*
                 * Skip the first two status bytes of the response;
                 * they're not part of the UTF-8 payload.
                 */
                validate = 1;
                skip_bytes = 2;
            }

            if (validate) {
                unsigned int utf8_state = state->frame->utf8_state;
                unsigned char c;

                for (i = 0; i < block_data_length; i++) {
                    c = block[block_offset++] ^
                        state->mask[state->mask_offset++ & 3];
                    if (message_len >= skip_bytes) {
                        utf8_state = validate_utf8[utf8_state + c];
                        if (utf8_state == UTF8_INVALID) {
                            state->payload_length = block_data_length;
                            break;
                        }
                    }
                    message_data[message_len++] = c;
                }
                state->frame->utf8_state = utf8_state;
            }
            else {
                /* Need to optimize the unmasking -- FIXME */
                for (i = 0; i < block_data_length; i++) {
                    message_data[message_len++] =
                        block[block_offset++] ^
                        state->mask[state->mask_offset++ & 3];
                }
            }
        }
        else if (block_data_length > 0) {
            /* TODO: consolidate this code with the branch above. */
            int validate = 0; /* whether we need to validate UTF-8 */
            apr_int64_t skip_bytes = 0; /* number of bytes to skip during validation */

            memcpy(&message_data[message_len], &block[block_offset],
                   block_data_length);

            if (state->opcode == OPCODE_TEXT) {
                validate = 1;
            } else if (state->opcode == OPCODE_CLOSE) {
                /*
                 * Skip the first two status bytes of the response;
                 * they're not part of the UTF-8 payload.
                 */
                validate = 1;
                skip_bytes = 2;
            }

            if (validate) {
                apr_int64_t i;
                apr_int64_t message_data_end = message_len + block_data_length;
                unsigned int utf8_state = state->frame->utf8_state;

                for (i = message_len; i < message_data_end; i++) {
                    if (i >= skip_bytes) {
                        utf8_state = validate_utf8[utf8_state + message_data[i]];
                        if (utf8_state == UTF8_INVALID) {
                            state->payload_length = block_data_length;
                            break;
                        }
                    }
                }
                state->frame->utf8_state = utf8_state;
            }
            message_len += block_data_length;
            block_offset += block_data_length;
        }
        state->payload_length -= block_data_length;

        if (state->payload_length == 0) {
            int message_type = MESSAGE_TYPE_INVALID;

            switch (state->opcode) {
            case OPCODE_TEXT:
                if ((state->fin &&
                    (state->frame->utf8_state != UTF8_VALID)) ||
                    (state->frame->utf8_state == UTF8_INVALID)) {
                    state->status_code = STATUS_CODE_INVALID_UTF8;
                    return 0;
                }
                message_type = MESSAGE_TYPE_TEXT;
                break;

            case OPCODE_BINARY:
                message_type = MESSAGE_TYPE_BINARY;
                break;

            case OPCODE_CLOSE:
                if (!is_valid_status_code(message_data, message_len,
                                          !conf->allow_reserved)) {
                    state->status_code = STATUS_CODE_PROTOCOL_ERROR;
                } else if (state->frame->utf8_state != UTF8_VALID) {
                    state->status_code = STATUS_CODE_INVALID_UTF8;
                } else {
                    state->status_code = STATUS_CODE_OK;
                }
                return 0;

            case OPCODE_PING:
                apr_thread_mutex_lock(server->state->mutex);
                mod_websocket_send_internal(server->state, MESSAGE_TYPE_PONG,
                                            message_data, message_len);
                apr_thread_mutex_unlock(server->state->mutex);
                break;

            case OPCODE_PONG:
                break;

            default:
                state->status_code = STATUS_CODE_PROTOCOL_ERROR;
                return 0;
            }

            if (state->fin && (message_type != MESSAGE_TYPE_INVALID)) {
                conf->plugin->on_message(plugin_private, server, message_type,
                                         message_data, message_len);
            }

            /* Get ready for the next frame. */
            state->framing_state = DATA_FRAMING_START;

            if (state->fin) {
                apr_pool_t *pool = state->frame->message_buf.pool;

                ap_varbuf_free(&state->frame->message_buf);
                ap_varbuf_init(pool, &state->frame->message_buf, 0);

                state->frame->message_length = 0;
                message_len = 0;
            }
        }
        state->frame->message_buf.strlen = message_len;
        break;
    }

    default:
        state->status_code = STATUS_CODE_PROTOCOL_ERROR;
        return 0;
    }

    if (!block_offset) {
        /* This shouldn't happen; we're supposed to have guaranteed forward
         * progress for every call! */
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, server->state->r,
                      "mod_websocket_handle_frame made no forward progress");
        state->status_code = STATUS_CODE_INTERNAL_ERROR;
    }

    return block_offset;
}

/**
 * Handles as many bytes as possible from the given block, calling the plugin
 * as necessary and storing interim state in the WebSocketReadState.
 *
 * Errors are indicated by setting the state->status_code and marking the
 * connection for closure.
 */
static void mod_websocket_handle_incoming(const WebSocketServer *server,
                                          unsigned char *block,
                                          apr_size_t block_size,
                                          WebSocketReadState *state,
                                          websocket_config_rec *conf,
                                          void *plugin_private)
{
    apr_size_t handled_bytes;

    while (block_size > 0) {
        handled_bytes = mod_websocket_handle_frame(server, block, block_size,
                                                   state, conf, plugin_private);

        if (!handled_bytes) {
            /* Close the connection. */
            state->closing = 1;
            return;
        }

        block += handled_bytes;
        block_size -= handled_bytes;
    }
}

static void mod_websocket_handle_outgoing(const WebSocketServer *server,
                                          WebSocketMessageData *msg)
{
    apr_thread_mutex_lock(server->state->mutex);
    msg->written = mod_websocket_send_internal(server->state, msg->type,
                                               msg->buffer, msg->buffer_size);

    /*
     * Notify plugin_send() that the message has been sent.
     *
     * XXX Wake up _all_ the waiting threads, since we don't know which one owns
     * this message. This is contentious if there are a lot of threads writing
     * in parallel.
     */
    msg->done = 1;
    apr_thread_cond_broadcast(server->state->cond);

    apr_thread_mutex_unlock(server->state->mutex);
}

/*
 * Compatibility wrapper for ap_get_conn_socket(), which doesn't exist in Apache
 * 2.2.
 */
static apr_socket_t *get_conn_socket(conn_rec *conn)
{
#if AP_MODULE_MAGIC_AT_LEAST(20110605,2)
    return ap_get_conn_socket(conn);
#else
    return ap_get_module_config(conn->conn_config, &core_module);
#endif
}

/*
 * The data framing handler requires that the server state mutex is locked by
 * the caller upon entering this function. It will be locked when leaving too.
 *
 * The framing loop is the only place where data is written to or read from the
 * socket via the bucket brigades, to prevent simultaneous access to the
 * brigades.  Having a read-only thread and a write-only thread isn't good
 * enough, because filters (mod_ssl in particular) may read from the socket
 * during a write and vice-versa.
 *
 * The framing loop runs on the main request thread given to us by Apache.
 * Outgoing messages queued from another thread (by mod_websocket_plugin_send())
 * are dequeued and written here.
 */
static void mod_websocket_data_framing(const WebSocketServer *server,
                                       websocket_config_rec *conf,
                                       void *plugin_private)
{
    WebSocketState *state = server->state;
    request_rec *r = state->r;
    apr_bucket_brigade *ibb, *obb;
    apr_pollset_t *pollset;
    apr_pollfd_t pollfd = { 0 };
    const apr_pollfd_t *signalled;
    apr_int32_t pollcnt;
    apr_queue_t * queue;

    if (((ibb = apr_brigade_create(r->pool, r->connection->bucket_alloc)) != NULL) &&
        ((obb = apr_brigade_create(r->pool, r->connection->bucket_alloc)) != NULL) &&
        (apr_pollset_create(&pollset, 1, r->pool, APR_POLLSET_WAKEABLE) == APR_SUCCESS) &&
        (apr_queue_create(&queue, QUEUE_CAPACITY, r->pool) == APR_SUCCESS)) {
        unsigned char block[BLOCK_DATA_SIZE];
        apr_size_t block_size;
        unsigned char status_code_buffer[2];
        WebSocketReadState read_state = { 0 };

        read_state.framing_state = DATA_FRAMING_START;
        read_state.status_code = STATUS_CODE_OK;

        ap_varbuf_init(r->pool, &read_state.control_frame.message_buf, 0);
        read_state.control_frame.message_buf.strlen = 0;
        read_state.control_frame.fin = 1;
        read_state.control_frame.opcode = 8;
        read_state.control_frame.utf8_state = UTF8_VALID;

        ap_varbuf_init(r->pool, &read_state.message_frame.message_buf, 0);
        read_state.message_frame.message_buf.strlen = 0;
        read_state.message_frame.fin = 1;
        read_state.message_frame.opcode = 0;
        read_state.message_frame.utf8_state = UTF8_VALID;

        read_state.frame = &read_state.control_frame;
        read_state.opcode = 0xFF;

        state->queue = queue;

        /* Initialize the pollset */
        pollfd.p = r->pool;
        pollfd.desc_type = APR_POLL_SOCKET;
        pollfd.reqevents = APR_POLLIN;
        pollfd.desc.s = get_conn_socket(state->r->connection);
        apr_pollset_add(pollset, &pollfd);

        state->pollset = pollset;

        /* Allow the plugin to now write to the client */
        state->obb = obb;
        apr_thread_mutex_unlock(state->mutex);

        /*
         * Main loop, inspired by mod_spdy. Alternate between data coming from
         * the client and data coming from the server. Only block in poll() if
         * there is no work to be done for either side.
         */
        while (!read_state.closing) {
            apr_status_t rv;
            apr_interval_time_t timeout;
            WebSocketMessageData *msg;
            int work_done = 0;

            /* Check to see if there is any data to read. */
            block_size = sizeof(block);
            rv = mod_websocket_read_nonblock(r, ibb, (char *)block, &block_size);

            if (rv == APR_SUCCESS) {
                mod_websocket_handle_incoming(server, block, block_size,
                                              &read_state, conf, plugin_private);
                work_done = 1;
            }
            else if (!APR_STATUS_IS_EAGAIN(rv)) {
                /*
                 * APR_EOF just means the client aborted the TCP connection; no
                 * point in spamming the logs with errors in that case.
                 */
                int log_level = APR_STATUS_IS_EOF(rv) ? APLOG_DEBUG : APLOG_ERR;
                ap_log_rerror(APLOG_MARK, log_level, rv, r,
                              "nonblocking read from input brigade failed");

                read_state.status_code = STATUS_CODE_INTERNAL_ERROR;
                break;
            }

            /* Check to see if there is any data to write. */
            do {
                void *el;
                rv = apr_queue_trypop(state->queue, &el);
                msg = el;
            } while (APR_STATUS_IS_EINTR(rv));

            if (rv == APR_SUCCESS) {
                mod_websocket_handle_outgoing(server, msg);
                work_done = 1;
            }
            else if (!APR_STATUS_IS_EAGAIN(rv)) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                              "trypop from outgoing queue failed");
                read_state.status_code = STATUS_CODE_INTERNAL_ERROR;

                break;
            }

            /*
             * If there's nothing to do, wait for new work to come in.
             *
             * Because Windows cannot poll on both a file pipe and a socket,
             * plugin_send() uses apr_pollset_wakeup() to signal that new data
             * is available to write. This is lossy (multiple threads calling
             * wakeup() will result in only one wakeup here) so it's important
             * that we do not block until state->queue has emptied. Otherwise
             * it's possible to lose messages in the queue.
             *
             * NOTE: The wakeup pipe is drained only during apr_pollset_poll(),
             * so we call it each iteration to avoid filling it up. We only
             * block in poll() (negative timeout) if there was no work done
             * during the current iteration.
             */
            timeout = work_done ? 0 : -1;
            rv = apr_pollset_poll(state->pollset, timeout, &pollcnt, &signalled);

            if ((rv != APR_SUCCESS) && !APR_STATUS_IS_EINTR(rv) &&
                    !APR_STATUS_IS_TIMEUP(rv)) {
                ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r,
                              "poll from state->pollset failed");
                read_state.status_code = STATUS_CODE_INTERNAL_ERROR;
                break;
            }
        }

        ap_varbuf_free(&read_state.message_frame.message_buf);
        ap_varbuf_free(&read_state.control_frame.message_buf);

        /* Send server-side closing handshake */
        status_code_buffer[0] = (read_state.status_code >> 8) & 0xFF;
        status_code_buffer[1] = read_state.status_code & 0xFF;

        apr_thread_mutex_lock(state->mutex);
        mod_websocket_send_internal(state, MESSAGE_TYPE_CLOSE,
                                    status_code_buffer,
                                    sizeof(status_code_buffer));

        /* We are done with the bucket brigades */
        state->obb = NULL;
        apr_brigade_destroy(ibb);
        apr_brigade_destroy(obb);

        state->pollset = NULL;
        apr_pollset_destroy(pollset);

        state->queue = NULL;
        apr_queue_term(queue);
    }
}

/*
 * Checks to see if the client is asking for a WebSocket upgrade.
 */
static int is_websocket_upgrade(request_rec *r)
{
    const char *upgrade = apr_table_get(r->headers_in, "Upgrade");
    const char *connection = apr_table_get(r->headers_in, "Connection");
    int upgrade_connection = 0;

    if (r->proto_num < HTTP_VERSION(1, 1)) {
        /* Upgrade requires at least HTTP/1.1. */
        return 0;
    }

    if ((upgrade != NULL) &&
        (connection != NULL) && !strcasecmp(upgrade, "WebSocket")) {
        upgrade_connection = !strcasecmp(connection, "Upgrade");
        if (!upgrade_connection) {
            char *token = ap_get_token(r->pool, &connection, 0);

            while (token && *token) {       /* Parse the Connection value */
                upgrade_connection = !strcasecmp(token, "Upgrade");
                if (upgrade_connection) {
                    break;
                }
                while (*connection == ';') {
                    ++connection;
                    ap_get_token(r->pool, &connection, 0);  /* Skip parameters */
                }
                if (*connection++ != ',') {
                    break;  /* Invalid without comma */
                }
                token =
                    (*connection) ? ap_get_token(r->pool, &connection,
                                                 0) : NULL;
            }
        }
    }

    return upgrade_connection;
}

/*
 * This function creates the WebSocketState and WebSocketServer structures that
 * will be used for the entire connection, sets up the plugin that will handle
 * communication, sends the 101 to upgrade the connection, and starts the
 * framing loop.
 */
static void handle_websocket_connection(request_rec *r,
                                        websocket_config_rec *conf,
                                        apr_int64_t protocol_version,
                                        apr_array_header_t *protocols)
{
    WebSocketState state = {
        r, NULL, apr_os_thread_current(), NULL, NULL, protocols, 0,
        protocol_version, NULL, NULL
    };
    WebSocketServer server = {
        sizeof(WebSocketServer), 1, &state,
        mod_websocket_request, mod_websocket_header_get,
        mod_websocket_header_set,
        mod_websocket_protocol_count,
        mod_websocket_protocol_index,
        mod_websocket_protocol_set,
        mod_websocket_plugin_send, mod_websocket_plugin_close
    };
    void *plugin_private = NULL;

    apr_thread_mutex_create(&state.mutex,
                            APR_THREAD_MUTEX_DEFAULT,
                            r->pool);
    apr_thread_cond_create(&state.cond, r->pool);

    apr_thread_mutex_lock(state.mutex);

    /*
     * If the plugin supplies an on_connect function, it must
     * return non-null on success
     */
    if ((conf->plugin->on_connect == NULL) ||
        ((plugin_private =
          conf->plugin->on_connect(&server)) != NULL)) {
        /*
         * Now that the connection has been established,
         * disable the socket timeout
         */
        apr_socket_timeout_set(get_conn_socket(r->connection),
                               -1);

        /* Set response status code and status line */
        r->status = HTTP_SWITCHING_PROTOCOLS;
        r->status_line = ap_get_status_line(r->status);

        /* Send the headers */
        ap_send_interim_response(r, 1);

        ap_log_cerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS,
                      r->connection,
                      "established new WebSocket connection");

        /* The main data framing loop */
        mod_websocket_data_framing(&server, conf,
                                   plugin_private);

        /* Wake up any waiting plugin_sends before closing */
        apr_thread_cond_broadcast(state.cond);

        apr_thread_mutex_unlock(state.mutex);

        /* Tell the plugin that we are disconnecting */
        if (conf->plugin->on_disconnect != NULL) {
            conf->plugin->on_disconnect(plugin_private,
                                        &server);
        }
        r->connection->keepalive = AP_CONN_CLOSE;
    }
    else {
        apr_table_clear(r->headers_out);

        /* The connection has been refused */
        r->status = HTTP_FORBIDDEN;
        r->status_line = ap_get_status_line(r->status);
        r->header_only = 1;
        r->connection->keepalive = AP_CONN_CLOSE;

        ap_send_error_response(r, 0);

        apr_thread_mutex_unlock(state.mutex);
    }

    /* Close the connection */
    ap_log_cerror(APLOG_MARK, APLOG_INFO, APR_SUCCESS,
                  r->connection, "closing client connection");
    ap_lingering_close(r->connection);

    apr_thread_cond_destroy(state.cond);
    apr_thread_mutex_destroy(state.mutex);
}

/*
 * Compatibility wrapper for ap_remove_input_filter_byhandle(), which doesn't
 * exist until Apache 2.4.5.
 */
static apr_status_t remove_input_filter_byhandle(ap_filter_t *next,
                                                 const char *handle)
{
#if AP_MODULE_MAGIC_AT_LEAST(20120211,12)
    return ap_remove_input_filter_byhandle(next, handle);
#else
    ap_filter_t *filter;

    for (filter = next; filter != NULL; filter = filter->next) {
        if ((filter->frec != NULL) &&
            (filter->frec->name != NULL) &&
            !strcasecmp(filter->frec->name, handle)) {
            ap_remove_input_filter(filter);
            return APR_SUCCESS;
        }
    }

    return APR_NOTFOUND;
#endif
}

/*
 * This is the WebSocket request handler. Since WebSocket headers are quite
 * similar to HTTP headers, we will use most of the HTTP protocol handling
 * code. The difference is that we will disable the HTTP content body handling,
 * and then process the body according to the WebSocket specification.
 */
static int mod_websocket_method_handler(request_rec *r)
{
    const char *host;
    const char *sec_websocket_key;
    const char *sec_websocket_version;
    apr_int64_t protocol_version;
    apr_array_header_t *protocols;
    websocket_config_rec *conf;

    if (strcmp(r->handler, "websocket-handler") || !r->headers_in) {
        /* We're not configured as a handler for this request. */
        return DECLINED;
    }

    if (!is_websocket_upgrade(r)) {
        /* Don't try to handle any non-WebSocket requests. */
        return DECLINED;
    }

    /*
     * At this point, we know we're the correct handler for this request. Now
     * check the client's handshake.
     *
     * Need to serialize the connections to minimize a denial of service attack -- FIXME
     */

    host                   = apr_table_get(r->headers_in, "Host");
    sec_websocket_key      = apr_table_get(r->headers_in, "Sec-WebSocket-Key");
    sec_websocket_version  = apr_table_get(r->headers_in,
                                           "Sec-WebSocket-Version");
    protocol_version       = parse_protocol_version(sec_websocket_version);
    protocols              = apr_array_make(r->pool, 1, sizeof(char *));

    if ((r->method_number != M_GET) || r->header_only ||
        !host || !r->parsed_uri.path ||
        !is_valid_key(sec_websocket_key) ||
        !is_supported_version(protocol_version) ||
        (parse_protocol(r, protocols) != APR_SUCCESS)) {

        /*
         * If the client requested an upgrade to WebSocket, but the
         * handshake failed, explicitly respond with 400 instead of passing
         * this request to the next handler.
         */
        if (!is_supported_version(protocol_version)) {
            /* Tell the client what versions we support. */
            apr_table_setn(r->err_headers_out, "Sec-WebSocket-Version",
                           make_supported_version_header(r->pool));
        }

        return HTTP_BAD_REQUEST;
    }

    /* Client handshake is good. Figure out which plugin we're calling. */
    conf = (websocket_config_rec *) ap_get_module_config(r->per_dir_config,
                                                         &websocket_module);

    if (!conf || !conf->plugin) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APR_SUCCESS, r,
                      "no WebSocket plugin is assigned for location %s (did "
                      "you forget to define a WebSocketHandler?)",
                      r->parsed_uri.path);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* Make sure the Origin sent by the client is good enough for us. */
    if (!is_trusted_origin(r, conf, protocol_version)) {
        return HTTP_FORBIDDEN;
    }

    /*
     * Since we are handling a WebSocket connection, not a standard HTTP
     * connection, remove the HTTP input filter.
     */
    remove_input_filter_byhandle(r->input_filters, "http_in");

    /*
     * mod_reqtimeout treats the entire WebSocket connection as the request body
     * and will kill our connection if it exceeds the body timeout. Remove it
     * from the filter list.
     */
    remove_input_filter_byhandle(r->input_filters, "reqtimeout");

    /*
     * Now fix up the input filters (ap_remove_input_filter_byhandle() doesn't
     * guarantee this for us).
     */
    r->input_filters = r->connection->input_filters;
    r->proto_input_filters = r->connection->input_filters;

    apr_table_clear(r->headers_out);
    apr_table_setn(r->headers_out, "Upgrade", "websocket");
    apr_table_setn(r->headers_out, "Connection", "Upgrade");

    /* Set the expected acceptance response */
    mod_websocket_handshake(r, sec_websocket_key);

    /* We're ready to go. Take control of the connection. */
    handle_websocket_connection(r, conf, protocol_version, protocols);

    return OK;
}

static const command_rec websocket_cmds[] = {
    AP_INIT_FLAG("WebSocketAllowReservedStatusCodes",
                 mod_websocket_conf_allow_reserved, NULL, OR_AUTHCFG,
                 "Specifies whether endpoints may send reserved close status codes"),
    AP_INIT_TAKE2("WebSocketHandler", mod_websocket_conf_handler, NULL,
                  OR_AUTHCFG,
                  "Shared library containing WebSocket implementation followed by function initialization function name"),
    AP_INIT_TAKE1("WebSocketOriginCheck", mod_websocket_conf_origin_check, NULL,
                  OR_AUTHCFG,
                  "Specifies whether (and how) the Origin header should be checked during the opening handshake (Off|Same|Trusted). Defaults to Same."),
    AP_INIT_ITERATE("WebSocketTrustedOrigin", mod_websocket_conf_add_origin,
                    NULL, OR_AUTHCFG,
                    "Specifies one or more trusted Origins that are accepted during the opening handshake"),
    AP_INIT_TAKE1("WebSocketMaxMessageSize",
                  mod_websocket_conf_max_message_size, NULL, OR_AUTHCFG,
                  "Maximum size (in bytes) of a message to accept; default is 33554432 bytes (32 MB)"),

    /* Obsolete alias for WebSocketMaxMessageSize. */
    AP_INIT_TAKE1("MaxMessageSize", mod_websocket_conf_max_message_size, NULL,
                  OR_AUTHCFG, "Obsolete; use WebSocketMaxMessageSize instead"),
    {NULL}
};

/* Declare the handlers for other events. */
static void mod_websocket_register_hooks(apr_pool_t *p)
{
    /* Register for method calls. */
    ap_hook_handler(mod_websocket_method_handler, NULL, NULL,
                    APR_HOOK_FIRST - 1);
}

module AP_MODULE_DECLARE_DATA websocket_module = {
    STANDARD20_MODULE_STUFF,
    mod_websocket_create_dir_config,    /* create per-directory config structure */
    NULL,                               /* merge per-directory config structures */
    NULL,                               /* create server config structure */
    NULL,                               /* merge server config structures */
    websocket_cmds,                     /* command table */
    mod_websocket_register_hooks,       /* register hooks */
};
