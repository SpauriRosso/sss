#!/usr/bin/env bash
set -Eeuo pipefail
IFS=$'\n\t'

VERSION="3.0"
BACKUP_DIR="${BACKUP_DIR:-/var/backups/hist-$(date +%Y%m%d-%H%M%S)}"
VERBOSE=0
DRY_RUN=0
CONFIRM=1
BACKUP=0

readonly RED='\033[0;31m'
readonly GREEN='\033[0;32m'
readonly YELLOW='\033[1;33m'
readonly NC='\033[0m'

# Gestion des erreurs et signaux
cleanup() {
    local exit_code=$?
    [[ $SIGNAL_HANDLER -eq 1 ]] && echo -e "${YELLOW}[INFO]${NC} Interruption détectée, nettoyage..." >&2
    exit $exit_code
}

trap cleanup EXIT INT TERM

log() { [[ $VERBOSE -eq 1 ]] && echo -e "${GREEN}[INFO]${NC} $*" >&2 || true; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*" >&2; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

usage() {
  cat <<EOF
Usage: hist [OPTIONS]
Suppression propre des historiques et logs système.

Options:
  -b, --backup    Créer une sauvegarde avant purge
  -d, --dry-run   Afficher ce qui serait supprimé
  -v, --verbose   Sortie détaillée
  -y, --yes       Ignorer la confirmation
  -h, --help      Afficher cette aide

Version: $VERSION
EOF
}

# Traitement des arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    -b|--backup) BACKUP=1; shift ;;
    -d|--dry-run) DRY_RUN=1; VERBOSE=1; shift ;;
    -v|--verbose) VERBOSE=1; shift ;;
    -y|--yes) CONFIRM=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) error "Option inconnue: $1" ;;
  esac
done

# Vérifications préliminaires
[[ "${EUID:-$(id -u)}" -eq 0 ]] || error "Doit être exécuté en tant que root."

# Confirmation utilisateur
if [[ $CONFIRM -eq 1 && $DRY_RUN -eq 0 ]]; then
  echo -e "${YELLOW}⚠️  ATTENTION${NC}" >&2
  echo "Suppression des historiques et logs système." >&2
  echo "Continuer ? [y/N] " 
  read -r -n 1
  echo
  [[ $REPLY =~ ^[Yy]$ ]] || { echo "Opération annulée." >&2; exit 0; }
fi

# Fonction d'effacement sécurisé
secure_erase() {
  local file=$1
  local real_path
  local backup_path
  
  # Validation du fichier
  [[ -z "$file" ]] && return 1
  real_path=$(readlink -f "$file" 2>/dev/null) || return 0
  [[ -z "$real_path" ]] && return 0
  
  # Protection contre les fichiers système critiques
  if [[ "$real_path" == "/" ]] || [[ "$real_path" =~ ^/(proc|sys|dev)/ ]]; then
    warn "Fichier système ignoré: $file"
    return 0
  fi
  
  # Vérification d'existence et taille
  if [[ ! -e "$real_path" ]]; then
    [[ $VERBOSE -eq 1 ]] && log "Fichier inexistant: $real_path"
    return 0
  fi
  
  local file_size=$(stat -c%s "$real_path" 2>/dev/null || echo 0)
  [[ $file_size -eq 0 ]] && { [[ $VERBOSE -eq 1 ]] && log "Fichier vide ignoré: $real_path"; return 0; }
  
  # Mode dry-run
  if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] Supprimerait: $real_path (${file_size} octets)"
    return 0
  fi
  
  # Création de backup si demandée
  if [[ $BACKUP -eq 1 && $file_size -gt 0 ]]; then
    if mkdir -p "$BACKUP_DIR" 2>/dev/null; then
      backup_path="$BACKUP_DIR/$(echo "$real_path" | tr '/' '_').backup"
      cp "$real_path" "$backup_path" 2>/dev/null && log "Sauvegarde: $backup_path" || warn "Backup échoué: $real_path"
    else
      warn "Impossible de créer le répertoire de sauvegarde"
    fi
  fi
  
  # Effacement sécurisé
  if command -v shred >/dev/null 2>&1; then
    # Utiliser shred si disponible
    if shred -uf -n 3 -z "$real_path" 2>/dev/null; then
      log "Supprimé (shred): $real_path"
    else
      # Fallback si shred échoue
      : > "$real_path" 2>/dev/null && log "Fichier vidé: $real_path" || warn "Suppression échouée: $real_path"
    fi
  else
    # Méthode alternative sans shred
    : > "$real_path" 2>/dev/null && log "Fichier vidé: $real_path" || warn "Suppression échouée: $real_path"
  fi
}

# Nettoyage de l'historique de session
clear_session_history() {
  if [[ $DRY_RUN -eq 0 ]]; then
    log "Suppression de l'historique de session..."
    
    # Sauvegarder et désactiver les variables d'historique
    export HISTFILE=/dev/null HISTSIZE=0 HISTFILESIZE=0
    
    # Vider l'historique actuel
    history -c 2>/dev/null || true
    history -w /dev/null 2>/dev/null || true
    
    # Désactiver l'historique pour cette session
    set +H 2>/dev/null || true
  fi
}

