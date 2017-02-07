/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
   vim: set autoindent expandtab shiftwidth=2 softtabstop=2 tabstop=2: */
/*
 * uvsocks.c
 *
 * Copyright (c) 2017 EMSTONE, All rights reserved.
 */

#include "uvsocks.h"
#include "aqueue.h"
#include <uv.h>
#include <glib.h>
#include <stdio.h>
#include <memory.h>
#include <string.h>
#include <limits.h>

#define UVSOCKS_BUF_MAX (1024 * 1024)

#ifndef UV_BUF_LEN
#ifdef _WIN32
#define UV_BUF_LEN(x) ((ULONG)(x))
#else
#define UV_BUF_LEN(x) ((size_t)(x))
#endif
#endif

typedef enum _UvSocksVersion
{
  UVSOCKS_VER_5       = 0x05,
} UvSocksVersion;

typedef enum _UvSocksAuthMethod
{
  UVSOCKS_AUTH_NONE   = 0x00,
  UVSOCKS_AUTH_GSSAPI = 0x01,
  UVSOCKS_AUTH_PASSWD = 0x02,
} UvSocksAuthMethod;

typedef enum _UvSocksAuthResult
{
  UVSOCKS_AUTH_ALLOW = 0x00,
  UVSOCKS_AUTH_DENY  = 0x01,
} UvSocksAuthResult;

typedef enum _UvSocksAddrType
{
  UVSOCKS_ADDR_TYPE_IPV4 = 0x01,
  UVSOCKS_ADDR_TYPE_HOST = 0x03,
  UVSOCKS_ADDR_TYPE_IPV6 = 0x04,
} UvSocksAddrType;

typedef enum _UvSocksCmd
{
  UVSOCKS_CMD_CONNECT       = 0x01,
  UVSOCKS_CMD_BIND          = 0x02,
  UVSOCKS_CMD_UDP_ASSOCIATE = 0x03,
} UvSocksCmd;

typedef enum _UvSocksStage
{
  UVSOCKS_STAGE_NONE                = 0x00,
  UVSOCKS_STAGE_HANDSHAKE           = 0x01,
  UVSOCKS_STAGE_AUTHENTICATE        = 0x02,
  UVSOCKS_STAGE_ESTABLISH           = 0x03,
  UVSOCKS_STAGE_BIND                = 0x04,
  UVSOCKS_STAGE_TUNNEL              = 0x05,
} UvSocksStage;

typedef struct _UvSocksContext UvSocksContext;

typedef struct _UvSocksPoll UvSocksPoll;
struct _UvSocksPoll
{
  UvSocksContext     *context;
  uv_tcp_t           *tcp;
  size_t              read;
  char               *buf;
};

typedef struct _UvSocksForward UvSocksForward;
struct _UvSocksForward
{
  UvSocks           *uvsocks;
  UvSocksForward    *prev;
  UvSocksForward    *next;

  char              *listen_host;
  int                listen_port;
  char              *listen_path;
  char              *remote_host;
  int                remote_port;
  char              *remote_path;

  UvSocksCmd         command;
  UvSocksPoll       *server;
  UvSocksForwardFunc callback_func;
  void              *callback_data;
};

struct _UvSocksContext
{
  UvSocks          *uvsocks;
  UvSocksForward   *forward;
  UvSocksContext   *prev;
  UvSocksContext   *next;

  UvSocksStage      stage;
  uv_mutex_t        mutex;
  UvSocksPoll      *remote;
  UvSocksPoll      *local;
};

typedef struct _UvSocks UvSocks;
struct _UvSocks
{
  uv_loop_t              loop;
  AQueue                *queue;
  uv_async_t             async;
  uv_thread_t            thread;

  char                  *host;
  int                    port;
  char                  *user;
  char                  *password;

  UvSocksForward        *forwards;
  UvSocksForward        *reverse_forwards;
  UvSocksContext        *contexts;

  UvSocksTunnelFunc      callback_func;
  void                  *callback_data;
};

typedef struct _UvSocksPacketReq UvSocksPacketReq;
struct _UvSocksPacketReq
{
  uv_write_t req;
  uv_buf_t   buf;
};

typedef void (*UvSocksFunc) (UvSocks *uvsocks,
                             void    *data);

typedef struct _UvSocksMessage UvSocksMessage;
struct _UvSocksMessage
{
  UvSocksFunc   func;
  void         *data;
  void        (*destroy_data) (void *data);
};

typedef void (*UvSocksDnsResolveFunc) (UvSocksContext   *context,
                                       struct addrinfo  *resolved);
typedef struct _UvSocksDnsResolve UvSocksDnsResolve;
struct _UvSocksDnsResolve
{
  UvSocksDnsResolveFunc func;
  void                 *data;
};

