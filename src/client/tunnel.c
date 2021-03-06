/* Copyright StrongLoop, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "common.h"
#include "sockaddr_universal.h"
#include "tunnel.h"

static bool tunnel_is_dead(struct tunnel_ctx *tunnel);
static void tunnel_add_ref(struct tunnel_ctx *tunnel);
static void tunnel_release(struct tunnel_ctx *tunnel);
static void socket_timer_expire_cb(uv_timer_t *handle);
static void socket_timer_start(struct socket_ctx *c);
static void socket_timer_stop(struct socket_ctx *c);
static void socket_connect_done_cb(uv_connect_t *req, int status);
static void socket_read_done_cb(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf);
static void socket_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void socket_getaddrinfo_done_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *ai);
static void socket_write_done_cb(uv_write_t *req, int status);
static void socket_close(struct socket_ctx *c);
static void socket_close_done_cb(uv_handle_t *handle);

static bool tunnel_is_dead(struct tunnel_ctx *tunnel) {
    return (tunnel->terminated != false);
}

static void tunnel_add_ref(struct tunnel_ctx *tunnel) {
    tunnel->ref_count++;
}

static void tunnel_release(struct tunnel_ctx *tunnel) {
    tunnel->ref_count--;
    if (tunnel->ref_count == 0) {
        if (tunnel->tunnel_dying) {
            tunnel->tunnel_dying(tunnel);
        }

        free(tunnel->incoming->buf);
        free(tunnel->incoming);

        free(tunnel->outgoing->buf);
        free(tunnel->outgoing);

        free(tunnel);
    }
}

/* |incoming| has been initialized by listener.c when this is called. */
void tunnel_initialize(uv_tcp_t *listener, unsigned int idle_timeout, bool(*init_done_cb)(struct tunnel_ctx *tunnel, void *p), void *p) {
    struct socket_ctx *incoming;
    struct socket_ctx *outgoing;
    struct tunnel_ctx *tunnel;
    uv_loop_t *loop = listener->loop;

    tunnel = (struct tunnel_ctx *) calloc(1, sizeof(*tunnel));

    tunnel->listener = listener;
    tunnel->ref_count = 0;

    incoming = (struct socket_ctx *) calloc(1, sizeof(*incoming));
    incoming->tunnel = tunnel;
    incoming->result = 0;
    incoming->rdstate = socket_stop;
    incoming->wrstate = socket_stop;
    incoming->idle_timeout = idle_timeout;
    VERIFY(0 == uv_timer_init(loop, &incoming->timer_handle));
    VERIFY(0 == uv_tcp_init(loop, &incoming->handle.tcp));
    VERIFY(0 == uv_accept((uv_stream_t *)listener, &incoming->handle.stream));
    tunnel->incoming = incoming;

    outgoing = (struct socket_ctx *) calloc(1, sizeof(*outgoing));
    outgoing->tunnel = tunnel;
    outgoing->result = 0;
    outgoing->rdstate = socket_stop;
    outgoing->wrstate = socket_stop;
    outgoing->idle_timeout = idle_timeout;
    VERIFY(0 == uv_timer_init(loop, &outgoing->timer_handle));
    VERIFY(0 == uv_tcp_init(loop, &outgoing->handle.tcp));
    tunnel->outgoing = outgoing;

    bool success = false;
    if (init_done_cb) {
        success = init_done_cb(tunnel, p);
    }

    if (success) {
        /* Wait for the initial packet. */
        socket_read(incoming);
    } else {
        tunnel_shutdown(tunnel);
    }
}

void tunnel_shutdown(struct tunnel_ctx *tunnel) {
    if (tunnel_is_dead(tunnel) != false) {
        return;
    }

    /* Try to cancel the request. The callback still runs but if the
    * cancellation succeeded, it gets called with status=UV_ECANCELED.
    */
    if (tunnel->getaddrinfo_pending) {
        uv_cancel(&tunnel->outgoing->t.req);
    }

    socket_close(tunnel->incoming);
    socket_close(tunnel->outgoing);

    tunnel->terminated = true;
}

static void socket_timer_start(struct socket_ctx *c) {
    VERIFY(0 == uv_timer_start(&c->timer_handle,
        socket_timer_expire_cb,
        c->idle_timeout,
        0));
}

static void socket_timer_stop(struct socket_ctx *c) {
    VERIFY(0 == uv_timer_stop(&c->timer_handle));
}

static void socket_timer_expire_cb(uv_timer_t *handle) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(handle, struct socket_ctx, timer_handle);
    c->result = UV_ETIMEDOUT;

    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    if (tunnel->tunnel_timeout_expire_done) {
        tunnel->tunnel_timeout_expire_done(tunnel, c);
    }

    tunnel_shutdown(tunnel);
}

/* Assumes that c->t.sa contains a valid AF_INET or AF_INET6 address. */
int socket_connect(struct socket_ctx *c) {
    ASSERT(c->t.addr.addr.sa_family == AF_INET || c->t.addr.addr.sa_family == AF_INET6);
    socket_timer_start(c);
    return uv_tcp_connect(&c->t.connect_req,
        &c->handle.tcp,
        &c->t.addr.addr,
        socket_connect_done_cb);
}

static void socket_connect_done_cb(uv_connect_t *req, int status) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(req, struct socket_ctx, t.connect_req);
    c->result = status;

    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == UV_ECANCELED || status == UV_ECONNREFUSED) {
        tunnel_shutdown(tunnel);
        return;  /* Handle has been closed. */
    }

    ASSERT(tunnel->tunnel_outgoing_connected_done);
    tunnel->tunnel_outgoing_connected_done(tunnel, c);
}

