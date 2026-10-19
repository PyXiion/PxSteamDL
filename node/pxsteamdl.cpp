// SPDX-License-Identifier: LGPL-3.0-or-later
#define NAPI_VERSION 8
#include <napi.h>

#include "pxsteamdl/pxsteamdl.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

// ErrorKind as it appears in JavaScript.
const char* KindName(pxsteamdl::ErrorKind kind) {
  switch (kind) {
    case pxsteamdl::ErrorKind::kNone:
      return "none";
    case pxsteamdl::ErrorKind::kCancelled:
      return "cancelled";
    case pxsteamdl::ErrorKind::kNotFound:
      return "notFound";
    case pxsteamdl::ErrorKind::kRejected:
      return "rejected";
    case pxsteamdl::ErrorKind::kNetwork:
      return "network";
    case pxsteamdl::ErrorKind::kData:
      return "data";
    case pxsteamdl::ErrorKind::kFilesystem:
      return "filesystem";
    case pxsteamdl::ErrorKind::kOther:
      break;
  }
  return "other";
}

// Item IDs and byte counts stay below 2^53 in practice, so they are plain JavaScript numbers.
Napi::Number Num(Napi::Env env, std::uint64_t value) { return Napi::Number::New(env, static_cast<double>(value)); }

Napi::Object ToJs(Napi::Env env, const pxsteamdl::Progress& p) {
  Napi::Object o = Napi::Object::New(env);
  o.Set("itemId", Num(env, p.item_id));
  o.Set("title", p.title);
  o.Set("downloadedBytes", Num(env, p.downloaded_bytes));
  o.Set("downloadedTotal", Num(env, p.downloaded_total));
  o.Set("unpackedBytes", Num(env, p.unpacked_bytes));
  o.Set("unpackedTotal", Num(env, p.unpacked_total));
  return o;
}

Napi::Object ToJs(Napi::Env env, const pxsteamdl::ItemInfo& i) {
  Napi::Object o = Napi::Object::New(env);
  o.Set("itemId", Num(env, i.item_id));
  o.Set("title", i.title);
  o.Set("error", i.error);
  o.Set("errorKind", KindName(i.error_kind));
  o.Set("ok", i.ok());
  o.Set("cancelled", i.cancelled());
  return o;
}

Napi::Object ToJs(Napi::Env env, const pxsteamdl::Result& r) {
  const std::u8string path = r.path.u8string();
  Napi::Object o = Napi::Object::New(env);
  o.Set("itemId", Num(env, r.item_id));
  o.Set("title", r.title);
  o.Set("path", std::string(path.begin(), path.end()));
  o.Set("error", r.error);
  o.Set("errorKind", KindName(r.error_kind));
  o.Set("downloadedBytes", Num(env, r.downloaded_bytes));
  o.Set("unpackedBytes", Num(env, r.unpacked_bytes));
  o.Set("ok", r.ok());
  o.Set("cancelled", r.cancelled());
  return o;
}

Napi::Error MakeError(Napi::Env env, const std::string& message, pxsteamdl::ErrorKind kind) {
  Napi::Error error = Napi::Error::New(env, message);
  error.Value().Set("kind", KindName(kind));
  return error;
}

// A failure of the whole operation, carried to the JavaScript side.
struct Failure {
  std::string message;
  pxsteamdl::ErrorKind kind = pxsteamdl::ErrorKind::kOther;
};

Failure ToFailure(const std::exception_ptr& pointer) {
  try {
    std::rethrow_exception(pointer);
  } catch (const pxsteamdl::Error& e) {
    return {e.what(), e.kind()};
  } catch (const std::exception& e) {
    return {e.what(), pxsteamdl::ErrorKind::kOther};
  } catch (...) {
    return {"unknown error", pxsteamdl::ErrorKind::kOther};
  }
}

