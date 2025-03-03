/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "server.h"
#include "http.h"
#include "http2.h"
#include "websocket.h"
#include "mqtt.h"
#include "redis.h"

static int swPort_onRead_raw(swReactor *reactor, swListenPort *lp, swEvent *event);
static int swPort_onRead_check_length(swReactor *reactor, swListenPort *lp, swEvent *event);
static int swPort_onRead_check_eof(swReactor *reactor, swListenPort *lp, swEvent *event);
static int swPort_onRead_http(swReactor *reactor, swListenPort *lp, swEvent *event);
static int swPort_onRead_redis(swReactor *reactor, swListenPort *lp, swEvent *event);

void swPort_init(swListenPort *port)
{
    port->socket = nullptr;
    port->ssl = 0;

    //listen backlog
    port->backlog = SW_BACKLOG;
    //tcp keepalive
    port->tcp_keepcount = SW_TCP_KEEPCOUNT;
    port->tcp_keepinterval = SW_TCP_KEEPINTERVAL;
    port->tcp_keepidle = SW_TCP_KEEPIDLE;
    port->open_tcp_nopush = 1;

    port->protocol.package_length_type = 'N';
    port->protocol.package_length_size = 4;
    port->protocol.package_body_offset = 4;
    port->protocol.package_max_length = SW_BUFFER_INPUT_SIZE;

    port->socket_buffer_size = SwooleG.socket_buffer_size;

    char eof[] = SW_DATA_EOF;
    port->protocol.package_eof_len = sizeof(SW_DATA_EOF) - 1;
    memcpy(port->protocol.package_eof, eof, port->protocol.package_eof_len);
}

#ifdef SW_USE_OPENSSL
int swPort_enable_ssl_encrypt(swListenPort *ls)
{
    if (ls->ssl_option.cert_file == NULL || ls->ssl_option.key_file == NULL)
    {
        swWarn("SSL error, require ssl_cert_file and ssl_key_file");
        return SW_ERR;
    }
    ls->ssl_context = swSSL_get_context(&ls->ssl_option);
    if (ls->ssl_context == NULL)
    {
        swWarn("swSSL_get_context() error");
        return SW_ERR;
    }
    if (ls->ssl_option.client_cert_file
            && swSSL_set_client_certificate(ls->ssl_context, ls->ssl_option.client_cert_file,
                    ls->ssl_option.verify_depth) == SW_ERR)
    {
        swWarn("swSSL_set_client_certificate() error");
        return SW_ERR;
    }
    if (ls->open_http_protocol)
    {
        ls->ssl_config.http = 1;
    }
    if (ls->open_http2_protocol)
    {
        ls->ssl_config.http_v2 = 1;
        swSSL_server_http_advise(ls->ssl_context, &ls->ssl_config);
    }
    if (swSSL_server_set_cipher(ls->ssl_context, &ls->ssl_config) < 0)
    {
        swWarn("swSSL_server_set_cipher() error");
        return SW_ERR;
    }
    return SW_OK;
}
#endif

int swPort_listen(swListenPort *ls)
{
    int sock = ls->socket->fd;
    int option = 1;

    //listen stream socket
    if (listen(sock, ls->backlog) < 0)
    {
        swSysWarn("listen(%s:%d, %d) failed", ls->host, ls->port, ls->backlog);
        return SW_ERR;
    }

#ifdef TCP_DEFER_ACCEPT
    if (ls->tcp_defer_accept)
    {
        if (setsockopt(sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, (const void*) &ls->tcp_defer_accept, sizeof(int)) != 0)
        {
            swSysWarn("setsockopt(TCP_DEFER_ACCEPT) failed");
        }
    }
#endif

#ifdef TCP_FASTOPEN
    if (ls->tcp_fastopen)
    {
        if (setsockopt(sock, IPPROTO_TCP, TCP_FASTOPEN, (const void*) &ls->tcp_fastopen, sizeof(int)) != 0)
        {
            swSysWarn("setsockopt(TCP_FASTOPEN) failed");
        }
    }
#endif

#ifdef SO_KEEPALIVE
    if (ls->open_tcp_keepalive == 1)
    {
        if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *) &option, sizeof(option)) != 0)
        {
            swSysWarn("setsockopt(SO_KEEPALIVE) failed");
        }
