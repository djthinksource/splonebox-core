/**
 *    Copyright (C) 2015 splone UG
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    This file incorporates code covered by the following terms:
 *
 *    Copyright Neovim contributors. All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <msgpack/object.h>
#include <msgpack/pack.h>
#include <msgpack/sbuffer.h>
#include <msgpack/unpack.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "tweetnacl.h"
#include "rpc/sb-rpc.h"
#include "rpc/connection/connection.h"
#include "api/sb-api.h"

STATIC int parse_cb(inputstream *istream, void *data, bool eof);
STATIC void close_cb(uv_handle_t *handle);
STATIC void timer_cb(uv_timer_t *timer);
STATIC int connection_handle_request(struct connection *con,
    msgpack_object *obj);
STATIC void connection_handle_response(struct connection *con,
    msgpack_object *obj);
STATIC void connection_request_event(connection_request_event_info *info);
STATIC void connection_close(struct connection *con);
STATIC void call_set_error(struct connection *con, char *msg);
STATIC int is_valid_rpc_response(msgpack_object *obj, struct connection *con);
STATIC void free_connection(struct connection *con);
STATIC void incref(struct connection *con);
STATIC void decref(struct connection *con);

static uint64_t next_con_id = 1;
static hashmap(uint64_t, ptr_t) *connections = NULL;
static hashmap(cstr_t, uint64_t) *pluginkeys = NULL;
static msgpack_sbuffer sbuf;
equeue *equeue_root;
uv_loop_t loop;

int connection_init(void)
{
  connections = hashmap_new(uint64_t, ptr_t)();
  pluginkeys = hashmap_new(cstr_t, uint64_t)();

  if (dispatch_table_init() == -1)
    return (-1);

  if (!connections || !pluginkeys)
    return (-1);

  msgpack_sbuffer_init(&sbuf);

  return (0);
}

int connection_teardown(void)
{
  if (!connections)
    return (-1);

  struct connection *con;

  hashmap_foreach_value(connections, con, {
    connection_close(con);
  });

  hashmap_free(uint64_t, ptr_t)(connections);
  hashmap_free(cstr_t, uint64_t)(pluginkeys);

  dispatch_teardown();
  msgpack_sbuffer_destroy(&sbuf);

  return (0);
}

int connection_create(uv_stream_t *stream)
{
  int r;

  stream->data = NULL;

  struct connection *con = MALLOC(struct connection);

  if (con == NULL)
    return (-1);

  con->id = next_con_id++;
  con->msgid = 1;
  con->refcount = 1;
  con->mpac = msgpack_unpacker_new(MSGPACK_UNPACKER_INIT_BUFFER_SIZE);
  con->closed = false;
  con->queue = equeue_new(equeue_root);
  con->streams.read = inputstream_new(parse_cb, STREAM_BUFFER_SIZE, con);
  con->streams.write = outputstream_new(1024 * 1024);
  con->streams.uv = stream;
  con->cc.nonce = (uint64_t) randommod(281474976710656LL);

  if (ISODD(con->cc.nonce)) {
    con->cc.nonce++;
  }

  con->cc.receivednonce = 0;
  con->cc.state = TUNNEL_INITIAL;

  /* crypto minutekey timer */
  randombytes(con->cc.minutekey, sizeof con->cc.minutekey);
  randombytes(con->cc.lastminutekey, sizeof con->cc.lastminutekey);
  con->minutekey_timer.data = &con->cc;
  r = uv_timer_init(&loop, &con->minutekey_timer);
  sbassert(r == 0);
  r = uv_timer_start(&con->minutekey_timer, timer_cb, 60000, 60000);
  sbassert(r == 0);

  con->packet.data = NULL;
  con->packet.start = 0;
  con->packet.end = 0;
  con->packet.pos = 0;

  kv_init(con->callvector);

  inputstream_set(con->streams.read, stream);
  inputstream_start(con->streams.read);
  outputstream_set(con->streams.write, stream);

  hashmap_put(uint64_t, ptr_t)(connections, con->id, con);

  return (0);
}

STATIC void incref(struct connection *con)
{
  con->refcount++;
}

STATIC void decref(struct connection *con)
{
  if (!(--con->refcount)) {
    free_connection(con);
  }
}


