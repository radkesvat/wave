#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 || EUID == 0 )); then
    printf 'Run as an ordinary user with systemd and non-interactive sudo: smoke.sh RELEASE_ASSETS TAG\n' >&2
    exit 1
fi

fail() {
    printf 'Wave release smoke test: %s\n' "$*" >&2
    exit 1
}

assets=$(realpath -e -- "$1")
tag=$2
systemctl show --property=Version >/dev/null || fail 'A running systemd system manager is required.'
sudo -n true || fail 'Authenticate sudo before running this smoke test.'
work_dir=$(mktemp -d)
work_dir=$(realpath -e -- "$work_dir")
install_parent="$work_dir/instances with % and \$"
first_install="$install_parent/wave"
second_install="$install_parent/wave2"
first_unit="wave-instance-$(systemd-escape --path -- "$first_install").service"
second_unit="wave-instance-$(systemd-escape --path -- "$second_install").service"
system_units=/etc/systemd/system

cleanup() {
    local result=$?
    local instance unit manager
    for instance in "$first_install" "$second_install"; do
        manager="$work_dir/bin/${instance##*/}"
        if [[ -x $manager ]]; then
            if (( result != 0 )); then
                "$manager" logs panel >&2 || true
            fi
            printf '%s\n' "${instance##*/}" | "$manager" uninstall || true
        fi
    done
    for unit in "$first_unit" "$second_unit"; do
        if [[ -e $system_units/$unit ]]; then
            sudo -n systemctl disable --now "$unit" || result=1
            sudo -n rm -f -- "$system_units/$unit" || result=1
            sudo -n systemctl daemon-reload || result=1
        fi
    done
    rm -rf -- "$work_dir"
    exit "$result"
}
trap cleanup EXIT

export HOME="$work_dir/home" XDG_BIN_HOME="$work_dir/bin"
mkdir -- "$HOME" "$XDG_BIN_HOME" "$install_parent"
chmod g+s -- "$install_parent"
setfacl -m d:g::rx -- "$install_parent"
export PATH="$XDG_BIN_HOME:$PATH"

read -r first_port second_port < <(python3 <<'PY'
from socket import socket

with socket() as first, socket() as second:
    for listener in (first, second):
        listener.bind(("127.0.0.1", 0))
    print(first.getsockname()[1], second.getsockname()[1])
PY
)

curl_local() {
    curl -q --fail --silent --show-error --noproxy '*' --max-time 8 "$@"
}

check_setup() {
    local instance=$1 port=$2
    local url="http://127.0.0.1:$port"
    curl_local --location "$url/" --output "$work_dir/setup.html"
    cmp -- "$instance/site/index.html" "$work_dir/setup.html"
    curl_local "$url/_wave-setup/index.wasm" --output "$work_dir/setup.wasm"
    cmp -- "$instance/site/index.wasm" "$work_dir/setup.wasm"
}

install_wave() {
    local input=$1 instance=$2 port=$3
    shift 3
    bash "$assets/install.sh" --from-dir "$input" --version "$tag" --yes \
        --path "$install_parent/$instance" --port "$port" "$@"
}

check_boot_service() {
    local instance=$1 unit=$2 record
    [[ $(stat -c '%a' -- "$instance") == 700 && $(stat -c '%a' -- "$instance/.wave-run") == 700 ]] \
        || fail 'Inherited permissions left the installation or management directory without mode 0700.'
    for record in wave-db.json .wave-run/command-link .wave-run/systemd-unit; do
        [[ $(stat -c '%a' -- "$instance/$record") == 600 ]] || fail "$record does not have mode 0600."
    done
    systemctl is-enabled --quiet "$unit" || fail "$unit is not enabled."
    [[ $system_units/$unit -ef $system_units/multi-user.target.wants/$unit ]] \
        || fail "$unit is missing its persistent boot target link."
    [[ $(systemctl show --value --property=User "$unit") == "$EUID" ]] \
        || fail "$unit is not configured to run as the installing user."
    [[ $(<"$instance/.wave-run/systemd-unit") == "$unit" ]] \
        || fail 'The installation did not record its system service.'
}

check_running_service() {
    local unit=$1 pid
    systemctl is-active --quiet "$unit" || fail "$unit is not active."
    pid=$(systemctl show --value --property=MainPID "$unit")
    [[ $pid =~ ^[1-9][0-9]*$ && $(stat -c '%u' "/proc/$pid") == "$EUID" ]] \
        || fail "$unit is not running under the installing account."
}

