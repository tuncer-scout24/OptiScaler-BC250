#pragma once

#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace Dx12ComputePipeline
{
template <typename T> struct alignas(void*) Subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};

struct Stream
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
    size_t cacheOffset = SIZE_MAX;
};

// Accept only plain compute streams. Unknown, graphics and extension subobjects retain the original path.
inline std::optional<Stream> Parse(const D3D12_PIPELINE_STATE_STREAM_DESC* stream)
{
    if (!stream || !stream->pPipelineStateSubobjectStream || stream->SizeInBytes == 0 || stream->SizeInBytes > 4096)
        return std::nullopt;
    Stream result;
    uint32_t seen = 0;
    auto bytes = static_cast<const uint8_t*>(stream->pPipelineStateSubobjectStream);
    for (size_t offset = 0; offset < stream->SizeInBytes;)
    {
        if (stream->SizeInBytes - offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
            return std::nullopt;
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        std::memcpy(&type, bytes + offset, sizeof(type));
        const auto index = static_cast<unsigned>(type);
        if (index >= 32 || (seen & (1u << index)))
            return std::nullopt;
        seen |= 1u << index;
        size_t size = 0;
        const auto read = [&]<typename T>(T& value)
        {
            using Item = Subobject<T>;
            size = sizeof(Item);
            if (size > stream->SizeInBytes - offset)
                return false;
            std::memcpy(&value, bytes + offset + offsetof(Item, value), sizeof(T));
            return true;
        };
        bool valid = false;
        switch (type)
        {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
            valid = read(result.desc.pRootSignature);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            valid = read(result.desc.CS);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
            valid = read(result.desc.NodeMask);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
            valid = read(result.desc.Flags);
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            valid = read(result.desc.CachedPSO);
            result.cacheOffset = offset + offsetof(Subobject<D3D12_CACHED_PIPELINE_STATE>, value);
            break;
        default:
            return std::nullopt;
        }
        if (!valid)
            return std::nullopt;
        offset += size;
    }
    if (!result.desc.CS.pShaderBytecode || !result.desc.CS.BytecodeLength)
        return std::nullopt;
    return result;
}

class CachedStream
{
    std::vector<uint64_t> storage;

  public:
    explicit CachedStream(const D3D12_PIPELINE_STATE_STREAM_DESC& original)
        : storage((original.SizeInBytes + sizeof(Subobject<D3D12_CACHED_PIPELINE_STATE>) + 7) / 8)
    {
    }

    D3D12_PIPELINE_STATE_STREAM_DESC Bind(const D3D12_PIPELINE_STATE_STREAM_DESC& original, const Stream& parsed,
                                          std::span<const uint8_t> blob)
    {
        std::memcpy(storage.data(), original.pPipelineStateSubobjectStream, original.SizeInBytes);
        auto modified = original;
        modified.pPipelineStateSubobjectStream = storage.data();
        const D3D12_CACHED_PIPELINE_STATE cached { blob.data(), blob.size() };
        auto bytes = reinterpret_cast<uint8_t*>(storage.data());
        if (parsed.cacheOffset != SIZE_MAX)
            std::memcpy(bytes + parsed.cacheOffset, &cached, sizeof(cached));
        else
        {
            const Subobject<D3D12_CACHED_PIPELINE_STATE> item { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO,
                                                                cached };
            std::memcpy(bytes + original.SizeInBytes, &item, sizeof(item));
            modified.SizeInBytes += sizeof(item);
        }
        return modified;
    }
};
} // namespace Dx12ComputePipeline