int connection_hashmap_put(uint64_t id, struct connection *con)
{
  hashmap_put(uint64_t, ptr_t)(connections, id, con);
}

int pluginkeys_hashmap_put(char *pluginkey, uint64_t id)
{
  hashmap_put(cstr_t, uint64_t)(pluginkeys, pluginkey, id);
}

STATIC void free_connection(struct connection *con)
{
  hashmap_del(uint64_t, ptr_t)(connections, con->id);
  hashmap_del(cstr_t, uint64_t)(pluginkeys, con->cc.pluginkeystring);
  msgpack_unpacker_free(con->mpac);
  kv_destroy(con->callvector);
  equeue_free(con->queue);

  if (con->packet.data)
    FREE(con->packet.data);

  FREE(con);
}

STATIC void timer_cb(uv_timer_t *timer)
{
  struct crypto_context *cc = (struct crypto_context*)timer->data;
  crypto_update_minutekey(cc);
}

STATIC void connection_close(struct connection *con)
{
  int is_closing;
  uv_handle_t *handle;
  uv_handle_t *timer_handle;

  if (con->closed)
    return;

  timer_handle = (uv_handle_t*) &con->minutekey_timer;
  if (timer_handle) {
    uv_close(timer_handle, NULL);
    uv_run(&loop, UV_RUN_ONCE);
  }

  inputstream_free(con->streams.read);
  outputstream_free(con->streams.write);
  handle = (uv_handle_t *)con->streams.uv;

  if (handle)
    uv_close(handle, close_cb);

  kv_destroy(con->callvector);

  con->closed = 0;

  decref(con);
}


STATIC void close_cb(uv_handle_t *handle)
{
  FREE(handle->data);
  FREE(handle);
}

STATIC void reset_packet(struct connection *con)
{
  con->packet.start = 0;
  con->packet.end = 0;
  con->packet.pos = 0;
}

STATIC void reset_parser(struct connection *con)
{
  FREE(con->packet.data);
  reset_packet(con);
}

STATIC int parse_cb(inputstream *istream, void *data, bool eof)
{
  unsigned char *packet;
  unsigned char hellopacket[192];
  unsigned char initiatepacket[256];
  struct connection *con = data;

  incref(con);

  size_t read = 0;
  size_t pending;
  size_t size;
  uint64_t plaintextlen;
  uint64_t consumedlen = 0;
  uint64_t dummylen = 0;
  msgpack_unpacked result;

  if (eof) {
    connection_close(con);
    goto fail;
  }

  if (con->cc.state == TUNNEL_INITIAL) {
    size = inputstream_read(istream, hellopacket, 192);
    if (crypto_recv_hello_send_cookie(&con->cc, hellopacket,
        con->streams.write) != 0)
      LOG_WARNING("establishing crypto tunnel failed at hello-cookie packet");

    goto fail;
  } else if (con->cc.state == TUNNEL_COOKIE_SENT) {
    size = inputstream_read(istream, initiatepacket, 256);
    if (crypto_recv_initiate(&con->cc, initiatepacket) != 0) {
      LOG_WARNING("establishing crypto tunnel failed at initiate packet");
      con->cc.state = TUNNEL_INITIAL;
    }

    hashmap_put(cstr_t, uint64_t)(pluginkeys, con->cc.pluginkeystring,
      con->id);
  }

  pending = inputstream_pending(istream);

  if (pending <= 0 || con->cc.state != TUNNEL_ESTABLISHED)
    goto fail;

  if (con->packet.end <= 0) {
    packet = inputstream_get_read(istream, &read);

    /* read the packet length */
    if (crypto_verify_header(&con->cc, packet, &con->packet.length)) {
      reset_packet(con);
      goto fail;
    }

    con->packet.end = con->packet.length;
    con->packet.data = MALLOC_ARRAY(MAX(con->packet.end, read), unsigned char);

    if (!con->packet.data) {
      LOG_ERROR("Failed to alloc mem for con packet.");
      goto fail;
    }

    if (msgpack_unpacker_reserve_buffer(con->mpac,
      MAX(read, con->packet.end)) == false) {
      LOG_ERROR("Failed to reserve mem msgpack buffer.");
      goto fail;
    };

    /* get decrypted message start position */
    con->unpackbuf = msgpack_unpacker_buffer(con->mpac);
  }

  while(read > 0) {
    con->packet.start = inputstream_read(istream,
      con->packet.data + con->packet.pos, con->packet.end);
    con->packet.pos += con->packet.start;
    con->packet.end -= con->packet.start;
    read -= con->packet.start;

    if (read > 0 && con->packet.end == 0) {
      if (crypto_read(&con->cc, con->packet.data, con->unpackbuf +
          consumedlen, con->packet.length, &plaintextlen) != 0) {
        reset_parser(con);
        goto fail;
      }

      consumedlen += plaintextlen;
      packet = inputstream_get_read(istream, &dummylen);

      if (packet == NULL) {
        reset_parser(con);
        goto fail;
      }

      if (crypto_verify_header(&con->cc, packet, &con->packet.length)) {
        reset_parser(con);
        goto fail;
      }

      con->packet.end = con->packet.length;

      continue;
    }

    if (con->packet.end > 0 && read == 0) {
      decref(con);
      return (0);
    }

    if (crypto_read(&con->cc, con->packet.data, con->unpackbuf +
        consumedlen, con->packet.length, &plaintextlen) != 0) {
      reset_parser(con);
      goto fail;
    }

    consumedlen += plaintextlen;
    reset_packet(con);
    FREE(con->packet.data);
  }

  msgpack_unpacker_buffer_consumed(con->mpac, consumedlen);
  msgpack_unpacked_init(&result);
  msgpack_unpack_return ret;

  /* deserialize objects, one by one */
  while ((ret =
      msgpack_unpacker_next(con->mpac, &result)) == MSGPACK_UNPACK_SUCCESS) {
    if (message_is_request(&result.data))
      connection_handle_request(con, &result.data);
    else if (message_is_response(&result.data)) {
      if (is_valid_rpc_response(&result.data, con)) {
        connection_handle_response(con, &result.data);
      } else {
        call_set_error(con, "Returned response that doesn't have a matching "
                            "request id. Ensure the client is properly "
                            "synchronized");
      }
    } else {
      LOG_WARNING("invalid msgpack object");
      msgpack_object_print(stdout, result.data);
    }
  }

  decref(con);

  return (0);

fail:
  decref(con);
  return (-1);
}