static void
uvsocks_add_forward0 (UvSocks        *uvsocks,
                      UvSocksForward *forward)
{
  forward->uvsocks = uvsocks;

  if (!uvsocks->forwards)
    uvsocks->forwards = forward;
  else
    {
      forward->next = uvsocks->forwards;
      uvsocks->forwards->prev = forward;
      uvsocks->forwards = forward;
    }
}

static void
uvsocks_remove_forward (UvSocks        *uvsocks,
                        UvSocksForward *forward)
{
  if (forward->next)
    forward->next->prev = forward->prev;
  if (forward->prev)
    forward->prev->next = forward->next;
  if (forward == uvsocks->forwards)
    uvsocks->forwards = forward->next;

  g_free (forward->listen_host);
  g_free (forward->listen_path);
  g_free (forward->remote_host);
  g_free (forward->remote_path);

  g_free (forward);
}

static void
uvsocks_free_forward (UvSocks *uvsocks)
{
  if (!uvsocks->forwards)
    return;

  while (uvsocks->forwards)
    uvsocks_remove_forward (uvsocks, uvsocks->forwards);
}

static void
uvsocks_add_reverse_forward0 (UvSocks        *uvsocks,
                              UvSocksForward *forward)
{
  forward->uvsocks = uvsocks;

  if (!uvsocks->reverse_forwards)
    uvsocks->reverse_forwards = forward;
  else
    {
      forward->next = uvsocks->reverse_forwards;
      uvsocks->reverse_forwards->prev = forward;
      uvsocks->reverse_forwards = forward;
    }
}

static void
uvsocks_remove_reverse_forward (UvSocks        *uvsocks,
                                UvSocksForward *forward)
{
  if (forward->next)
    forward->next->prev = forward->prev;
  if (forward->prev)
    forward->prev->next = forward->next;
  if (forward == uvsocks->reverse_forwards)
    uvsocks->reverse_forwards = forward->next;

  g_free (forward->listen_host);
  g_free (forward->listen_path);
  g_free (forward->remote_host);
  g_free (forward->remote_path);

  g_free (forward);
}

static void
uvsocks_free_reverse_forward (UvSocks *uvsocks)
{
  if (!uvsocks->reverse_forwards)
    return;

  while (uvsocks->reverse_forwards)
    uvsocks_remove_reverse_forward (uvsocks, uvsocks->reverse_forwards);
}

static void
uvsocks_receive_async (uv_async_t *handle)
{
  UvSocks *uvsocks = handle->data;

  while (1)
    {
      UvSocksMessage *msg;

      msg = aqueue_pop (uvsocks->queue);
      if (!msg)
        break;

      msg->func (uvsocks, msg->data);

      if (msg->destroy_data)
        msg->destroy_data (msg->data);
      free (msg);
    }
}

static void
uvsocks_send_async (UvSocks      *uvsocks,
                    UvSocksFunc   func,
                    void         *data,
                    void        (*destroy_data) (void *data))
{
  UvSocksMessage *msg;

  msg = malloc (sizeof (*msg));
  if (!msg)
    return;

  msg->func = func;
  msg->data = data;
  msg->destroy_data = destroy_data;
  aqueue_push (uvsocks->queue, msg);
  uv_async_send (&uvsocks->async);
}

void
uvset_thread_name (const char *name)
{
  char thread_name[17];

  snprintf (thread_name, sizeof (thread_name), "%s-%s", PACKAGE, name);
#ifdef linux
  prctl (PR_SET_NAME, (unsigned long) thread_name, 0, 0, 0);
#endif
}

static void
uvsocks_thread_main (void *arg)
{
  UvSocks *uvsocks = arg;

  uvset_thread_name ("uvsocks");

  uv_run (&uvsocks->loop, UV_RUN_DEFAULT);
}

UvSocks *
uvsocks_new (void)
{
  UvSocks *uvsocks;

  uvsocks = g_new0 (UvSocks, 1);
  if (!uvsocks)
    return NULL;

  int iret = uv_loop_init (&uvsocks->loop);
  uvsocks->queue = aqueue_new (128);
  uv_async_init (&uvsocks->loop, &uvsocks->async, uvsocks_receive_async);
  uvsocks->async.data = uvsocks;
  uv_thread_create (&uvsocks->thread, uvsocks_thread_main, uvsocks);

  return uvsocks;
}

static void
uvsocks_quit (UvSocks  *uvsocks,
              void     *data)
{
  uv_stop (&uvsocks->loop);
}

static void
uvsocks_add_context (UvSocks        *uvsocks,
                     UvSocksContext *context)
{
  context->uvsocks = uvsocks;

  if (!uvsocks->contexts)
    uvsocks->contexts = context;
  else
    {
      context->next = uvsocks->contexts;
      uvsocks->contexts->prev = context;
      uvsocks->contexts = context;
    }
}

