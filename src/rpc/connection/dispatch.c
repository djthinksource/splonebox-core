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

#include <stdlib.h>

#include "rpc/sb-rpc.h"
#include "api/sb-api.h"
#include "sb-common.h"

static msgpack_sbuffer sbuf;
static hashmap(string, dispatch_info) *dispatch_table = NULL;
static hashmap(uint64_t, ptr_t) *callids = NULL;

int handle_error(uint64_t con_id, struct message_request *request,
    char *pluginkey, struct api_error *error)
{
  return (0);
}

/*
 * Dispatch a register message to API-register function
 *
 * @param params register arguments saved in `array`
 * @param api_error `struct api_error` error object-instance
 * @return 0 if success, -1 otherwise
 */
int handle_register(uint64_t con_id, struct message_request *request,
    char *pluginkey, struct api_error *error)
{
  array *meta = NULL;
  array functions;
  string name, description, author, license;

  if (!error || !request)
    return (-1);

  /* check params size */
  if (request->params.size != 2) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. Invalid params size");
    return (-1);
  }

  if (request->params.obj[0].type == OBJECT_TYPE_ARRAY)
    meta = &request->params.obj[0].data.params;
  else {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. meta params has wrong type");
    return (-1);
  }

  /*
   * meta params:
   * [name, description, author, license]
   */

  if (!meta) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. meta params is NULL");
    return (-1);
  }

  if (meta->size != 4) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. Invalid meta params size");
    return (-1);
  }

  /* extract meta information */
  if ((meta->obj[0].type != OBJECT_TYPE_STR) ||
      (meta->obj[1].type != OBJECT_TYPE_STR) ||
      (meta->obj[2].type != OBJECT_TYPE_STR) ||
      (meta->obj[3].type != OBJECT_TYPE_STR)) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. meta element has wrong type");
    return (-1);
  }

  if (!meta->obj[0].data.string.str || !meta->obj[1].data.string.str ||
      !meta->obj[2].data.string.str || !meta->obj[3].data.string.str) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. Invalid meta params size");
    return (-1);
  }

  name = meta->obj[0].data.string;
  description = meta->obj[1].data.string;
  author = meta->obj[2].data.string;
  license = meta->obj[3].data.string;

  if (request->params.obj[1].type != OBJECT_TYPE_ARRAY) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. functions has wrong type");
    return (-1);
  }

  functions = request->params.obj[1].data.params;

  if (api_register(name, description, author, license,
      functions, con_id, request->msgid, pluginkey, error) == -1) {

    if (!error->isset)
      error_set(error, API_ERROR_TYPE_VALIDATION,
         "Error running register API request.");

    return (-1);
  }

  return (0);
}


int handle_run(uint64_t con_id, struct message_request *request,
    char *pluginkey, struct api_error *error)
{
  uint64_t callid;
  array *meta = NULL;
  string function_name;
  char *targetpluginkey;

  struct message_object args_object;

  if (!error || !request)
    return (-1);

  /* check params size */
  if (request->params.size != 3) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. Invalid params size");
    return (-1);
  }

  if (request->params.obj[0].type == OBJECT_TYPE_ARRAY)
    meta = &request->params.obj[0].data.params;
  else {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. meta params has wrong type");
    return (-1);
  }

  if (!meta) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. meta params is NULL");
    return (-1);
  }

  /* meta = [targetpluginkey, nil]*/
  if (meta->size != 2) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. Invalid meta params size");
    return (-1);
  }

  /* extract meta information */
  if (meta->obj[0].type != OBJECT_TYPE_STR) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. meta elements have wrong type");
    return (-1);
  }

  if (!meta->obj[0].data.string.str ||
    meta->obj[0].data.string.length+1 != PLUGINKEY_STRING_SIZE) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. Invalid meta params size");
    return (-1);
  }

  targetpluginkey = meta->obj[0].data.string.str;
  to_upper(targetpluginkey);

  if (meta->obj[1].type != OBJECT_TYPE_NIL) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. meta elements have wrong type");
    return (-1);
  }

  if (request->params.obj[1].type != OBJECT_TYPE_STR) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. function string has wrong type");
    return (-1);
  }

  if (!request->params.obj[1].data.string.str) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching register API request. Invalid meta params size");
    return (-1);
  }

  function_name = request->params.obj[1].data.string;

  if (request->params.obj[2].type != OBJECT_TYPE_ARRAY) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching run API request. function string has wrong type");
    return (-1);
  }

  args_object = request->params.obj[2];
  callid = (uint64_t) randommod(281474976710656LL);
  LOG_VERBOSE(VERBOSE_LEVEL_1, "generated callid %lu\n", callid);
  hashmap_put(uint64_t, ptr_t)(callids, callid, pluginkey);

  if (api_run(targetpluginkey, function_name, callid, args_object, con_id,
      request->msgid, error) == -1) {
    if (false == error->isset)
      error_set(error, API_ERROR_TYPE_VALIDATION,
         "Error executing run API request.");
    return (-1);
  }

  return (0);
}