// A message from a worker thread to the JavaScript thread.
using ClientPtr = std::shared_ptr<pxsteamdl::Client>;
using Message =
    std::variant<pxsteamdl::Progress, pxsteamdl::ItemInfo, std::vector<pxsteamdl::Result>, ClientPtr, Failure>;

Napi::Object WrapClient(Napi::Env env, ClientPtr client);

// Everything below is touched on the JavaScript thread only.
struct Context {
  explicit Context(Napi::Env env) : deferred(Napi::Promise::Deferred::New(env)) {}
  Napi::Promise::Deferred deferred;
  Napi::FunctionReference on_progress;
  Napi::FunctionReference on_resolved;
};

void Deliver(Napi::Env env, Napi::Function /*unused*/, Context* context, Message* raw) {
  const std::unique_ptr<Message> message(raw);
  if (env == nullptr) return;  // shutting down
  if (const auto* progress = std::get_if<pxsteamdl::Progress>(message.get())) {
    if (!context->on_progress.IsEmpty()) context->on_progress.Call({ToJs(env, *progress)});
  } else if (const auto* info = std::get_if<pxsteamdl::ItemInfo>(message.get())) {
    if (!context->on_resolved.IsEmpty()) context->on_resolved.Call({ToJs(env, *info)});
  } else if (const auto* results = std::get_if<std::vector<pxsteamdl::Result>>(message.get())) {
    Napi::Array array = Napi::Array::New(env, results->size());
    for (std::size_t i = 0; i < results->size(); ++i)
      array.Set(static_cast<std::uint32_t>(i), ToJs(env, (*results)[i]));
    context->deferred.Resolve(array);
  } else if (auto* client = std::get_if<ClientPtr>(message.get())) {
    context->deferred.Resolve(WrapClient(env, std::move(*client)));
  } else {
    const auto& failure = std::get<Failure>(*message);
    context->deferred.Reject(MakeError(env, failure.message, failure.kind).Value());
  }
}

using Channel = Napi::TypedThreadSafeFunction<Context, Message, Deliver>;

// Queues a message for the JavaScript thread; frees it if the environment is already shutting down.
void Post(const Channel& channel, std::unique_ptr<Message> message) {
  if (channel.NonBlockingCall(message.get()) == napi_ok) message.release();
}

Channel MakeChannel(Napi::Env env, std::unique_ptr<Context> context, const char* name) {
  // The queue is unbounded and the channel is released after the last message, so no message is lost; the
  // pending promise keeps the event loop alive, like any other asynchronous operation.
  return Channel::New(env, name, 0, 1, context.release(), [](Napi::Env, void*, Context* c) { delete c; });
}

class CancelToken : public Napi::ObjectWrap<CancelToken> {
 public:
  static void Init(Napi::Env env, Napi::Object exports) {
    exports.Set("CancelToken", DefineClass(env, "CancelToken", {InstanceMethod("cancel", &CancelToken::Cancel)}));
  }
  explicit CancelToken(const Napi::CallbackInfo& info) : ObjectWrap(info) {}
  std::stop_source source;

 private:
  Napi::Value Cancel(const Napi::CallbackInfo& info) {
    source.request_stop();
    return info.Env().Undefined();
  }
};

class Client : public Napi::ObjectWrap<Client> {
 public:
  static Napi::FunctionReference constructor;

  static void Init(Napi::Env env, Napi::Object exports) {
    Napi::Function function = DefineClass(env, "Client", {InstanceMethod("download", &Client::Download)});
    constructor = Napi::Persistent(function);
    exports.Set("Client", function);
    exports.Set("createClient", Napi::Function::New(env, Create));
  }

  explicit Client(const Napi::CallbackInfo& info) : ObjectWrap(info) {}

  void Set(ClientPtr client) { m_client = std::move(client); }

