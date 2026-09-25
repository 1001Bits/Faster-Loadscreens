# Build dependency provenance

The 2.1.1 source uses the following pinned inputs. No dependency revisions were
changed for the worldspace initialization/lifetime fix.

| Input | Repository | Immutable revision | Notes |
| --- | --- | --- | --- |
| CommonLibF4 | <https://github.com/alandtse/CommonLibF4> | `2b64114a449ebdcaa82d33d83d8df9bb8d8092d5` | MIT at this revision; the CMake configure step verifies the revision and rejects a dirty checkout by default. |
| vcpkg registry baseline | <https://github.com/microsoft/vcpkg> | `b4a3d89125e45bc8f80fb94bef9761d4f4e14fb9` | Dependency names and the `cpptrace` 1.0.0 override are recorded in `vcpkg.json`. |

The local validation used Visual Studio 2022, the `x64-windows-static` triplet,
the static MSVC runtime, and Release configuration. CommonLibF4 was clean at the
revision above. Supply an existing checkout through `COMMONLIBF4_PATH`; CMake
does not fetch or update that dependency.

This repository's root GPL-3.0 license remains authoritative for the mod.
Dependencies keep their own licenses and attribution requirements. The MIT
CommonLibF4 pin does not change this repository's license.

The corresponding production DLL tested for 2.1.1 has SHA-256
`477d056983eb3a4a7ffed59bac82810579d6a7119099aebd685544d5c1a55be5`.
The runtime findings and limitations are documented in
[the issue #2 investigation](docs/ISSUE_2_INVESTIGATION_2026-09-26.md).
