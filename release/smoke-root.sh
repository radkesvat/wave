#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 || EUID != 0 )); then
    printf 'Run as root with systemd: smoke-root.sh RELEASE_ASSETS TAG\n' >&2
    exit 1
fi

fail() {
    printf 'Wave root release smoke test: %s\n' "$*" >&2
    exit 1
}

assets=$(realpath -e -- "$1")
tag=$2
work_dir=$(mktemp -d -- /opt/.wave-root-smoke.XXXXXXXX)
suffix=${work_dir##*.}
first_name=wave-root-smoke-$suffix
second_name=$first_name-2
system_units=/etc/systemd/system
declare -A original_users=() service_users=() service_uids=()

while IFS=: read -r account _; do
    original_users["$account"]=1
done < /etc/passwd

unit_name() {
    printf 'wave-instance-%s.service\n' "$(systemd-escape --path -- "$work_dir/$1")"
}

service_account() {
    local account record username uid
    account=$(systemctl show --property=User --value -- "$1") || return 1
    [[ -n $account ]] || return 1
    record=$(getent -s files passwd "$account") || return 1
    IFS=: read -r username _ uid _ <<< "$record"
    [[ $uid =~ ^[1-9][0-9]*$ && ! ${original_users[$username]+present} ]] || return 1
    printf '%s:%s\n' "$username" "$uid"
}

cleanup() {
    local result=$?
    local name unit account
    for name in "$first_name" "$second_name"; do
        unit=$(unit_name "$name")
        if [[ -f $system_units/$unit ]]; then
            if account=$(service_account "$unit"); then
                service_users["$name"]=${account%:*}
            fi
            if (( result != 0 )); then
                journalctl --no-pager --lines=40 --unit="$unit" >&2 || true
            fi
            systemctl disable --now -- "$unit" || result=1
            rm -- "$system_units/$unit" || result=1
            systemctl daemon-reload || result=1
        fi
        if [[ ${service_users[$name]+present} ]] &&
            getent -s files passwd "${service_users[$name]}" >/dev/null; then
            userdel -- "${service_users[$name]}" || result=1
        fi
    done
    rm -rf --one-file-system -- "$work_dir"
    exit "$result"
}
trap cleanup EXIT

chmod 0755 -- "$work_dir"
export XDG_BIN_HOME="$work_dir/bin"
mkdir -m 0700 -- "$XDG_BIN_HOME" "$work_dir/tools"
cat > "$work_dir/tools/sudo" <<'EOF'
#!/usr/bin/env bash
printf 'A root invocation tried to use sudo.\n' >&2
exit 1
EOF
chmod 0700 -- "$work_dir/tools/sudo"
export PATH="$work_dir/tools:$XDG_BIN_HOME:$PATH"

read -r first_port second_port < <(python3 <<'PY'
from socket import socket

with socket() as first, socket() as second:
    for listener in (first, second):
        listener.bind(("127.0.0.1", 0))
    print(first.getsockname()[1], second.getsockname()[1])
PY
)

install_wave() {
    local name=$1 port=$2
    shift 2
    bash "$assets/install.sh" --from-dir "$assets" --version "$tag" --yes \
        --path "$work_dir/$name" --port "$port" "$@"
}

check_installation() {
    local name=$1
    local instance=$work_dir/$name
    local data=$instance/.wave-data
    local manager=$instance/$name
    local unit account username uid path
    unit=$(unit_name "$name")
    account=$(service_account "$unit") || fail 'The systemd service does not use a newly created non-root account.'
    IFS=: read -r username uid <<< "$account"
    service_users["$name"]=$username
    service_uids["$name"]=$uid
    systemctl is-enabled --quiet "$unit" || fail 'The root-installed service is not enabled.'
    [[ -O $instance && -O $manager && $(stat -c '%u:%a' -- "$instance/.wave-run") == 0:700 ]] \
        || fail 'The root manager and its state are not owned by root.'
    [[ $(readlink -- "$XDG_BIN_HOME/$name") == "$manager" ]] \
        || fail 'The installer did not create the root command link.'
    [[ $(stat -c '%u:%a' -- "$data") == "$uid:700" && \
        $(stat -c '%u:%a' -- "$data/.wave-run") == "$uid:700" && \
        $(stat -c '%u:%a' -- "$data/wave-db.json") == "$uid:600" && \
        -x $data/wave-backend && -s $data/site/index.wasm ]] \
        || fail 'The service account could not own a private Wave data directory.'
    for path in "$instance" "$manager" "$instance/.wave-run" "$XDG_BIN_HOME"; do
        runuser --user "$username" -- test ! -w "$path" \
            || fail "The service account can modify $path."
    done
}

check_running() {
    local name=$1 port=$2 unit pid status
    unit=$(unit_name "$name")
    systemctl is-active --quiet "$unit" || fail 'The root-installed service is not running.'
    pid=$(systemctl show --property=MainPID --value -- "$unit")
    [[ $pid =~ ^[1-9][0-9]*$ && $(stat -c '%u' -- "/proc/$pid") == "${service_uids[$name]}" ]] \
        || fail 'The running backend has the wrong service account.'
    status=$("$name" status)
    [[ $status == *"Panel URL http://127.0.0.1:$port/"* ]] || fail 'Root could not read the panel URL.'
    curl -q --fail --silent --show-error --noproxy '*' --max-time 8 \
        "http://127.0.0.1:$port/_wave-setup/index.wasm" --output "$work_dir/setup.wasm"
    cmp -- "$work_dir/$name/.wave-data/site/index.wasm" "$work_dir/setup.wasm"
}

install_wave "$first_name" "$first_port" --no-start
check_installation "$first_name"
first_unit=$(unit_name "$first_name")
if systemctl is-active --quiet "$first_unit"; then
    fail 'The root-installed service started despite --no-start.'
fi
"$first_name" start
check_running "$first_name" "$first_port"

install_wave "$second_name" "$second_port"
check_installation "$second_name"
check_running "$second_name" "$second_port"
[[ ${service_uids[$first_name]} != "${service_uids[$second_name]}" ]] \
    || fail 'The root-installed instances share a service account.'
runuser --user "${service_users[$first_name]}" -- test ! -r "$work_dir/$second_name/.wave-data/wave-db.json" \
    || fail 'The first service account can read the second instance database.'

settings=$("$first_name" settings)
[[ $settings == *"Port: $first_port"* ]] || fail 'The root manager did not show saved settings.'
panel_logs=$("$first_name" logs panel)
[[ $panel_logs == *'Wave started.'* ]] || fail 'The root manager did not show the panel journal.'
"$first_name" logs core
old_pid=$(systemctl show --property=MainPID --value -- "$first_unit")
"$first_name" restart
check_running "$first_name" "$first_port"
[[ $(systemctl show --property=MainPID --value -- "$first_unit") != "$old_pid" ]] \
    || fail 'Restart kept the old backend process.'
"$first_name" stop
if systemctl is-active --quiet "$first_unit"; then
    fail 'Root could not stop the service.'
fi
check_running "$second_name" "$second_port"

for name in "$first_name" "$second_name"; do
    printf '%s\n' "$name" | "$name" uninstall
    unit=$(unit_name "$name")
    [[ ! -e $work_dir/$name && ! -L $XDG_BIN_HOME/$name && ! -e $system_units/$unit &&
        ! -L $system_units/multi-user.target.wants/$unit ]] \
        || fail 'Uninstall left a root instance, command, or service behind.'
    if getent -s files passwd "${service_users[$name]}" >/dev/null; then
        fail 'Uninstall left the dedicated Wave account behind.'
    fi
done
