#!/usr/bin/env bash
set -euo pipefail
umask 077

repository=radkesvat/wave
release_pattern='v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-beta)?'
version=
instance=
port=
source_dir=
assume_yes=false
start=true

usage() {
    cat <<'EOF'
Usage: bash install.sh [options]
  --version TAG    Wave release tag, or latest (default)
  --path PATH      Absolute, unused installation directory (default /opt/wave, /opt/wave2, ...)
  --port PORT      Loopback listener port (default first available from 2045)
  --from-dir DIR   Read the ZIP and SHA256SUMS from DIR for an explicit tag
  --yes            Use supplied values and defaults without prompting
  --no-start       Enable Wave at boot without starting it now
  --help           Show this help
EOF
}

fail() {
    printf 'Wave installer: %s\n' "$*" >&2
    exit 1
}

while (($#)); do
    case $1 in
        --version|--path|--port|--from-dir)
            [[ $# -gt 1 && -n $2 && $2 != --* ]] || fail "Missing value for $1."
            case $1 in
                --version) version=$2 ;;
                --path) instance=$2 ;;
                --port) port=$2 ;;
                --from-dir) source_dir=$2 ;;
            esac
            shift 2
            ;;
        --yes) assume_yes=true; shift ;;
        --no-start) start=false; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "Unknown option $1. See --help." ;;
    esac
done

[[ $(uname -s) == Linux ]] || fail 'Wave release packages require Linux.'
architecture=$(uname -m)
case $architecture in
    x86_64|aarch64) ;;
    amd64) architecture=x86_64 ;;
    arm64) architecture=aarch64 ;;
    *) fail "Unsupported Linux architecture $architecture." ;;
esac

dependencies=(curl python3 flock systemctl systemd-escape journalctl)
admin=()
if (( EUID == 0 )); then
    dependencies+=(setpriv useradd userdel nologin)
    default_bin=/usr/local/bin
