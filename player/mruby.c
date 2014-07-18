/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <mruby.h>
#include <mruby/array.h>
#include <mruby/compile.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

#include "common/msg.h"
#include "options/m_property.h"
#include "options/path.h"
#include "player/command.h"
#include "player/core.h"
#include "player/client.h"
#include "libmpv/client.h"
#include "talloc.h"

struct script_ctx {
    mrb_state *state;

    const char *name;
    const char *filename;
    struct mp_log *log;
    struct mpv_handle *client;
    struct MPContext *mpctx;
};

static struct script_ctx *get_ctx(mrb_state *mrb)
{
    mrb_sym sym = mrb_intern_cstr(mrb, "mpctx");
    mrb_value mrbctx = mrb_vm_const_get(mrb, sym);;
    return mrb_cptr(mrbctx);
}

static mrb_value _log(mrb_state *mrb, mrb_value self)
{
    struct script_ctx *ctx = get_ctx(mrb);
    char *string;
    int len;
    mrb_get_args(mrb, "s", &string, &len);
    MP_ERR(ctx, "%s", string);
    return mrb_nil_value();
}

static mrb_value _property_list(mrb_state *mrb, mrb_value self)
{
    const struct m_property *props = mp_get_property_list();
    mrb_value mrb_props = mrb_ary_new(mrb);
    int ai = mrb_gc_arena_save(mrb);
    for (int i = 0; props[i].name; i++) {
        mrb_value name = mrb_str_new_cstr(mrb, props[i].name);
        mrb_ary_push(mrb, mrb_props, name);
    }
    mrb_gc_arena_restore(mrb, ai);
    return mrb_props;
}

static bool get_node(mrb_state *mrb, void *value)
{
    struct script_ctx *ctx = get_ctx(mrb);
    char *name;
    int len;
    mrb_get_args(mrb, "s", &name, &len);
    int err = mpv_get_property(ctx->client, name, MPV_FORMAT_NODE, value);
    if (err < 0) {
        MP_ERR(ctx, "get_property(\"%s\") failed: %s.\n",
                    name, mpv_error_string(err));
    }
    return err >= 0;
}

static mrb_value mpv_to_mrb_root(mrb_state *mrb, mpv_node node, bool root)
{
    switch (node.format) {
    case MPV_FORMAT_STRING:
        return mrb_str_new_cstr(mrb, node.u.string);
    case MPV_FORMAT_FLAG:
        return mrb_bool_value(node.u.flag >= 0);
    case MPV_FORMAT_INT64:
        return mrb_fixnum_value(node.u.int64);
    case MPV_FORMAT_DOUBLE:
        return mrb_float_value(mrb, node.u.double_);
    case MPV_FORMAT_NODE_ARRAY: {
        mrb_value ary = mrb_ary_new(mrb);
        int ai = mrb_gc_arena_save(mrb);
        for (int n = 0; n < node.u.list->num; n++) {
            mrb_value item = mpv_to_mrb_root(mrb, node.u.list->values[n], false);
            mrb_ary_push(mrb, ary, item);
        }
        if (root)
            mrb_gc_arena_restore(mrb, ai);
        return ary;
    }
    case MPV_FORMAT_NODE_MAP: {
        mrb_value hash = mrb_hash_new(mrb);
        int ai = mrb_gc_arena_save(mrb);
        for (int n = 0; n < node.u.list->num; n++) {
            mrb_value key = mrb_str_new_cstr(mrb, node.u.list->keys[n]);
            mrb_value val = mpv_to_mrb_root(mrb, node.u.list->values[n], false);
            mrb_hash_set(mrb, hash, key, val);
        }
        if (root)
            mrb_gc_arena_restore(mrb, ai);
        return hash;
    }
    default: {
        struct script_ctx *ctx = get_ctx(mrb);
        MP_ERR(ctx, "mpv_node mapping failed (format: %d).\n", node.format);
        return mrb_nil_value();
    }
    }
}

#define mpv_to_mrb(mrb, node) mpv_to_mrb_root(mrb, node, true)

static mrb_value _get_property(mrb_state *mrb, mrb_value self)
{
    mpv_node node;
    if (get_node(mrb, &node))
        return mpv_to_mrb(mrb, node);
    return mrb_nil_value();
}

#define MRB_FN(a,b) \
    mrb_define_module_function(mrb, mod, #a, _ ## a, MRB_ARGS_REQ(b));
static void define_module(mrb_state *mrb)
{
    struct RClass *mod = mrb_define_module(mrb, "M");
    MRB_FN(log, 1);
    MRB_FN(property_list, 0);
    MRB_FN(get_property, 1);
}
#undef MRB_FN

static void print_backtrace(mrb_state *mrb)
{
    if (!mrb->exc)
        return;

    mrb_value exc = mrb_obj_value(mrb->exc);
    mrb_value bt  = mrb_exc_backtrace(mrb, exc);

    int ai = mrb_gc_arena_save(mrb);

    char *err = talloc_strdup(NULL, "");
    mrb_value exc_str = mrb_inspect(mrb, exc);
    err = talloc_asprintf_append(err, "%s\n", RSTRING_PTR(exc_str));

    mrb_int bt_len = mrb_ary_len(mrb, bt);
    err = talloc_asprintf_append(err, "backtrace:\n");
    for (int i = 0; i < bt_len; i++) {
        mrb_value s = mrb_ary_entry(bt, i);
        err = talloc_asprintf_append(err, "\t[%d] => %s\n", i, RSTRING_PTR(s));
    }

    mrb_gc_arena_restore(mrb, ai);

    struct script_ctx *ctx = get_ctx(mrb);
    MP_ERR(ctx, "%s", err);
    talloc_free(err);
}

static void load_script(mrb_state *mrb, const char *fname)
{
    struct script_ctx *ctx = get_ctx(mrb);
    char *file_path = mp_get_user_path(NULL, ctx->mpctx->global, fname);
    FILE *fp = fopen(file_path, "r");
    mrbc_context *mrb_ctx = mrbc_context_new(mrb);
    mrbc_filename(mrb, mrb_ctx, file_path);

    mrb_load_file_cxt(mrb, fp, mrb_ctx);
    print_backtrace(mrb);

    mrbc_context_free(mrb, mrb_ctx);

    fclose(fp);
    talloc_free(file_path);
}

static int load_mruby(struct mpv_handle *client, const char *fname)
{
    struct MPContext *mpctx = mp_client_get_core(client);
    int r = -1;

    struct script_ctx *ctx = talloc_ptrtype(NULL, ctx);
    *ctx = (struct script_ctx) {
        .name     = mpv_client_name(client),
        .filename = fname,
        .log      = mp_client_get_log(client),
        .client   = client,
        .mpctx    = mpctx,
    };

    mrb_state *mrb = ctx->state = mrb_open();
    mrb_sym sym = mrb_intern_cstr(mrb, "mpctx");
    mrb_vm_const_set(mrb, sym, mrb_cptr_value(mrb, ctx));
    define_module(mrb);

    if (!mrb)
        goto err_out;

    load_script(mrb, fname);

    r = 0;

err_out:
    if (ctx->state)
        mrb_close(ctx->state);
    talloc_free(ctx);
    return r;
}


const struct mp_scripting mp_scripting_mruby = {
    .file_ext = "mrb",
    .load = load_mruby,
};
