# Rpc

## Requirements

- [conan](https://conan.io/) package manager

## Install

Run from within this folder:

```sh
conan create .
```

Which will create this package in the local conan cache.

## Usage

Inside a `conanfile.txt` specify:

```toml
[requires]
dory-rpc/0.0.1
```

and use in source files as follows:

```cpp
#include <dory/rpc/server.hpp>
#include <dory/rpc/abstract-handler.hpp>
```
