# Goal

We are building a cache and build farm for bazel and related tools (pants, buck2).

## Components

The cache: an implementation of a smart cache thread-per core model using PhotonLibOs. We write
actual in SeaweedFS a performant distributed filesystem.

The action server: a greenfield implementation of an action server in C++ and thread per core
PhotonLibOS. We can take inspiration from bazel-remote except that we are written in C++.

## Plan

We should implement a full, compliant gRPC client and server implementation on top of PhotonLibOs
HTTP2 support.

Then we will implement the cache.
