/*
 * Copyright 2013 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#define _GNU_SOURCE

#include "mongoc-cursor.h"
#include "mongoc-cursor-private.h"
#include "mongoc-client-private.h"
#include "mongoc-counters-private.h"
#include "mongoc-error.h"
#include "mongoc-log.h"
#include "mongoc-opcode.h"
#include "mongoc-trace.h"


#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "cursor"


static const char *
_mongoc_cursor_get_read_mode_string (mongoc_read_mode_t mode)
{
   switch (mode) {
   case MONGOC_READ_PRIMARY:
      return "primary";
   case MONGOC_READ_PRIMARY_PREFERRED:
      return "primaryPreferred";
   case MONGOC_READ_SECONDARY:
      return "secondary";
   case MONGOC_READ_SECONDARY_PREFERRED:
      return "secondaryPreferred";
   case MONGOC_READ_NEAREST:
      return "nearest";
   default:
      return "";
   }
}

static int32_t
_mongoc_n_return (mongoc_cursor_t * cursor)
{
   /* by default, use the batch size */
   int32_t r = cursor->batch_size;

   /* if we have a limit */
   if (cursor->limit) {
      /* calculate remaining */
      uint32_t remaining = cursor->limit - cursor->count;

      /* if we had a batch size */
      if (r) {
         /* use min of batch or remaining */
         r = MIN(r, (int32_t)remaining);
      } else {
         /* if we didn't, just use the remaining */
         r = remaining;
      }

      r = -r;
   }

   return r;
}

mongoc_cursor_t *
_mongoc_cursor_new (mongoc_client_t           *client,
                    const char                *db_and_collection,
                    mongoc_query_flags_t       flags,
                    uint32_t                   skip,
                    uint32_t                   limit,
                    uint32_t                   batch_size,
                    bool                       is_command,
                    const bson_t              *query,
                    const bson_t              *fields,
                    const mongoc_read_prefs_t *read_prefs)
{
   mongoc_read_mode_t mode;
   mongoc_cursor_t *cursor;
   const bson_t *tags;
   const char *mode_str;
   bson_t child;

   ENTRY;

   BSON_ASSERT(client);
   BSON_ASSERT(db_and_collection);
   BSON_ASSERT(query);

   /* we can't have exhaust queries with limits */
   BSON_ASSERT (!((flags & MONGOC_QUERY_EXHAUST) && limit));

   /* we can't have exhaust queries with sharded clusters */
   BSON_ASSERT (!((flags & MONGOC_QUERY_EXHAUST) && client->cluster.isdbgrid));

   /*
    * Cursors execute their query lazily. This sadly means that we must copy
    * some extra data around between the bson_t structures. This should be
    * small in most cases, so it reduces to a pure memcpy. The benefit to this
    * design is simplified error handling by API consumers.
    */

   cursor = bson_malloc0(sizeof *cursor);
   cursor->client = client;
   bson_strcpy_w_null(cursor->ns, db_and_collection, sizeof cursor->ns);
   cursor->nslen = (uint32_t)strlen(cursor->ns);
   cursor->flags = flags;
   cursor->skip = skip;
   cursor->limit = limit;
   cursor->batch_size = batch_size;

   cursor->is_command = is_command;

   if (!cursor->is_command && !bson_has_field (query, "$query")) {
      bson_init (&cursor->query);
      bson_append_document (&cursor->query, "$query", 6, query);
   } else {
      bson_copy_to (query, &cursor->query);
   }

   if (read_prefs) {
      cursor->read_prefs = mongoc_read_prefs_copy (read_prefs);

      mode = mongoc_read_prefs_get_mode (read_prefs);
      tags = mongoc_read_prefs_get_tags (read_prefs);

      if (mode != MONGOC_READ_PRIMARY) {
         flags |= MONGOC_QUERY_SLAVE_OK;

         if ((mode != MONGOC_READ_SECONDARY_PREFERRED) || tags) {
            bson_append_document_begin (&cursor->query, "$readPreference",
                                        15, &child);
            mode_str = _mongoc_cursor_get_read_mode_string (mode);
            bson_append_utf8 (&child, "mode", 4, mode_str, -1);
            if (tags) {
               bson_append_array (&child, "tags", 4, tags);
            }
            bson_append_document_end (&cursor->query, &child);
         }
      }
   }

   if (fields) {
      bson_copy_to(fields, &cursor->fields);
   } else {
      bson_init(&cursor->fields);
   }

   _mongoc_buffer_init(&cursor->buffer, NULL, 0, NULL);

   mongoc_counter_cursors_active_inc();

   RETURN(cursor);
}


