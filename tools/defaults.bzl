"""Shared build defaults for every walleye C++ target.

Centralizes the styleguide rules so all targets inherit them uniformly:
C++20, all warnings as errors, static libraries, fully static binaries
(the linker feature comes from .bazelrc).
"""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_library", "cc_test")

WALLEYE_COPTS = [
    "-std=c++20",
    "-Wall",
    "-Wextra",
    "-Werror",
]

def walleye_cc_library(name, copts = [], **kwargs):
    cc_library(
        name = name,
        copts = WALLEYE_COPTS + copts,
        linkstatic = True,
        **kwargs
    )

def walleye_cc_binary(name, copts = [], **kwargs):
    cc_binary(
        name = name,
        copts = WALLEYE_COPTS + copts,
        linkstatic = True,
        **kwargs
    )

def walleye_cc_test(name, copts = [], **kwargs):
    cc_test(
        name = name,
        copts = WALLEYE_COPTS + copts,
        linkstatic = True,
        **kwargs
    )
