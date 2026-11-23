load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_proto//proto:defs.bzl", "proto_library")

"""mustang_grpc_proto: protoc codegen (C++ messages + mustang/photon stubs).

Runs protoc once per .proto with --cpp_out and our --mustang-grpc_out
plugin, then wraps the generated sources in a cc_library. The .proto
files must be self-contained within their directory (no imports outside
it) for v1: protoc is invoked with --proto_path set to the file's
directory.
"""

def _basename(src):
    name = src[src.rfind("/") + 1:] if "/" in src else src
    return name[:-len(".proto")]

def mustang_grpc_proto(name, srcs, deps = [], visibility = None):
    outs = []
    for src in srcs:
        base = _basename(src)
        outs.extend([
            base + ".pb.h",
            base + ".pb.cc",
            base + ".grpc.pb.h",
            base + ".grpc.pb.cc",
        ])

    native.genrule(
        name = name + "_gen",
        srcs = srcs,
        outs = outs,
        tools = [
            "@protobuf//:protoc",
            "//tools:protoc_gen_mustang_grpc",
        ],
        cmd = """
            set -e
            for f in $(SRCS); do
                dir=$$(dirname $$f)
                $(location @protobuf//:protoc) \
                    --cpp_out=$(@D) \
                    --plugin=protoc-gen-mustang-grpc=$(location //tools:protoc_gen_mustang_grpc) \
                    --mustang-grpc_out=$(@D) \
                    --proto_path=$$dir $$f
            done
        """,
    )

    # proto_library keeps the .proto files visible to tooling and validates
    # them independently of our codegen.
    proto_library(
        name = name + "_protos",
        srcs = srcs,
        deps = deps,
        visibility = visibility,
    )

    cc_library(
        name = name,
        srcs = [name + "_gen"],
        # Protobuf's headers only compile with their release semantics:
        # ABSL_DCHECK internals are ambiguous without NDEBUG.
        copts = ["-DNDEBUG"],
        hdrs = [name + "_gen"],
        # Generated headers include each other by basename.
        includes = ["."],
        deps = [
            "//mustang/grpc:call",
            "//mustang/grpc:server",
            "//mustang/grpc:typed",
            "@abseil-cpp//absl/status",
            "@abseil-cpp//absl/status:statusor",
            "@abseil-cpp//absl/strings",
            "@abseil-cpp//absl/time",
            "@protobuf//:protobuf",
        ],
        visibility = visibility,
    )