static void
_mongoc_cursor_kill_cursor (mongoc_cursor_t *cursor,
                            int64_t     cursor_id)
{
   mongoc_rpc_t rpc = {{ 0 }};

   ENTRY;

   bson_return_if_fail(cursor);
   bson_return_if_fail(cursor_id);

   rpc.kill_cursors.msg_len = 0;
   rpc.kill_cursors.request_id = 0;
   rpc.kill_cursors.response_to = 0;
   rpc.kill_cursors.opcode = MONGOC_OPCODE_KILL_CURSORS;
   rpc.kill_cursors.zero = 0;
   rpc.kill_cursors.cursors = &cursor_id;
   rpc.kill_cursors.n_cursors = 1;

   _mongoc_client_sendv (cursor->client, &rpc, 1, 0, NULL, NULL, NULL);

   EXIT;
}


void
mongoc_cursor_destroy (mongoc_cursor_t *cursor)
{
   BSON_ASSERT(cursor);

   if (cursor->iface.destroy) {
      cursor->iface.destroy(cursor);
   } else {
      _mongoc_cursor_destroy(cursor);
   }

   EXIT;
}

void
_mongoc_cursor_destroy (mongoc_cursor_t *cursor)
{
   ENTRY;

   bson_return_if_fail(cursor);

   if (cursor->in_exhaust) {
      cursor->client->in_exhaust = false;

      if (!cursor->done) {
         _mongoc_cluster_disconnect_node (
            &cursor->client->cluster,
            &cursor->client->cluster.nodes[cursor->hint - 1]);
      }
   } else if (cursor->rpc.reply.cursor_id) {
      _mongoc_cursor_kill_cursor(cursor, cursor->rpc.reply.cursor_id);
   }

   if (cursor->reader) {
      bson_reader_destroy(cursor->reader);
      cursor->reader = NULL;
   }

   bson_destroy(&cursor->query);
   bson_destroy(&cursor->fields);
   _mongoc_buffer_destroy(&cursor->buffer);
   mongoc_read_prefs_destroy(cursor->read_prefs);

   bson_free(cursor);

   mongoc_counter_cursors_active_dec();
   mongoc_counter_cursors_disposed_inc();

   EXIT;
}


static void
_mongoc_cursor_populate_error (mongoc_cursor_t *cursor,
                               const bson_t    *doc,
                               bson_error_t    *error)
{
   uint32_t code = MONGOC_ERROR_QUERY_FAILURE;
   bson_iter_t iter;
   const char *msg = "Unknown query failure";

   BSON_ASSERT (cursor);
   BSON_ASSERT (doc);
   BSON_ASSERT (error);

   if (bson_iter_init_find (&iter, doc, "code") &&
       BSON_ITER_HOLDS_INT32 (&iter)) {
      code = bson_iter_int32 (&iter);
   }

   if (bson_iter_init_find (&iter, doc, "$err") &&
       BSON_ITER_HOLDS_UTF8 (&iter)) {
      msg = bson_iter_utf8 (&iter, NULL);
   }

   if (cursor->is_command &&
       bson_iter_init_find (&iter, doc, "errmsg") &&
       BSON_ITER_HOLDS_UTF8 (&iter)) {
      msg = bson_iter_utf8 (&iter, NULL);
   }

   bson_set_error(error, MONGOC_ERROR_QUERY, code, "%s", msg);
}


static bool
_mongoc_cursor_unwrap_failure (mongoc_cursor_t *cursor)
{
   bson_iter_t iter;
   bson_t b;

   ENTRY;

   bson_return_val_if_fail(cursor, false);

   if (cursor->rpc.header.opcode != MONGOC_OPCODE_REPLY) {
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_PROTOCOL,
                     MONGOC_ERROR_PROTOCOL_INVALID_REPLY,
                     "Received rpc other than OP_REPLY.");
      RETURN(true);
   }

   if ((cursor->rpc.reply.flags & MONGOC_REPLY_QUERY_FAILURE)) {
      if (_mongoc_rpc_reply_get_first(&cursor->rpc.reply, &b)) {
         _mongoc_cursor_populate_error(cursor, &b, &cursor->error);
         bson_destroy(&b);
      } else {
         bson_set_error(&cursor->error,
                        MONGOC_ERROR_QUERY,
                        MONGOC_ERROR_QUERY_FAILURE,
                        "Unknown query failure.");
      }
      RETURN(true);
   } else if (cursor->is_command) {
      if (_mongoc_rpc_reply_get_first(&cursor->rpc.reply, &b)) {
         if ( bson_iter_init_find(&iter, &b, "ok") &&
              bson_iter_as_bool(&iter)) {
            return false;
         } else {
            _mongoc_cursor_populate_error(cursor, &b, &cursor->error);
            bson_destroy(&b);
            return true;
         }
      } else {
         return true;
      }
   }

   if ((cursor->rpc.reply.flags & MONGOC_REPLY_CURSOR_NOT_FOUND)) {
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_CURSOR,
                     MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                     "The cursor is invalid or has expired.");
      RETURN(true);
   }

   RETURN(false);
}


