// Compliance suite: the official gRPC Java client against the mustang
// (PhotonLibOS) gRPC server. Uses MethodDescriptors with a byte-array
// marshaller and hand-rolled proto3 wire encoding, so no Java codegen is
// involved; everything on the wire is produced by the real grpc-java
// stack (netty HTTP/2, HPACK, flow control, deadlines, cancellation).
//
// Usage: InteropClient <host> <port>. Exits 0 iff every check passes.

package mustang.interop;

import com.google.protobuf.CodedInputStream;
import com.google.protobuf.CodedOutputStream;
import io.grpc.CallOptions;
import io.grpc.ClientCall;
import io.grpc.ConnectivityState;
import io.grpc.ManagedChannel;
import io.grpc.ManagedChannelBuilder;
import io.grpc.Metadata;
import io.grpc.MethodDescriptor;
import io.grpc.Status;
import io.grpc.StatusRuntimeException;
import io.grpc.stub.ClientCalls;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Runs the interop checks and prints PASS/FAIL per scenario. */
public final class InteropClient {

  private static final MethodDescriptor.Marshaller<byte[]> BYTES =
      new MethodDescriptor.Marshaller<byte[]>() {
        @Override
        public InputStream stream(byte[] value) {
          return new ByteArrayInputStream(value);
        }

        @Override
        public byte[] parse(InputStream stream) {
          try {
            return stream.readAllBytes();
          } catch (IOException e) {
            throw new StatusRuntimeException(Status.INTERNAL.withDescription("read failed"));
          }
        }
      };

  private static final String SERVICE = "mustang.interop.TestService/";

  private static final MethodDescriptor<byte[], byte[]> UNARY =
      method("Unary", MethodDescriptor.MethodType.UNARY);
  private static final MethodDescriptor<byte[], byte[]> FAIL =
      method("Fail", MethodDescriptor.MethodType.UNARY);
  private static final MethodDescriptor<byte[], byte[]> SLOW =
      method("Slow", MethodDescriptor.MethodType.UNARY);
  private static final MethodDescriptor<byte[], byte[]> SERVER_STREAM =
      method("ServerStream", MethodDescriptor.MethodType.SERVER_STREAMING);
  private static final MethodDescriptor<byte[], byte[]> CLIENT_STREAM =
      method("ClientStream", MethodDescriptor.MethodType.CLIENT_STREAMING);
  private static final MethodDescriptor<byte[], byte[]> CHAT =
      method("Chat", MethodDescriptor.MethodType.BIDI_STREAMING);
  private static final MethodDescriptor<byte[], byte[]> METADATA =
      method("Metadata", MethodDescriptor.MethodType.UNARY);
  private static final MethodDescriptor<byte[], byte[]> MISSING =
      method("DoesNotExist", MethodDescriptor.MethodType.UNARY);

  private static MethodDescriptor<byte[], byte[]> method(String name, MethodDescriptor.MethodType type) {
    return MethodDescriptor.<byte[], byte[]>newBuilder()
        .setType(type)
        .setFullMethodName(SERVICE + name)
        .setRequestMarshaller(BYTES)
        .setResponseMarshaller(BYTES)
        .build();
  }

  // --- hand-rolled proto3 wire format ---

  private static byte[] simple(String text, int n) throws IOException {
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    CodedOutputStream cos = CodedOutputStream.newInstance(bos);
    if (!text.isEmpty()) {
      cos.writeString(1, text);
    }
    if (n != 0) {
      cos.writeInt32(2, n);
    }
    cos.flush();
    return bos.toByteArray();
  }

  private static byte[] streamReq(int count, int size, String text) throws IOException {
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    CodedOutputStream cos = CodedOutputStream.newInstance(bos);
    if (count != 0) {
      cos.writeInt32(1, count);
    }
    if (size != 0) {
      cos.writeInt32(2, size);
    }
    if (!text.isEmpty()) {
      cos.writeString(3, text);
    }
    cos.flush();
    return bos.toByteArray();
  }

  private static byte[] metadataReq(String key, String value) throws IOException {
    ByteArrayOutputStream bos = new ByteArrayOutputStream();
    CodedOutputStream cos = CodedOutputStream.newInstance(bos);
    cos.writeString(1, key);
    cos.writeString(2, value);
    cos.flush();
    return bos.toByteArray();
  }

  private static final class Resp {
    String text = "";
    int n;

