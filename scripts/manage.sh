#!/usr/bin/env bash
# Supports root installation

set -uo pipefail
umask 077

usage() {
    printf 'Usage: %s [status|start|stop|restart|settings|logs panel [--follow]|logs core|uninstall]\n' "$1"
    printf 'Run without arguments to open the instance menu.\n'
}

fail() {
    printf '%s\n' "$*" >&2
    return 1
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage "${0##*/}"
    exit 0
fi

manager=$(readlink -f -- "$0") || exit 1
instance_dir=${manager%/*}
instance_name=${instance_dir##*/}
data_dir=$instance_dir
if [[ $(stat -c %u -- "$instance_dir") == 0 ]]; then
    data_dir=$instance_dir/.wave-data
fi
backend=$data_dir/wave-backend
site_dir=$data_dir/site
state_dir=$instance_dir/.wave-run
runtime_dir=$data_dir/.wave-run
command_file=$state_dir/command-link
service_file=$state_dir/systemd-unit
account_file=$state_dir/service-user
ready_file=$runtime_dir/ready
temporary_dir=$runtime_dir/tmp
service_user=$EUID
as_service=()
admin=()
if (( EUID != 0 )); then
    admin=(sudo --)
fi

check_instance() {
    if [[ ${manager##*/} != "$instance_name" || ! -O $instance_dir || ! -f $backend || -L $backend ||
          ! -x $backend ]]; then
        fail 'Cannot find a Wave installation owned by this account beside this manager.'
        return 1
    fi
    if [[ -L $state_dir || ! -d $state_dir || ! -O $state_dir || $(stat -c %a -- "$state_dir") != 700 ]]; then
        fail 'Wave management files need a mode-0700 directory owned by this account.'
        return 1
    fi
}

read_service_account() {
    local uid gid
    if [[ ! -f $account_file || -L $account_file || ! -O $account_file ||
          $(stat -c %a -- "$account_file") != 600 ]] ||
        ! IFS= read -r service_user < "$account_file" || [[ ! $service_user =~ ^wave-[a-z0-9]{8}$ ]] ||
        ! uid=$(id -u -- "$service_user") || ! gid=$(id -g -- "$service_user") ||
        [[ $uid == 0 || $gid == 0 || -L $data_dir || ! -d $data_dir ||
           $(stat -c '%u:%g' -- "$data_dir") != "$uid:$gid" ]]; then
        fail 'The Wave service account does not match this installation.'
        return 1
    fi
    as_service=(setpriv --reuid "$uid" --regid "$gid" --clear-groups --no-new-privs --reset-env --)
}

read_service_unit() {
    local escaped
    local -a recorded
    if [[ ! -f $service_file || -L $service_file || ! -O $service_file ||
          $(stat -c %a -- "$service_file") != 600 ]] ||
        ! mapfile -t recorded < "$service_file" ||
        ! escaped=$(systemd-escape --path -- "$instance_dir") ||
        [[ $escaped == */* || ${recorded[0]:-} != "wave-instance-$escaped.service" || ${#recorded[@]} != 1 ]]; then
        fail 'The systemd service record does not match this Wave installation.'
        return 1
    fi
    service_unit=${recorded[0]}
    unit_file=/etc/systemd/system/$service_unit
}

with_manager_lock() (
    local lock_path=$state_dir/manager.lock
    if [[ -L $lock_path || ( -e $lock_path && ( ! -f $lock_path || ! -O $lock_path ) ) ]]; then
        fail 'Cannot use the Wave manager lock file.'
        return 1
    fi
    exec 9>>"$lock_path" || return 1
    flock -x 9 || return 1
    check_instance || return 1
    "$@"
)

check_unit_file() {
    local marker
    if [[ ! -f $unit_file || -L $unit_file || $(stat -c '%u:%a' -- "$unit_file") != 0:644 ]] ||
        ! IFS= read -r marker < "$unit_file" || [[ $marker != '# Wave instance service' ]]; then
        fail "Cannot verify the Wave service file at $unit_file."
        return 1
    fi
}

read_service_status() {
    local properties key value fragment='' account=''
    check_unit_file || return 1
    properties=$(systemctl --system show --property=ActiveState,MainPID,UnitFileState,FragmentPath,User \
        "$service_unit") || return 1
    service_state=unknown
    service_boot=unknown
    process_pid=0
    while IFS='=' read -r key value; do
        case $key in
            ActiveState) service_state=$value ;;
            MainPID) process_pid=$value ;;
            UnitFileState) service_boot=$value ;;
            FragmentPath) fragment=$value ;;
            User) account=$value ;;
        esac
    done <<< "$properties"
    if [[ $fragment != "$unit_file" || $account != "$service_user" ]]; then
        fail "The loaded systemd service does not match this Wave installation."
        return 1
    fi
}

service_panel_ready() {
    [[ $service_state == active && $process_pid =~ ^[1-9][0-9]*$ ]] || return 1
    # Check process identity and read the mutable readiness file as the service account.
    # shellcheck disable=SC2016
    panel_url=$("${as_service[@]}" timeout 1 /bin/bash -c '
        [[ /proc/$1/exe -ef $2 && -f $3 && ! -L $3 && -O $3 ]] || exit 1
        { IFS= read -r pid && IFS= read -r url; } < "$3" || exit 1
        [[ $pid == "$1" && $url =~ ^https?://[^[:space:][:cntrl:]]+$ ]] || exit 1
        printf "%s\n" "$url"
    ' wave-readiness "$process_pid" "$backend" "$ready_file")
}

show_status() {
    read_service_status || return 1
    if service_panel_ready; then
        printf 'Wave is running (PID %s).\nPanel URL %s\n' "$process_pid" "$panel_url"
    else
        case $service_state in
            inactive) printf 'Wave is stopped.\n' ;;
            active|activating) printf 'Wave is starting.\n' ;;
            failed) printf 'Wave failed to start. Run %s logs panel for details.\n' "$instance_name" ;;
            *) printf 'Wave service state is %s.\n' "$service_state" ;;
        esac
    fi
    printf 'Systemd service %s (%s at boot).\n' "$service_unit" "$service_boot"
}

show_settings() {
    if ! show_status; then
        printf 'Wave service status is unavailable. Saved settings follow.\n' >&2
    fi
    "${as_service[@]}" "$backend" --local-settings
}

run_systemd_wave() {
    if (( EUID == 0 )); then
        fail 'The Wave service requires a non-root account.'
        return 1
    fi
    if [[ -L $runtime_dir || ! -d $runtime_dir || ! -O $runtime_dir ||
          $(stat -c %a -- "$runtime_dir") != 700 ]]; then
        fail 'Wave needs a mode-0700 runtime directory owned by the service account.'
        return 1
    fi
    if [[ -L $site_dir || ! -f $site_dir/index.wasm ]]; then
        fail 'Cannot find the packaged site directory beside Wave.'
        return 1
    fi
    if [[ -L $temporary_dir ]] || ! mkdir -p -- "$temporary_dir"; then
        fail 'Cannot prepare the instance temporary directory.'
        return 1
    fi
    rm -f -- "$ready_file" || return 1
    cd -- "$data_dir" || return 1
    export HISTFILE="$runtime_dir/console.history" TMPDIR="$temporary_dir"
    exec "$backend" --site-dir "$site_dir" --ready-file "$ready_file" </dev/null
}

change_service() {
    local action=$1 attempt
    read_service_status || return 1
    if [[ $action == start ]] && service_panel_ready; then
        printf 'Wave is running (PID %s).\nPanel URL %s\n' "$process_pid" "$panel_url"
        return 0
    fi
    if [[ $action == stop && $service_state == inactive ]]; then
        printf 'Wave is stopped.\n'
        return 0
    fi
    "${admin[@]}" systemctl --system "$action" "$service_unit" || return 1
    if [[ $action == stop ]]; then
        printf 'Wave is stopped.\n'
        return 0
    fi
    for (( attempt = 0; attempt < 150; ++attempt )); do
        read_service_status || return 1
        if service_panel_ready; then
            printf 'Wave is running (PID %s).\nPanel URL %s\n' "$process_pid" "$panel_url"
            return 0
        fi
        if [[ $service_state == failed || $service_state == inactive ]]; then
            fail "Wave exited before opening the panel. Run $instance_name logs panel for details."
            return 1
        fi
        sleep 0.1
    done
    fail 'Wave did not report a listener within 15 seconds. Use status and logs panel to check startup.'
}

show_panel_logs() {
    local -a options=(--no-pager --quiet --lines=100 --unit="$service_unit")
    read_service_status || return 1
    if [[ ${1:-} == --follow ]]; then
        options+=(--follow)
    fi
    "${admin[@]}" journalctl "${options[@]}"
}

instance_command_link() {
    local default_bin=${HOME:+$HOME/.local/bin}
    local candidate
    local -a candidates
    if (( EUID == 0 )); then
        default_bin=/usr/local/bin
    fi
    if [[ -e $command_file || -L $command_file ]]; then
        if [[ ! -f $command_file || -L $command_file || ! -O $command_file ]] ||
            ! IFS= read -r candidate < "$command_file" || [[ $candidate != /* ]]; then
            return 0
        fi
        candidates=("$candidate")
    else
        candidates=("${XDG_BIN_HOME:-$default_bin}/$instance_name" "$0")
    fi
    for candidate in "${candidates[@]}"; do
        if [[ ${candidate##*/} == "$instance_name" && -L $candidate &&
              $(readlink -- "$candidate") == "$manager" ]]; then
            printf '%s\n' "$candidate"
            return
        fi
    done
}

has_mounts() {
    local mount
    while read -r _ _ _ _ mount _; do
        mount=${mount//\\040/ }
        mount=${mount//\\011/$'\t'}
        mount=${mount//\\012/$'\n'}
        mount=${mount//\\134/\\}
        if [[ $mount == "$instance_dir" || $mount == "$instance_dir/"* ]]; then
            return 0
        fi
    done < /proc/self/mountinfo
    return 1
}

service_enablement_link() {
    local path=/etc/systemd/system/multi-user.target.wants/$service_unit
    local target
    if [[ ! -e $path && ! -L $path ]]; then
        return 0
    fi
    if [[ ! -L $path ]] || ! target=$(readlink -- "$path") ||
        [[ $target != "$unit_file" && $target != "../$service_unit" ]]; then
        fail "Cannot verify the Wave service enablement link at $path."
        return 1
    fi
    printf '%s\n' "$path"
}

uninstall_wave() {
    local confirmation link database_fd enablement_link
    local parent=${instance_dir%/*}
    local -a remaining
    printf 'This will remove %s and its Wave command.\n' "$instance_dir"
    printf 'The systemd service %s will also be removed.\n' "$service_unit"
    printf 'To keep a workspace backup, use Backup in the browser panel before continuing.\n'
    read -r -p "Type $instance_name to uninstall, or press Enter to cancel. " confirmation || return 1
    if [[ $confirmation != "$instance_name" ]]; then
        printf 'Uninstall canceled.\n'
        return 0
    fi
    if has_mounts; then
        fail "Unmount filesystems under $instance_dir before uninstalling."
        return 1
    fi
    link=$(instance_command_link)
    enablement_link=$(service_enablement_link) || return 1
    if (( EUID != 0 )); then
        sudo -v || return 1
    fi

    if [[ -e $unit_file || -L $unit_file ]]; then
        read_service_status || return 1
        "${admin[@]}" systemctl --system disable --now "$service_unit" || return 1
    fi
    exec {database_fd}<"$data_dir" || return 1
    if ! flock -n "$database_fd"; then
        fail 'The instance database is still in use.'
        return 1
    fi
    if [[ -e $unit_file || -L $unit_file ]]; then
        check_unit_file || return 1
        "${admin[@]}" rm -- "$unit_file" || return 1
    elif [[ -n $enablement_link && -L $enablement_link ]]; then
        "${admin[@]}" unlink -- "$enablement_link" || return 1
    fi
    "${admin[@]}" systemctl --system daemon-reload || return 1
    if [[ -w $parent ]]; then
        rm -rf --one-file-system -- "$instance_dir" || return 1
    else
        # Delete contents as the owner. Elevation is only needed for removing the empty directory.
        rm -rf --one-file-system -- "$instance_dir" 2>/dev/null || :
        if [[ -d $instance_dir ]]; then
            shopt -s dotglob nullglob
            remaining=("$instance_dir"/*)
            if (( ${#remaining[@]} != 0 )); then
                fail "Some files could not be removed from $instance_dir."
                return 1
            fi
            "${admin[@]}" rmdir -- "$instance_dir" || return 1
        fi
    fi
    if [[ -n $link && -L $link && $(readlink -- "$link") == "$manager" ]]; then
        rm -- "$link" || return 1
    fi
    if (( EUID == 0 )); then
        userdel -- "$service_user" || return 1
    fi
    printf 'Removed %s.\n' "$instance_dir"
}

menu() {
    local answer
    while true; do
        printf '\nWave instance %s\n' "$instance_name"
        printf '1) Status\n2) Start\n3) Stop\n4) Restart\n5) Settings\n6) Panel logs\n7) Core logs\n8) Uninstall\n0) Exit\n'
        read -r -p 'Choose an action. ' answer || return 0
        case $answer in
            1) show_status ;;
            2) with_manager_lock change_service start ;;
            3) with_manager_lock change_service stop ;;
            4) with_manager_lock change_service restart ;;
            5) show_settings ;;
            6) show_panel_logs ;;
            7) "${as_service[@]}" "$backend" --local-core-logs ;;
            8) with_manager_lock uninstall_wave; [[ -d $instance_dir ]] || return 0 ;;
            0|q|Q) return 0 ;;
            *) printf 'Choose a number from the menu.\n' ;;
        esac
    done
}

# Systemd starts a root-managed instance as the owner of the workspace.
if (( EUID != 0 && $# == 1 )) && [[ $1 == __run-service && $data_dir != "$instance_dir" ]]; then
    run_systemd_wave
    exit $?
fi
check_instance && read_service_unit || exit 1
if (( EUID == 0 )); then
    read_service_account || exit 1
fi
case $#:${1:-}:${2:-}:${3:-} in
    0:::) menu ;;
    1:status::) show_status ;;
    1:start::|1:stop::|1:restart::) with_manager_lock change_service "$1" ;;
    1:settings::) show_settings ;;
    2:logs:panel:|3:logs:panel:--follow) show_panel_logs "${3:-}" ;;
    2:logs:core:) "${as_service[@]}" "$backend" --local-core-logs ;;
    1:uninstall::) with_manager_lock uninstall_wave ;;
    1:__run-service::) run_systemd_wave ;;
    *) usage "$instance_name" >&2; exit 2 ;;
esac
