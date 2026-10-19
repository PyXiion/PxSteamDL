// SPDX-License-Identifier: LGPL-3.0-or-later
#include "fake_pool.hpp"

namespace pxsteamdl::detail::testing {

asio::thread_pool& FakePool() {
  static asio::thread_pool pool(16);
  return pool;
}

}  // namespace pxsteamdl::detail::testing
