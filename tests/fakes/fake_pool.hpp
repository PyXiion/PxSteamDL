// SPDX-License-Identifier: LGPL-3.0-or-later
// Threads for the fakes' handlers: tests block and race inside them, which must not stall the io thread.
#pragma once

#include <asio/thread_pool.hpp>

namespace pxsteamdl::detail::testing {

asio::thread_pool& FakePool();

}  // namespace pxsteamdl::detail::testing
