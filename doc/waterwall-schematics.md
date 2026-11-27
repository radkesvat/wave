# Authoring a WaterWall schematic

A schematic is a Wave-owned JSON description of one WaterWall release. Wave
loads it when starting the backend or switching the selected release, and
sends it to authenticated workspaces. The schematic supplies the node and
core forms, graph links, checks, help, and release package information. The
Wave version and WaterWall version are independent.

Use [the bundled 1.6.0 schematic](../backend/schematics/waterwall-1.6.0.json)
as a starting point. Generate a separate file for each release and review it
against the exact source used for the release archives.

## Identify the release inputs

Work from the upstream release tag and its dereferenced commit. The WaterWall
v1.6.0 tag points to
`f4a5b3b1a4949d520ad5cd0c60afff8bc921290d`. Use an existing read-only upstream
checkout or obtain the release tag in a separate audit checkout.

```sh
release=1.6.0
git -C local/WaterWall rev-parse "v${release}^{}"
git -C local/WaterWall show "v${release}:core/imported_tunnels.c"
git -C local/WaterWall show "v${release}:ww/cmake/preset/linux.json"
git -C local/WaterWall show "v${release}:.github/workflows/ci.yaml"
```

For a later release, compare the previous and new tags. Review changes to
the build options, node registration, startup, configuration parsing, and
constructors, including any helpers the constructors call.

```sh
previous=1.6.0
release=1.6.1
git -C local/WaterWall diff --name-status "v${previous}" "v${release}" -- \
  CMakeLists.txt CMakePresets.json .github/workflows core ww tunnels
```

Fill in `version`, `source.tag`, and `source.commit`. Download the intended
official Linux archives for the target tag and compute each archive's SHA-256
with `sha256sum`. Set its filename and lowercase checksum in `packages`
under `linux-x86_64` or `linux-aarch64`. Wave uses the matching official release
URL and checks those archive bytes before extracting `Waterwall`. Check the
asset names and architecture coverage on the release itself. The bundled
schematic uses the two Linux GCC old-CPU builds.

## Inventory the nodes

Read `loadImportedTunnelsIntoCore` in `core/imported_tunnels.c`, then resolve
its preprocessor conditions from the release's CMake options, Linux presets,
and CI overrides. Read the registered type string and capabilities from each
node getter. The filename and C getter name are useful for locating the source.
The type string is the key Wave uses when reading a config.

For the 1.6.0 Linux builds, the 108 registration entries in the source resolve
to 95 runnable config types. Two entries, `HintConnectionIN` and
`HintPacketIN`, are replacement markers that fail startup until replaced with
a concrete input. Other excluded registrations depend on non-release features,
such as the disabled ProtoBuf and PreConnect builds. Check the actual release
options before including a type from a registration list or source directory.
Some runnable types, such as `DomainResolver` and `UserController`, live under
`tunnels/Internals/`.

For every included node, record the following in `nodes`.

- Source path and declared previous and next ports, layer groups, and dual-ended
  behavior. Check any settings that disable or change a port.
- Constructor settings and inherited helpers. Capture required and optional
  keys, nested fields, accepted JSON forms, choices, aliases, ranges,
  defaults, and UI labels and hints. Record required-but-generated settings
  and default-dependent behavior in the relevant metadata. When the core
  supplies a default, the form can describe it while keeping the override
  absent until edited.
- Branches and references to other nodes, including collections of links,
  aliases, target-type restrictions, and conditions that make a link active.
  Check startup, chain, or link resolution code for constraints that appear
  outside the constructor.
- Dependencies between fields and graph constraints. Use `rules` for form
  settings and `graph_rules` for checks on whole nodes in a config. See the
  [rule expression reference](schematic-rules.md).
- An empty `fields` or `links` array when the constructor or graph exposes
  none. Imported values outside the form can remain editable through Custom
  JSON. Verify upstream restrictions for those values too.

Use the tagged node `description.md`, examples, and upstream tests as
cross-checks. Where their descriptions and the release parser differ, follow
the release parser and record the relevant distinction in the hint.

For example, a registered node with an optional integer setting can use this
entry under its registered type name.

```json
{
  "ports": { "previous": true, "next": false, "dual_ended": false },
  "fields": [
    {
      "key": "idle-timeout-ms",
      "label": "Idle timeout (ms)",
      "kind": "integer",
      "minimum": 1,
      "maximum": 60000,
      "hint": "Defaults to 5000 milliseconds."
    }
  ],
  "links": [],
  "rules": [],
  "graph_rules": []
}
```

Every node setting of kind `choice` needs a nonempty `choices` array of
strings, including settings nested in `children`. The `object` and
`object_list` kinds take nested `children`. Use `required`, `nullable`, and
`aliases` to match how the constructor reads a value. Other kinds include
`text`, `boolean`, `number`, `string_list`, `array`, `text_or_list`,
`integer_or_boolean`, `port`, `port_list`, `port_range`, `json`, and
`node_target`. Use `node_target` and the matching entry in `links` for
a graph-visible target. A `branch` carries traffic and a `reference` names
a dependency. Review the corresponding examples in the bundled file for
nested settings objects, arrays of routes, and conditional links.

## Record core and integration behavior

Review `core/core_settings.c` and `core/readme.md` alongside the release's
startup options and config loader.

