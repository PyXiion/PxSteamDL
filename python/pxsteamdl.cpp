// SPDX-License-Identifier: LGPL-3.0-or-later
#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include "pxsteamdl/pxsteamdl.hpp"

#include <optional>

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

}  // namespace

NB_MODULE(_pxsteamdl, m) {
  m.doc() = "Anonymous Steam Workshop downloader (RimWorld).";

  nb::class_<pxsteamdl::Progress>(m, "Progress", "Download progress of one item.")
      .def_ro("item_id", &pxsteamdl::Progress::item_id)
      .def_ro("bytes_done", &pxsteamdl::Progress::bytes_done)
      .def_ro("title", &pxsteamdl::Progress::title)
      .def_ro("bytes_total", &pxsteamdl::Progress::bytes_total)
      .def("__repr__", [](const pxsteamdl::Progress& p) {
        return nb::str("Progress(item_id={}, title={!r}, bytes_done={}, bytes_total={})")
            .format(p.item_id, p.title, p.bytes_done, p.bytes_total);
      });

  nb::class_<pxsteamdl::ItemInfo>(m, "ItemInfo",
                                  "What Steam says about an item, known before the item's bytes are downloaded.")
      .def_ro("item_id", &pxsteamdl::ItemInfo::item_id)
      .def_ro("title", &pxsteamdl::ItemInfo::title)
      .def_ro("error", &pxsteamdl::ItemInfo::error,
              "Empty if Steam accepted the item; otherwise why it was rejected (not returned, non-success result\n"
              "code, unusable details, failed details request) or \"cancelled\". Download failures are reported\n"
              "later, in Result.error.")
      .def("__repr__", [](const pxsteamdl::ItemInfo& i) {
        return nb::str("ItemInfo(item_id={}, title={!r}, error={!r})").format(i.item_id, i.title, i.error);
      });

  nb::class_<pxsteamdl::Result>(m, "Result", "Outcome of downloading one item.")
      .def_ro("item_id", &pxsteamdl::Result::item_id)
      .def_ro("title", &pxsteamdl::Result::title)
      .def_ro("path", &pxsteamdl::Result::path)
      .def_ro("error", &pxsteamdl::Result::error, "Empty on success.")
      .def_prop_ro(
          "ok", [](const pxsteamdl::Result& r) { return r.error.empty(); }, "True if the item downloaded.")
      .def("__repr__", [](nb::handle self) {
        return nb::str("Result(item_id={!r}, title={!r}, path={!r}, error={!r})")
            .format(self.attr("item_id"), self.attr("title"), self.attr("path"), self.attr("error"));
      });

  nb::class_<std::stop_source>(m, "CancelToken", "Cancels the Client.download calls it is passed to.")
      .def(nb::init<>())
      .def(
          "cancel", [](std::stop_source& source) { source.request_stop(); },
          "Requests stop; safe to call from any thread, repeated calls are no-ops.");

  nb::class_<pxsteamdl::Client>(m, "Client",
                                "Anonymous Steam session. Thread-safe: one client may serve several threads.")
      .def(nb::init<>(), nb::call_guard<nb::gil_scoped_release>(),
           "Logs in to Steam anonymously; raises RuntimeError on failure.")
      .def("download", &Download, "ids"_a, "root"_a, nb::kw_only(), "parallel_items"_a = 2, "threads_per_item"_a = 4,
           "on_progress"_a = nb::none(), "on_resolved"_a = nb::none(), "cancel"_a.none() = nb::none(),
           "Downloads each item into root/<item id>/, updating existing copies incrementally.\n\n"
           "Per-item failures are reported in Result.error. on_resolved(info) is called once per item on the\n"
           "calling thread, in order, as soon as its batch of up to 100 items is answered by Steam and before\n"
           "the item's bytes are downloaded; earlier items already download meanwhile. ItemInfo is Steam's\n"
           "verdict only, not a download outcome.\n"
           "on_progress is called from worker\n"
           "threads; exceptions either callback raises are reported as unraisable and do not stop the download.\n"
           "After cancel.cancel(), in-flight chunk requests finish, remaining work is skipped and\n"
           "unfinished items report error == \"cancelled\"; completed items stay ok.\n"
           "The GIL is released while downloading.");
}
