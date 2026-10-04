// SPDX-License-Identifier: LGPL-3.0-or-later
#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "pxsteamdl/pxsteamdl.hpp"

#include <exception>
#include <optional>
#include <string>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

using ProgressCallback = nb::typed<nb::callable, void(const pxsteamdl::Progress&)>;
using ResolvedCallback = nb::typed<nb::callable, void(const pxsteamdl::ItemInfo&)>;

std::vector<pxsteamdl::Result> Download(pxsteamdl::Client& client, const std::vector<std::uint64_t>& ids,
                                        const std::filesystem::path& root, int parallel_items, int threads_per_item,
                                        std::optional<ProgressCallback> on_progress,
                                        std::optional<ResolvedCallback> on_resolved, const std::stop_source* cancel) {
  if (parallel_items < 1) throw nb::value_error("parallel_items must be >= 1");
  if (threads_per_item < 1) throw nb::value_error("threads_per_item must be >= 1");

  pxsteamdl::Options options;
  options.parallel_items = static_cast<unsigned>(parallel_items);
  options.threads_per_item = static_cast<unsigned>(threads_per_item);
  if (cancel) options.stop = cancel->get_token();
  if (on_progress) {
    // The caller's argument keeps the callable alive for the whole (synchronous) download,
    // so a borrowed handle avoids touching its refcount from worker threads without the GIL.
    options.on_progress = [callback = nb::handle(*on_progress)](const pxsteamdl::Progress& progress) {
      nb::gil_scoped_acquire gil;
      try {
        callback(progress);
      } catch (nb::python_error& e) {
        // Like threading.excepthook: report and keep the worker going.
        e.discard_as_unraisable(callback);
      }
    };
  }
  if (on_resolved) {
    options.on_resolved = [callback = nb::handle(*on_resolved)](const pxsteamdl::ItemInfo& info) {
      nb::gil_scoped_acquire gil;
      try {
        callback(info);
      } catch (nb::python_error& e) {
        e.discard_as_unraisable(callback);
      }
    };
  }

  nb::gil_scoped_release release;
  return client.download(ids, root, options);
}

// pxsteamdl.Error, defined in __init__.py (so that it has a typed stub) and registered through _set_error_type.
// pxsteamdl::Error is raised as an instance of it with the kind as an attribute.
PyObject* g_error_type = nullptr;

void TranslateError(const std::exception_ptr& pointer, void* /*payload*/) {
  try {
    std::rethrow_exception(pointer);
  } catch (const pxsteamdl::Error& e) {
    PyObject* type = g_error_type ? g_error_type : PyExc_RuntimeError;
    nb::object exception = nb::borrow(type)(e.what());
    exception.attr("kind") = nb::cast(e.kind());
    PyErr_SetObject(type, exception.ptr());
  }
}

}  // namespace

