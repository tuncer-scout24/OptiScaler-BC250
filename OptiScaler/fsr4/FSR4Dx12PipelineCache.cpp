#include "pch.h"
#include "FSR4Dx12PipelineCache.h"
#include "Dx12ComputePipeline.h"
#include "FSR4Models.h"
#include "FSR4DriverProviderDx12.h"

#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <dxgi1_6.h>
#include <bcrypt.h>
#include <json.hpp>
#include <chrono>
#include <atomic>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")

namespace FSR4Dx12PipelineCache
{
using namespace Dx12PipelineCacheData;

struct Device
{
    LUID luid;
    Identity identity;
    std::filesystem::path directory;
};

namespace
{
thread_local Scope* active = nullptr;
thread_local bool creatingPipeline = false;
struct PipelineGuard
{
    PipelineGuard() { creatingPipeline = true; }
    ~PipelineGuard() { creatingPipeline = false; }
};
std::mutex statisticsMutex;
Counters statistics;
std::atomic_uint64_t serial { 0 };
constexpr GUID rootIdentityGuid { 0xe3812b6f, 0x4afb, 0x4689, { 0xa2, 0x74, 0x62, 0x05, 0x6d, 0xdc, 0xb0, 0xa1 } };

class Hash
{
    BCRYPT_HASH_HANDLE hash = nullptr;

