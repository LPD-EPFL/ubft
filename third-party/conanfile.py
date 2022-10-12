#!/usr/bin/env python3

from conans import ConanFile, CMake


class DoryThirdParty(ConanFile):
    name = "dory-third-party"
    version = "0.0.1"
    license = "MIT"
    # url = "TODO"
    description = "Third party sources"
    topics = ("external sources", "third party")
    settings = {
        "os": None,
        "compiler": {
            "gcc": {"libcxx": "libstdc++11", "cppstd": ["17", "20"], "version": None},
            "clang": {"libcxx": "libstdc++11", "cppstd": ["17", "20"], "version": None},
        },
        "build_type": None,
        "arch": None,
    }
    options = {"shared": [True, False], "fPIC": [True, False], "lto": [True, False]}
    default_options = {"shared": False, "fPIC": True, "lto": True}
    generators = "cmake"
    exports_sources = "src/*"
    python_requires = "dory-compiler-options/0.0.1@dory/stable"

    def configure(self):
        pass

    def requirements(self):
        pass

    def build(self):
        self.python_requires["dory-compiler-options"].module.setup_cmake(
            self.build_folder
        )
        generator = self.python_requires["dory-compiler-options"].module.generator()
        cmake = CMake(self, generator=generator)

        self.python_requires["dory-compiler-options"].module.set_options(
            cmake, strict=False
        )
        lto_decision = self.python_requires[
            "dory-compiler-options"
        ].module.lto_decision(cmake, self.options.lto)
        cmake.definitions["DORY_LTO"] = str(lto_decision).upper()

        cmake.configure(source_folder="src")
        cmake.build()

    def package(self):
        self.copy("*.hpp", dst="include/dory/third-party", src="src")
        self.copy("*.h", dst="include/dory/third-party", src="src")
        self.copy("*.a", dst="lib", src="lib", keep_path=False)
        self.copy("*.so", dst="lib", src="lib", keep_path=False)

    def package_info(self):
        self.cpp_info.libs = [
            "dorythirdpartysetproctitle",
            "dorythirdpartymica",
            "dorythirdpartyblake3",
            "dorythirdpartyliquibook",
        ]
        self.cpp_info.cxxflags = self.python_requires[
            "dory-compiler-options"
        ].module.get_cxx_options_for(
            self.settings.compiler, self.settings.build_type, strict=False
        )


if __name__ == "__main__":
    import os, pathlib, sys

    # Find dory root directory
    root_dir = pathlib.Path(os.path.dirname(os.path.abspath(__file__)))
    while not os.path.isfile(os.path.join(root_dir, ".dory-root")):
        root_dir = root_dir.parent

    sys.path.append(os.path.join(root_dir, "conan", "invoker"))

    import invoker

    invoker.run(buildable=True, tidyable=False)
