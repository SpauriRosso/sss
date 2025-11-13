#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

VERSION="2.0"
BACKUP_DIR="${BACKUP_DIR:-/var/backups/hist-$(date +%Y%m%d-%H%M%S)}"
VERBOSE=0
DRY_RUN=0
CONFIRM=1

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly NC='\033[0m'

log() { [[ $VERBOSE -eq 1 ]] && echo -e "${GREEN}[INFO]${NC} $*" >&2 || true; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: hist [OPTIONS]
Purge terminal and application history.

Options:
  -b, --backup    Create backup before purge
  -d, --dry-run   Show what would be deleted
  -v, --verbose   Verbose output
  -y, --yes       Skip confirmation
  -h, --help      Show this help

Version: $VERSION
EOF
}

while [[ $# -gt 0 ]]; do
  case $1 in
    -b|--backup) BACKUP=1; shift ;;
    -d|--dry-run) DRY_RUN=1; VERBOSE=1; shift ;;
    -v|--verbose) VERBOSE=1; shift ;;
    -y|--yes) CONFIRM=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) error "Unknown option: $1" ;;
  esac
done

[[ "${EUID:-$(id -u)}" -eq 0 ]] || error "Run as root."

if [[ $CONFIRM -eq 1 && $DRY_RUN -eq 0 ]]; then
  read -rp "⚠️  Permanently erase ALL history? [y/N] " -n 1
  echo
  [[ $REPLY =~ ^[Yy]$ ]] || { echo "Cancelled." >&2; exit 0; }
fi

secure_erase() {
  local file=$1
  [[ -e "$file" ]] || return 0
  
  if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] Would purge: $file"
    return 0
  fi
  
  if [[ $BACKUP -eq 1 && -s "$file" ]]; then
    mkdir -p "$BACKUP_DIR"
    cp "$file" "$BACKUP_DIR/$(echo "$file" | tr '/' '_').backup" 2>/dev/null || true
  fi
  
  if [[ -s "$file" ]] && command -v shred >/dev/null; then
    shred -uf -n 3 "$file" 2>/dev/null || : > "$file" 2>/dev/null || true
  else
    : > "$file" 2>/dev/null || true
  fi
  
  [[ $VERBOSE -eq 1 ]] && log "Purged: $file"
}

if [[ $DRY_RUN -eq 0 ]]; then
  log "Clearing session history..."
  export HISTFILE=/dev/null HISTSIZE=0 HISTFILESIZE=0
  unset HISTFILE HISTSIZE HISTFILESIZE 2>/dev/null || true
  history -c 2>/dev/null || true
  history -w /dev/null 2>/dev/null || true
fi

for home in /root /home/*; do
  [[ -d "$home" ]] || continue
  [[ $VERBOSE -eq 1 ]] && log "Processing: $home"
  
  files=(
    # Bash
    "$home/.bash_history" "$home/.bash_eternal_history"
    "$home/.local/state/bash/history"
    # Zsh
    "$home/.zsh_history" "$home/.zsh_sessions" "$home/.local/state/zsh/history"
    # Fish
    "$home/.local/share/fish/fish_history" "$home/.config/fish/fish_history" "$home/.local/state/fish/fish_history"
    # Tcsh/Ksh
    "$home/.history" "$home/.ksh_history" "$home/.sh_history"
    # Applications
    "$home/.python_history" "$home/.lesshst" "$home/.wget-hsts"
    "$home/.mysql_history" "$home/.psql_history" "$home/.sqlite_history"
    "$home/.rediscli_history" "$home/.node_repl_history"
    "$home/.viminfo" "$home/.local/share/nvim/shada/main.shada"
    "$home/.nano/history" "$home/.irb_history" "$home/.pry_history"
    # Desktop
    "$home/.local/share/recently-used.xbel"
  )
  
  for f in "${files[@]}"; do secure_erase "$f"; done
  
  # Clean directories
  [[ -d "$home/.zsh_sessions" ]] && rm -rf "$home/.zsh_sessions" 2>/dev/null && log "Removed: $home/.zsh_sessions"
  
  # Find additional history files
  find "$home" -maxdepth 3 -name "*history*" -type f -exec secure_erase {} \; 2>/dev/null
done

# System logs
log_files=(
  "/var/log/apt/history.log" "/var/log/apt/term.log" "/var/log/dpkg.log"
  "/var/log/yum.log" "/var/log/dnf.log" "/var/log/kern.log"
  "/var/log/wtmp" "/var/log/btmp" "/var/log/lastlog"
)
for lf in "${log_files[@]}"; do secure_erase "$lf"; done

rm -f /var/log/apt/history.log.* /var/log/apt/term.log.* /var/log/dpkg.log.* 2>/dev/null || true
rm -f /var/log/yum.log.* /var/log/dnf.log.* /var/log/kern.log.* 2>/dev/null || true

# Journal
if command -v journalctl >/dev/null && [[ $DRY_RUN -eq 0 ]]; then
  log "Cleaning journal..."
  journalctl --rotate 2>/dev/null || true
  journalctl --vacuum-time=1s 2>/dev/null || true
fi

# Temp directories
if [[ $DRY_RUN -eq 0 ]]; then
  log "Cleaning temporary directories..."
  find /tmp /var/tmp -mindepth 1 -delete 2>/dev/null || true
fi

# Memory cache
if [[ -e /proc/sys/vm/drop_caches && $DRY_RUN -eq 0 ]]; then
  sync
  echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
fi

if [[ $DRY_RUN -eq 1 ]]; then
  echo "[DRY-RUN] Completed. No changes made."
else
  log "History purge completed."
  [[ $BACKUP -eq 1 ]] && echo "Backup available in: $BACKUP_DIR"
fi