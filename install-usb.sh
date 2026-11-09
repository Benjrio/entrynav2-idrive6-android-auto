#!/bin/sh
# Install and control the USB Android Auto application.
set -eu
umask 077
cache=/opt/app/cisu/.ownandroidauto-369c9726-v24
legacy_cache=/tmp/ownandroidauto-installed-369c9726
mode=${1:-}
work=${2:-}
case "$work" in /tmp/ownandroidauto-kit-[A-Za-z0-9-]*) ;; *) exit 64 ;; esac
case "$work" in *[!A-Za-z0-9/.-]*) exit 64 ;; esac
base=${work#/tmp/}
case "$base" in *[!A-Za-z0-9-]*) exit 64 ;; esac
[ -d "$work" ] && [ ! -L "$work" ] || exit 65
ctl=$work/ownandroidauto-control.elf
[ -f "$ctl" ] && [ ! -L "$ctl" ] || exit 65
chmod 700 "$ctl"

aa_files='hu-aa.elf usb-device.conf lib/libavcodec.so.61 lib/libavutil.so.59 lib/libssl.so.1.0.0 lib/libcrypto.so.1.0.0 identity/phone.crt identity/phone.key scene/winston scene/libwlxs-genivi.so'
# In the kit only, never copied to the runtime cache.
kit_only_files='usb-scan.elf vmost-listen.elf'

if [ "$mode" = vmost-listen ]; then
    # Passive: logs what the virtual MOST daemon delivers for FBlock shadows (default CF.00 and E5.00) for N seconds
    # (default 20). Registers shadows only and sends no MOST message. Operate the iDrive controller meanwhile.
    # Usage: run-hu.sh vmost-listen <kit> [seconds] [FB.II ...]
    seconds=${3:-20}
    case "$seconds" in [1-9]|[1-9][0-9]|[1-5][0-9][0-9]|600) ;; *) exit 64 ;; esac
    if [ $# -ge 3 ]; then shift 3; else shift $#; fi
    [ $# -le 8 ] || exit 64
    for s in "$@"; do case "$s" in [0-9A-Fa-f][0-9A-Fa-f].[0-9A-Fa-f][0-9A-Fa-f]) ;; *) exit 64 ;; esac; done
    for f in aa aa/vmost-listen.elf; do [ -e "$work/$f" ] && [ ! -L "$work/$f" ] || exit 65; done
    chmod 700 "$work/aa/vmost-listen.elf"
    exec "$work/aa/vmost-listen.elf" -s "$seconds" "$@"
fi

if [ "$mode" = usb-scan ]; then
    # Read-only: which USB device hu-aa.elf would pick with this kit's usb-device.conf. Reads sysfs only; sends
    # nothing to any USB device. Optional third argument: watch for N seconds (plug/unplug meanwhile).
    watch=${3:-}
    case "$watch" in ''|[1-9]|[1-9][0-9]|[1-5][0-9][0-9]|600) ;; *) exit 64 ;; esac
    for f in aa aa/usb-scan.elf aa/usb-device.conf; do [ -e "$work/$f" ] && [ ! -L "$work/$f" ] || exit 65; done
    chmod 700 "$work/aa/usb-scan.elf"
    cd "$work/aa"
    if [ -n "$watch" ]; then exec ./usb-scan.elf -w "$watch"; else exec ./usb-scan.elf; fi
fi

aa_validate() {
    for d in "$1/aa" "$1/aa/lib" "$1/aa/identity" "$1/aa/scene"; do
        [ -d "$d" ] && [ ! -L "$d" ] || return 1
    done
    for f in $aa_files; do [ -f "$1/aa/$f" ] && [ ! -L "$1/aa/$f" ] || return 1; done
}
aa_remove() {
    [ ! -e "$1/aa" ] && [ ! -L "$1/aa" ] && return 0
    for d in "$1/aa" "$1/aa/lib" "$1/aa/identity" "$1/aa/scene"; do
        [ ! -L "$d" ] || return 1
        [ ! -e "$d" ] || [ -d "$d" ] || return 1
    done
    for f in $aa_files $kit_only_files; do [ ! -L "$1/aa/$f" ] || return 1; done
    for f in $aa_files $kit_only_files; do if [ -f "$1/aa/$f" ]; then rm "$1/aa/$f"; fi; done
    for d in "$1/aa/lib" "$1/aa/identity" "$1/aa/scene" "$1/aa"; do
        if [ -d "$d" ]; then rmdir "$d" || return 1; fi
    done
}

