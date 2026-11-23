"""Official protoc-gen-grpc-java plugin binary, downloaded from Maven
Central as a prebuilt executable (the BCR grpc-java source module drags
in conflicting maven universes, so we take the released binary instead).
"""

def _impl(repository_ctx):
    repository_ctx.download(
        url = repository_ctx.attr.urls,
        sha256 = repository_ctx.attr.sha256,
        output = "protoc-gen-grpc-java",
        executable = True,
    )
    repository_ctx.file(
        "BUILD.bazel",
        content = 'exports_files(["protoc-gen-grpc-java"])\n',
    )

grpc_java_plugin = repository_rule(
    implementation = _impl,
    attrs = {
        "urls": attr.string_list(mandatory = True),
        "sha256": attr.string(mandatory = True),
    },
)
