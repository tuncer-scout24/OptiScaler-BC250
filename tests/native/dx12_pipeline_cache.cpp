#include "fsr4/Dx12PipelineCacheData.h"
#include "fsr4/Dx12ComputePipeline.h"
#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace Dx12PipelineCacheData;

void Storage(const std::filesystem::path& directory)
{
    const auto path = directory / "cache" / "pipeline.pso";
    const auto temporary = directory / "cache" / "pipeline.tmp";
    Identity identity {};
    identity[4] = 123;
    const Bytes blob { 1, 2, 3, 4, 5 };
    auto publish = [](auto from, auto to)
    {
        std::filesystem::rename(from, to);
        return true;
    };
    assert(Read(path, identity).empty());
    assert(Write(path, temporary, identity, blob, publish));
    assert(Read(path, identity) == blob);
    auto different = identity;
    different[31] = 1;
    assert(Read(path, different).empty());
    assert(!Write(path, temporary, identity, {}, publish));

    const auto good = [&]
    {
        std::ifstream input(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(input), {});
    }();
    auto replace = [&](const Bytes& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    for (size_t offset : { size_t(0), size_t(8), size_t(40), size_t(48), HeaderBytes })
    {
        auto corrupt = good;
        corrupt[offset] ^= 1;
        replace(corrupt);
        assert(Read(path, identity).empty());
    }
    auto truncated = good;
    truncated.pop_back();
    replace(truncated);
    assert(Read(path, identity).empty());
    auto trailing = good;
    trailing.push_back(0);
    replace(trailing);
    assert(Read(path, identity).empty());

    replace(good);
    assert(!Write(path, temporary, identity, Bytes { 9 }, [](auto, auto) { return false; }));
    assert(!std::filesystem::exists(temporary));
    assert(Read(path, identity) == blob); // A failed publish preserves the working file.
    std::filesystem::remove(path);
    assert(Write(path, temporary, identity, Bytes { 9 }, publish));
    assert(Read(path, identity) == Bytes { 9 });
    std::filesystem::resize_file(path, MaxBlobBytes + HeaderBytes + 1);
    assert(Read(path, identity).empty());
}

void Creation()
{
    constexpr int32_t invalid = static_cast<int32_t>(0x80070057u);
    for (auto rejection : { invalid, static_cast<int32_t>(0x887e0001u), static_cast<int32_t>(0x887e0002u) })
    {
        Counters counters;
        unsigned calls = 0, saves = 0;
        const auto result = CreateCached(
            [&](std::span<const uint8_t> blob) -> int32_t
            {
                ++calls;
                return blob.empty() ? 0 : rejection;
            },
            [] { return Bytes { 1 }; },
            [&]
            {
                ++saves;
                return true;
            },
            counters);
        assert(result == 0 && calls == 2 && saves == 1);
        assert(counters.accepted == 0 && counters.rejected == 1 && counters.compiled == 1);
    }
    for (auto failure :
         { static_cast<int32_t>(0x8007000eu), static_cast<int32_t>(0x887a0005u), static_cast<int32_t>(0x887a0006u) })
    {
        Counters counters;
        unsigned calls = 0;
        const auto result = CreateCached(
            [&](auto)
            {
                ++calls;
                return failure;
            },
            [] { return Bytes { 2 }; },
            []
            {
                assert(false);
                return true;
            },
            counters);
        assert(result == failure && calls == 1 && counters.compiled == 0);
    }
    {
        Counters counters;
        const auto result = CreateCached(
            [](auto blob) -> int32_t
            {
                assert(blob.size() == 1);
                return 0;
            },
            [] { return Bytes { 2 }; },
            []
            {
                assert(false);
                return true;
            },
            counters);
        assert(result == 0 && counters.accepted == 1 && counters.compiled == 0);
    }
    for (bool throwOnLoad : { false, true })
    {
        Counters counters;
        unsigned calls = 0;
        const auto result = CreateCached(
            [&](auto blob) -> int32_t
            {
                ++calls;
                assert(blob.empty());
                return 0;
            },
            [&]() -> Bytes
            {
                if (throwOnLoad)
                    throw std::runtime_error("unreadable");
                return {};
            },
            []() -> bool { throw std::runtime_error("read-only"); }, counters);
        assert(result == 0 && calls == 1 && counters.unavailable == 1);
    }
    {
        Counters counters;
        unsigned calls = 0;
        const auto result = CreateCached(
            [&](auto blob) -> int32_t
            {
                ++calls;
                return blob.empty() ? -42 : invalid;
            },
            [] { return Bytes { 2 }; },
            []
            {
                assert(false);
                return true;
            },
            counters);
        assert(result == -42 && calls == 2); // Return the original descriptor's error after a rejected blob.
    }
}

void Streams()
{
    using namespace Dx12ComputePipeline;
    static const char shader[] = "unmodified DXIL";
    struct
    {
        Subobject<ID3D12RootSignature*> root { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,
                                               reinterpret_cast<ID3D12RootSignature*>(uintptr_t(1234)) };
        Subobject<D3D12_SHADER_BYTECODE> cs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, { shader, sizeof(shader) } };
        Subobject<UINT> node { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, 7 };
        Subobject<D3D12_PIPELINE_STATE_FLAGS> flags { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS,
                                                      D3D12_PIPELINE_STATE_FLAG_TOOL_DEBUG };
        Subobject<D3D12_CACHED_PIPELINE_STATE> cache { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, {} };
    } input;
    D3D12_PIPELINE_STATE_STREAM_DESC stream { sizeof(input), &input };
    const auto parsed = Parse(&stream);
    assert(parsed && parsed->desc.pRootSignature == input.root.value && parsed->desc.CS.pShaderBytecode == shader);
    assert(parsed->desc.CS.BytecodeLength == sizeof(shader) && parsed->desc.NodeMask == 7);
    assert(parsed->desc.Flags == input.flags.value);
    assert(parsed->cacheOffset == offsetof(decltype(input), cache) + offsetof(decltype(input.cache), value));
    const auto originalBytes = std::span(reinterpret_cast<const uint8_t*>(&input), sizeof(input));
    const Bytes snapshot(originalBytes.begin(), originalBytes.end());
    const Bytes blob { 12, 34, 56 };
    CachedStream storage(stream);
    auto modified = storage.Bind(stream, *parsed, blob);
    auto cached = Parse(&modified);
    assert(cached && cached->desc.CachedPSO.pCachedBlob == blob.data());
    assert(cached->desc.CachedPSO.CachedBlobSizeInBytes == blob.size());
    assert(cached->desc.pRootSignature == input.root.value && cached->desc.CS.pShaderBytecode == shader);
    assert(cached->desc.NodeMask == 7 && cached->desc.Flags == input.flags.value);
    assert(modified.SizeInBytes == stream.SizeInBytes);
    assert(std::equal(snapshot.begin(), snapshot.end(), originalBytes.begin()));
    assert(!Parse(nullptr));
    stream.SizeInBytes -= 1;
    assert(!Parse(&stream));
    stream.SizeInBytes = sizeof(input);
    input.node.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS; // Duplicate subobject.
    assert(!Parse(&stream));
    input.node.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK;
    input.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS; // Graphics and mixed streams are untouched.
    assert(!Parse(&stream));
    input.cs.type = static_cast<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE>(1000);
    assert(!Parse(&stream));
    input.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
    input.cache.value = { shader, sizeof(shader) };
    assert(Parse(&stream)->desc.CachedPSO.pCachedBlob == shader); // Provider cache must remain visible to bypass.
    stream.SizeInBytes = offsetof(decltype(input), cache);
    assert(Parse(&stream)->cacheOffset == SIZE_MAX);
    const auto withoutCache = *Parse(&stream);
    modified = storage.Bind(stream, withoutCache, blob);
    cached = Parse(&modified);
    assert(cached && cached->desc.CachedPSO.pCachedBlob == blob.data());
    assert(modified.SizeInBytes == stream.SizeInBytes + sizeof(Subobject<D3D12_CACHED_PIPELINE_STATE>));
    input.cs.value.pShaderBytecode = nullptr;
    assert(!Parse(&stream));
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    Storage(argv[1]);
    Creation();
    Streams();
    std::cout << "DX12 cache storage, rejection recovery, error handling and compute streams: PASS\n";
}