static void
uvsocks_close_socket (uv_os_sock_t sock)
{
#ifdef _WIN32
  if (sock != INVALID_SOCKET)
    {
      shutdown (sock, SD_BOTH);
      closesocket (sock);
    }
#else
  if (sock >= 0)
    {
      shutdown (sock, SHUT_WR);
      close (sock);
    }
#endif

}

static void
uvsocks_free_handle (uv_handle_t *handle)
{
  UvSocksPoll *poll = handle->data;

  free (poll->buf);
  free (poll);
  free (handle);
}

static void
uvsocks_free_packet_req (uv_write_t *req,
                         int         status)
{
  free (req);
}

static void
uvsocks_write_packet (uv_tcp_t *tcp,
                      char     *packet,
                      size_t    len)
{
  UvSocksPoll *poll = tcp->data;
  UvSocksPacketReq *req;

  req = (UvSocksPacketReq *) malloc (sizeof (*req));
  req->buf = uv_buf_init (packet, (unsigned int) len);
  uv_mutex_lock (&poll->context->mutex);
  uv_write ((uv_write_t *) req,
            (uv_stream_t *) tcp,
            &req->buf,
             1,
             uvsocks_free_packet_req);
  uv_mutex_unlock (&poll->context->mutex);
}

static void
uvsocks_remove_context (UvSocks        *uvsocks,
                        UvSocksContext *context)
{
  if (context->next)
    context->next->prev = context->prev;
  if (context->prev)
    context->prev->next = context->next;
  if (context == uvsocks->contexts)
    uvsocks->contexts = context->next;

  uv_read_stop ((uv_stream_t *) context->remote->tcp);
  uv_close ((uv_handle_t *) context->remote->tcp, uvsocks_free_handle);

  uv_read_stop ((uv_stream_t *) context->local->tcp);
  uv_close ((uv_handle_t *) context->local->tcp, uvsocks_free_handle);

  uv_mutex_destroy (&context->mutex);
  g_free (context);
}

static void
uvsocks_remote_set_stage (UvSocksContext *context,
                          UvSocksStage    stage)
{
  context->stage = stage;
}

static void
uvsocks_remote_login (UvSocksContext *context)
{
  char packet[20];
  size_t packet_size;

  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_HANDSHAKE);

  //+----+----------+----------+
  //|VER | NMETHODS | METHODS  |
  //+----+----------+----------+
  //| 1  |    1     | 1 to 255 |
  //+----+----------+----------+
  // The initial greeting from the client is
  // field 1: SOCKS version number (must be 0x05 for this version)
  // field 2: number of authentication methods supported, 1 byte
  // field 3: authentication methods, variable length, 1 byte per method supported
  packet_size = 0;
  packet[packet_size++] = 0x05;
  packet[packet_size++] = 0x01;
  packet[packet_size++] = UVSOCKS_AUTH_PASSWD;

  uvsocks_write_packet (context->remote->tcp, packet, 3);
}

static void
uvsocks_remote_auth (UvSocksContext *context)
{
  char packet[1024];
  size_t packet_size;
  size_t length;

  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_AUTHENTICATE);

  //+----+------+----------+------+----------+
  //|VER | ULEN |  UNAME   | PLEN |  PASSWD  |
  //+----+------+----------+------+----------+
  //| 1  |  1   | 1 to 255 |  1   | 1 to 255 |
  //+----+------+----------+------+----------+
  //field 1: version number, 1 byte (must be 0x01)
  //field 2: username length, 1 byte
  //field 3: username
  //field 4: password length, 1 byte
  //field 5: password
  packet_size = 0;
  packet[packet_size++] = 0x01;
  length = strlen (context->uvsocks->user);
  packet[packet_size++] = (char) length;
  memcpy (&packet[packet_size], context->uvsocks->user, length);
  packet_size += length;

  length = strlen (context->uvsocks->password);
  packet[packet_size++] = (char) length;
  memcpy (&packet[packet_size], context->uvsocks->password, length);
  packet_size += length;

  uvsocks_write_packet (context->remote->tcp, packet, packet_size);
}