owned() {
    [ -e "$1" ] || return 0
    [ -d "$1" ] && [ ! -L "$1" ] && [ -f "$1/OWNANDROIDAUTO-OWNER" ] && [ ! -L "$1/OWNANDROIDAUTO-OWNER" ] || return 1
    marker=
    IFS= read -r marker < "$1/OWNANDROIDAUTO-OWNER" || :
    [ "$marker" = OWN-ANDROID-AUTO-LOCAL-SURFACE-2 ]
}
valid_path() {
    case "$1" in /*/OwnAndroidAuto) ;; *) return 1 ;; esac
    case "$1" in *[!A-Za-z0-9/._-]*|*/../*|*/./*) return 1 ;; esac
}
normalize_root() {
    # The actual variant server returns paths containing // before Plugins.
    # Collapse separators only; never resolve symlinks or . / .. components.
    normalized=$1
    while :; do
        case "$normalized" in
            *//*) normalized=${normalized%%//*}/${normalized#*//} ;;
            *) break ;;
        esac
    done
    case "$normalized" in /*/Plugins) ;; *) return 1 ;; esac
    case "$normalized" in *[!A-Za-z0-9/._-]*|*/../*|*/./*) return 1 ;; esac
}
root_state() {
    # Return a classification; this is read-only. Creating a missing root is
    # deferred until the chosen path and the own executable cache are validated.
    root_status=unusable
    if [ -L "$normalized" ]; then root_status=symlink
    elif [ -d "$normalized" ]; then
        if [ -w "$normalized" ]; then root_status=existing; else root_status=not-writable; fi
    elif [ -e "$normalized" ]; then root_status=not-directory
    else
        root_parent=${normalized%/*}
        if [ -L "$root_parent" ]; then root_status=parent-symlink
        elif [ ! -d "$root_parent" ]; then root_status=parent-missing
        elif [ ! -w "$root_parent" ]; then root_status=parent-not-writable
        else root_status=create-leaf
        fi
    fi
}
persistent_storage() {
    # Validate the supported persistent mount before writing files.
    : > "$work/storage-state.txt"
    storage_mounts=0
    while read -r storage_source storage_target storage_fs storage_options storage_rest; do
        [ "$storage_target" = /opt/app/cisu ] || continue
        printf 'MOUNT|%s|%s|%s|%s\n' "$storage_source" "$storage_target" "$storage_fs" "$storage_options" >> "$work/storage-state.txt"
        [ "$storage_source" = /dev/mmcblk0p6 ] && [ "$storage_fs" = ext4 ] || return 1
        case ",$storage_options," in *,rw,*) ;; *) return 1 ;; esac
        case ",$storage_options," in *,noexec,*) return 1 ;; esac
        storage_mounts=$((storage_mounts+1))
    done < /proc/mounts
    [ "$storage_mounts" -eq 1 ] || return 1
    for storage_parent in /opt /opt/app /opt/app/cisu; do
        [ -d "$storage_parent" ] && [ ! -L "$storage_parent" ] || return 1
    done
    [ -w /opt/app/cisu ] || return 1
    "$ctl" storage /opt/app/cisu >> "$work/storage-state.txt" 2>&1 || return 1
    printf 'CACHE|%s\n' "$cache" >> "$work/storage-state.txt"
}
hmi_info() {
    hmi_name= hmi_state= hmi_ppid=
    IFS= read -r hmi_name < "/proc/$1/comm" 2>/dev/null || return 1
    [ "$hmi_name" = HmiMain3D ] || return 1
    while read -r info_key info_value info_rest; do
        case "$info_key" in State:) hmi_state=$info_value ;; PPid:) hmi_ppid=$info_value ;; esac
    done < "/proc/$1/status" 2>/dev/null || return 1
    case "$hmi_ppid" in ''|*[!0-9]*) return 1 ;; esac
    [ -n "$hmi_state" ]
}
hmi_live() {
    # Neither a zombie nor a dead task can receive our control requests.
    case "$hmi_state" in Z|X|x|'') return 1 ;; esac
    return 0
}
hmi_service() {
    hmi_unit=
    [ -r "/proc/$1/cgroup" ] || return 1
    while IFS=: read -r group_hierarchy group_controllers group_path; do
        while [ -n "$group_path" ]; do
            group_component=${group_path%%/*}
            case "$group_component" in *.service)
                case "$group_component" in *[!A-Za-z0-9@_.-]*) return 1 ;; esac
                [ -z "$hmi_unit" ] || [ "$hmi_unit" = "$group_component" ] || return 1
                hmi_unit=$group_component ;;
            esac
            case "$group_path" in */*) group_path=${group_path#*/} ;; *) break ;; esac
        done
    done < "/proc/$1/cgroup" 2>/dev/null || return 1
    [ -n "$hmi_unit" ]
}
selection_failed() {
    printf 'ERROR: %s; see hmi-processes.txt; no HMI control attempted\n' "$1"
    # A process-selection failure must not prevent disabling our next-start
    # manifest. Preserve all other files because STOP cannot be confirmed.
    if [ "$mode" = uninstall ] && owned "$cache" &&
       [ -f "$cache/install-path.txt" ] && [ ! -L "$cache/install-path.txt" ]; then
        rollback_path=
        IFS= read -r rollback_path < "$cache/install-path.txt" || :
        if valid_path "$rollback_path" && owned "$rollback_path" &&
           [ -f "$rollback_path/OwnAndroidAuto.manifest" ] && [ ! -L "$rollback_path/OwnAndroidAuto.manifest" ]; then
            rm "$rollback_path/OwnAndroidAuto.manifest"
            echo 'REGISTRATION_DISABLED_ONLY: own manifest removed; STOP unconfirmed, remaining resources retained'
        fi
    fi
    exit 66
}
collect_diagnostics() {
    # Read-only platform queries. No helper/IPC, job cancellation, service
    # start/stop/restart, ptrace, full command lines or environment dumps.
    {
        echo 'OWNANDROIDAUTO_PLATFORM_STATE_1'
        echo 'USB_INVENTORY port|vid|pid|class|bus|devnum'
        for devpath in /sys/bus/usb/devices/*; do
            [ -f "$devpath/idVendor" ] && [ -f "$devpath/idProduct" ] || continue
            usb_name=${devpath##*/}
            case "$usb_name" in *[!0-9.-]*) continue ;; esac
            usb_vid= usb_pid= usb_class= usb_bus= usb_dev=
            IFS= read -r usb_vid < "$devpath/idVendor" || :
            IFS= read -r usb_pid < "$devpath/idProduct" || :
            IFS= read -r usb_class < "$devpath/bDeviceClass" || :
            IFS= read -r usb_bus < "$devpath/busnum" || :
            IFS= read -r usb_dev < "$devpath/devnum" || :
            printf 'USB_DEVICE|%s|%s|%s|%s|%s|%s\n' "$usb_name" "$usb_vid" "$usb_pid" "$usb_class" "$usb_bus" "$usb_dev"
        done
        printf 'UPTIME|'; cat /proc/uptime || :
        printf 'BOOT_ID|'; cat /proc/sys/kernel/random/boot_id || :
        while IFS='|' read -r diag_kind diag_pid diag_rest; do
            [ "$diag_kind" = CANDIDATE ] || continue
            printf 'WCHAN|%s|' "$diag_pid"
            cat "/proc/$diag_pid/wchan" || :
            printf '\n'
        done < "$work/hmi-processes.txt"
        for diag_unit in $diagnostic_units; do
            printf '\nSERVICE|%s\n' "$diag_unit"
            if command -v systemctl >/dev/null 2>&1; then
                systemctl show -p Id -p MainPID -p ControlPID -p Type -p ActiveState -p SubState \
                    -p KillMode -p KillSignal -p SendSIGKILL -p Restart -p RestartUSec \
                    -p WatchdogUSec -p WatchdogTimestampMonotonic -p TimeoutStopUSec \
                    -p Result -p ExecMainCode -p ExecMainStatus -p Job -p FailureAction \
                    -p StartLimitAction -p ExecMainStartTimestampMonotonic \
                    -p ActiveEnterTimestampMonotonic -p FragmentPath -p DropInPaths "$diag_unit" || \
                    echo 'SERVICE_PROPERTIES_UNAVAILABLE'
            fi
            # Filter the on-disk unit to lifecycle settings only. Do not export
            # Environment/ExecStart arguments or unrelated application logs.
            for diag_dir in /etc/systemd/system /lib/systemd/system /usr/lib/systemd/system; do
                diag_path=$diag_dir/$diag_unit
                [ -f "$diag_path" ] || continue
                printf 'UNIT_FILE|%s\n' "$diag_path"
                while IFS= read -r diag_line || [ -n "$diag_line" ]; do
                    case "$diag_line" in '[Unit]'|'[Service]'|Type=*|KillMode=*|KillSignal=*|SendSIGKILL=*|Restart=*|RestartSec=*|WatchdogSec=*|TimeoutStopSec=*|FailureAction=*|StartLimitAction=*|OnFailure=*) printf '%s\n' "$diag_line" ;; esac
                done < "$diag_path"
            done
            if command -v journalctl >/dev/null 2>&1; then
                # PID1 records for this service only, bounded. The previous
                # boot may be absent when the journal is volatile.
                echo 'PID1_UNIT_JOURNAL_ALL_BOOTS_LAST80'
                journalctl --no-pager -n 80 -o short-monotonic _PID=1 "UNIT=$diag_unit" || echo 'JOURNAL_UNAVAILABLE'
                echo 'PID1_UNIT_JOURNAL_PREVIOUS_BOOT_LAST80'
                journalctl --no-pager -b -1 -n 80 -o short-monotonic _PID=1 "UNIT=$diag_unit" || echo 'PREVIOUS_BOOT_JOURNAL_UNAVAILABLE'
            fi
        done
        if command -v systemctl >/dev/null 2>&1; then
            echo 'JOBS_FOR_COLLECTED_UNITS'
            systemctl list-jobs --no-pager | while read -r diag_job diag_job_unit diag_job_rest; do
                case " $diagnostic_units " in *" $diag_job_unit "*) printf '%s %s %s\n' "$diag_job" "$diag_job_unit" "$diag_job_rest" ;; esac
            done
        fi
    } > "$work/hmi-diagnostics.txt" 2>&1
}
if [ "$mode" = cleanup ]; then
    # Remove only the known files in this private staging directory.
    for file in ownandroidauto-control.elf libOwnAndroidAuto.so ownandroidauto-producer.elf OwnAndroidAuto.xml OwnAndroidAuto.manifest OwnAndroidAuto_Texts.zip OwnAndroidAuto_Images.zip run-hu.sh hmi.pid hmi-processes.txt hmi-diagnostics.txt storage-state.txt install-stage.txt manager.txt install-path.txt uninstall-status.txt root-state.txt root-candidates.txt; do
        if [ -f "$work/$file" ] && [ ! -L "$work/$file" ]; then rm "$work/$file"; fi
    done
    for path in "$work/results/"*; do
        file=${path##*/}
        case "$file" in ownandroidauto-aa-*-android-auto.log|ownandroidauto-aa-*-summary.json|ownandroidauto-aa-*-usb-transport.json|ownandroidauto-native-[0-9]*.log|ownandroidauto-producer-[0-9]*.log|manager.txt|install-path.txt|root-state.txt|hmi-processes.txt|hmi-diagnostics.txt|storage-state.txt|install-stage.txt) ;;
            *) continue ;; esac
        if [ -f "$path" ] && [ ! -L "$path" ]; then rm "$path"; fi
    done
    if [ -d "$work/results" ]; then rmdir "$work/results"; fi
    aa_remove "$work" || exit 69
    rmdir "$work"
    exit 0
