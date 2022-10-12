#pragma once

namespace dory::ubft {

template <typename T>
static std::vector<T> without(std::vector<T> temp, T const &to_remove) {
  temp.erase(std::remove(temp.begin(), temp.end(), to_remove), temp.end());
  return temp;
}

template <typename T>
static std::vector<T> move_back(std::vector<T> temp, T const &to_move) {
  auto new_position = std::remove(temp.begin(), temp.end(), to_move);
  if (new_position != temp.end()) {
    temp.erase(new_position, temp.end());
    temp.emplace_back(to_move);
  }
  return temp;
}

}  // namespace dory::ubft