else
    dependencies+=(sudo)
    admin=(sudo --)
    [[ ${HOME:-} == /* ]] || fail 'Set HOME to your absolute home directory.'
    default_bin=$HOME/.local/bin
fi
for dependency in "${dependencies[@]}"; do
    command -v "$dependency" >/dev/null || fail "Install $dependency and rerun the installer."
done
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' || fail 'Install Python 3.9 or later and rerun the installer.'
systemctl show --property=Version --value >/dev/null 2>&1 || fail 'Wave requires a running systemd system manager.'

bin_dir=${XDG_BIN_HOME:-$default_bin}
[[ $bin_dir == /* && ! $bin_dir =~ [[:cntrl:]] ]] || fail 'Use an absolute command directory.'
bin_dir=$(realpath -m -- "$bin_dir")

if ! $assume_yes; then
    if ! { exec 3<>/dev/tty; } 2>/dev/null; then
        fail 'Open a terminal for setup prompts or use --yes with your preferred options.'
    fi
fi

ask() {
    printf '%s' "$1" >&3
    IFS= read -r answer <&3 || fail 'Setup input ended.'
}

confirm() {
    ask "$1"
    case $answer in
        '') [[ ${2:-yes} == yes ]] ;;
        y|Y|yes|Yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

work=$(mktemp -d)
pending_directory=
created_link=
manager_target=
unit_stage=
created_unit=
created_account=
elevate=()
as_service=()

cleanup() {
    if [[ -n $created_unit ]]; then
        "${admin[@]}" systemctl disable -- "$unit" || :
        "${admin[@]}" rm -f -- "$created_unit" || :
        "${admin[@]}" systemctl daemon-reload || :
    fi
    if [[ -n $unit_stage ]]; then
        "${admin[@]}" rm -f -- "$unit_stage" || :
    fi
    if [[ -n $created_link && $(readlink -- "$created_link" 2>/dev/null || :) == "$manager_target" ]]; then
        rm -f -- "$created_link"
    fi
    if [[ -n $pending_directory && -d $pending_directory ]]; then
        find "$pending_directory" -xdev -mindepth 1 -delete || :
        "${elevate[@]}" rmdir -- "$pending_directory" || :
    fi
    if [[ -n $created_account ]]; then
        userdel -- "$created_account" || :
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

curl_https() {
    curl -q --silent --show-error --location --proto '=https' --proto-redir '=https' \
        --connect-timeout 15 "$@"
}

choose_release() {
    python3 - "$architecture" "$release_pattern" "$@" <<'PY'
import json
import re
import sys

architecture, pattern, *pages = sys.argv[1:]
candidates = []
for page in pages:
    with open(page, encoding='utf-8') as source:
        response = json.load(source)
    releases = response if isinstance(response, list) else [response]
    for release in releases:
        tag = release['tag_name']
        version = re.fullmatch(pattern, tag)
        if not version or release['draft']:
            continue
        is_beta = version[4] is not None
        if release['prerelease'] != is_beta:
            continue
        assets = {asset['name'] for asset in release['assets']}
        if {'SHA256SUMS', f'wave-{tag}-linux-{architecture}.zip'} <= assets:
            candidates.append((not is_beta, tuple(map(int, version.groups()[:3])), tag))
if candidates:
    print(max(candidates)[2])
PY
}

if [[ -z $version ]] && ! $assume_yes; then
    ask 'Wave version [latest]: '
    version=$answer
fi
version=${version:-latest}

if [[ $version == latest ]]; then
    [[ -z $source_dir ]] || fail '--from-dir requires an explicit --version tag.'
    api="https://api.github.com/repos/$repository/releases"
    status=$(curl_https --max-time 30 --output "$work/release.json" --write-out '%{http_code}' "$api/latest") \
        || fail 'Could not check GitHub for the latest release.'
    case $status in
        200)
            version=$(choose_release "$work/release.json") || fail 'Could not read the latest Wave release.'
            ;;
        404) version= ;;
        *) fail "GitHub returned HTTP $status while checking the latest release." ;;
    esac
    if [[ -z $version ]]; then
        release_pages=()
        page=1
        while :; do
            page_file="$work/releases-$page.json"
            curl_https --fail --max-time 30 --output "$page_file" --dump-header "$work/headers" \
                "$api?per_page=100&page=$page" || fail 'Could not list Wave releases on GitHub.'
            release_pages+=("$page_file")
            grep -iqE '^link:.*; *rel="next"' "$work/headers" || break
            ((page += 1))
        done
        version=$(choose_release "${release_pages[@]}") || fail 'Could not read published Wave releases.'
    fi
    [[ -n $version ]] || fail "No Wave release package is available for Linux $architecture."
    if [[ $version == *-beta ]]; then
        if $assume_yes; then
            fail "No stable Wave package is available. To install the beta, specify --version $version."
        fi
        confirm "No stable package is available. Install $version? [y/N] " no || exit 0
    fi
else
    version=v${version#v}
fi
[[ $version =~ ^${release_pattern}$ ]] || fail 'Enter a Wave tag such as v1.2.3 or v1.2.3-beta.'

asset="wave-$version-linux-$architecture.zip"
if [[ -n $source_dir ]]; then
    [[ -r $source_dir/$asset && -r $source_dir/SHA256SUMS ]] \
        || fail "Could not read $asset and SHA256SUMS in $source_dir."
    cp -- "$source_dir/$asset" "$source_dir/SHA256SUMS" "$work/"
else
    download="https://github.com/$repository/releases/download/$version"
    printf 'Downloading Wave %s for Linux %s.\n' "$version" "$architecture"
    curl_https --fail --max-time 60 --output "$work/SHA256SUMS" "$download/SHA256SUMS" \
        || fail "Could not download checksums for $version."
    curl_https --fail --max-time 300 --output "$work/$asset" "$download/$asset" \
        || fail "Could not download $asset."
fi

name_available() {
    [[ ! -e $1 && ! -L $1 && ! -e $bin_dir/$2 && ! -L $bin_dir/$2 ]] && ! command -v "$2" >/dev/null
}

trusted_root_path() {
    local directory=$1 info owner mode
    while :; do
        if [[ -e $directory ]]; then
            [[ -d $directory ]] || return 1
            info=$(stat -c '%u:%a' -- "$directory") || return 1
            IFS=: read -r owner mode <<< "$info"
            [[ $owner == 0 && $((8#$mode & 8#022)) == 0 ]] || return 1
        fi
        [[ $directory == / ]] && return 0
        directory=$(dirname -- "$directory")
    done
}

if [[ -z $instance ]]; then
    default_name=wave
    number=2
    until name_available "/opt/$default_name" "$default_name"; do
        default_name=wave$number
        ((number += 1))
    done
    instance=/opt/$default_name
    if ! $assume_yes; then
        ask "Installation directory [$instance]: "
        instance=${answer:-$instance}
    fi
fi

[[ $instance == /* && ! $instance =~ [[:cntrl:]] ]] || fail 'Enter an absolute installation directory.'
name=$(basename -- "$instance")
[[ $name =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || fail 'Use letters, digits, dots, underscores or hyphens in the instance name.'
case $name in
    wave-db.json|configs|waterwall_bin|logs|schematics) fail "The name $name is reserved by Wave." ;;
esac
IFS= read -r -d '' parent < <(realpath -ez -- "$(dirname -- "$instance")") \
    || fail 'Choose an existing parent directory.'
[[ -d $parent ]] || fail 'Choose an existing parent directory.'
instance=${parent%/}/$name
[[ ! $instance =~ [[:cntrl:]] ]] || fail 'The resolved installation path cannot contain control characters.'
name_available "$instance" "$name" || fail "The directory or command name $name is already in use."
unit="wave-instance-$(systemd-escape --path -- "$instance").service"
(( ${#unit} <= 255 )) || fail 'Choose a shorter path for the systemd service name.'
unit_path=/etc/systemd/system/$unit
if [[ -e $unit_path || -L $unit_path || $(systemctl show --property=LoadState --value -- "$unit") != not-found ]]; then
    fail "The systemd service $unit already exists or is unavailable."
fi

directory=$parent
while [[ $directory != / ]]; do
    if [[ ( -x $directory/wave-backend && -f $directory/site/index.wasm ) ||
          -f $directory/.wave-run/service-user ]]; then
        fail "Choose a directory outside the existing Wave installation at $directory."
    fi
    directory=$(dirname -- "$directory")
done

if (( EUID == 0 )); then
    trusted_root_path "$parent" || fail "Choose a root-owned installation parent such as /opt."
    trusted_root_path "$bin_dir" || fail "Choose a root-owned command directory such as /usr/local/bin."
elif [[ ! -w $parent ]]; then
    trusted_root_path "$parent" || fail "Choose a writable parent directory instead of $parent."
    elevate=("${admin[@]}")
fi

staging=$("${elevate[@]}" mktemp -d -- "${parent%/}/.wave-install.XXXXXXXX") \
    || fail "Could not prepare an installation directory in $parent."
pending_directory=$staging
if ((${#elevate[@]})); then
    "${elevate[@]}" chown -- "$(id -u):$(id -g)" "$staging"
fi
# Clear any setgid bit inherited from the parent before creating instance files.
chmod 00700 -- "$staging"
data=$staging
if (( EUID == 0 )); then
    data=$staging/.wave-data
    mkdir -m 0700 -- "$data"
fi

python3 - "$work/$asset" "$work/SHA256SUMS" "$data" <<'PY' || fail 'Release verification or extraction failed.'
from pathlib import Path
import hashlib
import re
import stat
import sys
import zipfile

asset, sums, destination = map(Path, sys.argv[1:])
matches = []
for line in sums.read_text(encoding='utf-8').splitlines():
    match = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', line)
    if match and match[2] == asset.name:
        matches.append(match[1].lower())

digest = hashlib.sha256()
with asset.open('rb') as source:
    for chunk in iter(lambda: source.read(1024 * 1024), b''):
        digest.update(chunk)
if matches != [digest.hexdigest()]:
    sys.exit(f'SHA-256 verification failed for {asset.name}.')

with zipfile.ZipFile(asset) as archive:
    entries = {}
    for entry in archive.infolist():
        name = entry.orig_filename
        components = name.removesuffix('/').split('/')
        key = '/'.join(components)
        expected = stat.S_IFDIR if entry.is_dir() else stat.S_IFREG
        mode = stat.S_IFMT(entry.external_attr >> 16)
        if (any(part in ('', '.', '..') for part in components)
                or any(ord(character) < 32 or character == '\\' for character in name)
                or mode not in (0, expected) or key in entries):
            sys.exit(f'Unsafe ZIP entry {name!r}.')
        entries[key] = entry.is_dir()

    files = {name for name, is_directory in entries.items() if not is_directory}
    required = {'wave-backend', 'wave-manager', 'site/index.html', 'site/index.js', 'site/index.wasm'}
    if not required <= files:
        sys.exit('Wave release ZIP is missing required files.')

    archive.extractall(destination)
PY

chmod 0700 -- "$data/wave-backend" "$data/wave-manager"
backend=$data/wave-backend
service_user=$EUID
service_options=
if (( EUID == 0 )); then
    if ! grep -Fxq '# Supports root installation' -- "$data/wave-manager"; then
        fail "Wave $version predates root installation support. Choose a newer Wave release."
    fi
    suffix=${staging##*.}
    service_user=wave-${suffix,,}
    useradd --system --user-group --no-create-home --home-dir "$instance/.wave-data" \
        --shell "$(command -v nologin)" -- "$service_user" || fail 'Could not create the Wave service account.'
    created_account=$service_user
    service_uid=$(id -u -- "$service_user")
    service_gid=$(id -g -- "$service_user")
    [[ $service_uid != 0 && $service_gid != 0 ]] || fail 'The Wave service account must be unprivileged.'
    as_service=(setpriv --reuid "$service_uid" --regid "$service_gid" --clear-groups --no-new-privs --reset-env --)
    "${as_service[@]}" test -x "$parent" || fail 'Choose an installation parent the service account can access, such as /opt.'
    chgrp -- "$service_gid" "$staging" "$data" "$backend" "$data/wave-manager"
    chmod 00750 -- "$staging" "$data"
    chmod 0750 -- "$backend" "$data/wave-manager"
    service_options=$'NoNewPrivileges=true\n'
fi
if ! built_version=$("${as_service[@]}" "$backend" --version); then
    fail 'The packaged backend could not run. Check the documented Linux and library requirements.'
fi
[[ $built_version == "${version#v}" ]] || fail "The packaged backend version does not match $version."

if [[ -z $port ]]; then
    default_port=2045
    until "${as_service[@]}" "$backend" --check-port "$default_port" >/dev/null 2>&1; do
        ((default_port < 65535)) || fail 'No loopback listener port was available.'
        ((default_port += 1))
    done
    port=$default_port
    if ! $assume_yes; then
        ask "Panel port [$port]: "
        port=${answer:-$port}
    fi
fi
if [[ ! $port =~ ^[1-9][0-9]{0,4}$ ]] || ((port > 65535)); then
    fail 'Enter a port between 1 and 65535.'
fi
"${as_service[@]}" "$backend" --check-port "$port" >/dev/null || fail "The panel cannot use loopback port $port."

if [[ $data/wave-manager != "$staging/$name" ]]; then
    [[ ! -e $staging/$name ]] || fail "The name $name is reserved by the Wave package."
    mv -T -- "$data/wave-manager" "$staging/$name"
fi
printf '{"admin":[],"listen":{"host":"127.0.0.1","port":%s}}\n' "$port" > "$data/wave-db.json"

mkdir -p -- "$bin_dir"
if (( EUID == 0 )); then
    trusted_root_path "$bin_dir" || fail "Choose a root-owned command directory such as /usr/local/bin."
fi
manager_target=$instance/$name
link=$bin_dir/$name
mkdir -m 0700 -- "$staging/.wave-run"
printf '%s\n' "$link" > "$staging/.wave-run/command-link"
printf '%s\n' "$unit" > "$staging/.wave-run/systemd-unit"
# A default ACL can give new files permissions that the umask would have removed.
chmod 0600 -- "$data/wave-db.json" "$staging/.wave-run/command-link" "$staging/.wave-run/systemd-unit"
if (( EUID == 0 )); then
    printf '%s\n' "$service_user" > "$staging/.wave-run/service-user"
    chmod 0600 -- "$staging/.wave-run/service-user"
    mkdir -m 0700 -- "$data/.wave-run"
    chmod 00700 -- "$data"
    chmod 0700 -- "$backend"
    chown -R --no-dereference -- "$service_uid:$service_gid" "$data"
fi

quote_systemd_word() {
    local value=${1//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//%/%%}
    printf '"%s"' "$value"
}

cat > "$work/$unit" <<EOF
# Wave instance service
[Unit]
Description=Wave panel ($name)
After=network.target
RequiresMountsFor=$(quote_systemd_word "$instance")

[Service]
Type=exec
User=$service_user
${service_options}UMask=0077
ExecStart=:/bin/bash $(quote_systemd_word "$manager_target") __run-service
StandardOutput=journal
StandardError=journal
Restart=on-failure
RestartSec=2s
KillMode=mixed
TimeoutStopSec=15s

[Install]
WantedBy=multi-user.target
EOF

ln -sT -- "$manager_target" "$link" || fail "Could not create the command $link."
created_link=$link
"${elevate[@]}" mv -T --no-clobber -- "$staging" "$instance" \
    || fail "Could not install Wave in $instance."
[[ ! -e $staging ]] || fail "The directory $instance is already in use."
pending_directory=$instance

unit_stage=$("${admin[@]}" mktemp -- /etc/systemd/system/.wave-unit.XXXXXXXX) \
    || fail 'Could not prepare the systemd service file.'
"${admin[@]}" install -m 0644 -o root -g root -- "$work/$unit" "$unit_stage" \
    || fail 'Could not write the systemd service file.'
"${admin[@]}" ln -T -- "$unit_stage" "$unit_path" || fail "Could not register $unit."
created_unit=$unit_path
"${admin[@]}" rm -- "$unit_stage"
unit_stage=
"${admin[@]}" systemctl enable -- "$unit" || fail "Could not enable $unit at boot."

pending_directory=
created_link=
created_unit=
created_account=

printf 'Wave %s is installed in %s.\n' "$version" "$instance"
printf 'Systemd service %s is enabled at boot.\n' "$unit"
case :${PATH:-}: in
    *":$bin_dir:"*) printf 'Run %s to manage this instance.\n' "$name" ;;
    *) printf 'Run %q to manage this instance. Add %q to PATH to use %s directly.\n' "$link" "$bin_dir" "$name" ;;
esac

if $start && { $assume_yes || confirm 'Start Wave now? [Y/n] '; }; then
    "$manager_target" start || fail "Wave is installed. Run $name logs panel to inspect the startup failure."
else
    printf 'Start with %q start when ready.\n' "$link"
fi
printf 'First setup URL: http://127.0.0.1:%s/\n' "$port"
printf 'From another computer, forward the port with ssh -N -L 127.0.0.1:%s:127.0.0.1:%s %q@your-server\n' \
    "$port" "$port" "$(id -un)"