struct callinfo connection_send_request(char *pluginkey, string method,
    array params, struct api_error *api_error)
{
  uint64_t id;
  struct connection *con;
  msgpack_packer packer;
  struct message_request request;

  id = hashmap_get(cstr_t, uint64_t)(pluginkeys, pluginkey);

  if (id == 0) {
    free_params(params);
    error_set(api_error, API_ERROR_TYPE_VALIDATION, "plugin not registered");
    return CALLINFO_INIT;
  }

  con = hashmap_get(uint64_t, ptr_t)(connections, id);

  /*
   * if no connection is available for the key, set the connection to the
   * the initial connection from the sender.
   */
  if (!con) {
    free_params(params);
    error_set(api_error, API_ERROR_TYPE_VALIDATION, "plugin not registered");
    return CALLINFO_INIT;
  }

  incref(con);
  request.msgid = con->msgid++;
  request.method = method;
  request.params = params;

  msgpack_packer_init(&packer, &sbuf, msgpack_sbuffer_write);
  message_serialize_request(&request, &packer);
  free_params(params);

  LOG_VERBOSE(VERBOSE_LEVEL_0, "sending request: method = %s,  callinfo id = %u\n",
      method, request.msgid);
  if (crypto_write(&con->cc, sbuf.data, sbuf.size, con->streams.write) != 0)
    return CALLINFO_INIT;

  struct callinfo cinfo = (struct callinfo) {request.msgid, false, false,((struct message_response) {0, ARRAY_INIT})};

  loop_wait_for_response(con, &cinfo);

  msgpack_sbuffer_clear(&sbuf);

  decref(con);

  if (cinfo.errorresponse)
    return CALLINFO_INIT;

  return cinfo;
}