fi
if [ "$mode" = export ]; then
    mkdir "$work/results"
    # Only our phone logs, no frame contents or TLS identity files.
    for dir in /tmp/ownandroidauto-aa-*; do
        [ -d "$dir" ] && [ ! -L "$dir" ] || continue
        run=${dir##*/}
        case "$run" in *[!A-Za-z0-9-]*) continue ;; esac
        for file in android-auto.log summary.json usb-transport.json; do
            if [ -f "$dir/$file" ] && [ ! -L "$dir/$file" ]; then
                cp "$dir/$file" "$work/results/$run-$file"
            fi
        done
    done
    count=0
    # Own logs only, bounded; also works when a failed plugin has stopped HMI.
    for path in /tmp/ownandroidauto-native-[0-9]*.log /tmp/ownandroidauto-producer-[0-9]*.log; do
        if [ -f "$path" ] && [ ! -L "$path" ]; then
            count=$((count+1)); [ "$count" -le 64 ] || break
            cp "$path" "$work/results/${path##*/}"
        fi
    done
    for file in manager.txt install-path.txt root-state.txt hmi-processes.txt hmi-diagnostics.txt storage-state.txt install-stage.txt; do
        if [ -f "$work/$file" ] && [ ! -L "$work/$file" ]; then cp "$work/$file" "$work/results/$file"; fi
    done
    exit 0