#ifdef TCP_KEEPIDLE
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, (void*) &ls->tcp_keepidle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *) &ls->tcp_keepinterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, (void *) &ls->tcp_keepcount, sizeof(int));
#endif
    }
#endif

    ls->buffer_high_watermark = ls->socket_buffer_size * 0.8;
    ls->buffer_low_watermark = 0;

    return SW_OK;
}

void swPort_set_protocol(swServer *serv, swListenPort *ls)
{
    ls->protocol.private_data_2 = serv;
    //Thread mode must copy the data.
    //will free after onFinish
    if (ls->open_eof_check)
    {
        if (ls->protocol.package_eof_len > SW_DATA_EOF_MAXLEN)
        {
            ls->protocol.package_eof_len = SW_DATA_EOF_MAXLEN;
        }
        ls->protocol.onPackage = swReactorThread_dispatch;
        ls->onRead = swPort_onRead_check_eof;
    }
    else if (ls->open_length_check)
    {
        if (ls->protocol.package_length_type != '\0')
        {
            ls->protocol.get_package_length = swProtocol_get_package_length;
        }
        ls->protocol.onPackage = swReactorThread_dispatch;
        ls->onRead = swPort_onRead_check_length;
    }
    else if (ls->open_http_protocol)
    {
#ifdef SW_USE_HTTP2
        if (ls->open_http2_protocol && ls->open_websocket_protocol)
        {
            ls->protocol.get_package_length = swHttpMix_get_package_length;
            ls->protocol.get_package_length_size = swHttpMix_get_package_length_size;
            ls->protocol.onPackage = swHttpMix_dispatch_frame;
        }
        else if (ls->open_http2_protocol)
        {
            ls->protocol.get_package_length = swHttp2_get_frame_length;
            ls->protocol.package_length_size = SW_HTTP2_FRAME_HEADER_SIZE;
            ls->protocol.onPackage = swReactorThread_dispatch;
        }
        else
#endif
        if (ls->open_websocket_protocol)
        {
            ls->protocol.get_package_length = swWebSocket_get_package_length;
            ls->protocol.package_length_size = SW_WEBSOCKET_HEADER_LEN + SW_WEBSOCKET_MASK_LEN + sizeof(uint64_t);
            ls->protocol.onPackage = swWebSocket_dispatch_frame;
        }
        ls->onRead = swPort_onRead_http;
    }
    else if (ls->open_mqtt_protocol)
    {
        ls->protocol.get_package_length = swMqtt_get_package_length;
        ls->protocol.onPackage = swReactorThread_dispatch;
        ls->onRead = swPort_onRead_check_length;
    }
    else if (ls->open_redis_protocol)
    {
        ls->protocol.onPackage = swReactorThread_dispatch;
        ls->onRead = swPort_onRead_redis;
    }
    else
    {
        ls->onRead = swPort_onRead_raw;
    }
}

void swPort_clear_protocol(swListenPort *ls)
{
    ls->open_eof_check = 0;
    ls->open_length_check = 0;
    ls->open_http_protocol = 0;
    ls->open_websocket_protocol = 0;
#ifdef SW_USE_HTTP2
    ls->open_http2_protocol = 0;
#endif
    ls->open_mqtt_protocol = 0;
    ls->open_redis_protocol = 0;
}

static int swPort_onRead_raw(swReactor *reactor, swListenPort *port, swEvent *event)
{
    ssize_t n;
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    char *buffer = SwooleTG.buffer_stack->str;

    n = swSocket_recv(_socket, buffer, SwooleTG.buffer_stack->size, 0);
    if (n < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swSysWarn("recv from connection#%d failed", event->fd);
            return SW_OK;
        case SW_CLOSE:
            conn->close_errno = errno;
            goto _close_fd;
        default:
            return SW_OK;
        }
    }
    else if (n == 0)
    {
        _close_fd:
        swReactor_trigger_close_event(reactor, event);
        return SW_OK;
    }
    else
    {
        return swReactorThread_dispatch(&port->protocol, _socket, buffer, n);
    }
}

