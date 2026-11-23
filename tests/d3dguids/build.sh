#!/bin/sh
# The D3D11 and D3D12 renderers name no interface or class GUID from the
# SDK (IID_..., CLSID_...): MSVC links no dxguid, so each would be an
# unresolved external there, while mingw-w64's import libraries define
# them and its lanes link. Each renderer defines the GUIDs it asks for.
# Runnable from ANY directory.
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$DIR/../.." && pwd)
if grep -nE "\b(IID|CLSID)_[A-Za-z0-9]" "$ROOT"/pcsx2/GS/Renderers/DX11/*.[ch] "$ROOT"/pcsx2/GS/Renderers/DX12/*.[ch]; then
	echo "d3dguids: FAIL: an SDK GUID, which MSVC has no library for"
	exit 1
fi
echo "d3dguids: ok"