NB_MODULE(_pxsteamdl, m) {
  m.doc() = "Anonymous Steam Workshop downloader (RimWorld).";
  m.attr("__version__") = PXSTEAMDL_VERSION_STRING;

  nb::enum_<pxsteamdl::ErrorKind>(m, "ErrorKind", "Why an item or an operation failed; see Result.error_kind.")
      .value("NONE", pxsteamdl::ErrorKind::kNone, "No error.")
      .value("CANCELLED", pxsteamdl::ErrorKind::kCancelled, "Stopped on request; the message is \"cancelled\".")
      .value("NOT_FOUND", pxsteamdl::ErrorKind::kNotFound, "Steam does not have the item.")
      .value("REJECTED", pxsteamdl::ErrorKind::kRejected,
             "Steam or a CDN refused (private item, access denied, HTTP 4xx).")
      .value("NETWORK", pxsteamdl::ErrorKind::kNetwork,
             "Network or service failure that survived the automatic retries; trying again later may work.")
      .value("DATA", pxsteamdl::ErrorKind::kData, "Steam sent data that cannot be used.")
      .value("FILESYSTEM", pxsteamdl::ErrorKind::kFilesystem, "A local file or directory could not be written.")
      .value("OTHER", pxsteamdl::ErrorKind::kOther, "Anything else.");

  m.def("_set_error_type", [](nb::handle type) {
    Py_XINCREF(type.ptr());
    g_error_type = type.ptr();
  });
  nb::register_exception_translator(TranslateError);

  nb::class_<pxsteamdl::Progress>(m, "Progress", "Download progress of one item.")
      .def(
          "__init__",
          [](pxsteamdl::Progress* self, std::uint64_t item_id, std::string title, std::uint64_t downloaded_bytes,
             std::uint64_t downloaded_total, std::uint64_t unpacked_bytes, std::uint64_t unpacked_total) {
            new (self) pxsteamdl::Progress{item_id,        downloaded_bytes, downloaded_total,
                                           unpacked_bytes, unpacked_total,   std::move(title)};
          },
          nb::kw_only(), "item_id"_a = 0, "title"_a = "", "downloaded_bytes"_a = 0, "downloaded_total"_a = 0,
          "unpacked_bytes"_a = 0, "unpacked_total"_a = 0)
      .def_ro("item_id", &pxsteamdl::Progress::item_id)
      .def_ro("title", &pxsteamdl::Progress::title)
      .def_ro("downloaded_bytes", &pxsteamdl::Progress::downloaded_bytes,
              "Bytes received over the network so far (the chunks as the CDN serves them: encrypted, compressed).")
      .def_ro("downloaded_total", &pxsteamdl::Progress::downloaded_total, "How many bytes will be received in all.")
      .def_ro("unpacked_bytes", &pxsteamdl::Progress::unpacked_bytes,
              "Bytes decrypted, decompressed and written to disk so far.")
      .def_ro("unpacked_total", &pxsteamdl::Progress::unpacked_total, "How many bytes will be written in all.")
      .def("__repr__", [](const pxsteamdl::Progress& p) {
        return nb::str(
                   "Progress(item_id={}, title={!r}, downloaded_bytes={}, downloaded_total={}, unpacked_bytes={}, "
                   "unpacked_total={})")
            .format(p.item_id, p.title, p.downloaded_bytes, p.downloaded_total, p.unpacked_bytes, p.unpacked_total);
      });

  nb::class_<pxsteamdl::ItemInfo>(m, "ItemInfo",
                                  "What Steam says about an item, known before the item's bytes are downloaded.")
      .def(
          "__init__",
          [](pxsteamdl::ItemInfo* self, std::uint64_t item_id, std::string title, std::string error,
             pxsteamdl::ErrorKind error_kind) {
            new (self) pxsteamdl::ItemInfo{item_id, std::move(title), std::move(error), error_kind};
          },
          nb::kw_only(), "item_id"_a = 0, "title"_a = "", "error"_a = "", "error_kind"_a = pxsteamdl::ErrorKind::kNone)
      .def_ro("item_id", &pxsteamdl::ItemInfo::item_id)
      .def_ro("title", &pxsteamdl::ItemInfo::title)
      .def_ro("error", &pxsteamdl::ItemInfo::error,
              "Empty if Steam accepted the item; otherwise why it was rejected (not returned, non-success result\n"
              "code, unusable details, failed details request) or \"cancelled\". Download failures are reported\n"
              "later, in Result.error.")
      .def_ro("error_kind", &pxsteamdl::ItemInfo::error_kind, "ErrorKind.NONE if error is empty.")
      .def_prop_ro(
          "ok", [](const pxsteamdl::ItemInfo& i) { return i.ok(); }, "True if Steam accepted the item.")
      .def_prop_ro(
          "cancelled", [](const pxsteamdl::ItemInfo& i) { return i.cancelled(); },
          "True if the download was stopped before the item was looked up.")
      .def("__repr__", [](const pxsteamdl::ItemInfo& i) {
        return nb::str("ItemInfo(item_id={}, title={!r}, error={!r}, error_kind={})")
            .format(i.item_id, i.title, i.error, nb::cast(i.error_kind));
      });

  nb::class_<pxsteamdl::Result>(m, "Result", "Outcome of downloading one item.")
      .def(
          "__init__",
          [](pxsteamdl::Result* self, std::uint64_t item_id, std::string title, std::filesystem::path path,
             std::string error, pxsteamdl::ErrorKind error_kind, std::uint64_t downloaded_bytes,
             std::uint64_t unpacked_bytes) {
            new (self) pxsteamdl::Result{item_id,    std::move(title), std::move(path), std::move(error),
                                         error_kind, downloaded_bytes, unpacked_bytes};
          },
          nb::kw_only(), "item_id"_a = 0, "title"_a = "", "path"_a = std::filesystem::path(), "error"_a = "",
          "error_kind"_a = pxsteamdl::ErrorKind::kNone, "downloaded_bytes"_a = 0, "unpacked_bytes"_a = 0)
      .def_ro("item_id", &pxsteamdl::Result::item_id)
      .def_ro("title", &pxsteamdl::Result::title)
      .def_ro("path", &pxsteamdl::Result::path,
              "The item's directory, root/<item id>; set for failed items too (it then holds the previous copy).")
      .def_ro("error", &pxsteamdl::Result::error,
              "Empty on success. The wording may change; match on error_kind. \"cancelled\" for a stopped item.")
      .def_ro("error_kind", &pxsteamdl::Result::error_kind, "ErrorKind.NONE on success.")
      .def_ro("downloaded_bytes", &pxsteamdl::Result::downloaded_bytes,
              "Bytes this run received over the network for the item (encrypted, compressed); 0 if up to date.")
      .def_ro("unpacked_bytes", &pxsteamdl::Result::unpacked_bytes,
              "Bytes this run decrypted, decompressed and wrote for the item; 0 if up to date.")
      .def_prop_ro(
          "ok", [](const pxsteamdl::Result& r) { return r.ok(); }, "True if the item downloaded.")
      .def_prop_ro(
          "cancelled", [](const pxsteamdl::Result& r) { return r.cancelled(); },
          "True if the item was stopped by a cancel request.")
      .def("__repr__", [](nb::handle self) {
        return nb::str(
                   "Result(item_id={!r}, title={!r}, path={!r}, error={!r}, error_kind={!r}, "
                   "downloaded_bytes={!r}, unpacked_bytes={!r})")
            .format(self.attr("item_id"), self.attr("title"), self.attr("path"), self.attr("error"),
                    self.attr("error_kind"), self.attr("downloaded_bytes"), self.attr("unpacked_bytes"));
      });

  nb::class_<std::stop_source>(m, "CancelToken", "Cancels the Client.download calls it is passed to.")
      .def(nb::init<>())
      .def(
          "cancel", [](std::stop_source& source) { source.request_stop(); },
          "Requests stop; safe to call from any thread, repeated calls are no-ops.");

  nb::class_<pxsteamdl::Client>(m, "Client",
                                "Anonymous Steam session. Thread-safe: one client may serve several threads.")
      .def(
          "__init__",
          [](pxsteamdl::Client* self, const std::optional<std::string>& proxy, int connect_timeout, int stall_timeout) {
            pxsteamdl::ClientOptions options;
            options.proxy = proxy;
            options.connect_timeout = std::chrono::seconds(connect_timeout);
            options.stall_timeout = std::chrono::seconds(stall_timeout);
            nb::gil_scoped_release release;
            new (self) pxsteamdl::Client(options);
          },
          nb::kw_only(), "proxy"_a.none() = nb::none(), "connect_timeout"_a = 10, "stall_timeout"_a = 30,
          "Logs in to Steam anonymously; raises pxsteamdl.Error (a RuntimeError) on failure.\n\n"
          "proxy: a proxy URL such as \"http://host:3128\" or \"socks5h://host:1080\" for every connection; None\n"
          "uses what libcurl finds in the environment (https_proxy, all_proxy), \"\" uses no proxy.\n"
          "connect_timeout: seconds to wait for a connection (at least 1).\n"
          "stall_timeout: seconds a transfer may stay below 1 byte/s before it is given up and retried (at least 1).")
      .def("download", &Download, "ids"_a, "root"_a, nb::kw_only(), "parallel_items"_a = 2, "threads_per_item"_a = 4,
           "on_progress"_a = nb::none(), "on_resolved"_a = nb::none(), "cancel"_a.none() = nb::none(),
           "Downloads each item into root/<item id>/, updating existing copies incrementally.\n\n"
           "Returns one Result per entry of ids, in order. A repeated ID is downloaded once, on_resolved is called\n"
           "once for it, and every occurrence gets the same Result.\n"
           "Per-item failures are reported in Result.error and Result.error_kind. on_resolved(info) is called once\n"
           "per item on the calling thread, in order, as soon as its batch of up to 100 items is answered by Steam\n"
           "and before the item's bytes are downloaded; earlier items already download meanwhile. ItemInfo is\n"
           "Steam's verdict only, not a download outcome.\n"
           "on_progress is called from worker threads; exceptions either callback raises are reported as\n"
           "unraisable and do not stop the download.\n"
           "After cancel.cancel(), in-flight chunk requests finish, remaining work is skipped and\n"
           "unfinished items report error == \"cancelled\" (error_kind CANCELLED); completed items stay ok.\n"
           "The GIL is released while downloading.");
}
