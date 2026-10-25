#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

// Portable cache storage and retry policy, shared with the native host tests.
namespace Dx12PipelineCacheData
{
using Bytes = std::vector<uint8_t>;
using Identity = std::array<uint8_t, 32>;
inline constexpr size_t MaxBlobBytes = 64 * 1024 * 1024;
inline constexpr size_t HeaderBytes = 56;
inline constexpr std::array<uint8_t, 8> Magic { 'B', 'C', '2', '5', '0', 'D', 'X', 1 };

inline uint64_t Checksum(std::span<const uint8_t> bytes)
{
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : bytes)
        hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

inline void Append64(Bytes& bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

inline uint64_t Read64(std::span<const uint8_t> bytes, size_t offset)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}

inline Bytes Read(const std::filesystem::path& path, const Identity& identity)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size <= HeaderBytes || size > HeaderBytes + MaxBlobBytes)
        return {};
    std::ifstream file(path, std::ios::binary);
    Bytes header(HeaderBytes);
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        !std::equal(Magic.begin(), Magic.end(), header.begin()) ||
        !std::equal(identity.begin(), identity.end(), header.begin() + 8) || Read64(header, 48) != size - HeaderBytes)
        return {};
    Bytes blob(static_cast<size_t>(size - HeaderBytes));
    if (!file.read(reinterpret_cast<char*>(blob.data()), blob.size()) || file.peek() != std::char_traits<char>::eof() ||
        Checksum(blob) != Read64(header, 40))
        return {};
    return blob;
}

// The caller supplies an atomic, replacing rename appropriate for its platform.
template <typename Publish>
bool Write(const std::filesystem::path& path, const std::filesystem::path& temporary, const Identity& identity,
           std::span<const uint8_t> blob, Publish publish)
{
    if (blob.empty() || blob.size() > MaxBlobBytes)
        return false;
    Bytes header(Magic.begin(), Magic.end());
    header.insert(header.end(), identity.begin(), identity.end());
    Append64(header, Checksum(blob));
    Append64(header, blob.size());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    file.write(reinterpret_cast<const char*>(blob.data()), blob.size());
    file.close();
    if (file && publish(temporary, path))
        return true;
    std::error_code error;
    std::filesystem::remove(temporary, error);
    return false;
}

struct Counters
{
    uint64_t calls = 0, compiled = 0, accepted = 0, rejected = 0, skipped = 0, unavailable = 0;
    double milliseconds = 0;
};

inline bool CacheRejected(int32_t result)
{
    return result == static_cast<int32_t>(0x80070057u) || // E_INVALIDARG
           result == static_cast<int32_t>(0x887E0001u) || // D3D12_ERROR_ADAPTER_NOT_FOUND
           result == static_cast<int32_t>(0x887E0002u);   // D3D12_ERROR_DRIVER_VERSION_MISMATCH
}

// IO failures never affect creation. Only a cache-specific rejection retries the original descriptor.
// In particular, device removal and out-of-memory errors are returned without a second creation attempt.
template <typename Create, typename Load, typename Save>
int32_t CreateCached(Create create, Load load, Save save, Counters& counters)
{
    Bytes blob;
    try
    {
        blob = load();
    }
    catch (const std::exception&)
    {
    }
    if (!blob.empty())
    {
        auto result = create(std::span<const uint8_t>(blob));
        if (result >= 0)
        {
            ++counters.accepted;
            return result;
        }
        ++counters.rejected;
        if (!CacheRejected(result))
            return result;
    }
    ++counters.compiled;
    auto result = create(std::span<const uint8_t>());
    if (result >= 0)
    {
        try
        {
            if (!save())
                ++counters.unavailable;
        }
        catch (const std::exception&)
        {
            ++counters.unavailable;
        }
    }
    return result;
}
} // namespace Dx12PipelineCacheData
