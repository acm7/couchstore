/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

/**
 * @copyright 2013 Couchbase, Inc.
 *
 * @author Filipe Manana  <filipe@couchbase.com>
 * @author Fulu Li        <fulu@couchbase.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 **/

#include "../bitfield.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include "bitmap.h"
#include "keys.h"
#include "reductions.h"
#include "values.h"
#include "reducers.h"
#include "../couch_btree.h"

typedef enum {
    VIEW_REDUCER_SUCCESS                = 0,
    VIEW_REDUCER_ERROR_NOT_A_NUMBER     = 1,
    VIEW_REDUCER_ERROR_BAD_STATS_OBJECT = 2
} builtin_reducer_error_t;

static const char *builtin_reducer_error_msg[] = {
    NULL,
    "Value is not a number",
    "Invalid _stats JSON object"
};

typedef struct {
    void  *parent_ctx;
    void  *mapreduce_ctx;
} reducer_ctx_t;

typedef couchstore_error_t (*reducer_fn_t)(const mapreduce_json_list_t *,
                                           const mapreduce_json_list_t *,
                                           reducer_ctx_t *,
                                           sized_buf *);

typedef struct {
    /* For builtin reduce operations that fail, this is the Key of the KV pair
       that caused the failure. For example, for a _sum reducer, if there's
       a value that is not a number, the reducer will abort and populate the
       field 'error_key' with the value's corresponding key and set 'error' to
       VIEW_REDUCER_ERROR_NOT_A_NUMBER. */
    const char              *error_key;
    builtin_reducer_error_t  builtin_error;

    /* For custom (JavaScript) reduce operations, this is the error message received
       from the JavaScript engine. */
    char                    *error_msg;
    mapreduce_error_t        mapreduce_error;

    unsigned                 num_reducers;
    reducer_fn_t             *reducers;
    reducer_ctx_t            *reducer_contexts;
} reducer_private_t;


#define DOUBLE_FMT "%.15lg"
#define scan_stats(buf, sum, count, min, max, sumsqr) \
        sscanf(buf, "{\"sum\":%lg,\"count\":%"SCNu64",\"min\":%lg,\"max\":%lg,\"sumsqr\":%lg}",\
               &sum, &count, &min, &max, &sumsqr)

#define sprint_stats(buf, sum, count, min, max, sumsqr) \
        sprintf(buf, "{\"sum\":%lg,\"count\":%llu,\"min\":%lg,\"max\":%lg,\"sumsqr\":%lg}",\
                sum, count, min, max, sumsqr)

static void free_key_excluding_elements(view_btree_key_t *key);
static void free_json_key_list(mapreduce_json_list_t *list);


static int json_to_str(const mapreduce_json_t *buf, char str[32])
{
    if (buf->length < 1 || buf->length > 31) {
        return 0;
    }
    memcpy(str, buf->json, buf->length);
    str[buf->length] = '\0';

    return 1;
}


static int json_to_double(const mapreduce_json_t *buf, double *out_num)
{
    char str[32];
    char *end;

    if (!json_to_str(buf, str)) {
        return 0;
    }
    *out_num = strtod(str, &end);

    return ((end > str) && (*end == '\0'));
}


static int json_to_uint64(const mapreduce_json_t *buf, uint64_t *out_num)
{
    char str[32];
    char *end;

    if (!json_to_str(buf, str)) {
        return 0;
    }
    *out_num = strtoull(str, &end, 10);

    return ((end > str) && (*end == '\0'));
}


couchstore_error_t view_id_btree_reduce(char *dst,
                                        size_t *size_r,
                                        const nodelist *leaflist,
                                        int count,
                                        void *ctx)
{
    view_id_btree_reduction_t *r = NULL;
    uint64_t subtree_count = 0;
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    const nodelist *i;

    (void) ctx;
    r = (view_id_btree_reduction_t *) malloc(sizeof(view_id_btree_reduction_t));
    if (r == NULL) {
        errcode = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto alloc_error;
    }
    memset(&r->partitions_bitmap, 0, sizeof(bitmap_t));

    for (i = leaflist; i != NULL && count > 0; i = i->next, count--) {
        view_id_btree_value_t *v = NULL;
        errcode = decode_view_id_btree_value(i->data.buf, i->data.size, &v);
        if (errcode != COUCHSTORE_SUCCESS) {
            goto alloc_error;
        }
        set_bit(&r->partitions_bitmap, v->partition);
        subtree_count++;
        free_view_id_btree_value(v);
    }
    r->kv_count = subtree_count;
    errcode = encode_view_id_btree_reduction(r, dst, size_r);

alloc_error:
    free_view_id_btree_reduction(r);

    return errcode;
}