    static Resp parse(byte[] bytes) throws IOException {
      Resp r = new Resp();
      CodedInputStream in = CodedInputStream.newInstance(bytes);
      while (!in.isAtEnd()) {
        int tag = in.readTag();
        if (tag == 10) {
          r.text = in.readString();
        } else if (tag == 16) {
          r.n = in.readInt32();
        } else {
          in.skipField(tag);
        }
      }
      return r;
    }
  }

  // --- harness ---

  private final ManagedChannel channel;
  private int failures = 0;

  private InteropClient(ManagedChannel channel) {
    this.channel = channel;
  }

  private void check(String name, Runnable scenario) {
    try {
      scenario.run();
      System.out.println("PASS " + name);
    } catch (Throwable t) {
      failures++;
      System.out.println("FAIL " + name + ": " + t);
    }
  }

  private static void require(boolean condition, String what) {
    if (!condition) {
      throw new AssertionError(what);
    }
  }

  private static void requireEq(Object expected, Object actual, String what) {
    if (!expected.equals(actual)) {
      throw new AssertionError(what + ": expected <" + expected + "> got <" + actual + ">");
    }
  }

  // --- scenarios ---

  private void unaryEcho() throws IOException {
    byte[] reply =
        ClientCalls.blockingUnaryCall(channel, UNARY, CallOptions.DEFAULT, simple("hello java", 42));
    Resp r = Resp.parse(reply);
    requireEq("echo:hello java", r.text, "unary text");
    requireEq(42, r.n, "unary n");
  }

