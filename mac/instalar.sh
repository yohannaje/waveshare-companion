#!/bin/bash
# Instala el "oido de musica" de Chipi en la Mac.
# Cada pocos segundos pregunta al cerebro de Chipi si hay un pedido de musica y se lo pasa a la app de Spotify.
# Solo puede reproducir las playlists que el cerebro le indique (spotify:playlist:<22 caracteres>), pausar o pasar de cancion.
# Para sacarlo:  bash instalar.sh --quitar
set -e
DIR="$HOME/.chipi"
PLIST="$HOME/Library/LaunchAgents/com.chipi.musica.plist"
LABEL="com.chipi.musica"

if [ "$1" = "--quitar" ]; then
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  rm -f "$PLIST" "$DIR/chipi-musica.sh" "$DIR/ultimo" "$DIR/error.log"
  rmdir "$DIR" 2>/dev/null || true
  echo "Listo: Chipi ya no controla Spotify en esta Mac."
  exit 0
fi

mkdir -p "$DIR" "$HOME/Library/LaunchAgents"

cat > "$DIR/chipi-musica.sh" <<'SCRIPT'
#!/bin/bash
URL="https://companero.vc5z8pn7nd.workers.dev/mac"
ESTADO="$HOME/.chipi/ultimo"

spotify_abierto() { pgrep -x Spotify >/dev/null 2>&1; }
abrir_spotify() {
  if ! spotify_abierto; then
    open -a Spotify
    for i in $(seq 1 30); do spotify_abierto && break; sleep 1; done
    sleep 4
  fi
}

# Al arrancar, no repetir pedidos viejos.
if [ ! -f "$ESTADO" ]; then
  R=$(curl -fsS -m 10 "$URL" 2>/dev/null || true)
  echo "${R%%|*}" > "$ESTADO"
fi

while true; do
  R=$(curl -fsS -m 10 "$URL" 2>/dev/null || true)
  if [ -n "$R" ]; then
    IFS='|' read -r N ACCION URI <<< "$R"
    ULTIMO=$(cat "$ESTADO" 2>/dev/null || echo 0)
    if [[ "$N" =~ ^[0-9]+$ ]] && [ "$N" -gt "${ULTIMO:-0}" ]; then
      echo "$N" > "$ESTADO"
      case "$ACCION" in
        play)
          if [[ "$URI" =~ ^spotify:playlist:[A-Za-z0-9]{22}$ ]]; then
            abrir_spotify
            osascript -e "tell application \"Spotify\" to play track \"$URI\"" >/dev/null 2>&1 || true
          fi ;;
        pause) if spotify_abierto; then osascript -e 'tell application "Spotify" to pause' >/dev/null 2>&1 || true; fi ;;
        next)  if spotify_abierto; then osascript -e 'tell application "Spotify" to next track' >/dev/null 2>&1 || true; fi ;;
      esac
    fi
  fi
  sleep 6
done
SCRIPT
chmod +x "$DIR/chipi-musica.sh"

cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>/bin/bash</string><string>$DIR/chipi-musica.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>$DIR/error.log</string>
</dict></plist>
PL

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "Listo. Chipi ya puede poner musica en esta Mac."
echo "La primera vez, macOS te va a preguntar si permitis controlar Spotify: toca Permitir."
