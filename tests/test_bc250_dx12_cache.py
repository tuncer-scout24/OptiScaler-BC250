"""Compile and run the production cache policy and stream parser; no GPU needed."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# On Windows the same test compiles against the real SDK. Other hosts need only these ABI data types.
D3D12_STUB = r"""
#pragma once
#include <cstddef>
#include <cstdint>
using UINT = uint32_t;
struct ID3D12RootSignature;
enum D3D12_PIPELINE_STATE_SUBOBJECT_TYPE {
 D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE=0, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS=1,
 D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS=6, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK=18,
 D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO=19, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS=20
};
enum D3D12_PIPELINE_STATE_FLAGS { D3D12_PIPELINE_STATE_FLAG_NONE=0, D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG=1 };
struct D3D12_SHADER_BYTECODE { const void* pShaderBytecode; size_t BytecodeLength; };
struct D3D12_CACHED_PIPELINE_STATE { const void* pCachedBlob; size_t CachedBlobSizeInBytes; };
struct D3D12_COMPUTE_PIPELINE_STATE_DESC {
 ID3D12RootSignature* pRootSignature; D3D12_SHADER_BYTECODE CS; UINT NodeMask;
 D3D12_CACHED_PIPELINE_STATE CachedPSO; D3D12_PIPELINE_STATE_FLAGS Flags;
};
struct D3D12_PIPELINE_STATE_STREAM_DESC { size_t SizeInBytes; void* pPipelineStateSubobjectStream; };
"""


class Dx12CacheTests(unittest.TestCase):
    def test_native_cache_and_streams(self):
        compiler = shutil.which("cl") if os.name == "nt" else shutil.which(os.environ.get("CXX", "clang++"))
        if not compiler:
            self.fail("A C++20 compiler is required (cl on Windows, clang++ or CXX elsewhere)")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binary = work / ("cache-test.exe" if os.name == "nt" else "cache-test")
            source = ROOT / "tests/native/dx12_pipeline_cache.cpp"
            if os.name == "nt":
                command = [compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", f"/I{ROOT / 'OptiScaler'}",
                           str(source), f"/Fe:{binary}", f"/Fo:{work / 'test.obj'}"]
            else:
                (work / "d3d12.h").write_text(D3D12_STUB)
                command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(work),
                           "-I", str(ROOT / "OptiScaler"), str(source), "-o", str(binary)]
            subprocess.run(command, cwd=work, check=True)
            subprocess.run([str(binary), str(work)], cwd=work, check=True)


if __name__ == "__main__":
    unittest.main()
