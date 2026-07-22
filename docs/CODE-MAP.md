<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Code map

This map is optimized for quick navigation by maintainers.
Start with the responsibility row, inspect the named public header, and keep a
change inside that owner unless an architectural seam genuinely changes.

| Path | Responsibility | Public or internal entry points | Mutable owner | Allowed dependencies | Forbidden dependencies | Compatibility policies | Tests | Focused command |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `src/api/` | Framework-neutral public C ABI | `anygm.h`; `AnygmEngine`; `AnygmHostServices` | Opaque `AnygmEngine`; no public global state | C fixed-width and size headers | Internals, adapters, platform headers | Neutral facts and profile are opaque | `tests/contract/public_header_*` | `make api-check` |
| `src/core/` | Lifecycle, frame orchestration, configuration, and state root | `anygm_create`; `anygm_load`; `anygm_run_frame`; state API | `AnygmEngine` and its owned subsystem contexts | API, host, compatibility, content, runtime, video, audio | Adapter headers and ambient OS services | Consumes the immutable resolved profile | `tests/contract/dummy_host.c`; `tests/integration/test_engine_instances.c`; state corpus | `make contract-check` |
| `src/compatibility/` | Structural facts and immutable semantic policy | `anygm_content_facts_detect`; `anygm_compatibility_resolve`; policy accessors | Engine-owned immutable `AnygmCompatibilityProfile` | Neutral content structures and standard C | Adapters, paths, content identity, host globals | Sole owner of cross-generation semantic selection | `tests/unit/compatibility/test_compatibility.c`; architecture scan | `make check TEST=compatibility` |
| `src/content/container/` | Content routing, safe archive extraction, paths, and disposable cache | `anygm_content_resolve_path`; `anygm_content_load_win` | One route/extraction transaction; cache is disposable | Host VFS and concrete normalizers | Shell, process execution, plugins, network, adapters | Detect structure only; does not select runtime semantics | `tests/fuzz/test_content_security.c` | `make check TEST=content_security` |
| `src/content/datafile/` | Bounded normalized FORM decoding and indexes | `gml_win_from_mem`; `gml_win_load_host`; `GmlWin` queries | `GmlWin` owns input, tables, indexes, and decode caches | Host VFS and bytecode layout readers | Adapters, direct filesystem, runtime behavior | Exposes structural facts; no family-wide semantic branching | `tests/fuzz/test_datafile_security.c` | `make check TEST=datafile_security` |
| `src/content/bytecode/` | Encoding-specific instruction and reference-layout readers | `gml_decode_bc_bounded`; `gml_bc_code_start`; `gml_bc_ref_layout` | No independent state; caches belong to `GmlWin` | Datafile model and standard C | Runtime scheduling, adapters, host services | Encoding dispatch only; per-record layout detection where required | `tests/unit/content/test_bytecode.c`; `tests/fuzz/test_bytecode_security.c` | `make check TEST=bytecode_security` |
| `src/content/project/` | Source-project parsing, compilation, and normalized package generation | `gmlc_project_load`; `gmlc_bytecode_compile_source_ex`; `gmlc_package_write_structural` | `GmlcProject`, compiler registries, and package transaction | Host VFS, JSON, bytecode model, image writers | Adapters and ambient process configuration | Produces explicit structural metadata; no runtime policy | `tests/unit/content/test_bytecode.c`; normalized-data corpus | `make check TEST=bytecode` |
| `src/content/classic/` | Classic container/project decoding into the shared normalized package | `gmlc_classic_decode`; import and project entry points | `GmlcClassicManifest` and one import transaction | Host VFS, project model, image/font helpers | Adapters, runtime execution, ambient filesystem fallback | Emits classic structural facts consumed by the resolver | `tests/unit/content/test_classic.c` | `make check TEST=classic` |
| `src/runtime/vm/` | Shared VM, values, instances, events, rooms, and state | `gml_vm_init`; `gml_run_code`; VM state API | `GmlVM` | Compatibility accessors, builtins, owned AV contexts, host callbacks | Adapters, raw version predicates, ambient OS state | Consumes named VM, event, collision, path, and RNG policies | runtime unit suites; engine isolation | `make check TEST=persistent_room` |
| `src/runtime/builtins/` | Built-in dispatch and language-visible host services | `gml_builtin_call`; field accessors | Calling `GmlVM` and explicitly owned renderer/audio/particle data | VM, compatibility accessors, host/VFS, AV subsystems | Adapters, raw version predicates, direct OS APIs | Consumes named builtin, draw, file, input, and platform policies | persistent-room, D3-state, and data-structure suites | `make check TEST=persistent_room` |
| `src/runtime/particles/` | Engine-owned particle simulation and serialization | particle create/update/draw/state entry points | `GmlParticleSystem` owned by one VM | VM, renderer, compatibility accessors | Adapters, global mutable registries, direct OS APIs | Consumes named draw and simulation policies | `tests/unit/runtime/test_d3_state.c` | `make check TEST=d3_state` |
| `src/video/renderer/` | Software rendering, assets, surfaces, fonts, and render state | `gml_render_init`; draw entry points; render state API | `GmlRender` and its worker/cache members | Compatibility accessors, host/VFS, codecs, immutable assets | Adapters, raw version predicates, ambient graphics APIs | Consumes named draw, blend, collision-geometry, and text policies | D3-state and persistent-room suites | `make check TEST=d3_state` |
| `src/video/assets/` | Independently generated visual fallback assets | `gm_qoi.h` immutable data | None | Renderer only | Runtime mutation and adapter dependencies | None | renderer fixtures and provenance review | `make check TEST=d3_state` |
| `src/audio/mixer/` | Voices, audio groups, mixing, and audio state | `gml_audio_init`; playback/mix/state entry points | `GmlAudio` | Host/VFS, banks, codecs | Adapters, direct audio devices, raw version predicates | Consumes named audio behavior policies | persistent-room and state corpora | `make check TEST=persistent_room` |
| `src/audio/codecs/` | Bounded compressed-audio decoder wrappers | codec open/decode/free entry points | Decoder instance owned by `GmlAudio` | Vendored decoder headers | Host devices, adapters, runtime semantics | None | audio fixtures in persistent-room suite | `make check TEST=persistent_room` |
| `src/audio/banks/` | Audio-bank metadata and stream lookup | `gml_fmod_bank_open`; lookup/decode helpers | Bank object owned by `GmlAudio` | Host/VFS, codec helpers, immutable setup data | Adapters and direct filesystem | Structural layout selection only | audio fixtures in persistent-room suite | `make check TEST=persistent_room` |
| `src/host/` | Portable helpers over `AnygmHostServices`; explicit test stdio VFS | `anygm_host_*`; `anygm_vfs_*`; `anygm_stdio_vfs_services_init` | Host-owned callback handles; helpers retain no process-global state | Public API and standard C; OS calls only in test stdio backend | Adapters in portable helpers and implicit filesystem fallback | None | `tests/unit/host/test_vfs.c`; dummy host | `make check TEST=vfs` |
| `src/adapters/libretro/` | Libretro callbacks, input mapping, frontend VFS, and A/V transport | Standard `retro_*` exports; `LibretroContext` | One frontend-owned `LibretroContext` | Public AnyGM API and vendored libretro header | AnyGM internals and semantic/version decisions | None; maps neutral API values only | export contract; adapter transport contract | `make check TEST=libretro_state_transport` |
| `src/generated/` | Checked-in immutable data with documented provenance | Generated data headers | None | Owning renderer/audio module | Runtime mutation, adapters, undocumented regeneration | None | owning subsystem fixtures and provenance review | `make check TEST=d3_state` |
| `src/third_party/` | Vendored dependencies with preserved upstream notices | Upstream APIs wrapped by owning modules | Library context owned by caller | Owning wrapper only | Product policy, adapters unless vendored for that adapter | None | core link plus notice/license audit | `make core` |
| `tests/unit/` | Narrow parser and semantic characterization | `test_*` executables | Per-test local fixture | Public/internal owner under test and test support | External corpus and network | Exercises both sides of named policies | Unit executables | `make check TEST=compatibility` |
| `tests/contract/` | Public API and binary-export contracts | `dummy_host.c`; public-header probes; export checker | Dummy host fixture | Public API, synthetic support, built artifact inspection | Private runtime headers in public-header probes | Neutral host contract only | Contract executables and shell audit | `make contract-check` |
| `tests/integration/` | Cross-module ownership, determinism, and isolation | `test_engine_instances.c` | Two independent test engines | Public API and synthetic content | External corpus and adapter internals | Same resolved profile in independent engines | Engine-instance executable | `make integration-check` |
| `tests/fuzz/` | Deterministic corrupt-input and boundary corpora | Security test executables | Per-case temporary buffers and fixtures | Smallest owning parser/API surface | Network, external corpus, nondeterministic seeds | Invalid input cannot alter selected policy or live state | Content, datafile, bytecode, and state corpora | `make security-check` |
| `tests/architecture/` | Dependency, ownership, inventory, cleanup, and isolation enforcement | `check_*.sh`; `safe_clean.sh` | None | Source tree and build metadata | Runtime mutation and external services | Enforces named-policy and adapter boundaries | Shell checks | `make architecture-check` |
| `tests/support/` | Shared synthetic content and host fixtures | `synthetic_content.*`; test VFS helpers | Fixture object or test process | Public API and explicit test-only stdio backend | Production linkage and external corpus | Neutral synthetic content only | Consumed by contract/integration/security tests | `make integration-check` |

## Common change routes

### Add or correct a serialized input field

Reader under `src/content/` -> shared content model -> parser fixture. Add a
compatibility policy only if the field changes runtime semantics.

### Correct a generation-dependent semantic

`anygm_compatibility.c` -> named policy accessor -> smallest branch in the
shared owner -> compatibility unit test plus the owner's focused test.

### Add a platform facility

`src/api/anygm.h` host service -> portable helper in `src/host/` -> adapter
implementation -> dummy-host contract. Do not call the platform directly from
the consumer.

### Change save-state data

Owning subsystem serializer -> root state framing in `engine.c` -> transactional
load test -> two-instance integration test -> review stable size bound and
schema compatibility. `anygm_state_size` remains the exact canonical size; any
fixed-capacity transport fallback belongs to an adapter.

### Add another frontend framework

New adapter directory -> `src/api/anygm.h` only -> framework-specific build
target. Follow `PORTING.md`; no VM, renderer, mixer, or parser fork is
needed.
