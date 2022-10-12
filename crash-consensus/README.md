# How to use
This is a conan package for a precompiled library.
Place `libcrashconsensus.so` in the relevant directories and run:
```sh
# Needed only for dory-exports
../build.py extern

# No need to run all of them if you are not interested
# in a particular compilation variant
./conanfile.py -b debug --export-only
./conanfile.py -b release --export-only
./conanfile.py -b relwithdebinfo --export-only
./conanfile.py -b minsizerel --export-only
```
