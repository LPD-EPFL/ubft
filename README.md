![alt text](logo/ubft-logo.png "uBFT")
### Microsecond-scale BFT using Disaggregated Memory
Check out our [ASPLOS paper]() for more details!

## Build Requirements

- [conan](https://conan.io/) package manager
    ```sh
    pip3 install --user "conan>=1.47.0"
    ```

    make sure to set the default ABI to C++11 with:

    ```sh
    conan profile new default --detect  # Generates default profile detecting GCC and sets old ABI
    conan profile update settings.compiler.libcxx=libstdc++11 default  # Sets libcxx to C++11 ABI
    ```

- cmake v3.9.x
- ninja
- gawk
- libibverbs-dev
- libmemcached-dev
- Other non-essential dependencies are given in the [Dockerfile](https://github.com/LPD-EPFL/ubft/blob/master/Dockerfile).

## Build

Run from within the root:

```sh
./build.py
```

this will create all conan packages and build the executables.

__Note:__ If `gcc` is available, it is used as the default compiler. In a system with `clang` only, then `clang` becomes the default compiler. In any case, you can check the available compilers/compiler versions by calling `./build.py --help`.

__Note:__ If your compiler is newer than gcc-11 or clang-12, you need to add [profiles](https://github.com/LPD-EPFL/ubft/tree/master/conan/profiles) and edit the [supported versions](https://github.com/LPD-EPFL/ubft/blob/master/conan/compilers/link.sh) list.

## Docker

You can manually build the [Dockerfile](https://github.com/LPD-EPFL/ubft/blob/master/Dockerfile) under the root of this repo.

```sh
docker build -t ubft .
```
---


## Usage

Follow the instructions on the [artifacts](https://github.com/LPD-EPFL/ubft-artifacts) repository!

