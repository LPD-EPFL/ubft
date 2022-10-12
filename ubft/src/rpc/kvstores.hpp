#pragma once

#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fmt/core.h>

namespace dory::ubft::kvstores {

static std::string exec(const char *cmd) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

template <class InputIt>
static std::string buff_repr(InputIt first, InputIt last) {
  std::string res;
  char tmp[4];

  for (InputIt it = first; it != last; it++) {
    char c = static_cast<char>(*it);

    switch (c) {
      case '\0':
        res += "\\0";
        break;
      case '\a':
        res += "\\a";
        break;
      case '\b':
        res += "\\b";
        break;
      case '\t':
        res += "\\t";
        break;
      case '\n':
        res += "\\n";
        break;
      case '\v':
        res += "\\v";
        break;
      case '\f':
        res += "\\f";
        break;
      case '\r':
        res += "\\r";
        break;
      default:
        if (isprint(c)) {
          res += c;
        } else {
          std::ostringstream ss;
          ss << std::hex << c;
          res += "\\x" + ss.str();
        }
    }
  }

  return res;
}

static void mkrndstr_ipa(int length, uint8_t *randomString) {
  static char charset[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  if (length) {
    if (randomString) {
      int l = static_cast<int>(sizeof(charset) - 1);
      for (int n = 0; n < length; n++) {
        int key = std::rand() % l;
        randomString[n] = charset[key];
      }

      randomString[length] = '\0';
    }
  }
}

inline size_t integer_repr_length(int n) {
  if (n < 0)
    n = (n == std::numeric_limits<int>::min()) ? std::numeric_limits<int>::max()
                                               : -n;
  if (n < 10) {
    return 1;
  }
  if (n < 100) {
    return 2;
  }
  if (n < 1000) {
    return 3;
  }
  if (n < 10000) {
    return 4;
  }
  if (n < 100000) {
    return 5;
  }
  if (n < 1000000) {
    return 6;
  }
  if (n < 10000000) {
    return 7;
  }
  if (n < 100000000) {
    return 8;
  }
  if (n < 1000000000) {
    return 9;
  }
  /*      2147483647 is 2^31-1 - add more ifs as needed
      and adjust this final return as well. */
  return 10;
}

namespace memcached {
static void spawn_memc(int port) {
  pid_t child_pid = fork();
  if (child_pid == 0) {
    char const *args[] = {"/usr/bin/memcached", "-p",
                          fmt::format("{}", port).c_str(), NULL};
    char const *envs[] = {"LD_PRELOAD=/home/xygkis/ubft_mu/libreparent.so",
                          NULL};
    int ret = execve("/usr/bin/memcached", const_cast<char *const *>(args),
                     const_cast<char *const *>(envs));
    if (ret == -1) {
      throw std::runtime_error(
          fmt::format("Could not start memcached: {}", std::strerror(errno)));
    }
  }
}

inline size_t put_max_buffer_size(int const key_size, int const value_size) {
  return 13 + key_size + 10 + value_size;
}

inline size_t put_buffer_size(int const key_size, int const value_size) {
  return 13 + key_size + integer_repr_length(value_size) + value_size;
}

static size_t put(void *buffer, int const key_size, int const value_size) {
  auto *buf = reinterpret_cast<uint8_t *>(buffer);
  size_t cur = 0;
  buf[cur++] = 's';
  buf[cur++] = 'e';
  buf[cur++] = 't';
  buf[cur++] = ' ';

  mkrndstr_ipa(key_size, buf + cur);
  cur += key_size;

  buf[cur++] = ' ';
  buf[cur++] = '0';
  buf[cur++] = ' ';
  buf[cur++] = '0';
  buf[cur++] = ' ';

  cur += std::sprintf(reinterpret_cast<char *>(buf + cur), "%d", value_size);

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  mkrndstr_ipa(value_size, buf + cur);
  cur += value_size;

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  if (cur > put_max_buffer_size(key_size, value_size)) {
    throw std::runtime_error("Buffer overflow");
  }

  return cur;
}

inline size_t get_max_buffer_size(int const key_size) { return 6 + key_size; }

inline size_t get_buffer_size(int const key_size) {
  return get_max_buffer_size(key_size);
}

static size_t get(void *buffer, int const key_size) {
  auto *buf = reinterpret_cast<uint8_t *>(buffer);
  size_t cur = 0;
  buf[cur++] = 'g';
  buf[cur++] = 'e';
  buf[cur++] = 't';
  buf[cur++] = ' ';

  mkrndstr_ipa(key_size, buf + cur);
  cur += key_size;

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  if (cur > get_max_buffer_size(key_size)) {
    throw std::runtime_error("Buffer overflow");
  }

  return cur;
}
}  // namespace memcached

namespace redis {
inline size_t put_max_buffer_size(int const key_size, int const value_size) {
  return 23 + 10 + key_size + 10 + value_size;
}

inline size_t put_buffer_size(int const key_size, int const value_size) {
  return 23 + integer_repr_length(key_size) + key_size +
         integer_repr_length(value_size) + value_size;
}

static size_t put(void *buffer, int const key_size, int const value_size) {
  auto *buf = reinterpret_cast<uint8_t *>(buffer);
  size_t cur = 0;
  buf[cur++] = '*';
  buf[cur++] = '3';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = '$';
  buf[cur++] = '3';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = 'S';
  buf[cur++] = 'E';
  buf[cur++] = 'T';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = '$';

  cur += std::sprintf(reinterpret_cast<char *>(buf + cur), "%d", key_size);

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  mkrndstr_ipa(key_size, buf + cur);
  cur += key_size;

  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = '$';

  cur += std::sprintf(reinterpret_cast<char *>(buf + cur), "%d", value_size);

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  mkrndstr_ipa(value_size, buf + cur);
  cur += value_size;

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  if (cur > put_max_buffer_size(key_size, value_size)) {
    throw std::runtime_error("Buffer overflow");
  }

  return cur;
}

inline size_t get_max_buffer_size(int const key_size) {
  return 18 + 10 + key_size;
}

inline size_t get_buffer_size(int const key_size) {
  return 18 + integer_repr_length(key_size) + key_size;
}

static size_t get(void *buffer, int const key_size) {
  auto *buf = reinterpret_cast<uint8_t *>(buffer);
  size_t cur = 0;
  buf[cur++] = '*';
  buf[cur++] = '2';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = '$';
  buf[cur++] = '3';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = 'G';
  buf[cur++] = 'E';
  buf[cur++] = 'T';
  buf[cur++] = '\r';
  buf[cur++] = '\n';
  buf[cur++] = '$';

  cur += std::sprintf(reinterpret_cast<char *>(buf + cur), "%d", key_size);

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  mkrndstr_ipa(key_size, buf + cur);
  cur += key_size;

  buf[cur++] = '\r';
  buf[cur++] = '\n';

  if (cur > get_max_buffer_size(key_size)) {
    throw std::runtime_error("Buffer overflow");
  }

  return cur;
}
}  // namespace redis
}  // namespace dory::ubft::kvstores