fi
pid= live_count=0 main_count=0 live_pid= main_pid= live_unit= unit_conflict=0 loaded_count=0
diagnostic_units='HmiMain3D.service winston.service'
printf '%s\n' 'FIELDS|pid|ppid|state|service|service_mainpid|own_socket' > "$work/hmi-processes.txt"
for entry in /proc/[0-9]*/comm; do
    candidate=${entry#/proc/}; candidate=${candidate%/comm}
    hmi_info "$candidate" || continue
    candidate_unit= candidate_main=- candidate_socket=no
    if hmi_service "$candidate"; then
        candidate_unit=$hmi_unit
        case " $diagnostic_units " in *" $candidate_unit "*) ;; *) diagnostic_units="$diagnostic_units $candidate_unit" ;; esac
        if command -v systemctl >/dev/null 2>&1 &&
           candidate_show=$(systemctl show -p MainPID "$candidate_unit" 2>/dev/null); then
            case "$candidate_show" in MainPID=*)
                candidate_number=${candidate_show#MainPID=}
                case "$candidate_number" in ''|*[!0-9]*) ;; *) candidate_main=$candidate_number ;; esac ;;
            esac
        fi
    fi
    if [ -S "/tmp/ownandroidauto-control-$candidate.sock" ]; then candidate_socket=yes; fi
    printf 'CANDIDATE|%s|%s|%s|%s|%s|%s\n' "$candidate" "$hmi_ppid" "$hmi_state" "${candidate_unit:--}" "$candidate_main" "$candidate_socket" >> "$work/hmi-processes.txt"
    hmi_live || continue
    live_count=$((live_count+1)); live_pid=$candidate
    if [ "$candidate_socket" = yes ]; then loaded_count=$((loaded_count+1)); fi
    if [ -z "$candidate_unit" ]; then unit_conflict=1
    elif [ -z "$live_unit" ]; then live_unit=$candidate_unit
    elif [ "$live_unit" != "$candidate_unit" ]; then unit_conflict=1
    fi
    if [ "$candidate_main" = "$candidate" ]; then
        main_count=$((main_count+1)); main_pid=$candidate
    fi