static void
uvsocks_remote_establish (UvSocksContext *context)
{
  char packet[1024];
  size_t packet_size;
  unsigned short port;
  struct sockaddr_in addr;

  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_ESTABLISH);

  //+----+-----+-------+------+----------+----------+
  //|VER | CMD |  RSV  | ATYP | DST.ADDR | DST.PORT |
  //+----+-----+-------+------+----------+----------+
  //| 1  |  1  | X'00' |  1   | Variable |    2     |
  //+----+-----+-------+------+----------+----------+
  //The client's connection request is
  //field 1: SOCKS version number, 1 byte (must be 0x05 for this version)
  //field 2: command code, 1 byte:
  //0x01 = establish a TCP/IP stream connection
  //0x02 = establish a TCP/IP port binding
  //0x03 = associate a UDP port
  //
  //field 3: reserved, must be 0x00
  //field 4: address type, 1 byte:
  //0x01 = IPv4 address
  //0x03 = Domain name
  //0x04 = IPv6 address
  //
  //field 5: destination address of 4 bytes for IPv4 address
  //1 byte of name length followed by the name for domain name
  //16 bytes for IPv6 address
  //
  //field 6: port number in a network byte order, 2 bytes
  packet_size = 0;
  packet[packet_size++] = 0x05;
  packet[packet_size++] = context->forward->command;
  packet[packet_size++] = 0x00;
  packet[packet_size++] = UVSOCKS_ADDR_TYPE_IPV4;
  if (context->forward->command == UVSOCKS_CMD_CONNECT)
    {
      uv_ip4_addr (context->forward->remote_host,
                   context->forward->remote_port,
                  &addr);
      port = htons (context->forward->remote_port);
    }
  if (context->forward->command == UVSOCKS_CMD_BIND)
    {
      uv_ip4_addr (context->forward->listen_host,
                   context->forward->listen_port,
                  &addr);
      port = htons (context->forward->listen_port);
    }
  memcpy (&packet[packet_size], &addr.sin_addr.S_un.S_addr, 4);
  packet_size += 4;
  memcpy (&packet[packet_size], &port, 2);
  packet_size += 2;
  uvsocks_write_packet (context->remote->tcp, packet, packet_size);
}

static UvSocksContext *
uvsocks_create_context (UvSocksForward *forward)
{
  UvSocksContext *context;
  UvSocksPoll *local;
  UvSocksPoll *remote;

  context = g_new0 (UvSocksContext, 1);
  if (!context)
    return NULL;
  local =  malloc (sizeof (*local));
  if (!local)
    {
      g_free (context);
      return NULL;
    }
  local->buf = malloc (UVSOCKS_BUF_MAX);
  local->read = 0;
  local->context = context;
  local->tcp = malloc (sizeof (*local->tcp));
  local->tcp->data = local;

  remote =  malloc (sizeof (*remote));
  if (!remote)
    {
      free (local);
      g_free (context);
      return NULL;
    }
  remote->buf = malloc (UVSOCKS_BUF_MAX);
  remote->read = 0;
  remote->context = context;
  remote->tcp = malloc (sizeof (*local->tcp));
  remote->tcp->data = remote;

  uv_mutex_init (&context->mutex);
  context->forward = forward;
  context->local = local;
  context->remote = remote;

  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_NONE);

  return context;
}

static void
uvsocks_free_context (UvSocks *uvsocks)
{
  if (!uvsocks->contexts)
    return;

  while (uvsocks->contexts)
    uvsocks_remove_context (uvsocks, uvsocks->contexts);
}

void
uvsocks_free (UvSocks *uvsocks)
{
  uvsocks_send_async (uvsocks, uvsocks_quit, NULL, NULL);
  uv_thread_join (&uvsocks->thread);
  uv_close ((uv_handle_t *) &uvsocks->async, NULL);
  uv_loop_close (&uvsocks->loop);

  uvsocks_free_context (uvsocks);
  uvsocks_free_forward (uvsocks);
  uvsocks_free_reverse_forward (uvsocks);

  g_free (uvsocks->host);
  g_free (uvsocks->user);
  g_free (uvsocks->password);
  g_free (uvsocks);
}

void
uvsocks_add_forward (UvSocks           *uvsocks,
                     char              *listen_host,
                     int                listen_port,
                     char              *listen_path,
                     char              *remote_host,
                     int                remote_port,
                     char              *remote_path,
                     UvSocksForwardFunc callback_func,
                     void              *callback_data)
{
  UvSocksForward *forward;

  forward = g_new0 (UvSocksForward, 1);
  if (!forward)
    return;

  forward->command = UVSOCKS_CMD_CONNECT;
  forward->listen_host = g_strdup (listen_host);
  forward->listen_port = listen_port;
  forward->listen_path = g_strdup (listen_path);
  forward->remote_host = g_strdup (remote_host);
  forward->remote_port = remote_port;
  forward->remote_path = g_strdup (remote_path);

  forward->callback_func = callback_func;
  forward->callback_data = callback_data;

  fprintf (stderr,
          "add forward: "
          "listen %s:%d -> %s:%d\n",
           forward->listen_host,
           forward->listen_port,
           forward->remote_host,
           forward->remote_port);
  uvsocks_add_forward0 (uvsocks, forward);
}