  public:
    Hash()
    {
        struct Algorithm
        {
            BCRYPT_ALG_HANDLE value = nullptr;
            Algorithm() { BCryptOpenAlgorithmProvider(&value, BCRYPT_SHA256_ALGORITHM, nullptr, 0); }
            ~Algorithm()
            {
                if (value)
                    BCryptCloseAlgorithmProvider(value, 0);
            }
        };
        static Algorithm algorithm;
        if (!algorithm.value || BCryptCreateHash(algorithm.value, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("SHA256 unavailable");
    }
    ~Hash()
    {
        if (hash)
            BCryptDestroyHash(hash);
    }
    Hash(const Hash&) = delete;
    void Add(const void* data, size_t size)
    {
        if (size > MaxBlobBytes || (size && !data) ||
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("SHA256 input rejected");
    }
    template <typename T> void Add(const T& value) { Add(&value, sizeof(value)); }
    Identity Finish()
    {
        Identity result;
        if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
            throw std::runtime_error("SHA256 failed");
        return result;
    }
};

std::string Hex(const Identity& identity)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : identity)
    {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

std::filesystem::path Temporary(const std::filesystem::path& path)
{
    return path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(serial.fetch_add(1)) + L".tmp";
}

bool Publish(const std::filesystem::path& from, const std::filesystem::path& to)
{
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void CaptureFile(const std::filesystem::path& path, const void* data, size_t size)
{
    if (!data || !size || size > MaxBlobBytes || std::filesystem::exists(path))
        return;
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = Temporary(path);
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char*>(data), size);
    file.close();
    if (!file || !Publish(temporary, path))
    {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    }
}

bool Participates(ID3D12Device* device)
{
    if (!active || !active->device || !device)
        return false;
    const auto luid = device->GetAdapterLuid();
    return luid.LowPart == active->device->luid.LowPart && luid.HighPart == active->device->luid.HighPart;
}

std::optional<Identity> PipelineIdentity(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
{
    if (!desc.CS.pShaderBytecode || !desc.CS.BytecodeLength || desc.CS.BytecodeLength > MaxBlobBytes)
        return std::nullopt;
    Identity root {};
    bool knownRoot = true;
    if (desc.pRootSignature)
    {
        UINT size = sizeof(root);
        knownRoot = SUCCEEDED(desc.pRootSignature->GetPrivateData(rootIdentityGuid, &size, root.data())) &&
                    size == sizeof(root);
    }
    Hash hash;
    hash.Add(active->device->identity);
    hash.Add(root);
    hash.Add(desc.NodeMask);
    hash.Add(desc.Flags);
    hash.Add(desc.CS.BytecodeLength);
    hash.Add(desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
    const auto identity = hash.Finish();
    if (Config::Instance()->Fsr4Dx12ShaderCapture.value_or_default())
    {
        // Capture is diagnostic only. Dump DXIL unchanged, before the Proton translation layer.
        try
        {
            const auto base = active->device->directory / "capture" / Hex(identity);
            CaptureFile(base.wstring() + L".dxil", desc.CS.pShaderBytecode, desc.CS.BytecodeLength);
            const auto model = FSR4Models::ActiveModel(FSR4DriverProviderDx12::LoadedModule());
            const nlohmann::json info { { "identity", Hex(identity) },
                                        { "root_identity", Hex(root) },
                                        { "root_identity_known", knownRoot },
                                        { "node_mask", desc.NodeMask },
                                        { "flags", static_cast<UINT>(desc.Flags) },
                                        { "bytecode_bytes", desc.CS.BytecodeLength },
                                        { "provider", "amdxcffx64.dll" },
                                        { "reported_model", model ? FSR4Models::Name(*model) : "Unknown" } };
            const auto text = info.dump(2);
            CaptureFile(base.wstring() + L".json", text.data(), text.size());
        }
        catch (const std::exception& error)
        {
            LOG_WARN("BC250 DX12 shader capture: {}", error.what());
        }
    }
    // Captures are still useful with an unobserved root signature; persistent caching is not.
    return knownRoot ? std::optional(identity) : std::nullopt;
}

template <typename Create> HRESULT Execute(Create create, const Identity& identity, void** output)
{
    auto& counters = active->counters;
    std::filesystem::path path;
    try
    {
        path = active->device->directory / "cache" / (Hex(identity) + ".pso");
    }
    catch (const std::exception&)
    {
        ++counters.skipped;
        return create({});
    }
    const auto timed = [&](std::span<const uint8_t> blob) -> int32_t
    {
        const auto started = std::chrono::steady_clock::now();
        const auto result = create(blob);
        counters.milliseconds +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        return static_cast<int32_t>(result);
    };
    return static_cast<HRESULT>(CreateCached(
        timed, [&] { return Read(path, identity); },
        [&]
        {
            if (!*output)
                return false;
            ID3DBlob* blob = nullptr;
            const auto result = static_cast<ID3D12PipelineState*>(*output)->GetCachedBlob(&blob);
            if (FAILED(result) || !blob)
                return false;
            bool written = false;
            try
            {
                written =
                    Write(path, Temporary(path), identity,
                          { static_cast<const uint8_t*>(blob->GetBufferPointer()), blob->GetBufferSize() }, Publish);
            }
            catch (...)
            {
                blob->Release();
                throw;
            }
            blob->Release();
            return written;
        },
        counters));
}
} // namespace

DevicePtr Prepare(ID3D12Device* device)
{
    if (!device || (!Config::Instance()->Fsr4Dx12PipelineCache.value_or_default() &&
                    !Config::Instance()->Fsr4Dx12ShaderCapture.value_or_default()))
        return {};
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc {};
    LARGE_INTEGER driver {};
    const auto luid = device->GetAdapterLuid();
    {
        ScopedSkipSpoofingGlobal skipSpoofing {};
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return {};
        const auto result = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        factory->Release();
        if (FAILED(result))
            return {};
        const auto described = adapter->GetDesc(&desc);
        const auto versioned = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        adapter->Release();
        if (FAILED(described) || desc.VendorId != 0x1002 || desc.DeviceId != 0x13fe)
            return {};
        if (FAILED(versioned))
            LOG_INFO("BC250 DX12 driver version unavailable; cached blobs will be validated by D3D12");
    }
    try
    {
        Hash hash;
        const uint32_t version = 1;
        hash.Add(version);
        hash.Add(desc.VendorId);
        hash.Add(desc.DeviceId);
        hash.Add(desc.SubSysId);
        hash.Add(desc.Revision);
        hash.Add(luid);
        hash.Add(driver);
        auto result = std::make_shared<Device>();
        result->luid = luid;
        result->identity = hash.Finish();
        result->directory = std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"BC250Dx12";
        LOG_INFO("BC250 DX12 tools: cache {}, shader capture {}, folder {}",
                 Config::Instance()->Fsr4Dx12PipelineCache.value_or_default(),
                 Config::Instance()->Fsr4Dx12ShaderCapture.value_or_default(), result->directory.string());
        return result;
    }
    catch (const std::exception& error)
    {
        LOG_WARN("BC250 DX12 tools unavailable: {}", error.what());
        return {};
    }
}

Scope::Scope(const DevicePtr& owner) : previous(active), device(owner.get()) { active = this; }

Scope::~Scope()
{
    active = previous;
    if (!device || counters.calls == 0)
        return;
    std::scoped_lock lock(statisticsMutex);
    statistics.calls += counters.calls;
    statistics.compiled += counters.compiled;
    statistics.accepted += counters.accepted;
    statistics.rejected += counters.rejected;
    statistics.skipped += counters.skipped;
    statistics.unavailable += counters.unavailable;
    statistics.milliseconds += counters.milliseconds;
    LOG_INFO("BC250 DX12 PSO: {} calls, {} compiled, {} cached accepted, {} cached rejected, {} skipped, "
             "{} blobs unavailable, {:.2f} ms in creation",
             counters.calls, counters.compiled, counters.accepted, counters.rejected, counters.skipped,
             counters.unavailable, counters.milliseconds);
}

Counters Statistics()
{
    std::scoped_lock lock(statisticsMutex);
    return statistics;
}

void RememberRootSignature(ID3D12Device* device, UINT nodeMask, const void* bytes, SIZE_T size, REFIID iid,
                           void** output)
{
    if (!Participates(device) || iid != __uuidof(ID3D12RootSignature) || !output || !*output)
        return;
    try
    {
        Hash hash;
        hash.Add(nodeMask);
        hash.Add(bytes, size);
        const auto identity = hash.Finish();
        static_cast<ID3D12RootSignature*>(*output)->SetPrivateData(rootIdentityGuid, sizeof(identity), identity.data());
        if (Config::Instance()->Fsr4Dx12ShaderCapture.value_or_default())
            CaptureFile(active->device->directory / "capture" / (Hex(identity) + ".rootsig"), bytes, size);
    }
    catch (const std::exception& error)
    {
        LOG_WARN("BC250 DX12 root capture: {}", error.what());
    }
}

HRESULT CreateCompute(ComputeCreate original, ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                      REFIID iid, void** output)
{
    if (creatingPipeline || !Participates(device))
        return original(device, desc, iid, output);
    PipelineGuard guard;
    ++active->counters.calls;
    std::optional<Identity> identity;
    try
    {
        if (desc)
            identity = PipelineIdentity(*desc);
    }
    catch (const std::exception& error)
    {
        LOG_WARN("BC250 DX12 cache lookup: {}", error.what());
    }
    if (!identity || !output || iid != __uuidof(ID3D12PipelineState) || desc->CachedPSO.pCachedBlob ||
        desc->CachedPSO.CachedBlobSizeInBytes || !Config::Instance()->Fsr4Dx12PipelineCache.value_or_default())
    {
        ++active->counters.skipped;
        return original(device, desc, iid, output);
    }
    return Execute(
        [&](std::span<const uint8_t> blob)
        {
            if (blob.empty())
                return original(device, desc, iid, output);
            auto cached = *desc;
            cached.CachedPSO = { blob.data(), blob.size() };
            return original(device, &cached, iid, output);
        },
        *identity, output);
}

HRESULT CreateStream(StreamCreate original, ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                     REFIID iid, void** output)
{
    if (creatingPipeline || !Participates(device))
        return original(device, desc, iid, output);
    PipelineGuard guard;
    ++active->counters.calls;
    const auto parsed = Dx12ComputePipeline::Parse(desc);
    std::optional<Identity> identity;
    try
    {
        if (parsed)
            identity = PipelineIdentity(parsed->desc);
    }
    catch (const std::exception& error)
    {
        LOG_WARN("BC250 DX12 stream lookup: {}", error.what());
    }
    if (!identity || !output || iid != __uuidof(ID3D12PipelineState) || parsed->desc.CachedPSO.pCachedBlob ||
        parsed->desc.CachedPSO.CachedBlobSizeInBytes || !Config::Instance()->Fsr4Dx12PipelineCache.value_or_default())
    {
        ++active->counters.skipped;
        return original(device, desc, iid, output);
    }
    // Allocate before invoking the backend. An allocation failure can safely use the untouched original stream.
    std::optional<Dx12ComputePipeline::CachedStream> storage;
    try
    {
        storage.emplace(*desc);
    }
    catch (const std::exception&)
    {
        return original(device, desc, iid, output);
    }
    return Execute(
        [&](std::span<const uint8_t> blob)
        {
            if (blob.empty())
                return original(device, desc, iid, output);
            const auto modified = storage->Bind(*desc, *parsed, blob);
            return original(device, &modified, iid, output);
        },
        *identity, output);
}
} // namespace FSR4Dx12PipelineCache