done
if [ "$live_count" -gt 1 ]; then
    case "$mode" in install|open)
        collect_diagnostics
        selection_failed 'multiple live HMI processes; OPEN blocked, platform state saved' ;;
    esac
fi
selection=none
if [ "$live_count" -eq 1 ]; then
    pid=$live_pid; selection=unique-live
elif [ "$live_count" -gt 1 ]; then
    # Multiple live tasks are accepted only within the same proved service,
    # with exactly one candidate matching that service's MainPID. Never pick
    # the first/last/newest PID or a socket pathname on its own.
    [ "$unit_conflict" -eq 0 ] && [ "$main_count" -eq 1 ] || selection_failed 'ambiguous live HMI processes'
    pid=$main_pid; selection=service-mainpid
fi
if [ -n "$pid" ]; then
    hmi_info "$pid" && hmi_live || selection_failed 'selected HMI exited or changed state'
    if [ "$selection" = service-mainpid ]; then
        hmi_service "$pid" && [ "$hmi_unit" = "$live_unit" ] || selection_failed 'selected HMI service changed'
        selected_show=$(systemctl show -p MainPID "$hmi_unit") || selection_failed 'cannot recheck HMI MainPID'
        [ "$selected_show" = "MainPID=$pid" ] || selection_failed 'selected HMI MainPID changed'
    fi
fi
printf 'SELECTED|%s|%s\n' "${pid:--}" "$selection" >> "$work/hmi-processes.txt"
if [ -n "$pid" ]; then printf '%s\n' "$pid" > "$work/hmi.pid"; fi
case "$mode" in install|open)
    [ "$live_count" -eq 1 ] && hmi_service "$pid" || selection_failed 'one HMI service is required for OPEN'
    open_unit=$hmi_unit
    open_main=$(systemctl show -p MainPID "$open_unit") || selection_failed 'cannot prove OPEN MainPID'
    open_active=$(systemctl show -p ActiveState "$open_unit") || selection_failed 'cannot prove OPEN service state'
    open_job=$(systemctl show -p Job "$open_unit") || selection_failed 'cannot prove OPEN job state'
    open_control=$(systemctl show -p ControlPID "$open_unit") || selection_failed 'cannot prove OPEN ControlPID'
    [ "$open_main" = "MainPID=$pid" ] && [ "$open_active" = ActiveState=active ] &&
        [ "$open_job" = Job= ] && [ "$open_control" = ControlPID=0 ] || selection_failed 'HMI service is changing or is not the selected MainPID'
    printf 'SERVICE_OPEN_GUARD|%s|%s|%s|%s|%s\n' "$open_unit" "$open_main" "$open_active" "$open_job" "$open_control" >> "$work/hmi-processes.txt" ;;
esac
case "$mode" in
    stop|status|open)
        [ -n "$pid" ] || { echo 'ERROR: HMI not running'; exit 66; }
        if [ "$mode" = stop ] && [ ! -S "/tmp/ownandroidauto-control-$pid.sock" ]; then
            echo 'STOP_NOT_NEEDED: OwnAndroidAuto is not loaded in the current HMI'
            exit 0
        fi
        # QUIE stops private children without native deactivation.
        case "$mode" in stop) cmd=QUIE; echo 'QUIESCE_ONLY: producer will stop; press native MENU, no deactivate is requested' ;; status) cmd=STAT ;; open) cmd=OPEN ;; esac
        exec "$ctl" "$cmd" "$pid" ;;
    inspect|install|uninstall) ;;
    *) exit 64 ;;
