#pragma once

// Do not tidy this file directly
#ifndef DORY_TIDIER_ON

#include <cstdint>

#include <uv.h>

#include <dory/shared/logger.hpp>

namespace dory::rpc {

template <typename RpcKind>
class AbstractRpcHandler {
 public:
  AbstractRpcHandler() : LOGGER_INIT(logger, "RpcAbstractHandler") {}

  virtual RpcKind kind() const = 0;

  void entryRead(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf,
                 size_t start);

  void read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);

  void write(uv_stream_t *client, size_t nwrite, void *buf);

  void write(uv_stream_t *client, size_t nwrite, const uv_buf_t *buf);

  void disconnect(uv_stream_t *client);

  virtual void feed(uv_stream_t *client, ssize_t nread, char const *buf) = 0;

  virtual void disconnected(uv_stream_t *client) = 0;

  virtual ~AbstractRpcHandler() = default;

 private:
  struct WriteReqT {
    uv_write_t req;
    uv_buf_t buf;
    uv_stream_t *handle;
  };

  static void freeWriteReq(uv_write_t *req);
  static void writeCb(uv_write_t *req, int status);

  LOGGER_DECL(logger);
};
}  // namespace dory::rpc
#endif
