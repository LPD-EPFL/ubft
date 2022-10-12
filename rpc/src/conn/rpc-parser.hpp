#pragma once

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace dory::rpc::conn {
template <typename ProcIdType>
class ConnectionRpcHandlerParser {
 public:
  enum Step { Step1, Step2 };

  void feed(ssize_t nread, char const *buf) {
    // Copy the buffer locally
    parsing_buf.insert(parsing_buf.end(), &buf[0], &buf[nread]);
  }

  std::optional<Step> parse() {
    switch (item) {
      case 0:
        if (!parseClientId()) {
          break;
        }
        [[fallthrough]];
      case 1:
        if (!parseConnectionInfo()) {
          break;
        } else {
          return std::optional<Step>(Step1);
        }
      case 2:
        if (!parseConnectionInitialized()) {
          break;
        } else {
          return std::optional<Step>(Step2);
        }

      default:
        return std::nullopt;
    }
    return std::nullopt;
  }

  bool initialized() const { return initialized; }

  ProcIdType clientId() const { return client_id_; }

  std::string connectionInfo() const { return connection_info_; }

 private:
  bool parseClientId() {
    if (parsing_buf.size() >= sizeof(client_id_)) {
      std::memcpy(&client_id_, parsing_buf.data(), sizeof(client_id_));
      parsing_buf.erase(parsing_buf.begin(),
                        parsing_buf.begin() + sizeof(client_id_));
      item++;
      return true;
    }

    return false;
  }

  bool parseConnectionInfo() {
    uint32_t length;
    if (parsing_buf.size() >= sizeof(length)) {
      std::memcpy(&length, parsing_buf.data(), sizeof(length));

      if (parsing_buf.size() >= length + sizeof(length)) {
        connection_info_ =
            std::string(parsing_buf.begin() + sizeof(length),
                        parsing_buf.begin() + sizeof(length) + length);
        parsing_buf.erase(parsing_buf.begin(),
                          parsing_buf.begin() + length + sizeof(length));
        item++;
        return true;
      }
    }

    return false;
  }

  bool parseConnectionInitialized() {
    constexpr auto Len = std::char_traits<char>::length(Done);

    char msg[Len + 1];
    if (parsing_buf.size() >= Len) {
      std::memcpy(msg, parsing_buf.data(), Len);
      msg[Len] = 0;

      if (std::string(msg) == std::string(Done)) {
        initialized_ = true;
        parsing_buf.erase(parsing_buf.begin(), parsing_buf.begin() + Len);
        item++;
        return true;
      }
    }

    return false;
  }

  static constexpr char const *Done = "DONE";

  ProcIdType client_id_;
  std::string connection_info_;
  bool initialized_;

  int item = 0;
  std::vector<char> parsing_buf;
};
}  // namespace dory::rpc::conn
