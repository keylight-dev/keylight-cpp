from conan import ConanFile
from conan.tools.files import copy


class KeylightConan(ConanFile):
    name = "keylight"
    version = "0.1.0"
    description = "Cryptographic licensing SDK for Apple platforms and beyond"
    homepage = "https://github.com/keylight-dev/keylight-cpp"
    url = "https://github.com/keylight-dev/keylight-cpp"
    license = "MIT"

    package_type = "header-library"

    # No exports/sources needed for header-only at this stage

    options = {
        "httplib_transport": [True, False],
    }

    default_options = {
        "httplib_transport": False,
    }

    def requirements(self):
        if self.options.httplib_transport:
            self.requires("openssl/[>=1.1.1]")
            self.requires("cpp-httplib/[>=0.14.0]")

    def package_id(self):
        # Header-only: clear settings and options since the package is always the same
        self.info.clear()

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []

        if self.options.httplib_transport:
            self.cpp_info.defines = ["KEYLIGHT_BUILD_HTTPLIB_TRANSPORT"]