esac
owned "$cache" || { echo 'ERROR: foreign executable cache preserved'; exit 69; }
destination=
if [ "$mode" = uninstall ] && [ -f "$cache/install-path.txt" ] && [ ! -L "$cache/install-path.txt" ]; then
    IFS= read -r destination < "$cache/install-path.txt" || :
    valid_path "$destination" || { echo 'ERROR: invalid saved installation path'; exit 69; }
fi
if [ "$mode" = uninstall ] && [ -z "$destination" ] && [ -f "$legacy_cache/install-path.txt" ] && [ ! -L "$legacy_cache/install-path.txt" ]; then
    owned "$legacy_cache" || { echo 'ERROR: foreign legacy cache preserved'; exit 69; }
    IFS= read -r destination < "$legacy_cache/install-path.txt" || :
    valid_path "$destination" || { echo 'ERROR: invalid legacy rollback path'; exit 69; }
fi
if [ -z "$destination" ]; then
    [ -n "$pid" ] || { echo 'ERROR: HMI absent and no saved rollback path; resources preserved'; exit 66; }
    "$ctl" inspect "$pid" > "$work/manager.txt" || { echo 'ERROR: exact native registration paths unavailable; no installation'; exit 67; }
    : > "$work/root-state.txt"
    : > "$work/root-candidates.txt"
    while IFS='|' read -r kind coded path; do
        [ "$kind" = ROOT ] && [ "$coded" = 0 ] || continue
        if ! normalize_root "$path"; then
            printf 'ROOT|unsupported-path|%s\n' "$path" >> "$work/root-state.txt"
            continue
        fi
        root_state
        printf 'ROOT|%s|%s\n' "$root_status" "$normalized" >> "$work/root-state.txt"
        case "$root_status" in existing|create-leaf)
            printf '%s|%s\n' "$root_status" "$normalized" >> "$work/root-candidates.txt" ;;
        esac
        if [ "$root_status" = existing ] && [ -d "$normalized/OwnAndroidAuto" ]; then
            [ -z "$destination" ] || { echo 'ERROR: ambiguous OwnAndroidAuto installation'; exit 68; }
            destination=$normalized/OwnAndroidAuto
        fi
    done < "$work/manager.txt"
    if [ -z "$destination" ]; then
        # Prefer an existing root; otherwise create only the registered Plugins
        # leaf under an already existing writable parent, never mkdir -p.
        for wanted in existing create-leaf; do
            while IFS='|' read -r candidate path; do
                if [ "$candidate" = "$wanted" ]; then destination=$path/OwnAndroidAuto; break; fi
            done < "$work/root-candidates.txt"
            [ -z "$destination" ] || break
        done
    fi
fi
[ -n "$destination" ] && valid_path "$destination" || { echo 'ERROR: no usable registered Plugins root/parent; see root-state.txt; no remount or policy change'; exit 68; }
owned "$destination" || { echo 'ERROR: foreign installation path preserved'; exit 69; }
printf '%s\n' "$destination" > "$work/install-path.txt"
if [ "$mode" = inspect ]; then printf 'INSTALL_PATH|%s\n' "$destination"; exit 0; fi
if [ "$mode" = uninstall ]; then
    plugin_root=${destination%/OwnAndroidAuto}
    # Disable only our future registration even if a bad plugin is crashing HMI.
    if [ -f "$destination/OwnAndroidAuto.manifest" ] && [ ! -L "$destination/OwnAndroidAuto.manifest" ]; then rm "$destination/OwnAndroidAuto.manifest"; fi
    if [ "$loaded_count" -gt 0 ]; then
        # Keep loaded resources mapped until HMI exits normally.
        collect_diagnostics
        echo 'REGISTRATION_DISABLED_ONLY: own manifest removed; loaded plugin resources retained, no UNIN/deactivate requested'
        exit 74
    fi
    if [ -d "$destination" ]; then
        for file in OwnAndroidAuto.xml OwnAndroidAuto_Texts.zip OwnAndroidAuto_Images.zip .OwnAndroidAuto.manifest.new OWNANDROIDAUTO-OWNER; do
            [ ! -L "$destination/$file" ] || { echo 'ERROR: symlink preserved'; exit 69; }
        done
        for file in OwnAndroidAuto.xml OwnAndroidAuto_Texts.zip OwnAndroidAuto_Images.zip .OwnAndroidAuto.manifest.new OWNANDROIDAUTO-OWNER; do
            if [ -f "$destination/$file" ]; then rm "$destination/$file"; fi
        done
        rmdir "$destination" || { echo 'ERROR: unexpected files preserved'; exit 69; }
    fi
    # Remove the shared Plugins directory only when this installer created it
    # and its own marker survives. rmdir preserves any later apps/files.
    if [ -f "$plugin_root/.ownandroidauto-created-root" ] && [ ! -L "$plugin_root/.ownandroidauto-created-root" ]; then
        marker=
        IFS= read -r marker < "$plugin_root/.ownandroidauto-created-root" || :
        if [ "$marker" = OWN-ANDROID-AUTO-PLUGIN-ROOT-1 ]; then
            rm "$plugin_root/.ownandroidauto-created-root"
            if ! rmdir "$plugin_root" 2>/dev/null; then echo 'ROOT_RETAINED: other entries remain in Plugins'; fi
        fi
    fi
    for removable_cache in "$cache" "$legacy_cache"; do
      owned "$removable_cache" || { echo 'ERROR: foreign cache preserved'; exit 69; }
      if [ -d "$removable_cache" ]; then
        aa_remove "$removable_cache" || exit 69
        for file in libOwnAndroidAuto.so ownandroidauto-producer.elf READY install-path.txt previous.manifest OWNANDROIDAUTO-OWNER; do
            [ ! -L "$removable_cache/$file" ] || { echo 'ERROR: cache symlink preserved'; exit 69; }
        done
        for file in libOwnAndroidAuto.so ownandroidauto-producer.elf READY install-path.txt previous.manifest OWNANDROIDAUTO-OWNER; do
            if [ -f "$removable_cache/$file" ]; then rm "$removable_cache/$file"; fi
        done
        rmdir "$removable_cache" || { echo 'ERROR: unexpected cache files preserved'; exit 69; }
      fi
    done
    echo 'UNINSTALLED: own registration/resources removed; no native deactivate or dlclose attempted'
    exit 0
