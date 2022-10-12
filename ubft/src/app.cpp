#include <iostream>
#include <limits>

#include <fmt/core.h>

#include "consensus/app.hpp"

int main() {
  dory::ubft::app::Application app;
  fmt::print("Hash before doing anything: {}\n", app.hash());

  app.execute(0);
  fmt::print("Hash after execute(0): {}\n", app.hash());

  app.execute(1);
  fmt::print("Hash after also execute(1): {}\n", app.hash());

  app.execute(2);
  fmt::print("Hash after also execute(2): {}\n", app.hash());

  app.execute(3);
  fmt::print("Hash after also execute(3): {}\n", app.hash());

  fmt::print("\n");

  fmt::print("{:<15} {:<15}\n", "Range [0,.)", "Checksum");
  for (uint64_t i = 1; i < std::numeric_limits<uint32_t>::max(); i = i << 1) {
    fmt::print("{:<15} {:<15}\n", i,
               dory::ubft::app::sequential_reference(0, i));
  }

  return 0;
}
