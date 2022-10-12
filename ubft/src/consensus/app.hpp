#pragma once

#include <stdexcept>

#include <fmt/core.h>
#include <xxhash.h>

#include <dory/shared/pointer-wrapper.hpp>

namespace dory::ubft::app {

class Application {
 public:
  Application() {
    auto *state = XXH3_createState();
    if (state == nullptr) {
      throw std::runtime_error("Could not create the application state!");
    }

    uniq_state =
        deleted_unique_ptr<XXH3_state_t>(state, [](XXH3_state_t *state) {
          auto ret = XXH3_freeState(state);
          if (ret != 0) {
            throw std::runtime_error(fmt::format(
                "Could not destroy the application state: {}", ret));
          }
        });
    XXH3_64bits_reset(uniq_state.get());
  }

  void execute(uint8_t const *const data, size_t const size) {
    XXH3_64bits_update(uniq_state.get(), data, size);
  }

  void execute(uint64_t const data) {
    execute(reinterpret_cast<uint8_t const *>(&data), sizeof(data));
  }

  XXH64_hash_t hash() const { return XXH3_64bits_digest(uniq_state.get()); }

 private:
  deleted_unique_ptr<XXH3_state_t> uniq_state;
};

static XXH64_hash_t sequential_reference(uint64_t start, uint64_t end) {
  Application app;

  for (uint64_t i = start; i < end; i++) {
    app.execute(i);
  }

  return app.hash();
}

}  // namespace dory::ubft::app