static int swPort_onRead_check_length(swReactor *reactor, swListenPort *port, swEvent *event)
{
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;

    swString *buffer = swSocket_get_buffer(_socket);
    if (!buffer)
    {
        return SW_ERR;
    }

    if (swProtocol_recv_check_length(protocol, _socket, buffer) < 0)
    {
        swTrace("Close Event.FD=%d|From=%d", event->fd, event->reactor_id);
        conn->close_errno = errno;
        swReactor_trigger_close_event(reactor, event);
    }

    return SW_OK;
}

#define CLIENT_INFO_FMT " from session#%u on %s:%d"
#define CLIENT_INFO_ARGS conn->session_id, port->host, port->port
#define CLIENT_INFO CLIENT_INFO_FMT, CLIENT_INFO_ARGS

/**
 * For Http Protocol
 */
static int swPort_onRead_http(swReactor *reactor, swListenPort *port, swEvent *event)
{
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swServer *serv = (swServer *) reactor->ptr;

    if (conn->websocket_status >= WEBSOCKET_STATUS_HANDSHAKE)
    {
        if (conn->http_upgrade == 0)
        {
            swHttpRequest_free(conn);
            conn->websocket_status = WEBSOCKET_STATUS_ACTIVE;
            conn->http_upgrade = 1;
        }
        return swPort_onRead_check_length(reactor, port, event);
    }

#ifdef SW_USE_HTTP2
    if (conn->http2_stream)
    {
        return swPort_onRead_check_length(reactor, port, event);
    }
#endif

    swHttpRequest *request = NULL;
    swProtocol *protocol = &port->protocol;

    if (conn->object == NULL)
    {
        request = (swHttpRequest *) sw_calloc(1, sizeof(swHttpRequest));
        if (!request)
        {
            swWarn("calloc(%ld) failed", sizeof(swHttpRequest));
            return SW_ERR;
        }
        conn->object = request;
    }
    else
    {
        request = (swHttpRequest *) conn->object;
    }

    if (!request->buffer)
    {
        request->buffer = swString_new(SW_HTTP_HEADER_MAX_SIZE);
        if (!request->buffer)
        {
            swReactor_trigger_close_event(reactor, event);
            return SW_ERR;
        }
    }

    swString *buffer = request->buffer;

    _recv_data:
    ssize_t n = swSocket_recv(_socket, buffer->str + buffer->length, buffer->size - buffer->length, 0);
    if (n < 0)
    {
        switch (swConnection_error(errno))
        {
        case SW_ERROR:
            swSysWarn("recv from connection#%d failed", event->fd);
            return SW_OK;
        case SW_CLOSE:
            conn->close_errno = errno;
            goto _close_fd;
        default:
            return SW_OK;
        }
    }

    if (n == 0)
    {
        if (0)
        {
            _bad_request:
#ifdef SW_HTTP_BAD_REQUEST_PACKET
            swSocket_send(_socket, SW_STRL(SW_HTTP_BAD_REQUEST_PACKET), 0);
#endif
        }
        if (0)
        {
            _too_large:
#ifdef SW_HTTP_REQUEST_ENTITY_TOO_LARGE_PACKET
            swSocket_send(_socket, SW_STRL(SW_HTTP_REQUEST_ENTITY_TOO_LARGE_PACKET), 0);
#endif
        }
        if (0)
        {
            _unavailable:
#ifdef SW_HTTP_SERVICE_UNAVAILABLE_PACKET
            swSocket_send(_socket, SW_STRL(SW_HTTP_SERVICE_UNAVAILABLE_PACKET), 0);
#endif
        }
        _close_fd:
        swHttpRequest_free(conn);
        swReactor_trigger_close_event(reactor, event);
        return SW_OK;
    }

    buffer->length += n;

    _parse:
    if (request->method == 0 && swHttpRequest_get_protocol(request) < 0)
    {
        if (!request->excepted && buffer->length < SW_HTTP_HEADER_MAX_SIZE)
        {
            return SW_OK;
        }
        swoole_error_log(SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL, "Bad Request: unknown protocol" CLIENT_INFO);
        goto _bad_request;
    }

    if (request->method > SW_HTTP_PRI)
    {
        swoole_error_log(SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL, "Bad Request: got unsupported HTTP method" CLIENT_INFO);
        goto _bad_request;
    }
    else if (request->method == SW_HTTP_PRI)
    {
#ifdef SW_USE_HTTP2
        if (sw_unlikely(!port->open_http2_protocol))
        {
#endif
            swoole_error_log(SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL, "Bad Request: can not handle HTTP2 request" CLIENT_INFO);
            goto _bad_request;
#ifdef SW_USE_HTTP2
        }
        conn->http2_stream = 1;
        swHttp2_send_setting_frame(protocol, _socket);
        if (buffer->length == sizeof(SW_HTTP2_PRI_STRING) - 1)
        {
            swHttpRequest_free(conn);
            return SW_OK;
        }
        swString *h2_buffer = swSocket_get_buffer(_socket);
        if (!h2_buffer)
        {
            goto _close_fd;
        }
        swString_append_ptr(h2_buffer, buffer->str + buffer->offset, buffer->length - buffer->offset);
        swHttpRequest_free(conn);
        conn->socket->skip_recv = 1;
        return swPort_onRead_check_length(reactor, port, event);
#endif
    }

    // http header is not the end
    if (request->header_length == 0)
    {
        if (swHttpRequest_get_header_length(request) < 0)
        {
            if (buffer->size == buffer->length)
            {
                swoole_error_log(SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL, "Bad Request: request header is too long" CLIENT_INFO);
                goto _bad_request;
            }
            goto _recv_data;
        }
    }

    // parse http header and got http body length
    if (!request->header_parsed)
    {
        swHttpRequest_parse_header_info(request);
        swTraceLog(SW_TRACE_SERVER, "content-length=%u, keep-alive=%u, chunked=%u", request->content_length, request->keep_alive, request->chunked);
    }

    // content length (equal to 0) or (field not found but not chunked)
    if (!request->tried_to_dispatch)
    {
        // recv nobody_chunked eof
        if (request->nobody_chunked)
        {
            if (buffer->length < request->header_length + (sizeof("0\r\n\r\n") - 1))
            {
                goto _recv_data;
            }
            request->header_length += (sizeof("0\r\n\r\n") - 1);
        }
        request->tried_to_dispatch = 1;
        // (know content-length is equal to 0) or (no content-length field and no chunked)
        if (request->content_length == 0 && (request->known_length || !request->chunked))
        {
            // send static file content directly in the reactor thread
            if (!serv->enable_static_handler || !swServer_http_static_handler_hit(serv, request, conn))
            {
                // dynamic request, dispatch to worker
                swReactorThread_dispatch(protocol, _socket, buffer->str, request->header_length);
            }
            if (conn->active && buffer->length > request->header_length)
            {
                // http pipeline, multi requests, parse the next one
                swString_pop_front(buffer, request->header_length);
                swHttpRequest_clean(request);
                goto _parse;
            }
            else
            {
                swHttpRequest_free(conn);
                return SW_OK;
            }
        }
    }

    size_t request_length;
    if (request->chunked)
    {
        /* unknown length, should find chunked eof */
        if (swHttpRequest_get_chunked_body_length(request) < 0)
        {
            if (request->excepted)
            {
                swoole_error_log(SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL, "Bad Request: protocol error when parse chunked length" CLIENT_INFO);
                goto _bad_request;
            }
            request_length = request->header_length + request->content_length;
            if (request_length > protocol->package_max_length)
            {
                swoole_error_log(
                    SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL,
                    "Request Entity Too Large: request length (chunked) has already been greater than the package_max_length(%u)" CLIENT_INFO_FMT,
                    protocol->package_max_length, CLIENT_INFO_ARGS
                );
                goto _too_large;
            }
            if (buffer->length == buffer->size && swString_extend(buffer, buffer->size * 2) < 0)
            {
                goto _unavailable;
            }
            if (request_length > buffer->size && swString_extend_align(buffer, request_length) < 0)
            {
                goto _unavailable;
            }
            goto _recv_data;
        }
        else
        {
            request_length = request->header_length + request->content_length;
        }
        swTraceLog(SW_TRACE_SERVER, "received chunked eof, real content-length=%u", request->content_length);
    }
    else
    {
        request_length = request->header_length + request->content_length;
        if (request_length > protocol->package_max_length)
        {
            swoole_error_log(
                SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL,
                "Request Entity Too Large: header-length (%u) + content-length (%u) is greater than the package_max_length(%u)" CLIENT_INFO_FMT,
                request->header_length, request->content_length, protocol->package_max_length, CLIENT_INFO_ARGS
            );
            goto _too_large;
        }

        if (request_length > buffer->size && swString_extend(buffer, request_length) < 0)
        {
            goto _unavailable;
        }

        if (buffer->length < request_length)
        {
    #ifdef SW_HTTP_100_CONTINUE
            // Expect: 100-continue
            if (swHttpRequest_has_expect_header(request))
            {
                swSocket_send(_socket, SW_STRL(SW_HTTP_100_CONTINUE_PACKET), 0);
            }
            else
            {
                swTraceLog(
                    SW_TRACE_SERVER, "PostWait: request->content_length=%d, buffer->length=%zu, request->header_length=%d\n",
                    request->content_length, buffer->length, request->header_length
                );
            }
    #endif
            goto _recv_data;
        }
    }

    // discard the redundant data
    if (buffer->length > request_length)
    {
        swoole_error_log(
            SW_LOG_TRACE, SW_ERROR_HTTP_INVALID_PROTOCOL,
            "Invalid Request: %zu bytes has been disacard" CLIENT_INFO_FMT,
            buffer->length - request_length, CLIENT_INFO_ARGS
        );
        buffer->length = request_length;
    }

    swReactorThread_dispatch(protocol, _socket, buffer->str, buffer->length);
    swHttpRequest_free(conn);

    return SW_OK;
}