couchstore_error_t view_id_btree_rereduce(char *dst,
                                          size_t *size_r,
                                          const nodelist *itmlist,
                                          int count,
                                          void *ctx)
{
    view_id_btree_reduction_t *r = NULL;
    uint64_t subtree_count = 0;
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    const nodelist *i;

    (void) ctx;
    r = (view_id_btree_reduction_t *) malloc(sizeof(view_id_btree_reduction_t));
    if (r == NULL) {
        errcode = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto alloc_error;
    }
    memset(&r->partitions_bitmap, 0, sizeof(bitmap_t));
    for (i = itmlist; i != NULL && count > 0; i = i->next, count--) {
        view_id_btree_reduction_t *r2 = NULL;
        errcode = decode_view_id_btree_reduction(i->pointer->reduce_value.buf, &r2);
        if (errcode != COUCHSTORE_SUCCESS) {
            goto alloc_error;
        }
        union_bitmaps(&r->partitions_bitmap, &r2->partitions_bitmap);
        subtree_count += r2->kv_count;
        free_view_id_btree_reduction(r2);
    }
    r->kv_count = subtree_count;
    errcode = encode_view_id_btree_reduction(r, dst, size_r);

alloc_error:
    free_view_id_btree_reduction(r);

    return errcode;
}


static couchstore_error_t builtin_sum_reducer(const mapreduce_json_list_t *keys,
                                              const mapreduce_json_list_t *values,
                                              reducer_ctx_t *ctx,
                                              sized_buf *buf)
{
    double n, sum = 0.0;
    int i, size;
    char red[512];

    for (i = 0; i < values->length; ++i) {
        mapreduce_json_t *value = &values->values[i];

        if (json_to_double(value, &n)) {
            sum += n;
        } else {
            reducer_private_t *priv = (reducer_private_t *) ctx->parent_ctx;

            priv->builtin_error = VIEW_REDUCER_ERROR_NOT_A_NUMBER;
            if (keys == NULL) {
                /* rereduce */
                return COUCHSTORE_ERROR_REDUCER_FAILURE;
            } else {
                /* reduce */
                mapreduce_json_t *key = &keys->values[i];
                char *error_key = (char *) malloc(key->length + 1);

                if (error_key == NULL) {
                    return COUCHSTORE_ERROR_ALLOC_FAIL;
                }
                memcpy(error_key, key->json, key->length);
                error_key[key->length] = '\0';
                priv->error_key = (const char *) error_key;

                return COUCHSTORE_ERROR_REDUCER_FAILURE;
            }
        }
    }

    size = sprintf(red, DOUBLE_FMT, sum);
    assert(size > 0);
    buf->buf = (char *) malloc(size);
    if (buf->buf == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }
    memcpy(buf->buf, red, size);
    buf->size = size;

    return COUCHSTORE_SUCCESS;
}


static couchstore_error_t builtin_count_reducer(const mapreduce_json_list_t *keys,
                                                const mapreduce_json_list_t *values,
                                                reducer_ctx_t *ctx,
                                                sized_buf *buf)
{
    uint64_t count = 0, n;
    int i, size;
    char red[512];

    for (i = 0; i < values->length; ++i) {
        if (keys == NULL) {
            /* rereduce */
            mapreduce_json_t *value = &values->values[i];

            if (json_to_uint64(value, &n)) {
                count += n;
            } else {
                reducer_private_t *priv = (reducer_private_t *) ctx->parent_ctx;
                priv->builtin_error = VIEW_REDUCER_ERROR_NOT_A_NUMBER;
                return COUCHSTORE_ERROR_REDUCER_FAILURE;
            }
        } else {
            /* reduce */
            count++;
        }
    }

    size = sprintf(red, "%llu", (unsigned long long) count);
    assert(size > 0);
    buf->buf = (char *) malloc(size);
    if (buf->buf == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }
    memcpy(buf->buf, red, size);
    buf->size = size;

    return COUCHSTORE_SUCCESS;
}