static bool
_mongoc_cursor_query (mongoc_cursor_t *cursor)
{
   uint32_t hint;
   uint32_t request_id;
   mongoc_rpc_t rpc;

   ENTRY;

   bson_return_val_if_fail(cursor, false);

   if (!_mongoc_client_warm_up (cursor->client, &cursor->error)) {
      cursor->failed = true;
      RETURN (false);
   }

   rpc.query.msg_len = 0;
   rpc.query.request_id = 0;
   rpc.query.response_to = 0;
   rpc.query.opcode = MONGOC_OPCODE_QUERY;
   rpc.query.flags = cursor->flags;
   rpc.query.collection = cursor->ns;
   rpc.query.skip = cursor->skip;
   if ((cursor->flags & MONGOC_QUERY_TAILABLE_CURSOR)) {
      rpc.query.n_return = 0;
   } else {
      rpc.query.n_return = _mongoc_n_return(cursor);
   }
   rpc.query.query = bson_get_data(&cursor->query);
   rpc.query.fields = bson_get_data(&cursor->fields);

   if (!(hint = _mongoc_client_sendv (cursor->client, &rpc, 1, 0,
                                      NULL, cursor->read_prefs,
                                      &cursor->error))) {
      goto failure;
   }

   cursor->hint = hint;
   request_id = BSON_UINT32_FROM_LE(rpc.header.request_id);

   _mongoc_buffer_clear(&cursor->buffer, false);

   if (!_mongoc_client_recv(cursor->client,
                            &cursor->rpc,
                            &cursor->buffer,
                            hint,
                            &cursor->error)) {
      goto failure;
   }

   if ((cursor->rpc.header.opcode != MONGOC_OPCODE_REPLY) ||
       (cursor->rpc.header.response_to != request_id)) {
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_PROTOCOL,
                     MONGOC_ERROR_PROTOCOL_INVALID_REPLY,
                     "A reply to an invalid request id was received.");
      goto failure;
   }

   if (_mongoc_cursor_unwrap_failure(cursor)) {
      if ((cursor->error.domain == MONGOC_ERROR_QUERY) &&
          (cursor->error.code == MONGOC_ERROR_QUERY_NOT_TAILABLE)) {
         cursor->failed = true;
      }
      goto failure;
   }

   if (cursor->reader) {
      bson_reader_destroy(cursor->reader);
   }

   cursor->reader = bson_reader_new_from_data(cursor->rpc.reply.documents,
                                              cursor->rpc.reply.documents_len);

   if (cursor->flags & MONGOC_QUERY_EXHAUST) {
      cursor->in_exhaust = true;
      cursor->client->in_exhaust = true;
   }

   cursor->done = false;
   cursor->end_of_event = false;
   cursor->sent = true;
   RETURN(true);

failure:
   cursor->failed = true;
   cursor->done = true;
   RETURN(false);
}


