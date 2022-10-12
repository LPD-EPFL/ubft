#pragma once

// Do not tidy this file directly
#ifndef DORY_TIDIER_ON

#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <uv.h>

#include <dory/shared/logger.hpp>

namespace dory::rpc {

template <typename RpcKind>
class RpcServer {
 public:
  RpcServer(std::string ip, int port);
  ~RpcServer() {
    stop();
    uv_loop_close(loop);
  }

  void attachHandler(std::unique_ptr<AbstractRpcHandler<RpcKind>> handler);

  bool start();
  bool startOrChangePort();
  bool stop();

  int port() const { return m_port; }

  std::string ip() const { return m_ip; }

  static void onCloseClient(uv_handle_t *handle);

 private:
  void handleOrClose(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);

  // void close_conn(uv_stream_t *client, uv_close_cb close_cb) {
  //   LOGGER_INFO(logger, "Closing connection");
  //   uv_close(reinterpret_cast<uv_handle_t*>(client), close_cb);
  // }

  static void onNewConnection(uv_stream_t *server, int status);

  static void allocCb(uv_handle_t *handle, size_t suggested_size,
                      uv_buf_t *buf);

  static void readRpcKind(uv_stream_t *client, ssize_t nread,
                          const uv_buf_t *buf);

  static void asyncCb(uv_async_t *async) {
    auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(async->data);

    LOGGER_INFO(this_ptr->logger, "Stopping");
    uv_stop(this_ptr->loop);

    uv_walk(
        this_ptr->loop,
        [](uv_handle_t *handle, void *arg) {
          auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(arg);

          if (this_ptr->default_handles.find(handle) !=
              this_ptr->default_handles.end()) {
            uv_close(handle, nullptr);
          } else {
            uv_close(handle, RpcServer<RpcKind>::onCloseClient);
          }

          uv_run(this_ptr->loop, UV_RUN_ONCE);
        },
        this_ptr);
  }

  std::string m_ip;
  int m_port;
  bool started;

  // LibUV initialization
  uv_loop_t *loop;
  uv_async_t async;
  uv_tcp_t server;
  struct sockaddr_in addr;

  // Rpc handlers
  std::unordered_map<RpcKind, std::unique_ptr<AbstractRpcHandler<RpcKind>>>
      handlers;
  std::unordered_map<uintptr_t, RpcKind> sessions;

  std::thread event_loop;

  std::unordered_set<uv_handle_t *> default_handles;

  LOGGER_DECL(logger);

  friend class AbstractRpcHandler<RpcKind>;
};
}  // namespace dory::rpc
#endif