fi
# New paths/files use mode0700/0600. The selected HU profile is a root HMI;
# verify its filesystem UID rather than guessing access for another user.
hmi_fsuid=
while read -r key real effective saved filesystem rest; do
    if [ "$key" = 'Uid:' ]; then hmi_fsuid=$filesystem; break; fi
done < "/proc/$pid/status"
printf 'HMI_FSUID|%s\n' "$hmi_fsuid" >> "$work/root-state.txt"
[ "$hmi_fsuid" = 0 ] || { echo 'ERROR: HMI filesystem UID is not the supported root profile; no installation'; exit 68; }
persistent_storage || { echo 'ERROR: persistent executable storage not attested or less than 8MiB free; see storage-state.txt; no remount/fallback'; exit 75; }
plugin_root=${destination%/OwnAndroidAuto}
if [ ! -d "$plugin_root" ]; then
    normalize_root "$plugin_root" || exit 68
    root_state
    [ "$root_status" = create-leaf ] || { echo 'ERROR: Plugins parent changed; no installation'; exit 68; }
    mkdir "$plugin_root" || { echo 'ERROR: cannot create the registered Plugins leaf; no remount or policy change'; exit 68; }
    printf '%s\n' OWN-ANDROID-AUTO-PLUGIN-ROOT-1 > "$plugin_root/.ownandroidauto-created-root"
    printf 'CREATED|%s\n' "$plugin_root" >> "$work/root-state.txt"
fi
if [ ! -d "$cache" ]; then
    mkdir "$cache"
    printf '%s\n' OWN-ANDROID-AUTO-LOCAL-SURFACE-2 > "$cache/OWNANDROIDAUTO-OWNER"
fi
for cache_file in READY libOwnAndroidAuto.so ownandroidauto-producer.elf install-path.txt previous.manifest; do
    [ ! -L "$cache/$cache_file" ] || { echo 'ERROR: cache symlink preserved'; exit 69; }
    if [ -e "$cache/$cache_file" ] && [ ! -f "$cache/$cache_file" ]; then
        echo 'ERROR: unexpected cache object preserved'; exit 69
    fi
done
# Save rollback path before making our manifest discoverable.
printf '%s\n' "$destination" > "$cache/install-path.txt"
if [ ! -f "$cache/READY" ] || [ ! -f "$cache/libOwnAndroidAuto.so" ] || [ ! -f "$cache/ownandroidauto-producer.elf" ]; then
    # Never rewrite a possibly mapped/partial generation. Removal is a separate
    # owner action after its manifest is disabled and no endpoint remains.
    for file in READY libOwnAndroidAuto.so ownandroidauto-producer.elf; do
        [ ! -e "$cache/$file" ] || { echo 'ERROR: incomplete immutable runtime preserved; use uninstall before retry'; exit 69; }
    done
    if [ -S "/tmp/ownandroidauto-control-$pid.sock" ]; then
        old_probe_exit=0
        "$ctl" PROBE "$pid" || old_probe_exit=$?
        # An older responsive module may keep running from its separate cache.
        # Do not open it; stage this fresh generation for the next normal boot.
        [ "$old_probe_exit" -eq 76 ] || { echo 'ERROR: loaded generation/cache preserved'; exit 70; }
    fi
    for file in libOwnAndroidAuto.so ownandroidauto-producer.elf; do
        [ ! -L "$cache/$file" ] || { echo 'ERROR: cache symlink preserved'; exit 69; }
        cp "$work/$file" "$cache/$file"
    done
    aa_validate "$work" || { echo 'ERROR: incomplete AA payload'; exit 69; }
    [ ! -e "$cache/aa" ] && [ ! -L "$cache/aa" ] || { echo 'ERROR: immutable AA runtime preserved'; exit 69; }
    mkdir "$cache/aa" "$cache/aa/lib" "$cache/aa/identity" "$cache/aa/scene"
    for f in $aa_files; do cp "$work/aa/$f" "$cache/aa/$f"; done
    chmod 700 "$cache/aa/hu-aa.elf"
    chmod 700 "$cache/ownandroidauto-producer.elf"
    printf '%s\n' OWN-ANDROID-AUTO-RUNTIME-V24 > "$cache/READY"