static couchstore_error_t builtin_stats_reducer(const mapreduce_json_list_t *keys,
                                                const mapreduce_json_list_t *values,
                                                reducer_ctx_t *ctx,
                                                sized_buf *buf)
{
    double n;
    int i, size, scanned;
    stats_t s, reduced;
    char red[4096];

    memset(&s, 0, sizeof(s));

    for (i = 0; i < values->length; ++i) {
        mapreduce_json_t *value = &values->values[i];

        if (keys == NULL) {
            /* rereduce */
            char *value_buf = (char *) malloc(value->length + 1);

            if (value_buf == NULL) {
                return COUCHSTORE_ERROR_ALLOC_FAIL;
            }

            memcpy(value_buf, value->json, value->length);
            value_buf[value->length] = '\0';
            scanned = scan_stats(value_buf,
                                 reduced.sum, reduced.count, reduced.min,
                                 reduced.max, reduced.sumsqr);
            free(value_buf);
            if (scanned == 5) {
                if (reduced.min < s.min || s.count == 0) {
                    s.min = reduced.min;
                }
                if (reduced.max > s.max || s.count == 0) {
                    s.max = reduced.max;
                }
                s.count += reduced.count;
                s.sum += reduced.sum;
                s.sumsqr += reduced.sumsqr;
            } else {
                reducer_private_t *priv = (reducer_private_t *) ctx->parent_ctx;
                priv->builtin_error = VIEW_REDUCER_ERROR_BAD_STATS_OBJECT;
                return COUCHSTORE_ERROR_REDUCER_FAILURE;
            }
        } else {
            /* reduce */
            if (json_to_double(value, &n)) {
                s.sum += n;
                s.sumsqr += n * n;
                if (s.count++ == 0) {
                    s.min = s.max = n;
                } else if (n > s.max) {
                    s.max = n;
                } else if (n < s.min) {
                    s.min = n;
                }
            } else {
                reducer_private_t *priv = (reducer_private_t *) ctx->parent_ctx;

                priv->builtin_error = VIEW_REDUCER_ERROR_NOT_A_NUMBER;
                if (keys == NULL) {
                    /* rereduce */
                    return COUCHSTORE_ERROR_REDUCER_FAILURE;
                } else {
                    /* reduce */
                    mapreduce_json_t *key = &keys->values[i];
                    char *error_key = (char *) malloc(key->length + 1);

                    if (error_key == NULL) {
                        return COUCHSTORE_ERROR_ALLOC_FAIL;
                    }
                    memcpy(error_key, key->json, key->length);
                    error_key[key->length] = '\0';
                    priv->error_key = (const char *) error_key;

                    return COUCHSTORE_ERROR_REDUCER_FAILURE;
                }
            }
        }
    }

    size = sprint_stats(red, s.sum, (unsigned long long) s.count, s.min, s.max, s.sumsqr);
    assert(size > 0);
    buf->buf = (char *) malloc(size);
    if (buf->buf == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }
    memcpy(buf->buf, red, size);
    buf->size = size;

    return COUCHSTORE_SUCCESS;
}


static couchstore_error_t js_reducer(const mapreduce_json_list_t *keys,
                                     const mapreduce_json_list_t *values,
                                     reducer_ctx_t *ctx,
                                     sized_buf *buf)
{
    mapreduce_error_t ret;
    char *error_msg = NULL;
    reducer_private_t *priv = (reducer_private_t *) ctx->parent_ctx;

    if (keys == NULL) {
        /* rereduce */
        mapreduce_json_t *result = NULL;

        ret = mapreduce_rereduce(ctx->mapreduce_ctx, 1, values, &result, &error_msg);
        if (ret != MAPREDUCE_SUCCESS) {
            priv->error_msg = error_msg;
            priv->mapreduce_error = ret;
            return COUCHSTORE_ERROR_REDUCER_FAILURE;
        }
        buf->buf = (char *) malloc(result->length);
        if (buf->buf == NULL) {
            free(result->json);
            free(result);
            return COUCHSTORE_ERROR_ALLOC_FAIL;
        }
        buf->size = result->length;
        memcpy(buf->buf, result->json, result->length);
        free(result->json);
        free(result);
    } else {
        /* reduce */
        mapreduce_json_list_t *results = NULL;

        ret = mapreduce_reduce_all(ctx->mapreduce_ctx,
                                   keys,
                                   values,
                                   &results,
                                   &error_msg);
        if (ret != MAPREDUCE_SUCCESS) {
            priv->error_msg = error_msg;
            priv->mapreduce_error = ret;
            return COUCHSTORE_ERROR_REDUCER_FAILURE;
        }
        assert(results->length == 1);
        buf->buf = (char *) malloc(results->values[0].length);
        if (buf->buf == NULL) {
            mapreduce_free_json_list(results);
            return COUCHSTORE_ERROR_ALLOC_FAIL;
        }
        buf->size = results->values[0].length;
        memcpy(buf->buf, results->values[0].json, results->values[0].length);
        mapreduce_free_json_list(results);
    }

    return COUCHSTORE_SUCCESS;
}


