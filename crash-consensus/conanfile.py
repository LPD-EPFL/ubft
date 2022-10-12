#!/usr/bin/env python3

from conans import ConanFile, CMake


class DoryCrashConsensusConan(ConanFile):
    name = "dory-crash-consensus"
    version = "0.0.1"
    license = "MIT"
    # url = "TODO"
    description = "Prebuilt crash consensus"
    settings = {
        "os": None,
        "compiler": {
            "gcc": {"libcxx": "libstdc++11", "cppstd": ["17", "20"], "version": None},
            "clang": {"libcxx": "libstdc++11", "cppstd": ["17", "20"], "version": None},
        },
        "build_type": None,
        "arch": None,
    }

    generators = "cmake"
    exports_sources = "src/*"
    python_requires = "dory-compiler-options/0.0.1@dory/stable"

    #def export_sources(self):
    #    self.output.info("Pre-building the library")
    #    self.run("yes | ./build_library.sh")

    def package(self):
        self.copy("*", dst="include", src="src/include")
        if self.settings.build_type == "Debug":
            self.copy("*", dst="lib", src="src/lib/debug")
        if self.settings.build_type == "Release":
            self.copy("*", dst="lib", src="src/lib/release")
        if self.settings.build_type == "RelWithDebInfo":
            self.copy("*", dst="lib", src="src/lib/relwithdebinfo")
        if self.settings.build_type == "MinSizeRel":
            self.copy("*", dst="lib", src="src/lib/minsizerel")

    def package_info(self):
        self.cpp_info.libs = ["crashconsensus"]


if __name__ == "__main__":
    import os, pathlib, sys

    # Find dory root directory
    root_dir = pathlib.Path(os.path.dirname(os.path.abspath(__file__)))
    while not os.path.isfile(os.path.join(root_dir, ".dory-root")):
        root_dir = root_dir.parent

    sys.path.append(os.path.join(root_dir, "conan", "invoker"))

    import invoker

    invoker.run(doNothing=False, buildable=False, tidyable=False, exportable=True)