void
uvsocks_add_reverse_forward (UvSocks           *uvsocks,
                             char              *listen_host,
                             int                listen_port,
                             char              *listen_path,
                             char              *remote_host,
                             int                remote_port,
                             char              *remote_path,
                             UvSocksForwardFunc callback_func,
                             void              *callback_data)
{
  UvSocksForward *forward;

  forward = g_new0 (UvSocksForward, 1);
  if (!forward)
    return;

  forward->command = UVSOCKS_CMD_BIND;
  forward->listen_host = g_strdup (listen_host);
  forward->listen_port = listen_port;
  forward->listen_path = g_strdup (listen_path);
  forward->remote_host = g_strdup (remote_host);
  forward->remote_port = remote_port;
  forward->remote_path = g_strdup (remote_path);

  forward->callback_func = callback_func;
  forward->callback_data = callback_data;

  fprintf (stderr,
          "add reverse forward: "
          "%s:%d\n",
           forward->remote_host,
           forward->remote_port);
  uvsocks_add_reverse_forward0 (uvsocks, forward);
}

static int
uvsocks_set_nonblocking (uv_os_sock_t sock)
{
  int r;
#ifdef _WIN32
  unsigned long on = 1;
  r = ioctlsocket (sock, FIONBIO, &on);
  if (r)
    return 1;
#else
  int flags = fcntl (sock, F_GETFL, 0);
  if (flags < 0)
    return 1;
  r = fcntl (sock, F_SETFL, flags | O_NONBLOCK);
  if (r < 0)
    return 1;
#endif
  return 0;
}

static void
uvsocks_dns_resolved (uv_getaddrinfo_t  *resolver,
                      int                status,
                      struct addrinfo   *resolved)
{
  UvSocksDnsResolve *d = resolver->data;
  UvSocksContext *context = d->data;

  if (status < 0)
    {
      fprintf (stderr,
              "failed to resolve dns name: %s\n",
               uv_strerror ((int) status));
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_DNS_RESOLVE,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      goto done;
    }

  if (d->func)
    d->func (context, resolved);

done:
  uv_freeaddrinfo (resolved);
  free (resolver);
  free (d);
}

static void
uvsocks_dns_resolve (UvSocks              *uvsocks,
                     char                 *host,
                     char                 *port,
                     UvSocksDnsResolveFunc func,
                     void                 *data)
{
  UvSocksContext *context = data;
  UvSocksDnsResolve *d;
  uv_getaddrinfo_t *resolver;
  struct addrinfo hints;
  int status;

  hints.ai_family = PF_INET; 
  hints.ai_socktype = SOCK_STREAM; 
  hints.ai_protocol = IPPROTO_TCP; 
  hints.ai_flags = 0;

  resolver = malloc (sizeof (*resolver));
  if (!resolver)
    return;
  d = malloc (sizeof (*d));
  if (!d)
    {
      free (resolver);
      g_free (port);
      return;
    }

  d->data = data;
  d->func = func;
  resolver->data = d;

  status = uv_getaddrinfo (uv_default_loop (),
                           resolver,
                           uvsocks_dns_resolved,
                           host,
                           port,
                           &hints);
  if (status)
    {
      fprintf (stderr,
              "failed getaddrinfo: %s\n",
               uv_err_name (status));
      if (uvsocks->callback_func)
        uvsocks->callback_func (uvsocks,
                                UVSOCKS_ERROR_DNS_ADDRINFO,
                                uvsocks->callback_data);

      uvsocks_remove_context (uvsocks, context);
      free (resolver);
    }
  g_free (port);
}

static void
uvsocks_remote_alloc_buffer (uv_handle_t *handle,
                             size_t       suggested_size,
                             uv_buf_t    *buf)
{
  UvSocksPoll *poll = handle->data;
  //UvSocksContext *context = handle->data;
  size_t size;

  size = UVSOCKS_BUF_MAX;
  if (size > suggested_size)
    size = suggested_size;

  buf->base = poll->context->remote->buf;
  buf->len = UV_BUF_LEN (size);
}

static void
uvsocks_local_alloc_buffer (uv_handle_t *handle,
                            size_t       suggested_size,
                            uv_buf_t    *buf)
{
  UvSocksPoll *poll = handle->data;
  size_t size;

  size = UVSOCKS_BUF_MAX;
  if (size > suggested_size)
    size = suggested_size;

  buf->base = poll->context->local->buf;
  buf->len = UV_BUF_LEN (size);
}

static void
uvsocks_local_connected (uv_connect_t *connect,
                         int           status)
{
  UvSocksContext *context = connect->data;

  if (status < 0)
    {
      fprintf (stderr,
              "failed to connect to %s@%s:%d - %s\n",
               context->uvsocks->user,
               context->uvsocks->host,
               context->uvsocks->port,
               uv_strerror ((int) status));
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_CONNECT,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      free (connect);
      return;
    }
  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_TUNNEL);
  uvsocks_local_start_read (context);

  free (connect);
}