static void free_key_excluding_elements(view_btree_key_t *key)
{
    if (key != NULL) {
        free(key->doc_id.buf);
        free(key);
    }
}


static void free_json_key_list(mapreduce_json_list_t *list)
{
    int i;

    if (list == NULL) {
        return;
    }

    for (i = list->length - 1; i > 0; --i) {
        if (list->values[i].json == list->values[i - 1].json) {
            list->values[i].length = 0;
        }
    }

    for (i = 0; i < list->length; ++i) {
        if (list->values[i].length != 0) {
            free(list->values[i].json);
        }
    }
    free(list->values);
    free(list);
}


view_reducer_ctx_t *make_view_reducer_ctx(const char *functions[],
                                          unsigned num_functions,
                                          char **error_msg)
{
    unsigned i;
    reducer_private_t *priv = calloc(1, sizeof(*priv));
    view_reducer_ctx_t *ctx = calloc(1, sizeof(*ctx));

    if (ctx == NULL || priv == NULL) {
        goto error;
    }

    priv->num_reducers = num_functions;
    priv->reducers = calloc(num_functions, sizeof(reducer_fn_t));
    if (priv->reducers == NULL) {
        goto error;
    }

    priv->reducer_contexts = calloc(num_functions, sizeof(reducer_ctx_t));
    if (priv->reducer_contexts == NULL) {
        goto error;
    }

    for (i = 0; i < num_functions; ++i) {
        priv->reducer_contexts[i].parent_ctx = (void *) priv;

        if (strcmp(functions[i], "_count") == 0) {
            priv->reducers[i] = builtin_count_reducer;
        } else if (strcmp(functions[i], "_sum") == 0) {
            priv->reducers[i] = builtin_sum_reducer;
        } else if (strcmp(functions[i], "_stats") == 0) {
            priv->reducers[i] = builtin_stats_reducer;
        } else {
            mapreduce_error_t mapred_error;
            void *mapred_ctx = NULL;
            const char *sources[] = { functions[i] };

            priv->reducers[i] = js_reducer;
            /* TODO: use single reduce context for all JS functions */
            mapred_error = mapreduce_start_reduce_context(sources, 1,
                                                          &mapred_ctx,
                                                          error_msg);
            if (mapred_error != MAPREDUCE_SUCCESS) {
                goto error;
            }
            priv->reducer_contexts[i].mapreduce_ctx = mapred_ctx;
        }
    }

    priv->builtin_error = VIEW_REDUCER_SUCCESS;
    priv->mapreduce_error = MAPREDUCE_SUCCESS;
    ctx->private = priv;
    *error_msg = NULL;

    return ctx;

error:
    if (priv != NULL) {
        if (priv->reducer_contexts != NULL) {
            for (i = 0; i < num_functions; ++i) {
                mapreduce_free_context(priv->reducer_contexts[i].mapreduce_ctx);
            }
            free(priv->reducer_contexts);
        }
        free(priv->reducers);
        free(priv);
    }
    free(ctx);

    return NULL;
}


void free_view_reducer_ctx(view_reducer_ctx_t *ctx)
{
    unsigned i;
    reducer_private_t *priv;

    if (ctx == NULL) {
        return;
    }

    priv = (reducer_private_t *) ctx->private;
    for (i = 0; i < priv->num_reducers; ++i) {
        mapreduce_free_context(priv->reducer_contexts[i].mapreduce_ctx);
    }
    free(priv->reducer_contexts);
    free(priv->reducers);
    free(priv);
    free((void *) ctx->error);
    free(ctx);
}