int handle_result(uint64_t con_id, struct message_request *request,
    char *pluginkey, struct api_error *error)
{
  uint64_t callid;
  array *meta = NULL;
  struct message_object args_object;

  char * targetpluginkey;

  if (!error || !request)
    return (-1);

  /* check params size */
  if (request->params.size != 2) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. Invalid params size");
    return (-1);
  }

  if (request->params.obj[0].type == OBJECT_TYPE_ARRAY)
    meta = &request->params.obj[0].data.params;
  else {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. meta params has wrong type");
    return (-1);
  }

  if (!meta) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. meta params is NULL");
    return (-1);
  }

  /* meta = [callid]*/
  if (meta->size != 1) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. Invalid meta params size");
    return (-1);
  }

  /* extract meta information */
  if (meta->obj[0].type != OBJECT_TYPE_UINT) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. meta elements have wrong type");
    return (-1);
  }

  callid = meta->obj[0].data.uinteger;

  if (request->params.obj[1].type != OBJECT_TYPE_ARRAY) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error dispatching result API request. function string has wrong type");
    return (-1);
  }

  args_object = request->params.obj[1];

  targetpluginkey = hashmap_get(uint64_t, ptr_t)(callids, callid);

  if (!targetpluginkey) {
    error_set(error, API_ERROR_TYPE_VALIDATION,
      "Failed to find target's key associated with given callid.");
    return (-1);
  }

  if (api_result(targetpluginkey, callid, args_object, con_id, request->msgid,
      error) == -1) {
    if (false == error->isset)
      error_set(error, API_ERROR_TYPE_VALIDATION,
        "Error executing result API request.");
    return (-1);
  }

  hashmap_del(uint64_t, ptr_t)(callids, callid);

  return (0);
}

void dispatch_table_put(string method, dispatch_info info)
{
  hashmap_put(string, dispatch_info)(dispatch_table, method, info);
}


dispatch_info dispatch_table_get(string method)
{
  return (hashmap_get(string, dispatch_info)(dispatch_table, method));
}

int dispatch_teardown(void)
{
  hashmap_free(string, dispatch_info)(dispatch_table);

  hashmap_free(uint64_t, ptr_t)(callids);

  return (0);
}


int dispatch_table_init(void)
{
  dispatch_info register_info = {.func = handle_register, .async = true,
      .name = (string) {.str = "register", .length = sizeof("register") - 1}};
  dispatch_info run_info = {.func = handle_run, .async = true,
      .name = (string) {.str = "run", .length = sizeof("run") - 1}};
  dispatch_info error_info = {.func = handle_error, .async = true,
      .name = (string) {.str = "error", .length = sizeof("error") - 1}};
  dispatch_info result_info = {.func = handle_result, .async = true,
      .name = (string) {.str = "result", .length = sizeof("result") - 1,}};

  msgpack_sbuffer_init(&sbuf);

  dispatch_table = hashmap_new(string, dispatch_info)();
  callids = hashmap_new(uint64_t, ptr_t)();

  if (!dispatch_table || !callids)
    return (-1);

  dispatch_table_put(register_info.name, register_info);
  dispatch_table_put(run_info.name, run_info);
  dispatch_table_put(error_info.name, error_info);
  dispatch_table_put(result_info.name, result_info);


  return (0);
}
