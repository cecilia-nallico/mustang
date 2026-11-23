// Compliance suite: the official gRPC Java client against the walleye
// (PhotonLibOS) gRPC server. Textbook usage: messages from protoc's
// --java_out and stubs from the OFFICIAL protoc-gen-grpc-java plugin
// (TestServiceGrpc); the wire traffic is produced entirely by grpc-java
// (netty HTTP/2, HPACK, flow control, deadlines, cancel).
//
// Usage: InteropClient <host> <port>. Exits 0 iff every check passes.

package walleye.interop;

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
import io.grpc.stub.StreamObserver;
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

  private final ManagedChannel channel;
  private final TestServiceGrpc.TestServiceBlockingStub blocking;
  private final TestServiceGrpc.TestServiceStub async;
  private int failures = 0;

  private InteropClient(ManagedChannel channel) {
    this.channel = channel;
    this.blocking = TestServiceGrpc.newBlockingStub(channel);
    this.async = TestServiceGrpc.newStub(channel);
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

  // --- scenarios, all through generated messages + generated stub ---

  private void unaryEcho() {
    SimpleResponse reply =
        blocking.unary(SimpleRequest.newBuilder().setText("hello java").setN(42).build());
    requireEq("echo:hello java", reply.getText(), "unary text");
    requireEq(42, reply.getN(), "unary n");
  }

  private void unaryLargePayload() {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < 1024 * 1024; i++) {
      sb.append('a' + (i % 26));
    }
    String big = sb.toString();
    SimpleResponse reply =
        blocking.unary(SimpleRequest.newBuilder().setText(big).setN(7).build());
    requireEq("echo:" + big, reply.getText(), "large payload echo");
    requireEq(7, reply.getN(), "large payload n");
  }

  private void unaryErrorStatus() {
    try {
      blocking.fail(SimpleRequest.newBuilder().setText("x").build());
      throw new AssertionError("Fail call should have errored");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.NOT_FOUND, e.getStatus().getCode(), "error code");
      requireEq("café 100% missing ☕", e.getStatus().getDescription(), "percent-encoded message");
    }
  }

  private void unaryDeadline() {
    long start = System.nanoTime();
    try {
      blocking.withDeadlineAfter(50, TimeUnit.MILLISECONDS)
          .slow(SimpleRequest.newBuilder().setText("x").build());
      throw new AssertionError("Slow call should have timed out");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.DEADLINE_EXCEEDED, e.getStatus().getCode(), "deadline code");
      long elapsedMs = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - start);
      require(elapsedMs < 250, "deadline enforced near the deadline, took " + elapsedMs + "ms");
    }
  }

  private void serverStreaming() {
    Iterator<SimpleResponse> it =
        blocking.serverStream(StreamRequest.newBuilder().setCount(5).setSize(32).setText("m").build());
    for (int i = 0; i < 5; i++) {
      require(it.hasNext(), "server stream message " + i);
      SimpleResponse r = it.next();
      require(r.getText().startsWith("m" + i + ":"), "server stream order at " + i + ": " + r.getText());
    }
    require(!it.hasNext(), "server stream end");
  }

  private void serverStreamLarge() {
    // 32 x 128 KiB = 4 MiB total: exercises server->client flow control.
    Iterator<SimpleResponse> it =
        blocking.serverStream(
            StreamRequest.newBuilder().setCount(32).setSize(128 * 1024).setText("big").build());
    long bytes = 0;
    int count = 0;
    while (it.hasNext()) {
      SimpleResponse r = it.next();
      bytes += r.getSerializedSize();
      count++;
    }
    requireEq(32, count, "large stream count");
    require(bytes >= 32L * 100 * 1024, "large stream payload, got " + bytes);
  }

  private void clientStreaming() throws Exception {
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<SimpleResponse> response = new AtomicReference<>();
    AtomicReference<Throwable> error = new AtomicReference<>();
    StreamObserver<SimpleRequest> requests =
        async.clientStream(
            new StreamObserver<SimpleResponse>() {
              @Override
              public void onNext(SimpleResponse value) {
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
      requests.onNext(SimpleRequest.newBuilder().setText("c" + i).setN(i).build());
    }
    requests.onCompleted();
    require(done.await(10, TimeUnit.SECONDS), "client streaming completed");
    if (error.get() != null) {
      throw new AssertionError("client streaming failed", error.get());
    }
    requireEq("count:10 total:55", response.get().getText(), "client streaming aggregate");
  }

  private void bidiChat() throws Exception {
    CountDownLatch done = new CountDownLatch(1);
    List<SimpleResponse> replies = new ArrayList<>();
    AtomicReference<Throwable> error = new AtomicReference<>();
    StreamObserver<SimpleRequest> requests =
        async.chat(
            new StreamObserver<SimpleResponse>() {
              @Override
              public void onNext(SimpleResponse value) {
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
      requests.onNext(SimpleRequest.newBuilder().setText("x" + i).build());
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
      requireEq(">x" + i, replies.get(i).getText(), "bidi reply " + i);
    }
  }

  private void metadataRoundtrip() throws Exception {
    // Raw ClientCall over the generated METHOD: blocking calls do not
    // expose trailers.
    ClientCall<MetadataRequest, SimpleResponse> call =
        channel.newCall(TestServiceGrpc.getMetadataMethod(), CallOptions.DEFAULT);
    Metadata headers = new Metadata();
    Metadata.Key<String> custom = Metadata.Key.of("java-meta", Metadata.ASCII_STRING_MARSHALLER);
    headers.put(custom, "from-java");
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<SimpleResponse> message = new AtomicReference<>();
    AtomicReference<Status> status = new AtomicReference<>();
    AtomicReference<Metadata> trailers = new AtomicReference<>();
    call.start(
        new ClientCall.Listener<SimpleResponse>() {
          @Override
          public void onMessage(SimpleResponse value) {
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
    call.sendMessage(MetadataRequest.newBuilder().setKey("java-meta").setValue("from-java").build());
    call.halfClose();
    call.request(1);
    require(done.await(10, TimeUnit.SECONDS), "metadata call completed");
    require(status.get().isOk(), "metadata call status: " + status.get());
    requireEq("meta:from-java", message.get().getText(), "server saw request metadata");
    String reply =
        trailers.get().get(Metadata.Key.of("resp-meta", Metadata.ASCII_STRING_MARSHALLER));
    requireEq("value-42", reply, "response trailer metadata");
  }

  private void unknownMethod() {
    // A method the server does not implement: UNIMPLEMENTED trailers-only.
    MethodDescriptor<SimpleRequest, SimpleResponse> missing =
        MethodDescriptor.<SimpleRequest, SimpleResponse>newBuilder()
            .setType(MethodDescriptor.MethodType.UNARY)
            .setFullMethodName(TestServiceGrpc.getServiceDescriptor().getName() + "/DoesNotExist")
            .setRequestMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(SimpleRequest.getDefaultInstance()))
            .setResponseMarshaller(io.grpc.protobuf.ProtoUtils.marshaller(SimpleResponse.getDefaultInstance()))
            .build();
    try {
      ClientCalls.blockingUnaryCall(
          channel, missing, CallOptions.DEFAULT, SimpleRequest.newBuilder().setText("x").build());
      throw new AssertionError("missing method should be UNIMPLEMENTED");
    } catch (StatusRuntimeException e) {
      requireEq(Status.Code.UNIMPLEMENTED, e.getStatus().getCode(), "unknown method code");
    }
  }

  private void cancelInFlight() throws Exception {
    ClientCall<SimpleRequest, SimpleResponse> call =
        channel.newCall(TestServiceGrpc.getSlowMethod(), CallOptions.DEFAULT);
    CountDownLatch done = new CountDownLatch(1);
    AtomicReference<Status> status = new AtomicReference<>();
    call.start(
        new ClientCall.Listener<SimpleResponse>() {
          @Override
          public void onClose(Status s, Metadata trailers) {
            status.set(s);
            done.countDown();
          }
        },
        new Metadata());
    call.sendMessage(SimpleRequest.newBuilder().setText("never-finish").build());
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
                SimpleResponse reply =
                    blocking.unary(
                        SimpleRequest.newBuilder().setText("t" + id + "i" + i).setN(i).build());
                require(reply.getText().startsWith("echo:t" + id), "concurrent unary");
                Iterator<SimpleResponse> it =
                    blocking.serverStream(
                        StreamRequest.newBuilder().setCount(3).setSize(16).setText("c").build());
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
    check("unaryEcho", this::unaryEcho);
    check("unaryLargePayload", this::unaryLargePayload);
    check("unaryErrorStatus", this::unaryErrorStatus);
    check("unaryDeadline", this::unaryDeadline);
    check("serverStreaming", this::serverStreaming);
    check("serverStreamLarge", this::serverStreamLarge);
    check("clientStreaming", () -> unchecked(this::clientStreaming));
    check("bidiChat", () -> unchecked(this::bidiChat));
    check("metadataRoundtrip", () -> unchecked(this::metadataRoundtrip));
    check("unknownMethod", this::unknownMethod);
    check("cancelInFlight", () -> unchecked(this::cancelInFlight));
    check("concurrentMixed", () -> unchecked(this::concurrentMixed));
    System.out.println(failures == 0 ? "ALL PASS" : failures + " FAILURES");
  }

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