void socket_read(struct socket_ctx *c) {
    ASSERT(c->rdstate == socket_stop);
    VERIFY(0 == uv_read_start(&c->handle.stream, socket_alloc_cb, socket_read_done_cb));
    c->rdstate = socket_busy;
    socket_timer_start(c);
}

static void socket_read_done_cb(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(handle, struct socket_ctx, handle);
    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    if (tunnel->tunnel_is_on_the_fly(tunnel) == false) {
        uv_read_stop(&c->handle.stream);
    }

    socket_timer_stop(c);

    if (nread == 0) {
        return;
    }
    if (nread < 0) {
        // http://docs.libuv.org/en/v1.x/stream.html
        ASSERT(nread == UV_EOF || nread == UV_ECONNRESET);
        tunnel_shutdown(tunnel);
        return;
    }

    ASSERT(c->buf == (uint8_t *)buf->base);
    if (tunnel->tunnel_is_on_the_fly(tunnel) == false) {
        ASSERT(c->rdstate == socket_busy);
    }
    c->rdstate = socket_done;
    c->result = nread;

    ASSERT(tunnel->tunnel_read_done);
    tunnel->tunnel_read_done(tunnel, c);
}

void socket_read_stop(struct socket_ctx *c) {
    uv_read_stop(&c->handle.stream);
    c->rdstate = socket_stop;
}

static void socket_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(handle, struct socket_ctx, handle);
    tunnel = c->tunnel;

    if (tunnel->tunnel_is_on_the_fly(tunnel) == false) {
        ASSERT(c->rdstate == socket_busy);
    }

    if (tunnel->tunnel_alloc_size) {
        size = tunnel->tunnel_alloc_size(tunnel, size);
    }
    c->buf = realloc(c->buf, size);
    c->buf_size = size;

    buf->base = (char *)c->buf;
    buf->len = (uv_buf_len_t)size;
}

void socket_getaddrinfo(struct socket_ctx *c, const char *hostname) {
    struct addrinfo hints;
    struct tunnel_ctx *tunnel;

    tunnel = c->tunnel;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    VERIFY(0 == uv_getaddrinfo(tunnel->listener->loop,
        &c->t.addrinfo_req,
        socket_getaddrinfo_done_cb,
        hostname,
        NULL,
        &hints));
    socket_timer_start(c);
    tunnel->getaddrinfo_pending = true;
}

static void socket_getaddrinfo_done_cb(uv_getaddrinfo_t *req, int status, struct addrinfo *ai) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = CONTAINER_OF(req, struct socket_ctx, t.addrinfo_req);
    c->result = status;

    tunnel = c->tunnel;
    tunnel->getaddrinfo_pending = false;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == 0) {
        /* FIXME(bnoordhuis) Should try all addresses. */
        if (ai->ai_family == AF_INET) {
            c->t.addr.addr4 = *(const struct sockaddr_in *) ai->ai_addr;
        } else if (ai->ai_family == AF_INET6) {
            c->t.addr.addr6 = *(const struct sockaddr_in6 *) ai->ai_addr;
        } else {
            UNREACHABLE();
        }
    }

    uv_freeaddrinfo(ai);

    ASSERT(tunnel->tunnel_getaddrinfo_done);
    tunnel->tunnel_getaddrinfo_done(tunnel, c);
}

void socket_write(struct socket_ctx *c, const void *data, size_t len) {
    uv_buf_t buf;
    struct tunnel_ctx *tunnel = c->tunnel;

    if (tunnel->tunnel_is_on_the_fly(tunnel) == false) {
        ASSERT(c->wrstate == socket_stop || c->wrstate == socket_done);
    }
    c->wrstate = socket_busy;

    // It's okay to cast away constness here, uv_write() won't modify the memory.
    buf = uv_buf_init((char *)data, (unsigned int)len);

    uv_write_t *req = (uv_write_t *)calloc(1, sizeof(uv_write_t));
    req->data = c;

    VERIFY(0 == uv_write(req, &c->handle.stream, &buf, 1, socket_write_done_cb));
    socket_timer_start(c);
}

static void socket_write_done_cb(uv_write_t *req, int status) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = (struct socket_ctx *)req->data;
    free(req);
    tunnel = c->tunnel;

    if (tunnel_is_dead(tunnel)) {
        return;
    }

    socket_timer_stop(c);

    if (status == UV_ECANCELED) {
        tunnel_shutdown(tunnel);
        return;  /* Handle has been closed. */
    }

    if (tunnel->tunnel_is_on_the_fly(tunnel) == false) {
        ASSERT(c->wrstate == socket_busy);
    }
    c->wrstate = socket_done;
    c->result = status;

    ASSERT(tunnel->tunnel_write_done);
    tunnel->tunnel_write_done(tunnel, c);
}

static void socket_close(struct socket_ctx *c) {
    struct tunnel_ctx *tunnel = c->tunnel;
    ASSERT(c->rdstate != socket_dead);
    ASSERT(c->wrstate != socket_dead);
    c->rdstate = socket_dead;
    c->wrstate = socket_dead;
    c->timer_handle.data = c;
    c->handle.handle.data = c;

    tunnel_add_ref(tunnel);
    uv_close(&c->handle.handle, socket_close_done_cb);
    tunnel_add_ref(tunnel);
    uv_close((uv_handle_t *)&c->timer_handle, socket_close_done_cb);
}

static void socket_close_done_cb(uv_handle_t *handle) {
    struct socket_ctx *c;
    struct tunnel_ctx *tunnel;

    c = (struct socket_ctx *) handle->data;
    tunnel = c->tunnel;

    tunnel_release(tunnel);
}