# Traitement des répertoires utilisateurs
process_user_directories() {
  local home
  local files
  
  log "Nettoyage des répertoires utilisateurs..."
  
  for home in /root /home/*; do
    [[ -d "$home" && -r "$home" ]] || continue
    [[ $VERBOSE -eq 1 ]] && log "Traitement de: $home"
    
    # Liste complète des fichiers d'historique
    files=(
      # Shells principaux
      "$home/.bash_history" "$home/.bash_eternal_history"
      "$home/.local/state/bash/history"
      "$home/.zsh_history" "$home/.zsh_sessions" 
      "$home/.local/state/zsh/history"
      "$home/.local/share/fish/fish_history" 
      "$home/.config/fish/fish_history" 
      "$home/.local/state/fish/fish_history"
      "$home/.history" "$home/.ksh_history" "$home/.sh_history"
      
      # Python et outils de développement
      "$home/.python_history" "$home/.ipython_history"
      "$home/.lesshst" "$home/.wget-hsts"
      
      # Bases de données
      "$home/.mysql_history" "$home/.psql_history" "$home/.sqlite_history"
      "$home/.rediscli_history"
      
      # Éditeurs
      "$home/.viminfo" "$home/.local/share/nvim/shada/main.shada"
      "$home/.nano/history" "$home/.irb_history" "$home/.pry_history"
      
      # Autres outils
      "$home/.node_repl_history"
    )
    
    # Traitement des fichiers
    for f in "${files[@]}"; do
      secure_erase "$f"
    done
    
    # Nettoyage des répertoires spécifiques
    [[ -d "$home/.zsh_sessions" ]] && {
      [[ $DRY_RUN -eq 0 ]] && rm -rf "$home/.zsh_sessions" 2>/dev/null
      log "Répertoire supprimé: $home/.zsh_sessions"
    }
    
    # Recherche de fichiers d'historique supplémentaires
    find "$home" -maxdepth 3 -name "*history*" -type f 2>/dev/null | while read -r hist_file; do
      secure_erase "$hist_file"
    done
  done
}

# Traitement des logs système
process_system_logs() {
  log "Nettoyage des logs système..."
  
  local log_files=(
    # Logs apt/dpkg
    "/var/log/apt/history.log" "/var/log/apt/term.log" "/var/log/dpkg.log"
    # Logs système principaux
    "/var/log/auth.log" "/var/log/syslog" "/var/log/kern.log"
    "/var/log/wtmp" "/var/log/btmp" "/var/log/lastlog"
    # Logs application
    "/var/log/mysql/" "/var/log/postgresql/" "/var/log/redis/"
  )
  
  # Suppression des logs principaux
  for lf in "${log_files[@]}"; do
    [[ -e "$lf" ]] && secure_erase "$lf"
  done
  
  # Suppression des anciens logs compressés
  if [[ $DRY_RUN -eq 0 ]]; then
    log "Suppression des anciens logs..."
    rm -f /var/log/apt/history.log.* /var/log/apt/term.log.* /var/log/dpkg.log.* 2>/dev/null || true
    rm -f /var/log/auth.log.* /var/log/syslog.* /var/log/kern.log.* 2>/dev/null || true
    rm -f /var/log/mysql/*.log* 2>/dev/null || true
    rm -f /var/log/postgresql/*.log* 2>/dev/null || true
    rm -f /var/log/redis/*.log* 2>/dev/null || true
  fi
  
  # Journal systemd
  if command -v journalctl >/dev/null && [[ $DRY_RUN -eq 0 ]]; then
    log "Nettoyage du journal systemd..."
    journalctl --rotate 2>/dev/null || true
    journalctl --vacuum-time=1s 2>/dev/null || true
    journalctl --flush 2>/dev/null || true
  fi
}

# Nettoyage des répertoires temporaires
clean_temporary_files() {
  if [[ $DRY_RUN -eq 0 ]]; then
    log "Nettoyage des fichiers temporaires..."
    
    # Nettoyage des répertoires temporaires
    find /tmp /var/tmp -mindepth 1 -delete 2>/dev/null || true
  fi
}

# Nettoyage du cache mémoire
clean_memory_cache() {
  if [[ $DRY_RUN -eq 0 ]]; then
    log "Nettoyage du cache mémoire..."
    
    # Synchronisation forcée
    sync
    
    # Nettoyage du cache si possible
    if [[ -e /proc/sys/vm/drop_caches ]]; then
      echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
    fi
  fi
}

# Fonction principale
main() {
  local start_time=$(date +%s)
  
  echo "=== Nettoyage des historiques et logs v$VERSION ===" >&2
  [[ $DRY_RUN -eq 1 ]] && echo "[MODE SIMULATION]" >&2
  echo
  
  # Étapes de nettoyage
  clear_session_history
  process_user_directories
  process_system_logs
  clean_temporary_files
  clean_memory_cache
  
  # Résumé
  local end_time=$(date +%s)
  local duration=$((end_time - start_time))
  
  echo
  if [[ $DRY_RUN -eq 1 ]]; then
    echo "[SIMULATION] Nettoyage simulé terminé." >&2
  else
    echo "✓ Nettoyage terminé avec succès." >&2
    echo "⏱️ Durée: ${duration}s" >&2
    [[ $BACKUP -eq 1 ]] && echo "📦 Sauvegarde: $BACKUP_DIR" >&2
  fi
}

# Exécution
main "$@"