static void add_error_message(view_reducer_ctx_t *red_ctx, int rereduce)
{
   reducer_private_t *priv = (reducer_private_t *) red_ctx->private;
   char *error_msg;

   if (red_ctx->error != NULL) {
       free((void *) red_ctx->error);
       red_ctx->error = NULL;
   }

   if (priv->builtin_error != VIEW_REDUCER_SUCCESS) {
       const char *base_msg = builtin_reducer_error_msg[priv->builtin_error];

       assert(priv->mapreduce_error == MAPREDUCE_SUCCESS);
       if (!rereduce && (priv->error_key != NULL)) {
           error_msg = (char *) malloc(strlen(base_msg) + 12 + strlen(priv->error_key));
           assert(error_msg != NULL);
           sprintf(error_msg, "%s (key %s)", base_msg, priv->error_key);
       } else {
           error_msg = strdup(base_msg);
           assert(error_msg != NULL);
       }
       if (priv->error_key != NULL) {
           free((void *) priv->error_key);
           priv->error_key = NULL;
       }
   } else {
       assert(priv->mapreduce_error != MAPREDUCE_SUCCESS);
       if (priv->error_msg != NULL) {
           error_msg = priv->error_msg;
       } else {
           if (priv->mapreduce_error == MAPREDUCE_TIMEOUT) {
               error_msg = strdup("function timeout");
               assert(error_msg != NULL);
           } else {
               error_msg = malloc(64);
               assert(error_msg != NULL);
               sprintf(error_msg, "mapreduce error: %d", priv->mapreduce_error);
           }
       }
   }

   red_ctx->error = (const char *) error_msg;
}


couchstore_error_t view_btree_reduce(char *dst,
                                     size_t *size_r,
                                     const nodelist *leaflist,
                                     int count,
                                     void *ctx)
{
    view_reducer_ctx_t *red_ctx = (view_reducer_ctx_t *) ctx;
    reducer_private_t *priv = (reducer_private_t *) red_ctx->private;
    unsigned i;
    reducer_fn_t reducer;
    view_btree_reduction_t *red = NULL;
    const nodelist *n;
    int c;
    couchstore_error_t ret = COUCHSTORE_SUCCESS;
    mapreduce_json_list_t *key_list = NULL;
    mapreduce_json_list_t *value_list = NULL;
    view_btree_value_t **values = NULL;

    values = (view_btree_value_t **) calloc(count, sizeof(view_btree_value_t *));
    red = (view_btree_reduction_t *) calloc(1, sizeof(*red));
    key_list = (mapreduce_json_list_t *) calloc(1, sizeof(*key_list));
    value_list = (mapreduce_json_list_t *) calloc(1, sizeof(*value_list));

    if (values == NULL || red == NULL || key_list == NULL || value_list == NULL) {
        ret = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto out;
    }

    for (n = leaflist, c = 0; n != NULL && c < count; n = n->next, ++c) {
        view_btree_value_t *v = NULL;

        ret = decode_view_btree_value(n->data.buf, n->data.size, &v);
        if (ret != COUCHSTORE_SUCCESS) {
            goto out;
        }
        set_bit(&red->partitions_bitmap, v->partition);
        red->kv_count += v->num_values;
        values[c] = v;
    }

    value_list->values = (mapreduce_json_t *) calloc(red->kv_count,
                                                     sizeof(mapreduce_json_t));
    if (value_list->values == NULL) {
        ret = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto out;
    }
    key_list->values = (mapreduce_json_t *) calloc(red->kv_count,
                                                   sizeof(mapreduce_json_t));
    if (key_list->values == NULL) {
        ret = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto out;
    }

    for (n = leaflist, c = 0; n != NULL && c < count; n = n->next, ++c) {
        view_btree_value_t *v = values[c];
        view_btree_key_t *k = NULL;

        ret = decode_view_btree_key(n->key.buf, n->key.size, &k);
        if (ret != COUCHSTORE_SUCCESS) {
            free_view_btree_value(v);
            goto out;
        }
        for (i = 0; i < v->num_values; ++i) {
            value_list->values[value_list->length].length = v->values[i].size;
            value_list->values[value_list->length].json = v->values[i].buf;
            value_list->length++;
            key_list->values[key_list->length].length = k->json_key.size;
            key_list->values[key_list->length].json = k->json_key.buf;
            key_list->length++;
        }
        free_key_excluding_elements(k);
    }

    if (priv->num_reducers > 0) {
        red->reduce_values = (sized_buf *) calloc(priv->num_reducers, sizeof(sized_buf));
        if (red->reduce_values == NULL) {
            ret = COUCHSTORE_ERROR_ALLOC_FAIL;
            goto out;
        }
    }

    red->num_values = priv->num_reducers;
    for (i = 0; i < priv->num_reducers; ++i) {
        sized_buf buf;
        reducer = priv->reducers[i];

        ret = (*reducer)(key_list, value_list, &priv->reducer_contexts[i], &buf);
        if (ret != COUCHSTORE_SUCCESS) {
            add_error_message(red_ctx, 0);
            goto out;
        }

        red->reduce_values[i] = buf;
    }

    ret = encode_view_btree_reduction(red, dst, size_r);

 out:
    if (red != NULL) {
        for (i = 0; i < red->num_values; ++i) {
            free(red->reduce_values[i].buf);
        }
        free(red->reduce_values);
        free(red);
    }
    free_json_key_list(key_list);
    if (values != NULL) {
        for (c = 0; c < count; ++c) {
            free_view_btree_value(values[c]);
        }
        free(values);
    }
    if (value_list != NULL) {
        free(value_list->values);
        free(value_list);
    }

    return ret;
}