static bool
_mongoc_cursor_get_more (mongoc_cursor_t *cursor)
{
   uint64_t cursor_id;
   uint32_t request_id;
   mongoc_rpc_t rpc;

   ENTRY;

   BSON_ASSERT(cursor);

   if (! cursor->in_exhaust) {
      if (!_mongoc_client_warm_up (cursor->client, &cursor->error)) {
         cursor->failed = true;
         RETURN (false);
      }

      if (!(cursor_id = cursor->rpc.reply.cursor_id)) {
         bson_set_error(&cursor->error,
                        MONGOC_ERROR_CURSOR,
                        MONGOC_ERROR_CURSOR_INVALID_CURSOR,
                        "No valid cursor was provided.");
         goto failure;
      }

      rpc.get_more.msg_len = 0;
      rpc.get_more.request_id = 0;
      rpc.get_more.response_to = 0;
      rpc.get_more.opcode = MONGOC_OPCODE_GET_MORE;
      rpc.get_more.zero = 0;
      rpc.get_more.collection = cursor->ns;
      if ((cursor->flags & MONGOC_QUERY_TAILABLE_CURSOR)) {
         rpc.get_more.n_return = 0;
      } else {
         rpc.get_more.n_return = _mongoc_n_return(cursor);
      }
      rpc.get_more.cursor_id = cursor_id;

      /*
       * TODO: Stamp protections for disconnections.
       */

      if (!_mongoc_client_sendv(cursor->client, &rpc, 1, cursor->hint,
                                NULL, cursor->read_prefs, &cursor->error)) {
         cursor->done = true;
         cursor->failed = true;
         RETURN(false);
      }

      request_id = BSON_UINT32_FROM_LE(rpc.header.request_id);
   } else {
      request_id = BSON_UINT32_FROM_LE(cursor->rpc.header.request_id);
   }

   _mongoc_buffer_clear(&cursor->buffer, false);

   if (!_mongoc_client_recv(cursor->client,
                            &cursor->rpc,
                            &cursor->buffer,
                            cursor->hint,
                            &cursor->error)) {
      goto failure;
   }

   if ((cursor->rpc.header.opcode != MONGOC_OPCODE_REPLY) ||
       (cursor->rpc.header.response_to != request_id)) {
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_PROTOCOL,
                     MONGOC_ERROR_PROTOCOL_INVALID_REPLY,
                     "A reply to an invalid request id was received.");
      goto failure;
   }

   if (_mongoc_cursor_unwrap_failure(cursor)) {
      goto failure;
   }

   if (cursor->reader) {
      bson_reader_destroy(cursor->reader);
   }

   cursor->reader = bson_reader_new_from_data(cursor->rpc.reply.documents,
                                              cursor->rpc.reply.documents_len);

   cursor->end_of_event = false;

   RETURN(true);

failure:
   cursor->done = true;
   cursor->failed = true;

   RETURN(false);
}


bool
mongoc_cursor_error (mongoc_cursor_t *cursor,
                     bson_error_t    *error)
{
   bool ret;

   BSON_ASSERT(cursor);

   if (cursor->iface.error) {
      ret = cursor->iface.error(cursor, error);
   } else {
      ret = _mongoc_cursor_error(cursor, error);
   }

   if (ret && error) {
      /*
       * Rewrite the error code if we are talking to an older mongod
       * and the command was not found. It used to simply return an
       * error code of 17 and we can synthesize 59.
       */
      if (cursor->is_command &&
          (error->code == MONGOC_ERROR_PROTOCOL_ERROR)) {
         error->code = MONGOC_ERROR_QUERY_COMMAND_NOT_FOUND;
      }
   }

   RETURN(ret);
}


bool
_mongoc_cursor_error (mongoc_cursor_t *cursor,
                      bson_error_t    *error)
{
   ENTRY;

   bson_return_val_if_fail(cursor, false);

   if (BSON_UNLIKELY(cursor->failed)) {
      bson_set_error(error,
                     cursor->error.domain,
                     cursor->error.code,
                     "%s",
                     cursor->error.message);
      RETURN(true);
   }

   RETURN(false);
}


bool
mongoc_cursor_next (mongoc_cursor_t  *cursor,
                    const bson_t    **bson)
{
   bool ret;

   BSON_ASSERT(cursor);
   BSON_ASSERT(bson);

   if (cursor->iface.next) {
      ret = cursor->iface.next(cursor, bson);
   } else {
      ret = _mongoc_cursor_next(cursor, bson);
   }

   cursor->count++;

   RETURN(ret);
}