static int swPort_onRead_redis(swReactor *reactor, swListenPort *port, swEvent *event)
{
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;

    swString *buffer = swSocket_get_buffer(_socket);
    if (!buffer)
    {
        return SW_ERR;
    }

    if (swRedis_recv(protocol, conn, buffer) < 0)
    {
        conn->close_errno = errno;
        swReactor_trigger_close_event(reactor, event);
    }

    return SW_OK;
}

static int swPort_onRead_check_eof(swReactor *reactor, swListenPort *port, swEvent *event)
{
    swSocket *_socket = event->socket;
    swConnection *conn = (swConnection *) _socket->object;
    swProtocol *protocol = &port->protocol;

    swString *buffer = swSocket_get_buffer(_socket);
    if (!buffer)
    {
        return SW_ERR;
    }

    if (swProtocol_recv_check_eof(protocol, _socket, buffer) < 0)
    {
        conn->close_errno = errno;
        swReactor_trigger_close_event(reactor, event);
    }

    return SW_OK;
}

void swPort_free(swListenPort *port)
{
#ifdef SW_USE_OPENSSL
    if (port->ssl)
    {
        if (port->ssl_context)
        {
            swSSL_free_context(port->ssl_context);
        }
        sw_free(port->ssl_option.cert_file);
        sw_free(port->ssl_option.key_file);
        if (port->ssl_option.client_cert_file)
        {
            sw_free(port->ssl_option.client_cert_file);
        }
    }
#endif

    swSocket_free(port->socket);

    //remove unix socket file
    if (port->type == SW_SOCK_UNIX_STREAM || port->type == SW_SOCK_UNIX_DGRAM)
    {
        unlink(port->host);
    }
}

