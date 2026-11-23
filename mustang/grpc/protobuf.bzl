load("@rules_cc//cc:defs.bzl", "cc_library")
load("@rules_java//java:defs.bzl", "java_library")
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

"""mustang_grpc_java_proto: Java messages + OFFICIAL gRPC Java stubs.

Runs protoc once with --java_out (messages) and the official
protoc-gen-grpc-java plugin (downloaded from Maven Central). The .proto
must set java_package and java_multiple_files. genrule outputs must be
declared up front, so expected message and service names are passed
explicitly.
"""

def mustang_grpc_java_proto(name, srcs, java_package, messages, services, holders = [], visibility = None):
    dir = java_package.replace(".", "/")

    # protoc 36 emits <Message>.java + <Message>OrBuilder.java per
    # message, a <Proto>OuterClass holder, and the official plugin's
    # <Service>Grpc.java. The outer holder is requested by name
    # (outer_class) since it has no OrBuilder companion.
    outs = []
    for m in messages:
        outs += [dir + "/" + m + ".java", dir + "/" + m + "OrBuilder.java"]
    outs += [dir + "/" + h + ".java" for h in holders]
    outs += [dir + "/" + s + "Grpc.java" for s in services]

    native.genrule(
        name = name + "_gen",
        srcs = srcs,
        outs = outs,
        tools = [
            "@protobuf//:protoc",
            "@protoc_gen_grpc_java//:protoc-gen-grpc-java",
        ],
        cmd = """
            set -e
            for f in $(SRCS); do
                d=$$(dirname $$f)
                $(location @protobuf//:protoc) \
                    --java_out=$(@D) \
                    --plugin=protoc-gen-grpc-java=$(location @protoc_gen_grpc_java//:protoc-gen-grpc-java) \
                    --grpc-java_out=$(@D) \
                    --proto_path=$$d $$f
            done
        """,
    )

    java_library(
        name = name,
        srcs = [name + "_gen"],
        deps = [
            # The official plugin's *FutureStub references Guava futures.
            "@maven//:com_google_guava_guava",
            "@maven//:com_google_protobuf_protobuf_java",
            "@maven//:io_grpc_grpc_api",
            "@maven//:io_grpc_grpc_protobuf",
            "@maven//:io_grpc_grpc_stub",
        ],
        visibility = visibility,
    )
