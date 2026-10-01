# SPDX-License-Identifier: LGPL-3.0-or-later
"""Anonymous Steam Workshop downloader (RimWorld)."""

from . import _pxsteamdl
from ._async import AsyncClient
from ._pxsteamdl import CancelToken, Client, ItemInfo, Progress, Result

# Set by the extension module from the version in CMakeLists.txt, which is also the version of the package.
__version__: str = getattr(_pxsteamdl, "__version__")

__all__ = ["AsyncClient", "CancelToken", "Client", "ItemInfo", "Progress", "Result", "__version__"]