static void
uvsocks_connect_local_real (UvSocksContext   *context,
                             struct addrinfo  *resolved)
{
  uv_connect_t *connect;

  connect = malloc (sizeof (*connect));
  if (!connect)
    return;

  connect->data = context;
  uv_tcp_init (uv_default_loop (), context->local->tcp);
  uv_tcp_connect (connect,
                  context->local->tcp,
                  (const struct sockaddr *)resolved->ai_addr,
                  uvsocks_local_connected);
}

static int
uvsocks_connect_local (UvSocksForward *forward,
                       UvSocksContext *context,
                       char           *host,
                       int             port)
{
  uvsocks_dns_resolve (forward->uvsocks,
                       host,
                       g_strdup_printf("%i", port),
                       uvsocks_connect_local_real,
                       context);
  return 0;
}

static int
uvsocks_local_start_read (UvSocksContext *context);

static void
uvsocks_reverse_forward (UvSocks *uvsocks,
                         void    *data);

static void
uvsocks_remote_read (uv_stream_t    *stream,
                     ssize_t         nread,
                     const uv_buf_t *buf)
{
  UvSocksPoll *poll = stream->data;
  UvSocksContext *context = poll->context;

  if (nread < 0)
    {
      fprintf (stderr,
              "failed to read remote client: %s\n",
               uv_strerror ((int) nread));

      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                       UVSOCKS_ERROR_REMOTE_READ,
                                       context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      if (context->forward->command == UVSOCKS_CMD_BIND)
        uvsocks_send_async (context->uvsocks,
                            uvsocks_reverse_forward,
                            context->forward, NULL);
      return;
    }
  if (nread == 0)
    return;

  switch (context->stage)
    {
      case UVSOCKS_STAGE_NONE:
      break;
      case UVSOCKS_STAGE_HANDSHAKE:
        {
          //+----+--------+
          //|VER | METHOD |
          //+----+--------+
          //| 1  |   1    |
          //+----+--------+
          //field 1: SOCKS version, 1 byte (0x05 for this version)
          //field 2: chosen authentication method, 1 byte, or 0xFF if no acceptable methods were offered
          if (nread < 2)
            break;
          if (context->remote->buf[0] != 0x05 ||
              context->remote->buf[1] != UVSOCKS_AUTH_PASSWD)
            {
              fprintf (stderr,
                      "failed to handshake\n");

              if (context->uvsocks->callback_func)
                context->uvsocks->callback_func (context->uvsocks,
                                               UVSOCKS_ERROR_HANDSHAKE,
                                               context->uvsocks->callback_data);

              uvsocks_remove_context (context->uvsocks, context);
              break;
            }
          uvsocks_remote_auth (context);
        }
      break;
      case UVSOCKS_STAGE_AUTHENTICATE:
        {
          //+----+--------+
          //|VER | STATUS |
          //+----+--------+
          //| 1  |   1    |
          //+----+--------+
          //field 1: version, 1 byte
          //field 2: status code, 1 byte 0x00 = success
          //any other value = failure, connection must be closed
          if (nread < 2)
            break;
          if (context->remote->buf[0] != 0x01 ||
              context->remote->buf[1] != UVSOCKS_AUTH_ALLOW)
            {
              fprintf (stderr,
                      "failed to login to %s@%s:%d - SOCKS ver:%d status:%d\n",
                       context->uvsocks->user,
                       context->uvsocks->host,
                       context->uvsocks->port,
                       context->remote->buf[0],
                       context->remote->buf[1]);
              if (context->uvsocks->callback_func)
                context->uvsocks->callback_func (context->uvsocks,
                                               UVSOCKS_ERROR_AUTH,
                                               context->uvsocks->callback_data);

              uvsocks_remove_context (context->uvsocks, context);
              break;
            }
          uvsocks_remote_establish (context);
        }
      break;
      case UVSOCKS_STAGE_ESTABLISH:
      case UVSOCKS_STAGE_BIND:
        {
          //field 1: SOCKS protocol version, 1 byte (0x05 for this version)
          //field 2: status, 1 byte:
          //0x00 = request granted
          //0x01 = general failure
          //0x02 = connection not allowed by ruleset
          //0x03 = network unreachable
          //0x04 = host unreachable
          //0x05 = connection refused by destination host
          //0x06 = TTL expired
          //0x07 = command not supported / protocol error
          //0x08 = address type not supported
          //
          //field 3: reserved, must be 0x00
          //field 4: address type, 1 byte: 0x01 = IPv4 address
          //0x03 = Domain name
          //0x04 = IPv6 address
          //
          //field 5: destination address of 4 bytes for IPv4 address
          //1 byte of name length followed by the name for domain name
          //16 bytes for IPv6 address
          //
          //field 6: network byte order port number, 2 bytes
          if (context->remote->buf[0] != 0x05 ||
              context->remote->buf[1] != 0x00)
            {
              fprintf (stderr,
                      "failed to forward %s:%d -> localhost:%d - SOCKS ver:%d status:%d",
                       context->forward->remote_host,
                       context->forward->remote_port,
                       context->forward->listen_port,
                       context->remote->buf[0],
                       context->remote->buf[1]);
              if (context->uvsocks->callback_func)
                context->uvsocks->callback_func (context->uvsocks,
                                               UVSOCKS_ERROR_FORWARD,
                                               context->uvsocks->callback_data);

              uvsocks_remove_context (context->uvsocks, context);
              break;
            }

          if (context->stage == UVSOCKS_STAGE_ESTABLISH &&
              context->forward->command == UVSOCKS_CMD_BIND)
            {
              int port;

              memcpy (&port, &poll->buf[8], 2);
              port = htons(port);

              if (context->forward->callback_func)
                context->forward->callback_func (context->uvsocks,
                                                 context->forward->remote_host,
                                                 context->forward->remote_port,
                                                 context->uvsocks->host,
                                                 port,
                                                 context->forward->callback_data);
              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_BIND);
              break;
            }

          if (context->stage == UVSOCKS_STAGE_BIND &&
              context->forward->command == UVSOCKS_CMD_BIND)
            {
              uvsocks_connect_local (context->forward,
                                     context,
                                     context->forward->remote_host,
                                     context->forward->remote_port);
              break;
            }

          uvsocks_remote_set_stage (context, UVSOCKS_STAGE_TUNNEL);
          uvsocks_local_start_read (context);
        }
      break;
      case UVSOCKS_STAGE_TUNNEL:
        {
          uvsocks_write_packet (context->local->tcp, context->remote->buf, nread);
        }
      break;
    }
}