check_removed_service() {
    local unit=$1
    if [[ -e $system_units/$unit || -L $system_units/multi-user.target.wants/$unit ]] ||
        systemctl is-enabled --quiet "$unit" >/dev/null 2>&1 || systemctl is-active --quiet "$unit"; then
        fail "Uninstall or a refused installation left $unit behind."
    fi
}

archive="wave-$tag-linux-$(uname -m).zip"
tampered="$work_dir/tampered-assets"
mkdir -- "$tampered"
cp -- "$assets/$archive" "$assets/SHA256SUMS" "$tampered/"
printf 'x' >> "$tampered/$archive"
if install_wave "$tampered" wave "$first_port" --no-start; then
    fail 'The installer accepted a ZIP with a mismatched checksum.'
fi
[[ ! -e $first_install && ! -L $XDG_BIN_HOME/wave ]] || fail 'Checksum rejection left files behind.'
check_removed_service "$first_unit"

install_wave "$assets" wave "$first_port" --no-start
[[ -L $XDG_BIN_HOME/wave && -x $XDG_BIN_HOME/wave ]]
[[ -s $first_install/site/licenses/musl.txt ]] || fail 'The release ZIP did not include the musl copyright notice.'
check_boot_service "$first_install" "$first_unit"
if systemctl is-active --quiet "$first_unit"; then
    fail 'Wave started despite --no-start.'
fi
wave start
check_running_service "$first_unit"
status=$(wave status)
[[ $status == *"http://127.0.0.1:$first_port/"* ]] || fail 'Status did not report the listening URL.'
check_setup "$first_install" "$first_port"

if install_wave "$assets" wave/wave2 "$second_port" --no-start; then
    fail 'The installer allowed one Wave instance inside another.'
fi
[[ ! -e $first_install/wave2 && ! -L $XDG_BIN_HOME/wave2 ]] || fail 'Nested installation left files behind.'

if install_wave "$assets" wave2 "$first_port" --no-start; then
    fail 'The installer accepted a port already in use.'
fi
[[ ! -e $second_install && ! -L $XDG_BIN_HOME/wave2 ]] || fail 'A refused installation left files behind.'
check_removed_service "$second_unit"
install_wave "$assets" wave2 "$second_port"
[[ -L $XDG_BIN_HOME/wave2 && -x $XDG_BIN_HOME/wave2 ]]
check_boot_service "$second_install" "$second_unit"
check_running_service "$second_unit"
check_setup "$second_install" "$second_port"

wave stop
if systemctl is-active --quiet "$first_unit" ||
    curl -q --silent --noproxy '*' --max-time 1 \
    "http://127.0.0.1:$first_port/" >/dev/null 2>&1; then
    fail 'Stopped instance still accepts requests.'
fi
systemctl is-enabled --quiet "$first_unit" || fail 'Stopping Wave disabled its boot service.'
check_running_service "$second_unit"
check_setup "$second_install" "$second_port"
wave start
old_pid=$(systemctl show --value --property=MainPID "$first_unit")
wave restart
check_running_service "$first_unit"
[[ $(systemctl show --value --property=MainPID "$first_unit") != "$old_pid" ]] || fail 'Wave retained its old process after restart.'
check_setup "$first_install" "$first_port"
settings=$(wave settings)
[[ $settings == *"Port: $first_port"* && $settings == *'TLS: disabled'* ]] || fail 'Settings did not show the port and TLS.'
panel_logs=$(wave logs panel)
[[ $panel_logs == *'Wave started.'* ]] || fail 'The panel log did not contain the startup event.'
wave logs core
printf '\n' | wave2 uninstall
check_boot_service "$second_install" "$second_unit"
check_setup "$second_install" "$second_port"
printf 'wave2\n' | XDG_BIN_HOME="$work_dir/unused-bin" "$second_install/wave2" uninstall
[[ ! -e $second_install && ! -L $XDG_BIN_HOME/wave2 ]] || fail 'Uninstall left the instance or command link.'
check_removed_service "$second_unit"
check_setup "$first_install" "$first_port"
# The core shutdown checks start a new test phase on this fixture unit.
sudo -n systemctl reset-failed "$first_unit"
node "$(dirname -- "${BASH_SOURCE[0]}")/smoke-core.cjs" "$first_install" "$first_port"
printf 'wave\n' | wave uninstall
[[ ! -e $first_install && ! -L $XDG_BIN_HOME/wave ]] || fail 'Uninstall left the first instance or command link.'
check_removed_service "$first_unit"

sudo -n -- /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    /bin/bash "$(dirname -- "${BASH_SOURCE[0]}")/smoke-root.sh" "$assets" "$tag"