  private void unaryLargePayload() throws IOException {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < 1024 * 1024; i++) {
      sb.append('a' + (i % 26));
    }
    String big = sb.toString();
    byte[] reply = ClientCalls.blockingUnaryCall(channel, UNARY, CallOptions.DEFAULT, simple(big, 7));
    Resp r = Resp.parse(reply);
    requireEq("echo:" + big, r.text, "large payload echo");
    requireEq(7, r.n, "large payload n");
  }

  private void unaryErrorStatus() throws IOException {
    try {
      ClientCalls.blockingUnaryCall(channel, FAIL, CallOptions.DEFAULT, simple("x", 0));
      throw new AssertionError("Fail call should have errored");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.NOT_FOUND, e.getStatus().getCode(), "error code");
      requireEq("café 100% missing ☕", e.getStatus().getDescription(), "percent-encoded message");
    }
  }

  private void unaryDeadline() throws IOException {
    CallOptions deadline =
        CallOptions.DEFAULT.withDeadlineAfter(50, TimeUnit.MILLISECONDS);
    long start = System.nanoTime();
    try {
      ClientCalls.blockingUnaryCall(channel, SLOW, deadline, simple("x", 0));
      throw new AssertionError("Slow call should have timed out");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.DEADLINE_EXCEEDED, e.getStatus().getCode(), "deadline code");
      long elapsedMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
      require(elapsedMs < 250, "deadline enforced near the deadline, took " + elapsedMs + "ms");
    }
  }

  private void serverStreaming() throws IOException {
    Iterator<byte[]> it =
        ClientCalls.blockingServerStreamingCall(
            channel, SERVER_STREAM, CallOptions.DEFAULT, streamReq(5, 32, "m"));
    for (int i = 0; i < 5; i++) {
      require(it.hasNext(), "server stream message " + i);
      Resp r = Resp.parse(it.next());
      require(r.text.startsWith("m" + i + ":"), "server stream order at " + i + ": " + r.text);
    }
    require(!it.hasNext(), "server stream end");
  }

  private void serverStreamLarge() throws IOException {
    // 32 x 128 KiB = 4 MiB total: exercises server->client flow control.
    Iterator<byte[]> it =
        ClientCalls.blockingServerStreamingCall(
            channel, SERVER_STREAM, CallOptions.DEFAULT, streamReq(32, 128 * 1024, "big"));
    long bytes = 0;
    int count = 0;
    while (it.hasNext()) {
      bytes += it.next().length;
      count++;
    }
    requireEq(32, count, "large stream count");
    require(bytes >= 32L * 100 * 1024, "large stream payload, got " + bytes);
  }

  private void clientStreaming() throws Exception {
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<byte[]> response = new AtomicReference<>();
    AtomicReference<Throwable> error = new AtomicReference<>();
    io.grpc.stub.StreamObserver<byte[]> requests =
        ClientCalls.asyncClientStreamingCall(
            channel.newCall(CLIENT_STREAM, CallOptions.DEFAULT),
            new io.grpc.stub.StreamObserver<byte[]>() {
              @Override
              public void onNext(byte[] value) {
                response.set(value);
              }

              @Override
              public void onError(Throwable t) {
                error.set(t);
                done.countDown();
              }

              @Override
              public void onCompleted() {
                done.countDown();
              }
            });
    for (int i = 1; i <= 10; i++) {
      requests.onNext(simple("c" + i, i));
    }
    requests.onCompleted();
    require(done.await(10, TimeUnit.SECONDS), "client streaming completed");
    if (error.get() != null) {
      throw new AssertionError("client streaming failed", error.get());
    }
    Resp r = Resp.parse(response.get());
    requireEq("count:10 total:55", r.text, "client streaming aggregate");
  }

  private void bidiChat() throws Exception {
    CountDownLatch done = new CountDownLatch(1);
    List<byte[]> replies = new ArrayList<>();
    AtomicReference<Throwable> error = new AtomicReference<>();
    io.grpc.stub.StreamObserver<byte[]> requests =
        ClientCalls.asyncBidiStreamingCall(
            channel.newCall(CHAT, CallOptions.DEFAULT),
            new io.grpc.stub.StreamObserver<byte[]>() {
              @Override
              public void onNext(byte[] value) {
                synchronized (replies) {
                  replies.add(value);
                }
              }

              @Override
              public void onError(Throwable t) {
                error.set(t);
                done.countDown();
              }

              @Override
              public void onCompleted() {
                done.countDown();
              }
            });
    for (int i = 0; i < 10; i++) {
      requests.onNext(simple("x" + i, 0));
      // Interleave: the reply for x_i should arrive before we send x_{i+2}.
      if (i >= 2) {
        long deadline = System.currentTimeMillis() + 5000;
        while (System.currentTimeMillis() < deadline) {
          synchronized (replies) {
            if (replies.size() > i - 2) {
              break;
            }
          }
          Thread.sleep(1);
        }
      }
    }
    requests.onCompleted();
    require(done.await(10, TimeUnit.SECONDS), "bidi completed");
    if (error.get() != null) {
      throw new AssertionError("bidi failed", error.get());
    }
    requireEq(10, replies.size(), "bidi reply count");
    for (int i = 0; i < 10; i++) {
      Resp r = Resp.parse(replies.get(i));
      requireEq(">" + "x" + i, r.text, "bidi reply " + i);
    }
  }

  private void metadataRoundtrip() throws Exception {
    // Raw ClientCall: blocking calls do not expose trailers.
    ClientCall<byte[], byte[]> call = channel.newCall(METADATA, CallOptions.DEFAULT);
    Metadata headers = new Metadata();
    Metadata.Key<String> custom = Metadata.Key.of("java-meta", Metadata.ASCII_STRING_MARSHALLER);
    headers.put(custom, "from-java");
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<byte[]> message = new AtomicReference<>();
    AtomicReference<Status> status = new AtomicReference<>();
    AtomicReference<Metadata> trailers = new AtomicReference<>();
    call.start(
        new ClientCall.Listener<byte[]>() {
          @Override
          public void onMessage(byte[] value) {
            message.set(value);
          }

          @Override
          public void onClose(Status s, Metadata t) {
            status.set(s);
            trailers.set(t);
            done.countDown();
          }
        },
        headers);
    call.sendMessage(metadataReq("java-meta", "from-java"));
    call.halfClose();
    call.request(1);
    require(done.await(10, TimeUnit.SECONDS), "metadata call completed");
    require(status.get().isOk(), "metadata call status: " + status.get());
    Resp r = Resp.parse(message.get());
    requireEq("meta:from-java", r.text, "server saw request metadata");
    String reply = trailers.get().get(Metadata.Key.of("resp-meta", Metadata.ASCII_STRING_MARSHALLER));
    requireEq("value-42", reply, "response trailer metadata");
  }

  private void unknownMethod() throws IOException {
    try {
      ClientCalls.blockingUnaryCall(channel, MISSING, CallOptions.DEFAULT, simple("x", 0));
      throw new AssertionError("missing method should be UNIMPLEMENTED");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.UNIMPLEMENTED, e.getStatus().getCode(), "unknown method code");
    }
  }

  private void cancelInFlight() throws Exception {
    ClientCall<byte[], byte[]> call = channel.newCall(SLOW, CallOptions.DEFAULT);
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<Status> status = new AtomicReference<>();
    call.start(
        new ClientCall.Listener<byte[]>() {
          @Override
          public void onClose(Status s, Metadata trailers) {
            status.set(s);
            done.countDown();
          }
        },
        new Metadata());
    call.sendMessage(simple("never-finish", 0));
    call.halfClose();
    call.request(1);
    Thread.sleep(20);  // let the request reach the server
    call.cancel("interop test cancel", null);
    require(done.await(5, TimeUnit.SECONDS), "cancelled call closed");
    require(
        status.get().getCode() == Status.Code.CANCELLED,
        "cancelled code, got " + status.get().getCode());
  }

  private void concurrentMixed() throws Exception {
    ExecutorService pool = Executors.newFixedThreadPool(8);
    AtomicInteger errors = new AtomicInteger();
    CountDownLatch done = new CountDownLatch(8);
    for (int t = 0; t < 8; t++) {
      final int id = t;
      pool.submit(
          () -> {
            try {
              for (int i = 0; i < 5; i++) {
                byte[] reply =
                    ClientCalls.blockingUnaryCall(
                        channel, UNARY, CallOptions.DEFAULT, simple("t" + id + "i" + i, i));
                require(Resp.parse(reply).text.startsWith("echo:t" + id), "concurrent unary");
                Iterator<byte[]> it =
                    ClientCalls.blockingServerStreamingCall(
                        channel, SERVER_STREAM, CallOptions.DEFAULT, streamReq(3, 16, "c"));
                int count = 0;
                while (it.hasNext()) {
                  it.next();
                  count++;
                }
                requireEq(3, count, "concurrent stream");
              }
              done.countDown();
            } catch (Throwable e) {
              errors.incrementAndGet();
              done.countDown();
            }
          });
    }
    require(done.await(60, TimeUnit.SECONDS), "concurrent workload finished");
    pool.shutdown();
    requireEq(0, errors.get(), "concurrent errors");
  }

  // --- plumbing ---

  private static void awaitReady(ManagedChannel channel) throws InterruptedException {
    long deadline = System.currentTimeMillis() + 10_000;
    while (System.currentTimeMillis() < deadline) {
      ConnectivityState state = channel.getState(true);  // true = trigger connect
      if (state == ConnectivityState.READY) {
        return;
      }
      CountDownLatch changed = new CountDownLatch(1);
      channel.notifyWhenStateChanged(state, changed::countDown);
      changed.await(1, TimeUnit.SECONDS);
    }
    throw new IllegalStateException("channel did not become ready");
  }

  private void runAll() {
    check("unaryEcho", unchecked(this::unaryEcho));
    check("unaryLargePayload", unchecked(this::unaryLargePayload));
    check("unaryErrorStatus", unchecked(this::unaryErrorStatus));
    check("unaryDeadline", unchecked(this::unaryDeadline));
    check("serverStreaming", unchecked(this::serverStreaming));
    check("serverStreamLarge", unchecked(this::serverStreamLarge));
    check("clientStreaming", unchecked(this::clientStreaming));
    check("bidiChat", unchecked(this::bidiChat));
    check("metadataRoundtrip", unchecked(this::metadataRoundtrip));
    check("unknownMethod", unchecked(this::unknownMethod));
    check("cancelInFlight", unchecked(this::cancelInFlight));
    check("concurrentMixed", unchecked(this::concurrentMixed));
    System.out.println(failures == 0 ? "ALL PASS" : failures + " FAILURES");
  }

  /** Runnable adapter for scenarios that throw checked exceptions. */
  private interface Scenario {
    void run() throws Exception;
  }

  private static Runnable unchecked(Scenario scenario) {
    return () -> {
      try {
        scenario.run();
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    };
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      System.err.println("usage: InteropClient <host> <port>");
      System.exit(2);
    }
    ManagedChannel channel =
        ManagedChannelBuilder.forAddress(args[0], Integer.parseInt(args[1]))
            .usePlaintext()
            .build();
    try {
      awaitReady(channel);
      InteropClient client = new InteropClient(channel);
      client.runAll();
      System.exit(client.failures == 0 ? 0 : 1);
    } finally {
      channel.shutdownNow();
      channel.awaitTermination(5, TimeUnit.SECONDS);
    }
  }

}
