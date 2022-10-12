#pragma once

#include <dory/third-party/liquibook/book/order_book.h>

struct ClientRequest {
  int client_id;

  uint64_t req_id;
  bool is_buy;
  liquibook::book::Price price;
  liquibook::book::Quantity qty;
};

struct ClientResponse {
  uint64_t req_id;
  liquibook::book::Quantity fill_qty;
  liquibook::book::Cost fill_cost;
};

struct ClientResponses {
  int num;
  ptrdiff_t offset; // The offset where the replies start from the beginning of the replication Response
};

struct ReplicationResponse {
  int server_id;
  enum Kind {
    OK,
    CHANGE_LEADER,
    FATAL
  };

  Kind kind;
  union {
    int commit_ret;
    int potential_leader;
    int fatal_error;
  } v;

  ClientResponses cli_resp;
};

template <size_t Alignment>
static constexpr size_t round_up_powerof2(size_t v) {
  return (v + Alignment - 1) & (-static_cast<ssize_t>(Alignment));
}

static constexpr uintptr_t cli_resp_offset = round_up_powerof2<16>(sizeof(ReplicationResponse));
static constexpr size_t max_num_cli_resp = 16;