int connection_send_response(uint64_t con_id, uint32_t msgid,
    array params, struct api_error *api_error)
{
  msgpack_packer packer;
  struct message_response response;
  struct connection *con;

  con = hashmap_get(uint64_t, ptr_t)(connections, con_id);

  /*
   * if no connection is available for the key, set the connection to the
   * the initial connection from the sender.
   */
  if (!con) {
    error_set(api_error, API_ERROR_TYPE_VALIDATION, "plugin not registered");
    return (-1);
  }

  response.msgid = msgid;
  response.params = params;

  msgpack_packer_init(&packer, &sbuf, msgpack_sbuffer_write);
  message_serialize_response(&response, &packer);

  if (api_error->isset) {
    return (-1);
  }

  if (crypto_write(&con->cc, sbuf.data, sbuf.size, con->streams.write) != 0) {
    return (-1);
  }

  msgpack_sbuffer_clear(&sbuf);
  free_params(params);

  return 0;
}


STATIC int connection_handle_request(struct connection *con,
    msgpack_object *obj)
{
  dispatch_info dispatcher;
  struct api_error api_error = ERROR_INIT;
  connection_request_event_info eventinfo;
  api_event event;

  if (!obj || !con)
    return (-1);

  if (message_deserialize_request(&eventinfo.request, obj, &api_error) != 0) {
    /* request wasn't parsed correctly, send error with pseudo RESPONSE ID*/
    eventinfo.request.msgid = MESSAGE_RESPONSE_UNKNOWN;
    eventinfo.request.method = cstring_copy_string("error");
  }

  LOG_VERBOSE(VERBOSE_LEVEL_0, "received request: method = %s\n",
      eventinfo.request.method.str);

  dispatcher = dispatch_table_get(eventinfo.request.method);

  if (dispatcher.func == NULL) {
    LOG_VERBOSE(VERBOSE_LEVEL_0, "could not dispatch method\n");
    error_set(&api_error, API_ERROR_TYPE_VALIDATION, "could not dispatch method");
    dispatcher.func = handle_error;
    dispatcher.async = true;
  }

  eventinfo.con = con;
  eventinfo.api_error = api_error;
  eventinfo.dispatcher = dispatcher;

  incref(con);

  if (dispatcher.async)
    connection_request_event(&eventinfo);
  else {
    event.handler = connection_request_event;
    event.info = eventinfo;
    equeue_put(con->queue, event);
    /* TODO: move this call to a suitable place (main?) */
    equeue_run_events(equeue_root);
  }

  return (0);
}


STATIC void connection_request_event(connection_request_event_info *eventinfo)
{
  msgpack_packer packer;
  struct connection *con;

  con = eventinfo->con;

  eventinfo->dispatcher.func(con->id, &eventinfo->request,
      con->cc.pluginkeystring, &eventinfo->api_error);

  if (eventinfo->api_error.isset) {
    msgpack_packer_init(&packer, &sbuf, msgpack_sbuffer_write);
    message_serialize_error_response(&packer, &eventinfo->api_error,
        eventinfo->request.msgid);

    crypto_write(&eventinfo->con->cc, sbuf.data, sbuf.size,
        eventinfo->con->streams.write);

    msgpack_sbuffer_clear(&sbuf);
  }

  free_params(eventinfo->request.params);
  free_string(eventinfo->request.method);

  decref(con);
}

STATIC int is_valid_rpc_response(msgpack_object *obj, struct connection *con)
{
  uint64_t msg_id = message_get_id(obj);

  return kv_size(con->callvector) && msg_id
      == kv_A(con->callvector, kv_size(con->callvector) - 1)->msgid;
}


STATIC void connection_handle_response(struct connection *con,
    msgpack_object *obj)
{
  struct callinfo *cinfo;
  struct api_error api_error = { .isset = false };

  cinfo = kv_A(con->callvector, kv_size(con->callvector) - 1);

  LOG_VERBOSE(VERBOSE_LEVEL_0, "received response: callinfo id = %u\n",
      cinfo->msgid);

  cinfo->hasresponse = true;
  cinfo->errorresponse = message_is_error_response(obj);

  if (cinfo->errorresponse) {
    message_deserialize_error_response(&cinfo->response, obj, &api_error);
  } else {
    message_deserialize_response(&cinfo->response, obj, &api_error);
  }
}

STATIC void call_set_error(struct connection *con, UNUSED(char *msg))
{
  struct callinfo *cinfo;

  for (size_t i = 0; i < kv_size(con->callvector); i++) {
      cinfo = kv_A(con->callvector, i);
      cinfo->errorresponse = true;
      cinfo->hasresponse = true;
  }

  connection_close(con);
}