bool
_mongoc_cursor_next (mongoc_cursor_t  *cursor,
                     const bson_t    **bson)
{
   const bson_t *b;
   bool eof;

   ENTRY;

   BSON_ASSERT(cursor);


   if (cursor->client->in_exhaust && ! cursor->in_exhaust) {
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_CLIENT,
                     MONGOC_ERROR_CLIENT_IN_EXHAUST,
                     "Another cursor derived from this client is in exhaust.");
      cursor->failed = true;
      RETURN(false);
   }

   if (bson) {
      *bson = NULL;
   }

   if (cursor->limit && cursor->count >= cursor->limit) {
      return false;
   }

   /*
    * Short circuit if we are finished already.
    */
   if (BSON_UNLIKELY(cursor->done)) {
      RETURN(false);
   }

   /*
    * Check to see if we need to send a GET_MORE for more results.
    */
   if (!cursor->sent) {
      if (!_mongoc_cursor_query(cursor)) {
         RETURN(false);
      }
   } else if (BSON_UNLIKELY(cursor->end_of_event)) {
      if (!_mongoc_cursor_get_more(cursor)) {
         RETURN(false);
      }
   }

   /*
    * Read the next BSON document from the event.
    */
   eof = false;
   b = bson_reader_read(cursor->reader, &eof);
   cursor->end_of_event = eof;

   cursor->done = cursor->end_of_event && (
      (cursor->in_exhaust && !cursor->rpc.reply.cursor_id) ||
      (!b && !(cursor->flags & MONGOC_QUERY_TAILABLE_CURSOR))
      );

   /*
    * Do a supplimental check to see if we had a corrupted reply in the
    * document stream.
    */
   if (!b && !eof) {
      cursor->failed = true;
      bson_set_error(&cursor->error,
                     MONGOC_ERROR_CURSOR,
                     MONGOC_ERROR_PROTOCOL_INVALID_REPLY,
                     "The reply was corrupt.");
      RETURN(false);
   }

   if (bson) {
      *bson = b;
   }

   RETURN(!!b);
}


bool
mongoc_cursor_more (mongoc_cursor_t *cursor)
{
   bool ret;

   ENTRY;

   BSON_ASSERT(cursor);

   if (cursor->iface.more) {
      ret = cursor->iface.more(cursor);
   } else {
      ret = _mongoc_cursor_more(cursor);
   }

   RETURN(ret);
}


bool
_mongoc_cursor_more (mongoc_cursor_t *cursor)
{
   bson_return_val_if_fail(cursor, false);

   return ((!cursor->sent) ||
           (cursor->rpc.reply.cursor_id) ||
           !cursor->end_of_event);
}


void
mongoc_cursor_get_host (mongoc_cursor_t    *cursor,
                        mongoc_host_list_t *host)
{
   BSON_ASSERT(cursor);
   BSON_ASSERT(host);

   if (cursor->iface.get_host) {
      cursor->iface.get_host(cursor, host);
   } else {
      _mongoc_cursor_get_host(cursor, host);
   }

   EXIT;
}


void
_mongoc_cursor_get_host (mongoc_cursor_t    *cursor,
                         mongoc_host_list_t *host)
{
   bson_return_if_fail(cursor);
   bson_return_if_fail(host);

   memset(host, 0, sizeof *host);

   if (!cursor->hint) {
      MONGOC_WARNING("%s(): Must send query before fetching peer.",
                     __FUNCTION__);
      return;
   }

   *host = cursor->client->cluster.nodes[cursor->hint - 1].host;
   host->next = NULL;
}


mongoc_cursor_t *
mongoc_cursor_clone (const mongoc_cursor_t *cursor)
{
   mongoc_cursor_t *ret;

   BSON_ASSERT(cursor);

   if (cursor->iface.clone) {
      ret = cursor->iface.clone(cursor);
   } else {
      ret = _mongoc_cursor_clone(cursor);
   }

   RETURN(ret);
}


mongoc_cursor_t *
_mongoc_cursor_clone (const mongoc_cursor_t *cursor)
{
   mongoc_cursor_t *clone;

   ENTRY;

   BSON_ASSERT (cursor);

   clone = bson_malloc0 (sizeof *clone);

   clone->client = cursor->client;
   clone->is_command = cursor->is_command;
   clone->flags = cursor->flags;
   clone->skip = cursor->skip;
   clone->batch_size = cursor->batch_size;
   clone->limit = cursor->limit;
   clone->nslen = cursor->nslen;

   if (cursor->read_prefs) {
      clone->read_prefs = mongoc_read_prefs_copy (cursor->read_prefs);
   }

   bson_copy_to (&cursor->query, &clone->query);
   bson_copy_to (&cursor->fields, &clone->fields);

   bson_strcpy_w_null(clone->ns, cursor->ns, sizeof clone->ns);

   _mongoc_buffer_init (&clone->buffer, NULL, 0, NULL);

   mongoc_counter_cursors_active_inc ();

   RETURN(clone);
}
