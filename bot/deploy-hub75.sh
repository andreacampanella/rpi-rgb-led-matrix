#!/usr/bin/env bash
# deploy-hub75.sh — Build and deploy ft-server + hub75_bot on a Raspberry Pi
# Usage: bash deploy-hub75.sh
set -euo pipefail

# --- Config ---
REPO_RGB="https://github.com/andreacampanella/rpi-rgb-led-matrix.git"
REPO_FT="https://github.com/hzeller/flaschen-taschen.git"
INSTALL_DIR="$HOME/hub75-matrix"
BIN_DIR="$HOME/bin"
CONF_FILE="$HOME/.hub75_bot.conf"
GIF_DIR="$HOME/gifs"

FT_FLAGS="--led-gpio-mapping=footleg-robotics --led-rows=64 --led-cols=64 --led-slowdown-gpio=3 --led-brightness=100"

# --- Colors ---
info()  { printf '\033[1;34m[INFO]\033[0m  %s\n' "$*"; }
warn()  { printf '\033[1;33m[WARN]\033[0m  %s\n' "$*"; }
err()   { printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# --- Preflight ---
[[ $EUID -eq 0 ]] && err "Don't run as root. The script will sudo when needed."

info "Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
  git build-essential \
  libgraphicsmagick++-dev libwebp-dev \
  libavcodec-dev libavformat-dev libswscale-dev libavutil-dev \
  python3 python3-pip \
  2>/dev/null

python3 -c "import telegram" 2>/dev/null || {
  info "Installing python-telegram-bot..."
  pip3 install python-telegram-bot --break-system-packages -q
}

# --- Directory setup ---
mkdir -p "$INSTALL_DIR" "$BIN_DIR" "$GIF_DIR"

# --- Clone / update repos ---
clone_or_pull() {
  local url="$1" dir="$2"
  if [[ -d "$dir/.git" ]]; then
    info "Updating $dir..."
    git -C "$dir" pull --ff-only || warn "Pull failed, using existing checkout"
  else
    info "Cloning $url -> $dir..."
    git clone "$url" "$dir"
  fi
}

clone_or_pull "$REPO_RGB" "$INSTALL_DIR/rpi-rgb-led-matrix"
clone_or_pull "$REPO_FT"  "$INSTALL_DIR/flaschen-taschen"

# --- Point flaschen-taschen at our patched rpi-rgb-led-matrix ---
info "Linking patched rpi-rgb-led-matrix into flaschen-taschen..."
rm -rf "$INSTALL_DIR/flaschen-taschen/server/rgb-matrix"
ln -sfn "$INSTALL_DIR/rpi-rgb-led-matrix" "$INSTALL_DIR/flaschen-taschen/server/rgb-matrix"

# --- Build ---
info "Building ft-server (FT_BACKEND=rgb-matrix)..."
make -C "$INSTALL_DIR/flaschen-taschen/server" clean 2>/dev/null || true
make -C "$INSTALL_DIR/flaschen-taschen/server" FT_BACKEND=rgb-matrix -j"$(nproc)"

info "Building ft client tools..."
make -C "$INSTALL_DIR/flaschen-taschen/client" -j"$(nproc)"

# --- Install binaries ---
info "Installing binaries to $BIN_DIR..."
cp "$INSTALL_DIR/flaschen-taschen/server/ft-server" "$BIN_DIR/"
for tool in send-image send-video send-text; do
  [[ -f "$INSTALL_DIR/flaschen-taschen/client/$tool" ]] && \
    cp "$INSTALL_DIR/flaschen-taschen/client/$tool" "$BIN_DIR/"
done

# --- Config file ---
CONF_EXAMPLE="$INSTALL_DIR/rpi-rgb-led-matrix/bot/hub75_bot.conf.example"
if [[ ! -f "$CONF_FILE" ]]; then
  if [[ -f "$CONF_EXAMPLE" ]]; then
    info "Creating config from example -> $CONF_FILE"
    cp "$CONF_EXAMPLE" "$CONF_FILE"
  else
    warn "Config example not found, creating minimal config"
    printf 'BOT_TOKEN=YOUR_TELEGRAM_BOT_TOKEN_HERE\n' > "$CONF_FILE"
  fi
  chmod 600 "$CONF_FILE"
  warn "Edit $CONF_FILE and set your BOT_TOKEN before starting the bot!"
else
  info "Config file $CONF_FILE already exists, leaving it alone."
fi

# --- Install systemd services from templates ---
SERVICES_DIR="$INSTALL_DIR/rpi-rgb-led-matrix/services"
BOT_SCRIPT="$INSTALL_DIR/rpi-rgb-led-matrix/bot/hub75_bot.py"

[[ -d "$SERVICES_DIR" ]] || err "Services directory not found: $SERVICES_DIR"

info "Installing systemd services..."

sed -e "s|__BIN_DIR__|$BIN_DIR|g" \
    -e "s|__FT_FLAGS__|$FT_FLAGS|g" \
    "$SERVICES_DIR/ft-server.service" \
  | sudo tee /etc/systemd/system/ft-server.service > /dev/null

sed -e "s|__USER__|$USER|g" \
    -e "s|__BOT_SCRIPT__|$BOT_SCRIPT|g" \
    -e "s|__BIN_DIR__|$BIN_DIR|g" \
    "$SERVICES_DIR/hub75_bot.service" \
  | sudo tee /etc/systemd/system/hub75_bot.service > /dev/null

sudo systemctl daemon-reload
sudo systemctl enable ft-server.service hub75_bot.service

# --- Blacklist onboard sound (conflicts with GPIO timing) ---
if lsmod | grep -q snd_bcm2835; then
  warn "snd_bcm2835 is loaded — blacklisting it (required for LED matrix)..."
  echo "blacklist snd_bcm2835" | sudo tee /etc/modprobe.d/blacklist-rgb-matrix.conf > /dev/null
  sudo update-initramfs -u
  warn "Reboot required for sound blacklist to take effect."
fi

# --- Summary ---
cat <<DONE

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  Deployment complete
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Install dir:  $INSTALL_DIR
  Binaries:     $BIN_DIR/{ft-server,send-image,send-video,send-text}
  Bot script:   $BOT_SCRIPT
  Config:       $CONF_FILE
  GIF folder:   $GIF_DIR

  Services:
    ft-server.service   — LED matrix server (runs as root)
    hub75_bot.service   — Telegram bot (runs as $USER)

  Next steps:
    1. Edit $CONF_FILE and set your BOT_TOKEN
    2. sudo systemctl start ft-server hub75_bot
       Or reboot — both auto-start on boot.
    3. Send /start to your Telegram bot

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
DONE