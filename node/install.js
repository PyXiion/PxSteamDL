// SPDX-License-Identifier: LGPL-3.0-or-later
// The package's install script: nothing to do if the package ships an addon for this platform, otherwise build it.
'use strict';

const { spawnSync } = require('node:child_process');
const path = require('node:path');
const { prebuiltPath } = require('./prebuild');

// A prebuilt addon that cannot be loaded here (an older C library, a missing runtime) is as good as none.
function prebuiltWorks() {
  const file = prebuiltPath();
  if (!file) return { works: false, reason: `no prebuilt addon for ${process.platform}-${process.arch}` };
  try {
    require(file);
    return { works: true };
  } catch (error) {
    return { works: false, reason: `the prebuilt addon does not load (${error.message})` };
  }
}

const prebuilt = prebuiltWorks();
if (prebuilt.works) {
  console.log(`pxsteamdl: using the prebuilt addon for ${process.platform}-${process.arch}`);
  process.exit(0);
}

console.log(`pxsteamdl: ${prebuilt.reason}, building from source`);
let cmakeJs;
try {
  cmakeJs = require.resolve('cmake-js/bin/cmake-js');
} catch {
  console.error('pxsteamdl: building from source needs cmake-js (Node.js 20.17+ or 22.9+), CMake and a C++20 compiler');
  process.exit(1);
}
const result = spawnSync(
  process.execPath,
  [cmakeJs, 'compile', '--out', 'build-node', '--CDPXSTEAMDL_BUILD_NODE=ON', '--CDPXSTEAMDL_BUILD_CLI=OFF', '--CDPXSTEAMDL_INSTALL=OFF'],
  { cwd: path.join(__dirname, '..'), stdio: 'inherit' }
);
process.exit(result.status ?? 1);