couchstore_error_t view_btree_rereduce(char *dst,
                                       size_t *size_r,
                                       const nodelist *leaflist,
                                       int count,
                                       void *ctx)
{
    view_reducer_ctx_t *red_ctx = (view_reducer_ctx_t *) ctx;
    reducer_private_t *priv = (reducer_private_t *) red_ctx->private;
    unsigned i;
    reducer_fn_t reducer;
    view_btree_reduction_t *red = NULL;
    const nodelist *n;
    int c;
    couchstore_error_t ret = COUCHSTORE_SUCCESS;
    mapreduce_json_list_t *value_list = NULL;
    view_btree_reduction_t **reductions = NULL;

    reductions = (view_btree_reduction_t **) calloc(count, sizeof(view_btree_reduction_t *));
    red = (view_btree_reduction_t *) calloc(1, sizeof(*red));

    if (reductions == NULL || red == NULL) {
        ret = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto out;
    }

    red->num_values = priv->num_reducers;
    red->reduce_values = (sized_buf *) calloc(red->num_values, sizeof(sized_buf));
    value_list = (mapreduce_json_list_t *) calloc(1, sizeof(*value_list));

    if (red->reduce_values == NULL || value_list == NULL) {
        ret = COUCHSTORE_ERROR_ALLOC_FAIL;
        goto out;
    }

    for (n = leaflist, c = 0; n != NULL && c < count; n = n->next, ++c) {
        view_btree_reduction_t *r = NULL;

        ret = decode_view_btree_reduction(n->pointer->reduce_value.buf,
                                          n->pointer->reduce_value.size, &r);
        if (ret != COUCHSTORE_SUCCESS) {
            goto out;
        }

        union_bitmaps(&red->partitions_bitmap, &r->partitions_bitmap);
        assert(r->num_values == priv->num_reducers);
        red->kv_count += r->kv_count;
        reductions[c] = r;
    }

    if (priv->num_reducers > 0) {
        value_list->values = (mapreduce_json_t *) calloc(count,
                                                         sizeof(mapreduce_json_t));
        if (value_list->values == NULL) {
            ret = COUCHSTORE_ERROR_ALLOC_FAIL;
            goto out;
        }
    }

    for (i = 0; i < priv->num_reducers; ++i) {
        sized_buf buf;

        for (n = leaflist, c = 0; n != NULL && c < count; n = n->next, ++c) {
            view_btree_reduction_t *r = reductions[c];

            value_list->values[value_list->length].json = r->reduce_values[i].buf;
            value_list->values[value_list->length].length = r->reduce_values[i].size;
            value_list->length++;
        }

        reducer = priv->reducers[i];
        ret = (*reducer)(NULL, value_list, &priv->reducer_contexts[i], &buf);
        if (ret != COUCHSTORE_SUCCESS) {
            add_error_message(red_ctx, 1);
            goto out;
        }

        value_list->length = 0;
        red->reduce_values[i] = buf;
    }

    ret = encode_view_btree_reduction(red, dst, size_r);

 out:
    if (red != NULL) {
        for (i = 0; i < red->num_values; ++i) {
            free(red->reduce_values[i].buf);
        }
        free(red->reduce_values);
        free(red);
    }
    if (reductions != NULL) {
        for (c = 0; c < count; ++c) {
            free_view_btree_reduction(reductions[c]);
        }
        free(reductions);
    }
    if (value_list != NULL) {
        free(value_list->values);
        free(value_list);
    }

    return ret;
}