static void
uvsocks_local_read (uv_stream_t    *stream,
                    ssize_t         nread,
                    const uv_buf_t *buf)
{
  UvSocksPoll *poll = stream->data;
  UvSocksContext *context = poll->context;

  if (nread < 0)
    {
      fprintf (stderr,
              "failed to read local client: %s\n",
               uv_strerror ((int) nread));

      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_LOCAL_READ,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      if (context->forward->command == UVSOCKS_CMD_BIND)
        uvsocks_send_async (context->uvsocks,
                            uvsocks_reverse_forward,
                            context->forward, NULL);
      return;
    }
  if (nread == 0)
    return;

  uvsocks_write_packet (context->remote->tcp, context->local->buf, nread);
}

static int
uvsocks_local_start_read (UvSocksContext *context)
{
  return uv_read_start ((uv_stream_t *) context->local->tcp,
                                        uvsocks_local_alloc_buffer,
                                        uvsocks_local_read);
}

static int
uvsocks_remote_start_read (UvSocksContext *context)
{
  return uv_read_start ((uv_stream_t *) context->remote->tcp,
                                        uvsocks_remote_alloc_buffer,
                                        uvsocks_remote_read);
}

static void
uvsocks_remote_connected (uv_connect_t *connect,
                          int           status)
{
  UvSocksContext *context = connect->data;

  if (status < 0)
    {
      fprintf (stderr,
              "failed to connect to %s@%s:%d - %s\n",
               context->uvsocks->user,
               context->uvsocks->host,
               context->uvsocks->port,
               uv_strerror ((int) status));
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_CONNECT,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      free (connect);
      return;
    }
  uvsocks_remote_login (context);
  if (uvsocks_remote_start_read (context))
    {
      fprintf (stderr,
              "failed uvsocks_remote_read_start\n");
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_POLL_REMOTE_READ_START,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      return;
    }
  free (connect);
}

static void
uvsocks_connect_remote_real (UvSocksContext   *context,
                             struct addrinfo  *resolved)
{
  uv_connect_t *connect;

  connect = malloc (sizeof (*connect));
  if (!connect)
    return;

  connect->data = context;
  uv_tcp_init (uv_default_loop (), context->remote->tcp);
  uv_tcp_connect (connect,
                  context->remote->tcp,
                  (const struct sockaddr *)resolved->ai_addr,
                  uvsocks_remote_connected);
}

static int
uvsocks_connect_remote (UvSocksForward *forward,
                        UvSocksContext *context,
                        char           *host,
                        int             port)
{
  uvsocks_dns_resolve (forward->uvsocks,
                       host,
                       g_strdup_printf("%i", port),
                       uvsocks_connect_remote_real,
                       context);
  return 0;
}

