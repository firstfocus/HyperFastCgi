#include <glib.h>
#include <pthread.h>
#include <string.h>
#include <event.h>
#include "fcgi.h"
#include "libev.h"
#include "mono-bridge.h"
#include "fcgi-transport.h"

#define GET_HASH(fd,reqid) (((guint64)(reqid)) << 32) ^ (fd)

typedef struct {
    guint64 hash;
    int fd;
    guint16 requestId;
    int request_num;
    FCGI_Header* header;
    unsigned char* body;
    gboolean stdout_sent;
    gboolean keep_alive;
} Request;

static GHashTable* requests;
static pthread_mutex_t requests_lock;

static int request_num = 0;

static gboolean
parse_params(Request *req, FCGI_Header* header, guint8* data);


void
transport_init()
{
    pthread_mutex_init (&requests_lock, NULL);
    requests = g_hash_table_new (g_int64_hash, g_int64_equal);
}

void
transport_finalize()
{
    pthread_mutex_destroy (&requests_lock);
    g_hash_table_destroy (requests);
}

static const char* Header="HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 20\r\n\r\n";
static const char* Response="<p>Hello, world!</p>";

static void process_internal (Request *req, FCGI_Header *header, guint8 *body, int len)
{
//    INFO_OUT("%s","process internal");
    HostInfo* host = find_host_by_path("123");
    //process(host, req->hash, req->request_num);
    add_body_part(host, req->hash, req->request_num, body, len, len == 0);

//    send_output(req->hash, req->request_num, (guint8 *)Header, strlen(Header));
//    send_output(req->hash, req->request_num, (guint8 *)Response, strlen(Response));
//    end_request(req->hash, req->request_num, 200, FCGI_REQUEST_COMPLETE);
}

void
process_record(int fd, FCGI_Header* header, guint8* body)
{
    Request *req = NULL;
    guint64 id = GET_HASH(fd,fcgi_get_request_id(header));

    //key not found. if header->type is FCGI_BEGIN_REQUEST
    //than create new request, otherwise skip request due to FastCGI spec
    pthread_mutex_lock (&requests_lock);
    req = g_hash_table_lookup (requests, &id);

    if (!req) {
        if (header->type == FCGI_BEGIN_REQUEST) {
            FCGI_BeginRequestBody *begin_body = (FCGI_BeginRequestBody *)body;

            req = g_new (Request, 1);
            req->hash = id;
            req->fd = fd;
            req->requestId = fcgi_get_request_id(header);
            req->header = header;
            req->body = body;
            req->request_num = ++request_num;
            req->keep_alive = begin_body->flags & FCGI_KEEP_CONN;
            req->stdout_sent = FALSE;
            g_hash_table_insert (requests, &req->hash, req);

            pthread_mutex_unlock (&requests_lock);

            HostInfo* host = find_host_by_path("123");
            create_request (host, req->hash, req->request_num);

            return;
        }
    }
    pthread_mutex_unlock (&requests_lock);

    switch(header->type)
    {
        case FCGI_BEGIN_REQUEST:
            //TODO: assert should not be reached
            break;
        case FCGI_ABORT_REQUEST:
            //TODO: remove from hash, send abort to web server
            break;
        case FCGI_PARAMS:
            //TODO: parse params
            parse_params (req, header, body);
            break;
        case FCGI_STDIN:
            //TODO: read until the end
            process_internal (req, header, body, fcgi_get_content_len(header));
            break;
        case FCGI_DATA:
            //TODO: nothing?
            break;
        case FCGI_GET_VALUES:
            //currently there are no server-side settings (values)
            break;
        default:
            break;
    }
}

static void
send_record (cmdsocket* sock, guint8 record_type, guint16 requestId, guint8* data, int offset, int len)
{
    FCGI_Header header = {
        .version = FCGI_VERSION_1,
        .type = record_type,
        .paddingLength = 0,
        .reserved = 0,
    };

    fcgi_set_request_id (&header, requestId);
    fcgi_set_content_len (&header, len);

    //TODO: lock buffer if needed
//    evbuffer_lock (sock->buffer);
//    evbuffer_add (sock->buffer, &header, FCGI_HEADER_SIZE);
//    evbuffer_add (sock->buffer, data + offset, len);
//    evbuffer_unlock (sock->buffer);

    struct evbuffer *output = bufferevent_get_output(sock->buf_event);
    evbuffer_lock(output);
    evbuffer_add (output, &header, FCGI_HEADER_SIZE);
    evbuffer_add (output, data + offset, len);
    evbuffer_unlock(output);


//    bufferevent_lock (sock->buf_event);
//    bufferevent_write_buffer(sock->buf_event, sock->buffer);
//    bufferevent_unlock (sock->buf_event);
}