  // createClient({ proxy, connectTimeout, stallTimeout }): Promise<Client>; the logon runs on its own thread.
  static Napi::Value Create(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    Napi::Object settings = info[0].As<Napi::Object>();
    pxsteamdl::ClientOptions options;
    if (settings.Get("proxy").IsString()) options.proxy = settings.Get("proxy").As<Napi::String>().Utf8Value();
    options.connect_timeout = std::chrono::seconds(settings.Get("connectTimeout").As<Napi::Number>().Int64Value());
    options.stall_timeout = std::chrono::seconds(settings.Get("stallTimeout").As<Napi::Number>().Int64Value());

    auto context = std::make_unique<Context>(env);
    Napi::Promise promise = context->deferred.Promise();
    Channel channel = MakeChannel(env, std::move(context), "pxsteamdl.createClient");
    std::thread([channel, options = std::move(options)]() mutable {
      std::unique_ptr<Message> message;
      try {
        message = std::make_unique<Message>(std::make_shared<pxsteamdl::Client>(options));
      } catch (...) {
        message = std::make_unique<Message>(ToFailure(std::current_exception()));
      }
      Post(channel, std::move(message));
      channel.Release();
    }).detach();
    return promise;
  }

 private:
  // download(ids: bigint[], root, parallelItems, threadsPerItem, onProgress, onResolved, cancelToken):
  // Promise<Result[]>
  Napi::Value Download(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::vector<std::uint64_t> ids;
    Napi::Array id_array = info[0].As<Napi::Array>();
    for (std::uint32_t i = 0; i < id_array.Length(); ++i) {
      bool lossless = false;
      ids.push_back(id_array.Get(i).As<Napi::BigInt>().Uint64Value(&lossless));
    }
    const std::string root_utf8 = info[1].As<Napi::String>().Utf8Value();
    const std::filesystem::path root(std::u8string(root_utf8.begin(), root_utf8.end()));

    pxsteamdl::Options options;
    options.parallel_items = info[2].As<Napi::Number>().Uint32Value();
    options.threads_per_item = info[3].As<Napi::Number>().Uint32Value();
    if (auto* token = info[6].IsObject() ? CancelToken::Unwrap(info[6].As<Napi::Object>()) : nullptr) {
      options.stop = token->source.get_token();
    }

    auto context = std::make_unique<Context>(env);
    if (info[4].IsFunction()) context->on_progress = Napi::Persistent(info[4].As<Napi::Function>());
    if (info[5].IsFunction()) context->on_resolved = Napi::Persistent(info[5].As<Napi::Function>());
    const bool want_progress = !context->on_progress.IsEmpty();
    const bool want_resolved = !context->on_resolved.IsEmpty();
    Napi::Promise promise = context->deferred.Promise();
    Channel channel = MakeChannel(env, std::move(context), "pxsteamdl.download");

    if (want_progress) {
      options.on_progress = [channel](const pxsteamdl::Progress& progress) {
        Post(channel, std::make_unique<Message>(progress));
      };
    }
    if (want_resolved) {
      options.on_resolved = [channel](const pxsteamdl::ItemInfo& item) {
        Post(channel, std::make_unique<Message>(item));
      };
    }

    std::thread([channel, client = m_client, ids = std::move(ids), root, options = std::move(options)]() mutable {
      std::unique_ptr<Message> message;
      try {
        message = std::make_unique<Message>(client->download(ids, root, options));
      } catch (...) {
        message = std::make_unique<Message>(ToFailure(std::current_exception()));
      }
      // Same queue as the callbacks above, so the promise settles after the last of them was called.
      Post(channel, std::move(message));
      channel.Release();
    }).detach();
    return promise;
  }

  ClientPtr m_client;
};

Napi::FunctionReference Client::constructor;

Napi::Object WrapClient(Napi::Env /*env*/, ClientPtr client) {
  Napi::Object object = Client::constructor.New({});
  Client::Unwrap(object)->Set(std::move(client));
  return object;
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  CancelToken::Init(env, exports);
  Client::Init(env, exports);
  exports.Set("version", std::string(pxsteamdl::Version()));
  return exports;
}

}  // namespace

NODE_API_MODULE(pxsteamdl, Init)
