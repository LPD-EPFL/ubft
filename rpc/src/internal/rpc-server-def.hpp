#pragma once

// Do not tidy this file directly
#ifndef DORY_TIDIER_ON

#include <stdexcept>

#include <dory/shared/unused-suppressor.hpp>
#include <utility>

#include <uv.h>

namespace dory::rpc {

template <typename RpcKind>
RpcServer<RpcKind>::RpcServer(std::string ip, int port)
    : m_ip{std::move(ip)},
      m_port{port},
      started{false},
      LOGGER_INIT(logger, "RpcServer") {
  loop = uv_default_loop();

  if (uv_async_init(loop, &async, RpcServer<RpcKind>::asyncCb) != 0) {
    throw std::runtime_error("uv_async_init failed!");
  }
  async.data = this;

  uv_tcp_init(loop, &server);

  uv_ip4_addr(m_ip.c_str(), m_port, &addr);
  uv_tcp_bind(&server, reinterpret_cast<struct sockaddr const *>(&addr), 0);

  // Store the class pointer to use in callbacks
  server.data = this;

  // Store the default handles used by libuv
  // For these handles, uv_close has a nullptr callback
  // The rest of the handles are created such that uv_close has
  // the `onCloseClient` callback.
  uv_walk(
      loop,
      [](uv_handle_t *handle, void *arg) {
        auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(arg);
        this_ptr->default_handles.insert(handle);
      },
      this);
}

template <typename RpcKind>
void RpcServer<RpcKind>::attachHandler(
    std::unique_ptr<AbstractRpcHandler<RpcKind>> handler) {
  auto [_, inserted] = handlers.insert({handler->kind(), std::move(handler)});

  if (!inserted) {
    throw std::runtime_error(
        "Cannot attach an RpcHandler that is already attached!");
  }
}

template <typename RpcKind>
bool RpcServer<RpcKind>::start() {
  if (started) {
    return false;
  }

  int r = uv_listen(reinterpret_cast<uv_stream_t *>(&server), 128,
                    RpcServer::onNewConnection);

  if (r != 0) {
    LOGGER_WARN(logger, "Listening failed: {}", uv_strerror(r));
    return false;
  }

  LOGGER_INFO(logger, "Binding to {}:{}", m_ip, m_port);

  event_loop = std::thread([this]() { uv_run(loop, UV_RUN_DEFAULT); });
  started = true;

  return true;
}

template <typename RpcKind>
bool RpcServer<RpcKind>::startOrChangePort() {
  if (started) {
    return false;
  }

  int r;
  while ((r = uv_listen(reinterpret_cast<uv_stream_t *>(&server), 128,
                        RpcServer::onNewConnection)) != 0) {
    if (r == UV_EADDRINUSE) {
      m_port += 1;
      uv_ip4_addr(m_ip.c_str(), m_port, &addr);
      uv_tcp_bind(&server, reinterpret_cast<struct sockaddr const *>(&addr), 0);
      continue;
    }

    if (r != 0) {
      LOGGER_WARN(logger, "Listening failed: {}", uv_strerror(r));
      return false;
    }
  }

  LOGGER_INFO(logger, "Binding to {}:{}", m_ip, m_port);

  event_loop = std::thread([this]() { uv_run(loop, UV_RUN_DEFAULT); });
  started = true;

  // uv_run(loop, UV_RUN_DEFAULT);
  return true;
}

template <typename RpcKind>
bool RpcServer<RpcKind>::stop() {
  if (!started) {
    return false;
  }
  uv_async_send(&async);
  event_loop.join();

  started = false;

  return true;
}

template <typename RpcKind>
void RpcServer<RpcKind>::onCloseClient(uv_handle_t *handle) {
  auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(handle->data);
  auto session_it =
      this_ptr->sessions.find(reinterpret_cast<uintptr_t>(handle));
  if (session_it != this_ptr->sessions.end()) {
    auto kind = session_it->second;
    auto handler_it = this_ptr->handlers.find(kind);

    if (handler_it != this_ptr->handlers.end()) {
      LOGGER_DEBUG(this_ptr->logger, "Disconnecting session");
      handler_it->second->disconnected(reinterpret_cast<uv_stream_t *>(handle));
    }

    LOGGER_DEBUG(this_ptr->logger, "Erasing session");
    this_ptr->sessions.erase(session_it);
  }

  LOGGER_DEBUG(this_ptr->logger, "Closing connection");
  delete handle;
}

template <typename RpcKind>
void RpcServer<RpcKind>::handleOrClose(uv_stream_t *client, ssize_t nread,
                                       const uv_buf_t *buf) {
  auto session_it = sessions.find(reinterpret_cast<uintptr_t>(client));
  if (session_it == sessions.end()) {
    auto kind = static_cast<RpcKind>(buf->base[0]);

    auto handler_it = handlers.find(kind);
    if (handler_it != handlers.end()) {
      sessions.insert({reinterpret_cast<uintptr_t>(client), kind});
      if (nread > 1) {
        handler_it->second->entryRead(client, nread - 1, buf, 1);
      } else {
        delete[] buf->base;
      }
    } else {
      LOGGER_ERROR(logger, "Unknown RpcKind {}", kind);
      uv_close(reinterpret_cast<uv_handle_t *>(client), onCloseClient);
      delete[] buf->base;
    }
  } else {
    auto kind = session_it->second;
    auto handler_it = handlers.find(kind);
    handler_it->second->read(client, nread, buf);
  }
}

// void close_conn(uv_stream_t *client, uv_close_cb close_cb) {
//   LOGGER_INFO(logger, "Closing connection");
//   uv_close(reinterpret_cast<uv_handle_t*>(client), close_cb);
// }

template <typename RpcKind>
void RpcServer<RpcKind>::onNewConnection(uv_stream_t *server, int status) {
  auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(server->data);

  LOGGER_DEBUG(this_ptr->logger, "New connection");

  if (status < 0) {
    LOGGER_WARN(this_ptr->logger, "New connection error: {}",
                uv_strerror(status));
    return;
  }

  auto *client = new uv_tcp_t;
  client->data = this_ptr;

  uv_tcp_init(this_ptr->loop, client);

  if (uv_accept(server, reinterpret_cast<uv_stream_t *>(client)) == 0) {
    uv_read_start(reinterpret_cast<uv_stream_t *>(client), allocCb,
                  readRpcKind);
  } else {
    uv_close(reinterpret_cast<uv_handle_t *>(client), onCloseClient);
  }
}

template <typename RpcKind>
void RpcServer<RpcKind>::allocCb(uv_handle_t *handle, size_t suggested_size,
                                 uv_buf_t *buf) {
  ignore(handle);

  buf->base = new char[suggested_size];
  buf->len = suggested_size;
}

template <typename RpcKind>
void RpcServer<RpcKind>::readRpcKind(uv_stream_t *client, ssize_t nread,
                                     const uv_buf_t *buf) {
  auto this_ptr = reinterpret_cast<RpcServer<RpcKind> *>(client->data);

  if (nread > 0) {
    this_ptr->handleOrClose(client, nread, buf);
  }

  if (nread < 0) {
    if (nread != UV_EOF) {
      LOGGER_WARN(this_ptr->logger, "Read error: {}",
                  uv_err_name(static_cast<int>(nread)));
    }

    uv_close(reinterpret_cast<uv_handle_t *>(client), onCloseClient);
    delete[] buf->base;
  }
}
}  // namespace dory::rpc
#endif