fi
aa_validate "$cache" || { echo "ERROR: incomplete installed AA runtime"; exit 69; }
cache_revision=
IFS= read -r cache_revision < "$cache/READY" || :
[ "$cache_revision" = OWN-ANDROID-AUTO-RUNTIME-V24 ] || { echo 'ERROR: unexpected immutable runtime revision; preserved'; exit 69; }
if [ ! -e "$destination" ]; then
    mkdir "$destination"
    printf '%s\n' OWN-ANDROID-AUTO-LOCAL-SURFACE-2 > "$destination/OWNANDROIDAUTO-OWNER"
fi
for file in OwnAndroidAuto.xml OwnAndroidAuto_Texts.zip OwnAndroidAuto_Images.zip; do
    [ ! -L "$destination/$file" ] || { echo 'ERROR: resource symlink preserved'; exit 69; }
    if [ -e "$destination/$file" ] && [ ! -f "$destination/$file" ]; then echo 'ERROR: unexpected resource object preserved'; exit 69; fi
    if [ ! -f "$destination/$file" ]; then
        [ ! -S "/tmp/ownandroidauto-control-$pid.sock" ] || { echo 'ERROR: active incomplete resources preserved'; exit 70; }
        cp "$work/$file" "$destination/$file"
    fi
done
manifest_current=no
[ ! -L "$destination/OwnAndroidAuto.manifest" ] || { echo 'ERROR: manifest symlink preserved'; exit 69; }
if [ -e "$destination/OwnAndroidAuto.manifest" ] && [ ! -f "$destination/OwnAndroidAuto.manifest" ]; then echo 'ERROR: unexpected manifest object preserved'; exit 69; fi
if [ -f "$destination/OwnAndroidAuto.manifest" ]; then
    while IFS= read -r manifest_line || [ -n "$manifest_line" ]; do
        case "$manifest_line" in *"<file>$cache/OwnAndroidAuto</file>"*) manifest_current=yes ;; esac
    done < "$destination/OwnAndroidAuto.manifest"
fi
if [ "$manifest_current" != yes ]; then
    [ ! -e "$destination/.OwnAndroidAuto.manifest.new" ] && [ ! -L "$destination/.OwnAndroidAuto.manifest.new" ] || { echo 'ERROR: staging name occupied; preserved'; exit 69; }
    [ ! -L "$cache/previous.manifest" ] || exit 69
    if [ -f "$destination/OwnAndroidAuto.manifest" ] && [ ! -e "$cache/previous.manifest" ]; then cp "$destination/OwnAndroidAuto.manifest" "$cache/previous.manifest"; fi
    cp "$work/OwnAndroidAuto.manifest" "$destination/.OwnAndroidAuto.manifest.new"
    mv "$destination/.OwnAndroidAuto.manifest.new" "$destination/OwnAndroidAuto.manifest"
fi
{
    printf 'RUNTIME|%s\n' "$cache"
    printf 'BOOT_ID|'; cat /proc/sys/kernel/random/boot_id
    printf 'HMI_PID|%s\n' "$pid"
    printf 'MANIFEST_ALREADY_CURRENT|%s\n' "$manifest_current"
} > "$work/install-stage.txt"
if [ -S "/tmp/ownandroidauto-control-$pid.sock" ]; then
    probe_exit=0
    "$ctl" PROBE "$pid" || probe_exit=$?
    if [ "$probe_exit" -eq 0 ]; then exec "$ctl" OPEN "$pid"; fi
    [ "$probe_exit" -eq 76 ] || exit 70
fi
echo 'REGISTRATION_PENDING: persistent generation24 staged, window NOT opened. Wait for a normal complete HU shutdown/start, then use the open command.'
exit 23
