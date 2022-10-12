#pragma once

#include <stdexcept>

namespace dory::ubft {

/**
 * @brief State machine to ensure proper building of abstractions that connect
 *        infiniband queues.
 *
 * @tparam T the target to build.
 */
template <typename T>
class Builder {
  enum class Step { Init, Announce, Connect, Build };

 public:
  virtual void announceQps() = 0;
  virtual void connectQps() = 0;
  virtual T build() = 0;
  virtual ~Builder() = default;

 protected:
  void announcing() {
    if (step != Step::Init) {
      throw std::runtime_error("Can only announce once.");
    }
    step = Step::Announce;
  }

  void connecting() {
    if (step != Step::Announce) {
      throw std::runtime_error("Can only connect once after having announced.");
    }
    step = Step::Connect;
  }

  void building() {
    if (step != Step::Connect) {
      throw std::runtime_error("Can only build once after having connected.");
    }
    step = Step::Build;
  }

 private:
  Step step = Step::Init;
};

}  // namespace dory::ubft