static void
send_stream_data (cmdsocket* sock, guint8 record_type, guint16 requestId, guint8* data, int len)
{
    if (len < FCGI_MAX_BODY_SIZE)
        send_record (sock, record_type, requestId, data, 0, len);
    else {
        int index=0;
        while (index < len) {
            int chunk_len = (len - index < FCGI_SUGGESTED_BODY_SIZE)
                            ? (len - index)
                            : FCGI_SUGGESTED_BODY_SIZE;
            send_record (sock, record_type, requestId, data, index, len);

            index += chunk_len;
        }
    }
}

void
send_output (guint64 requestId, int request_num, guint8* data, int len)
{
    pthread_mutex_lock (&requests_lock);
    Request* req=(Request *)g_hash_table_lookup (requests, &requestId);
    pthread_mutex_unlock (&requests_lock);

    if (req && req->request_num == request_num) {
        cmdsocket* sock = find_cmdsocket (req->fd);
        if (sock != NULL) {
            send_stream_data (sock, FCGI_STDOUT, req->requestId, data, len);
        }
    }
}

void
end_request (guint64 requestId, int request_num, int app_status, int protocol_status)
{
    FCGI_EndRequestBody body = {
        .reserved1 = 0,
        .reserved2 = 0,
        .reserved3 = 0
    };

    pthread_mutex_lock (&requests_lock);
    Request *req=(Request *)g_hash_table_lookup (requests, &requestId);

    if (req && req->request_num == request_num) {
        g_hash_table_remove(requests, &requestId);
        pthread_mutex_unlock (&requests_lock);
        cmdsocket* sock = find_cmdsocket (req->fd);
        if (sock != NULL) {
            fcgi_set_app_status (&body, app_status);
            body.protocolStatus=protocol_status;
            send_record (sock, FCGI_END_REQUEST, req->requestId, (guint8 *)&body, 0, sizeof (body));

            //flush and disconnect cmdsocket if KEEP_ALIVE is false
            if (!req->keep_alive) {
                flush_cmdsocket(sock);
            }
        }
        g_free (req);
    }
    else {
        pthread_mutex_unlock (&requests_lock);
    }
}

static gboolean
parse_params(Request *req, FCGI_Header *header, guint8 *data)
{
    int data_len = fcgi_get_content_len(header);
    int offset = 0;
    int nlen, vlen;
    guint8 *name, *value;

    //TODO: find appropriate server keys, and only then find host
    HostInfo* host = find_host_by_path("123");

    while (offset < data_len) {
        nlen = data[offset++];

        if (nlen >= 0x80) {
            nlen = ((0x7F & nlen) << 24)
            | (*(data + offset) << 16)
            | (*(data + offset + 1) << 8)
            | *(data + offset + 2);
            offset += 3;
        }

        vlen = data [offset++];

        if (vlen >= 0x80) {
            vlen = ((0x7F & vlen) << 24)
            | (*(data + offset) << 16)
            | (*(data + offset + 1) << 8)
            | *(data + offset + 2);

            offset += 3;
        }

//        if (offset + nlen + vlen > dataLength)
//                throw new ArgumentOutOfRangeException ("offset");
        name = data + offset;
        offset += nlen;
        value = data + offset;
        offset += vlen;

        //params can be server vars or http headers.
        //If param starts from "HTTP_" then it is HTTP header and needs to be reformatted
        if (nlen > 5 && !memcmp(name,"HTTP_",5)) {
            //reformat HTTP header to common name
            //skip "HTTP_" prefix
            name += 5;
            nlen -= 5;
            int i = 0;
            gboolean upper_case = TRUE;

            while (i < nlen) {
                if (name [i] == '_') {
                    name [i] = '-';
                    upper_case = TRUE;
                } else {
                    name [i] = upper_case ? name [i] : g_ascii_tolower (name [i]);
                    upper_case = FALSE;
                }
                i++;
            }
            //call add header function
//            if (host)
//                add_header(host, req->hash, req->request_num, name, nlen, value, vlen);
            //TODO: save to temp array
        }
        else { /* server variable */
            //TODO: call function to add server param
            if (host)
                add_server_variable(host, req->hash, req->request_num, name, nlen, value, vlen);
        }
    }

    //when we receive FCGI_PARAMS with content_len == 0
    //this means, that server passed all the data.
    return data_len == 0;
}







