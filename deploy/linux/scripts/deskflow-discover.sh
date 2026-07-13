#!/usr/bin/env bash
# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
#
# Keeps a stable hostname pointed at the Deskflow server's current address.
#
# Home networks hand out addresses by DHCP, so a server address written into
# Deskflow's config goes stale the moment the router reshuffles its leases (or
# the network is renumbered). Instead, point Deskflow at a name:
#
#     remoteHost=deskflow-server
#
# and let this script maintain the /etc/hosts entry that name resolves through.
#
# It is deliberately quiet: while the client holds an established connection it
# does nothing at all. Only when the client is disconnected does it scan the
# local network for a host listening on the Deskflow port, and only if the
# server has actually moved does it rewrite /etc/hosts and restart the client.
#
# Configuration (via /etc/default/deskflow-discover, all optional):
#   DESKFLOW_ALIAS   hostname to maintain      (default: deskflow-server)
#   DESKFLOW_PORT    Deskflow port to probe    (default: 24800)
#   DESKFLOW_UNIT    client unit to restart    (default: deskflow-uinput.service)
#   DESKFLOW_SUBNETS space-separated /24 prefixes to scan instead of autodetecting

set -uo pipefail

ALIAS="${DESKFLOW_ALIAS:-deskflow-server}"
PORT="${DESKFLOW_PORT:-24800}"
UNIT="${DESKFLOW_UNIT:-deskflow-uinput.service}"
HOSTS_FILE="${DESKFLOW_HOSTS_FILE:-/etc/hosts}"
DRYRUN="${DESKFLOW_DRYRUN:-0}"
CONNECT_TIMEOUT="${DESKFLOW_CONNECT_TIMEOUT:-1}"
MAX_PARALLEL=64

log() { echo "deskflow-discover: $*"; }

current_mapping() {
  awk -v h="$ALIAS" '!/^#/ && $2 == h { print $1; exit }' "$HOSTS_FILE"
}

# An established socket on the Deskflow port means the client is talking to the
# server; there is nothing for us to do.
is_connected() {
  ss -tn state established "( dport = :$PORT )" 2>/dev/null | grep -q ":$PORT"
}

probe() { # probe <ip> -- succeeds if the Deskflow port is open
  timeout "$CONNECT_TIMEOUT" bash -c "echo > /dev/tcp/$1/$PORT" 2>/dev/null
}

# Which /24s to sweep: the one we live on first (DHCP nearly always lands there),
# then the rest of our interface prefix if it is wider than a /24.
subnets_to_scan() {
  if [[ -n "${DESKFLOW_SUBNETS:-}" ]]; then
    echo "$DESKFLOW_SUBNETS"
    return
  fi

  local self_ip cidr prefix o1 o2 o3 count i third
  self_ip=$(ip -4 route get 1.1.1.1 2>/dev/null |
    awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit } }')
  [[ -z "$self_ip" ]] && return 1

  local base="${self_ip%.*}"
  local out=("$base")

  cidr=$(ip -4 -o addr show scope global | awk '{ print $4; exit }')
  prefix="${cidr#*/}"
  if [[ -n "$cidr" && "$prefix" =~ ^[0-9]+$ ]] && ((prefix < 24)); then
    IFS=. read -r o1 o2 o3 _ <<<"${cidr%/*}"
    count=$((1 << (24 - prefix)))
    # Cap the sweep: a very wide prefix is not worth brute-forcing.
    ((count > 8)) && count=8
    for ((i = 0; i < count; i++)); do
      third=$((o3 + i))
      [[ "$o1.$o2.$third" == "$base" ]] && continue
      out+=("$o1.$o2.$third")
    done
  fi
  echo "${out[@]}"
}

scan_for_server() {
  local results found
  results=$(mktemp)
  # shellcheck disable=SC2046  # word splitting is intended here
  for net in $(subnets_to_scan); do
    : >"$results"
    for i in $(seq 1 254); do
      (probe "$net.$i" && echo "$net.$i" >>"$results") &
      while (($(jobs -r | wc -l) >= MAX_PARALLEL)); do wait -n; done
    done
    wait
    found=$(head -n1 "$results")
    if [[ -n "$found" ]]; then
      rm -f "$results"
      echo "$found"
      return 0
    fi
  done
  rm -f "$results"
  return 1
}

set_mapping() { # set_mapping <ip>
  local ip="$1" tmp
  tmp=$(mktemp)
  awk -v h="$ALIAS" '!($2 == h && !/^#/)' "$HOSTS_FILE" >"$tmp"
  printf '%s\t%s\n' "$ip" "$ALIAS" >>"$tmp"
  # Write through the existing file so its inode, owner and mode are preserved.
  cat "$tmp" >"$HOSTS_FILE"
  rm -f "$tmp"
}

restart_client() {
  if [[ "$DRYRUN" == "1" ]]; then
    log "[dry-run] would restart $UNIT"
  else
    systemctl restart "$UNIT"
  fi
}

main() {
  # If the client is deliberately stopped, stay out of the way -- never
  # resurrect a service the user turned off.
  if [[ "$DRYRUN" != "1" ]] && ! systemctl is-active --quiet "$UNIT"; then
    exit 0
  fi

  if is_connected && [[ "$DRYRUN" != "1" ]]; then
    exit 0
  fi

  local mapped found
  mapped=$(current_mapping)
  log "client is not connected (${ALIAS} -> ${mapped:-unmapped}); looking for a server on port $PORT"

  if ! found=$(scan_for_server); then
    log "no Deskflow server found on the local network; will try again later"
    exit 0
  fi

  if [[ "$found" == "$mapped" ]]; then
    # Server is reachable at the address we already know: the client is simply
    # not connected. Nudge it rather than touching /etc/hosts.
    log "server still at $found; restarting the client"
    restart_client
    exit 0
  fi

  log "server moved: ${mapped:-unmapped} -> $found; updating $ALIAS and restarting the client"
  set_mapping "$found"
  restart_client
}

main "$@"