| Section | What to verify |
| --- | --- |
| `core_settings` | Startup config list key, ordered tabs and field groups, JSON pointer paths, fields, ranges, choices, defaults, legacy forms, and help. |
| `config` | Config version keys and import behavior. Review `ww/node_builder/config_file.c` and `config_lexical.c` for comment and variable rules. |
| `runtime` | Startup argument, log directory and filenames, dated file and line format, log levels, and the offline database and authenticated API protocol. |
| `ui.users` | User fields, limits, defaults, labels, hints, and activity displayed by the corresponding user integration. |

Core field kinds are `integer`, `boolean`, `text`, `string_list`, `choice`,
and `flags`. Choices carry their saved JSON `value` and display `label`. Flags
also carry explicit bit values so legacy masks can be decoded. Use
`legacy_values`, `aliases`, and `case` to describe accepted legacy choice
forms where applicable. The optional `invalid` message can explain a
field-specific check. Group `preserve_empty` retains an explicit empty object
where that object changes the core's behavior.

For a user integration, review the AuthenticationServer database reader and
list-users implementation, then check the APIServer access checks, codec,
and client examples. Update the registered roles, setting keys, database
defaults, API listener types, and optional starter template and substitution
slots in `config`. The starter template should contain placeholders for
credentials generated by Wave. Wave keeps the template's top-level members,
defaults absent `config-version` and `core-minimum-version` to `1` and `0`,
and supplies the config name, credentials, port, and database paths.

When a node enables `user_database` with `auth-json-and-api-v1`, define
`ui.users.fields.id` with key `id` and path `/id`, and
`ui.users.fields.password` with key `password`. These keys refer to the
adapter's canonical fields. The optional `group` control uses key `gid`
and path `/gid`. The optional `name`, `email`, `notes`, and `enabled`
controls use their role names as keys. The optional `network` control uses
key `wireguard-allowed-ips`. Each path in `access_fields` or `limit_groups`
must identify an editable numeric field accepted by the adapter. A release
without user integration can omit `ui.users`.

An editable numeric descriptor can omit either `minimum` or `maximum`.
The adapter supplies the missing endpoint from its inclusive range. An
explicit ID ranges from `1` through `UINT64_MAX`, while
`record-stat-interval-ms` ranges from `0` through `INT_MAX`. Other editable
unsigned integers, including the group ID, range from `0` through
`UINT64_MAX`. Explicit bounds can narrow these ranges. Leave the user ID
blank in the editor to let Wave assign one automatically.

For a quota displayed with `"gigabytes": true`, specify the bounds in bytes.
The editor accepts three decimal places in GB, so the range must include
at least one multiple of 1,000,000 bytes. For example, a range of 500,000
to 2,500,000 bytes allows `0.001` and `0.002` GB. To accept exact byte
amounts, omit `gigabytes` and label the field in bytes.

Keep `schema_version` at `1` for files using the current format. Its runtime
adapters recognize `config-stdin` for startup, `dated-files` for logs, and
`auth-json-and-api-v1` or `none` for the user integration. Use an existing
adapter only when its protocol matches the release. A change to the protocol,
config structure, or a rule the current engine cannot express requires a Wave
adapter or format change before that release can use it.

## Validate and publish the release

1. Save `backend/schematics/waterwall-<version>.json` in the Wave project.
   The filename and declared release version must agree. The following
   commands validate the bundled file after building the backend.

   ```sh
   cmake --build --preset backend-release
   build/backend-release/wave-backend --check-schematic backend/schematics/waterwall-1.6.0.json
   ```

2. Test in an isolated Wave deployment with the matching WaterWall binary.
   Check the node catalog and graph ports. Exercise required fields, alias
   normalization, edge values, branch links, and preserved imported configs.
   Open the core settings, configure and start a minimal chain, check log
   sources, and exercise the offline and live user views if supported. Include
   examples and fixtures from the exact tag and keep any compatibility tests
   with the corresponding Wave code.
3. Publish the reviewed file to `backend/schematics/` on the Wave GitHub
   repository's `main` branch. The file becomes discoverable by deployed Wave
   backends once it is on that branch. Keep the existing package filenames
   and hashes when publishing a correction to a schematic for an installed
   release. Wave reports a conflict if a published correction changes a
   selected or installed release's package descriptors.
4. In an isolated deployment, click **Check for updates** and confirm
   that the new release appears in **Target version**. With the core stopped,
   select it and click **Change core version**. Verify the archive installation,
   selected release, and reloaded settings and config forms before starting
   the new core.

The backend caches each published file at
`waterwall_bin/<version>/waterwall-<version>.json` beside `wave-backend`.
It verifies the bytes against the Git blob ID from the directory listing and
validates the schematic's format and runtime protocols before publishing it
to the cache. Checks retain valid files already in the cache. A correction
with unchanged package descriptors can be picked up on backend startup or by
switching away from and back to that release.

The native Wave executable embeds the reviewed 1.6.0 schematic and seeds
its cache from that copy on a fresh installation. To provision 1.6.0
offline, put the executable with execute permission at
`waterwall_bin/1.6.0/Waterwall`. For another release, provide its reviewed
schematic and its executable together in `waterwall_bin/<version>/`.

For existing deployments, Wave imports missing schematics from
`waterwall_bin/schematics/` and `schematics/` beside the backend executable.
It imports the legacy `waterwall_bin/Waterwall` into the 1.6.0 directory.
Version-folder files take precedence and the legacy originals are preserved.
You can preselect a release by setting `waterwall_version` in `wave-db.json`
with the Wave backend stopped. Wave loads 1.6.0 when the property is absent.

Refer to
[Panel Controls and versioning](building.md#panel-controls-and-versioning)
for lifecycle and deployment details.
