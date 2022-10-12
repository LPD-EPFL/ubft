#pragma once

// Do not tidy this file directly
#ifndef DORY_TIDIER_ON

#include <cstring>

#include <uv.h>

namespace dory::rpc {

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::entryRead(uv_stream_t *client, ssize_t nread,
                                            const uv_buf_t *buf, size_t start) {
  feed(client, nread, &buf->base[start]);
  delete[] buf->base;
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::read(uv_stream_t *client, ssize_t nread,
                                       const uv_buf_t *buf) {
  feed(client, nread, buf->base);
  delete[] buf->base;
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::write(uv_stream_t *client, size_t nwrite,
                                        void *buf) {
  char *buf_mem = new char[nwrite];
  std::memcpy(buf_mem, buf, nwrite);
  uv_buf_t wrbuf = uv_buf_init(buf_mem, static_cast<unsigned int>(nwrite));
  write(client, nwrite, &wrbuf);
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::write(uv_stream_t *client, size_t nwrite,
                                        const uv_buf_t *buf) {
  auto *req = new WriteReqT;
  req->buf = uv_buf_init(buf->base, static_cast<unsigned int>(nwrite));
  req->handle = client;
  uv_write(reinterpret_cast<uv_write_t *>(req), client, &req->buf, 1, writeCb);
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::disconnect(uv_stream_t *client) {
  uv_close(reinterpret_cast<uv_handle_t *>(client),
           RpcServer<RpcKind>::onCloseClient);
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::freeWriteReq(uv_write_t *req) {
  auto *wr = reinterpret_cast<WriteReqT *>(req);
  delete[] wr->buf.base;
  delete wr;
}

template <typename RpcKind>
void AbstractRpcHandler<RpcKind>::writeCb(uv_write_t *req, int status) {
  auto handle = reinterpret_cast<WriteReqT *>(req)->handle;

  if (status) {
    auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(handle->data);
    LOGGER_WARN(this_ptr->logger, "Write error {}", uv_strerror(status));
  }

  freeWriteReq(req);

  if (status) {
    uv_close(reinterpret_cast<uv_handle_t *>(handle),
             RpcServer<RpcKind>::onCloseClient);
  }
}
}  // namespace dory::rpc
#endif