static void
uvsocks_local_new_connection (uv_stream_t *stream,
                              int          status)
{
  UvSocksForward *forward = stream->data;
  UvSocksPoll *server = forward->server;
  UvSocksContext *context;

  if (status == -1)
    return;

  context = uvsocks_create_context (forward);
  if (!context)
    {
      return;
    }

  uv_tcp_init (uv_default_loop (), context->local->tcp);
  if (uv_accept (stream, (uv_stream_t *) context->local->tcp))
    {
      free (context);
      return;
    }

  uvsocks_add_context (forward->uvsocks, context);

  uvsocks_connect_remote (forward,
                          context,
                          forward->uvsocks->host,
                          forward->uvsocks->port);
}

static UvSocksPoll *
uvsocks_start_local_server (UvSocks    *uvsocks,
                            const char *host,
                            int        *port)
{
  UvSocksPoll *server;
  struct sockaddr_in addr;
  struct sockaddr_in name;
  int namelen;
  int r;

  if (*port < 0 || *port > 65535)
    return NULL;
  uv_ip4_addr (host, *port, &addr);

  server = calloc (1, sizeof (*server));
  if (!server)
    return NULL;

  server->tcp = malloc (sizeof (*server->tcp));
  if (!server->tcp)
    {
      free (server);
      return NULL;
    }

  uv_tcp_init (uv_default_loop (), server->tcp);

  r = uv_tcp_bind (server->tcp, (const struct sockaddr *) &addr, 0);
  if (r < 0)
    {
      fprintf (stderr,
              "failed to bind to %s:0 - %s\n", host,
               uv_strerror (r));
      goto fail;
    }

  namelen = sizeof (name);
  uv_tcp_getsockname (server->tcp, (struct sockaddr *) &name, &namelen);
  *port = ntohs (name.sin_port);

  r = uv_listen ((uv_stream_t *) server->tcp, 16, uvsocks_local_new_connection);
  if (r < 0)
    {
      fprintf (stderr,
              "failed to listen on %s:%d - %s\n",
               host,
              *port,
                   uv_strerror (r));
      goto fail;
    }
  return server;

fail:
  free (server->tcp);
  free (server);
  return NULL;
}

static void
uvsocks_forward (UvSocks *uvsocks,
                 void    *data)
{
  UvSocksForward *forward = data;
  UvSocksPoll *s;
  int port;

  port = forward->listen_port;
  s = uvsocks_start_local_server (uvsocks,
                                  forward->listen_host,
                                  &port);
  if (!s)
    {
      fprintf (stderr,
              "failed to forward -> "
              "local:%s:%d -> server:%s:%d  -> remote:%s:%d\n",
               forward->listen_host,
               forward->listen_port,
               uvsocks->host,
               uvsocks->port,
               forward->remote_host,
               forward->remote_port);

      if (uvsocks->callback_func)
       uvsocks->callback_func (uvsocks,
                               UVSOCKS_ERROR_LOCAL_SERVER,
                               uvsocks->callback_data);
      return;
    }

  forward->server = s;
  forward->server->tcp->data = forward;
  forward->listen_port = port;

  if (forward->callback_func)
    forward->callback_func (uvsocks,
                            forward->remote_host,
                            forward->remote_port,
                            forward->listen_host,
                            forward->listen_port,
                            forward->callback_data);
}

static void
uvsocks_reverse_forward (UvSocks *uvsocks,
                         void    *data)
{
  UvSocksForward *forward = data;
  UvSocksContext *context;

  context = uvsocks_create_context (forward);
  if (!context)
    return;

  uvsocks_add_context (forward->uvsocks, context);

  uvsocks_connect_remote (forward,
                          context,
                          forward->uvsocks->host,
                          forward->uvsocks->port);
}

static void
uvsocks_tunnel_real (UvSocks  *uvsocks,
                     void     *data)
{
  UvSocksStatus status;
  UvSocksForward *l;

  status = UVSOCKS_OK;

  for (l = uvsocks->reverse_forwards; l != NULL; l = l->next)
    uvsocks_send_async (uvsocks, uvsocks_reverse_forward, l, NULL);

  for (l = uvsocks->forwards; l != NULL; l = l->next)
    uvsocks_send_async (uvsocks, uvsocks_forward, l, NULL);
}

int
uvsocks_tunnel (UvSocks           *uvsocks,
                char              *host,
                int                port,
                char              *user,
                char              *password,
                UvSocksTunnelFunc  callback_func,
                void              *callback_data)
{
  fprintf (stderr,
          "tunnel -> "
          "host:%s:%d user:%s\n",
           host,
           port,
           user);

  uvsocks->host = g_strdup (host);
  uvsocks->port = port;
  uvsocks->user = g_strdup (user);
  uvsocks->password = g_strdup (password);

  uvsocks->callback_func = callback_func;
  uvsocks->callback_data = callback_data;

  uvsocks_send_async (uvsocks, uvsocks_tunnel_real, NULL, NULL);
  return 0;
}