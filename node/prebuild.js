// SPDX-License-Identifier: LGPL-3.0-or-later
'use strict';

const fs = require('node:fs');
const path = require('node:path');

// The addons built by the release workflow sit in prebuilds/<platform>-<arch>/. The Linux one is built against
// glibc, so musl systems (Alpine) build from source instead.
function prebuiltPath() {
  if (process.platform === 'linux' && !process.report?.getReport().header.glibcVersionRuntime) return null;
  const file = path.join(__dirname, '..', 'prebuilds', `${process.platform}-${process.arch}`, 'pxsteamdl-addon.node');
  return fs.existsSync(file) ? file : null;
}

module.exports = { prebuiltPath };
