#define DDEBUG 0
#include "ddebug.h"

#include "ngx_http_echo_util.h"
#include "ngx_http_echo_sleep.h"


ngx_int_t
ngx_http_echo_init_ctx(ngx_http_request_t *r, ngx_http_echo_ctx_t **ctx_ptr)
{
    ngx_http_echo_ctx_t         *ctx;

    *ctx_ptr = ngx_pcalloc(r->pool, sizeof(ngx_http_echo_ctx_t));
    if (*ctx_ptr == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx = *ctx_ptr;

    ctx->sleep.handler   = ngx_http_echo_sleep_event_handler;
    ctx->sleep.data      = r;
    ctx->sleep.log       = r->connection->log;

    return NGX_OK;
}

ngx_int_t
ngx_http_echo_eval_cmd_args(ngx_http_request_t *r,
        ngx_http_echo_cmd_t *cmd, ngx_array_t *computed_args,
        ngx_array_t *opts)
{
    ngx_uint_t                       i;
    ngx_array_t                     *args = cmd->args;
    ngx_str_t                       *arg, *raw, *opt;
    ngx_http_echo_arg_template_t    *value;
    ngx_flag_t                       expecting_opts = 1;


    value = args->elts;

    for (i = 0; i < args->nelts; i++) {
        raw = &value[i].raw_value;

        if (value[i].lengths == NULL && raw->len > 0) {
            if (expecting_opts) {
                if (raw->len == 1 || raw->data[0] != '-') {
                    expecting_opts = 0;

                } else if (raw->data[1] == '-') {
                    expecting_opts = 0;
                    continue;

                } else {
                    opt = ngx_array_push(opts);
                    if (opt == NULL) {
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }

                    opt->len = raw->len - 1;
                    opt->data = raw->data + 1;

                    continue;
                }
            }
        }

        arg = ngx_array_push(computed_args);
        if (arg == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (value[i].lengths == NULL) { /* does not contain vars */
            dd("Using raw value \"%.*s\"", (int) raw->len, raw->data);
            *arg = *raw;

        } else {
            if (ngx_http_script_run(r, arg, value[i].lengths->elts,
                        0, value[i].values->elts) == NULL)
            {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
        }
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_echo_send_chain_link(ngx_http_request_t* r,
        ngx_http_echo_ctx_t *ctx, ngx_chain_t *cl)
{
    ngx_int_t       rc;
    size_t          size;
    ngx_chain_t     *p;

    rc = ngx_http_echo_send_header_if_needed(r, ctx);

    if (r->header_only || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    if (r->http_version < NGX_HTTP_VERSION_11 && !ctx->headers_sent) {
        ctx->headers_sent = 1;

        size = 0;

        for (p = cl; p; p = p->next) {
            if (p->buf->memory) {
                size += p->buf->last - p->buf->pos;
            }
        }

        r->headers_out.content_length_n = (off_t) size;

        if (r->headers_out.content_length) {
            r->headers_out.content_length->hash = 0;
        }

        r->headers_out.content_length = NULL;

        rc = ngx_http_send_header(r);

        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }
    }

    if (cl == NULL) {

#if defined(nginx_version) && nginx_version <= 8004

        /* earlier versions of nginx does not allow subrequests
            to send last_buf themselves */
        if (r != r->main) {
            return NGX_OK;
        }

#endif

        rc = ngx_http_send_special(r, NGX_HTTP_LAST);
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }

        return NGX_OK;
    }

    return ngx_http_output_filter(r, cl);
}

ngx_int_t
ngx_http_echo_send_header_if_needed(ngx_http_request_t* r,
        ngx_http_echo_ctx_t *ctx) {
    /* ngx_int_t   rc; */

    if ( ! ctx->headers_sent ) {
        r->headers_out.status = NGX_HTTP_OK;
        if (ngx_http_set_content_type(r) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_clear_content_length(r);
        ngx_http_clear_accept_ranges(r);

        if (r->http_version >= NGX_HTTP_VERSION_11) {
            ctx->headers_sent = 1;
            return ngx_http_send_header(r);
        }
    }
    return NGX_OK;
}

ssize_t
ngx_http_echo_atosz(u_char *line, size_t n) {
    ssize_t  value;

    if (n == 0) {
        return NGX_ERROR;
    }

    for (value = 0; n--; line++) {
        if (*line == '_') { /* we ignore undercores */
            continue;
        }

        if (*line < '0' || *line > '9') {
            return NGX_ERROR;
        }

        value = value * 10 + (*line - '0');
    }

    if (value < 0) {
        return NGX_ERROR;
    } else {
        return value;
    }
}

/* Modified from the ngx_strlcasestrn function in ngx_string.h
 * Copyright (C) by Igor Sysoev */
u_char *
ngx_http_echo_strlstrn(u_char *s1, u_char *last, u_char *s2, size_t n)
{
    ngx_uint_t  c1, c2;

    c2 = (ngx_uint_t) *s2++;
    last -= n;

    do {
        do {
            if (s1 >= last) {
                return NULL;
            }

            c1 = (ngx_uint_t) *s1++;

        } while (c1 != c2);

    } while (ngx_strncmp(s1, s2, n) != 0);

    return --s1;
}


ngx_int_t
ngx_http_echo_post_request_at_head(ngx_http_request_t *r,
        ngx_http_posted_request_t *pr)
{
    dd_enter();

    if (pr == NULL) {
        pr = ngx_palloc(r->pool, sizeof(ngx_http_posted_request_t));
        if (pr == NULL) {
            return NGX_ERROR;
        }
    }

    pr->request = r;
    pr->next = r->main->posted_requests;
    r->main->posted_requests = pr;

    return NGX_OK;
}

