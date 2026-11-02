// gRPC key-value server for the Phase 2 mutex baseline.
//
// Usage: kv_server [--port=50051] [--store=mutex|shared] [--workers=N] [--keys=K]
//   --store    MutexStore or SharedMutexStore behind the service
//   --workers  N > 0: handlers hand the work to our ThreadPool of N threads and reply from there
//              N = 0: handlers run the store call directly on gRPC's own threads (no pool hop)
//   --keys     prefill keys 0..K-1 (same key format as the client) so Gets hit
//
// Uses gRPC's callback API: a handler returns a reactor immediately and completes it later with
// Finish(), possibly from another thread. That is what lets the ThreadPool run the work.
// Prints "READY" once listening. Stops cleanly on SIGINT or SIGTERM.
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "bench_util.h"
#include "kv.grpc.pb.h"
#include "lldstore/locked_store.h"
#include "lldstore/thread_pool.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void OnSignal(int) { g_stop = 1; }  // only sets a flag: nothing else is async-signal-safe

template <typename StoreT>
class KVServiceImpl final : public lldstore::v1::KV::CallbackService {
 public:
  KVServiceImpl(StoreT* store, lldstore::ThreadPool* pool) : store_(store), pool_(pool) {}

  grpc::ServerUnaryReactor* Get(grpc::CallbackServerContext* context,
                                const lldstore::v1::GetRequest* request,
                                lldstore::v1::GetResponse* response) override {
    auto* reactor = context->DefaultReactor();
    Run([this, request, response, reactor] {
      std::string value;
      if (store_->Get(request->key(), &value)) {
        response->set_found(true);
        response->set_value(std::move(value));
      } else {
        response->set_found(false);
      }
      reactor->Finish(grpc::Status::OK);
    });
    return reactor;
  }

  grpc::ServerUnaryReactor* Put(grpc::CallbackServerContext* context,
                                const lldstore::v1::PutRequest* request,
                                lldstore::v1::PutResponse*) override {
    auto* reactor = context->DefaultReactor();
    Run([this, request, reactor] {
      store_->Put(request->key(), request->value());
      reactor->Finish(grpc::Status::OK);
    });
    return reactor;
  }

  grpc::ServerUnaryReactor* Delete(grpc::CallbackServerContext* context,
                                   const lldstore::v1::DeleteRequest* request,
                                   lldstore::v1::DeleteResponse* response) override {
    auto* reactor = context->DefaultReactor();
    Run([this, request, response, reactor] {
      response->set_existed(store_->Delete(request->key()));
      reactor->Finish(grpc::Status::OK);
    });
    return reactor;
  }

 private:
  // The request/response objects stay valid until Finish() is called, so capturing raw pointers
  // in a task that outlives this handler call is safe.
  template <typename F>
  void Run(F&& work) {
    if (pool_ != nullptr) {
      pool_->Submit(std::forward<F>(work));
    } else {
      work();
    }
  }

  StoreT* store_;
  lldstore::ThreadPool* pool_;  // null means run inline on the gRPC thread
};

struct Options {
  int port = 50051;
  std::string store = "mutex";
  int workers = 8;
  uint64_t keys = 1'000'000;
};

bool ParseFlag(const char* arg, const char* name, std::string* value) {
  const std::string prefix = std::string("--") + name + "=";
  if (std::strncmp(arg, prefix.c_str(), prefix.size()) != 0) return false;
  *value = arg + prefix.size();
  return true;
}

template <typename StoreT>
int Serve(const Options& o) {
  using namespace lldstore;
  StoreT store;
  for (uint64_t i = 0; i < o.keys; ++i) {
    char buf[bench::kKeySize];
    store.Put(bench::FormatKey(i, buf), bench::Value());
  }

  std::unique_ptr<ThreadPool> pool;
  if (o.workers > 0) pool = std::make_unique<ThreadPool>(static_cast<size_t>(o.workers));
  KVServiceImpl<StoreT> service(&store, pool.get());

  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:" + std::to_string(o.port), grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    std::fprintf(stderr, "failed to start server on port %d\n", o.port);
    return 1;
  }
  std::printf("READY store=%s workers=%d keys=%llu port=%d\n", o.store.c_str(), o.workers,
              static_cast<unsigned long long>(o.keys), o.port);
  std::fflush(stdout);

  while (g_stop == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    if (ParseFlag(argv[i], "port", &v)) o.port = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "store", &v)) o.store = v;
    else if (ParseFlag(argv[i], "workers", &v)) o.workers = std::atoi(v.c_str());
    else if (ParseFlag(argv[i], "keys", &v)) o.keys = std::strtoull(v.c_str(), nullptr, 10);
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  if (o.store == "mutex") return Serve<lldstore::MutexStore>(o);
  if (o.store == "shared") return Serve<lldstore::SharedMutexStore>(o);
  std::fprintf(stderr, "unknown --store=%s\n", o.store.c_str());
  return 2;
}
