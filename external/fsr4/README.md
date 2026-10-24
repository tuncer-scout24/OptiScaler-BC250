# Bundled AMD FSR 4 providers

These user-supplied Windows x64 provider DLLs are included starting with BC250
Alpha 2. Both report file version `2.4.0.4370` and contain INT8 provider classes.
Game/runtime compatibility still needs testing on BC250.

| Source file | Path in the release ZIP | SHA256 |
| --- | --- | --- |
| `amdvkffx64.dll` | `OptiScaler/amdvkffx64.dll` | `80e761f7c8c0ec8ead7b321df01911baaf917defae51388d29eaecb1d6bc58bd` |
| `amdxcffx64.dll` | `OptiScaler/amdxcffx64.dll` | `c33a287f3048bf02311cae430b6a977500b94ca7b315cd7bff751e6b03e483e3` |

The Release post-build event copies both files into the runtime directory.
`scripts/build.ps1` requires both files to be present before creating the ZIP.
`FSR4DriverDll::Load` searches `OptiDllPath` (the `OptiScaler/` runtime directory
by default) using these exact filenames, so they must not be renamed to the
SDK's `amd_fidelityfx_*` filenames.

These are AMD binaries, not OptiScaler-authored source code.
