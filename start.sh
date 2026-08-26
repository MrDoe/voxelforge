#!/usr/bin/env bash
set -euo pipefail
# VoxelForge + llama.cpp (gemma4) starter
# - starts llama-server with gemma4 models if needed
# - ensures world assets exist
# - launches voxelforge with VF_LLM_* pointing at llama.cpp's OpenAI endpoint

ROOT="$(cd "$(dirname "$0")" && pwd)"
LLAMA_BIN="${LLAMA_BIN:-/opt/llama.cpp/build/bin/llama-server}"
# fall back to /usr/local/bin/llama-server (symlink)
if [[ ! -x "$LLAMA_BIN" && -x /usr/local/bin/llama-server ]]; then
  LLAMA_BIN=/usr/local/bin/llama-server
fi
MODELS_INI="${MODELS_INI:-/home/christoph/models/models.ini}"
LLAMA_HOST="${LLAMA_HOST:-127.0.0.1}"
LLAMA_PORT="${LLAMA_PORT:-8080}"
LLAMA_URL="http://${LLAMA_HOST}:${LLAMA_PORT}/v1"
# gemma4 preset names from models.ini
LLM_MODEL="${VF_LLM_MODEL:-${LLM_MODEL:-gemma-4-e4b}}"
BUILD_DIR="${BUILD_DIR:-build}"
# VRAM-aware defaults (override via env): 32k ctx + draft + models-max 2 OOMs on 16GB with 2 models
LLAMA_CTX="${LLAMA_CTX:-8192}"
LLAMA_NGL="${LLAMA_NGL:-99}"
LLAMA_MODELS_MAX="${LLAMA_MODELS_MAX:-1}"
LLAMA_SPEC="${LLAMA_SPEC:-0}"

# 1) build if needed
if [[ ! -x "$ROOT/$BUILD_DIR/voxelforge" ]]; then
  echo "[start] building voxelforge (ninja -C $BUILD_DIR)..."
  ninja -C "$ROOT/$BUILD_DIR" -j"$(nproc)"
fi

# 2) world assets are NOT generated here — baking is an explicit extra tool.
if [[ ! -f "$ROOT/assets/world.json" ]]; then
  echo "[start] assets/world.json missing - run 'ninja -C $BUILD_DIR world' first" >&2
  exit 1
fi

# 3) llama.cpp server — reuse any existing instance to avoid double VRAM
need_server=1
# probe requested port first, then common fallbacks (avoids spawning second router on 8088 when 8080 already uses 10GB)
CANDIDATES=()
_add_candidate() {
  local _p="$1"
  for _e in "${CANDIDATES[@]}"; do [[ "$_e" == "$_p" ]] && return 0; done
  CANDIDATES+=("$_p")
}
_add_candidate "$LLAMA_PORT"
_add_candidate "8080"
_add_candidate "8088"
for _p in "${CANDIDATES[@]}"; do
  if curl -sf "http://${LLAMA_HOST}:${_p}/v1/models" >/dev/null 2>&1; then
    if [[ "$_p" != "$LLAMA_PORT" ]]; then
      echo "[start] reusing existing llama-server at ${LLAMA_HOST}:${_p} (requested :${LLAMA_PORT})"
    else
      echo "[start] llama-server already up at ${LLAMA_HOST}:${_p}"
    fi
    LLAMA_PORT="$_p"
    LLAMA_URL="http://${LLAMA_HOST}:${LLAMA_PORT}/v1"
    need_server=0
    break
  fi
done

if [[ $need_server -eq 0 ]]; then
  : # reused — no launch needed
else
  # --- hard guarantee: never run a second llama-server ---
  # curl probes can miss a server that is still starting; check process table directly
  if pgrep -f "[l]lama-server" >/dev/null 2>&1; then
    echo "[start] detected running llama-server process — not starting second instance" >&2
    ps aux | grep "[l]lama-server" | sed 's/^/[start]   /' >&2 || true
    _FOUND_PORT=""
    for _pp in 8080 8088 "$LLAMA_PORT"; do
      # dedupe already-checked ports
      _dup=0; for _c in "${CANDIDATES[@]}"; do [[ "$_c" == "$_pp" ]] && _dup=1; done
      if curl -sf "http://${LLAMA_HOST}:${_pp}/v1/models" >/dev/null 2>&1; then
        _FOUND_PORT="$_pp"
        break
      fi
    done
    # also try to extract port from /proc cmdline if curl still fails
    if [[ -z "$_FOUND_PORT" ]]; then
      for _pid in $(pgrep -f "[l]lama-server" 2>/dev/null); do
        _cmd="$(tr '\0' ' ' </proc/"$_pid"/cmdline 2>/dev/null || true)"
        _pport="$(echo "$_cmd" | grep -oP '(?:--port\s+|:)\\K[0-9]{4,5}' | head -n1 || true)"
        if [[ -n "$_pport" ]] && curl -sf "http://${LLAMA_HOST}:${_pport}/v1/models" >/dev/null 2>&1; then
          _FOUND_PORT="$_pport"
          break
        fi
      done
    fi
    if [[ -n "$_FOUND_PORT" ]]; then
      echo "[start] reusing detected instance at ${LLAMA_HOST}:${_FOUND_PORT} — will not launch second server" >&2
      LLAMA_PORT="$_FOUND_PORT"
      LLAMA_URL="http://${LLAMA_HOST}:${LLAMA_PORT}/v1"
      need_server=0
    else
      echo "[start] ERROR: llama-server process exists but no endpoint (8080/8088/${LLAMA_PORT}) responds" >&2
      echo "       another start is in progress or server is starting — not launching second copy" >&2
      echo "       check: ps aux | grep llama-server; ss -ltnp | grep llama; curl -sf http://127.0.0.1:8080/v1/models" >&2
      echo "       fix: pkill -f llama-server  then retry, or wait 10s and retry" >&2
      exit 1
    fi
  fi
  if [[ $need_server -eq 0 ]]; then
    : # discovered via ps — skip launch, reuse existing
  else
  # serialize concurrent start.sh invocations (do NOT touch fd 2 here —
  # exec redirections persist and would swallow all later >&2 diagnostics)
  exec 9>/tmp/voxelforge-llama.lock || true
  if ! flock -n 9 2>/dev/null; then
    echo "[start] another start.sh holds llama launch lock — waiting for it..." >&2
    flock 9 2>/dev/null || sleep 2
    # re-probe after lock
    for _p in "${CANDIDATES[@]}"; do
      if curl -sf "http://${LLAMA_HOST}:${_p}/v1/models" >/dev/null 2>&1; then
        echo "[start] reusing server that appeared while waiting at ${LLAMA_HOST}:${_p}" >&2
        LLAMA_PORT="$_p"
        LLAMA_URL="http://${LLAMA_HOST}:${LLAMA_PORT}/v1"
        need_server=0
        break
      fi
    done
    if [[ $need_server -eq 0 ]]; then
      : # lock winner started it
    else
      echo "[start] ERROR: lock released but still no server — aborting to avoid duplicate" >&2
      exit 1
    fi
  fi
  if [[ $need_server -eq 0 ]]; then
    : # re-probed via lock — no launch
  else
  if [[ ! -x "$LLAMA_BIN" ]]; then
    echo "[start] ERROR: llama-server not found at $LLAMA_BIN" >&2
    echo "        install llama.cpp or set LLAMA_BIN, or use Ollama (VF_LLM_URL=http://127.0.0.1:11434)" >&2
    exit 1
  fi
  # build llama args from env-controlled defaults
  LLAMA_ARGS=(--host "$LLAMA_HOST" --port "$LLAMA_PORT" -ngl "$LLAMA_NGL" -c "$LLAMA_CTX" --jinja --models-max "$LLAMA_MODELS_MAX" --reasoning off)
  if [[ "$LLAMA_SPEC" == "1" ]]; then
    LLAMA_ARGS+=(--spec-type draft-mtp --spec-draft-n-max 3)
  fi
  if [[ ! -f "$MODELS_INI" ]]; then
    echo "[start] WARN: models preset $MODELS_INI missing — starting with single model" >&2
    # fallback: try default gemma model blob
    GEMMA_BLOB="$(ls -1 /home/christoph/.ollama/models/blobs/sha256-127* 2>/dev/null | head -n1 || true)"
    if [[ -n "$GEMMA_BLOB" ]]; then
      echo "[start] launching $LLAMA_BIN --model $GEMMA_BLOB (ctx $LLAMA_CTX) ..."
      "$LLAMA_BIN" --host "$LLAMA_HOST" --port "$LLAMA_PORT" --model "$GEMMA_BLOB" -c "$LLAMA_CTX" --jinja -ngl "$LLAMA_NGL" &
    else
      echo "[start] launching $LLAMA_BIN (no preset, no blob — server must resolve a model itself) ..." >&2
      "$LLAMA_BIN" "${LLAMA_ARGS[@]}" &
    fi
  else
    echo "[start] launching $LLAMA_BIN --models-preset $MODELS_INI (ctx $LLAMA_CTX, ngl $LLAMA_NGL, models-max $LLAMA_MODELS_MAX, spec $LLAMA_SPEC) ..."
    "$LLAMA_BIN" --models-preset "$MODELS_INI" "${LLAMA_ARGS[@]}" &
  fi
  LLAMA_PID=$!
  echo "[start] waiting for llama-server at $LLAMA_URL (ctx $LLAMA_CTX, ngl $LLAMA_NGL) ..."
  for i in $(seq 1 60); do
    if curl -sf "${LLAMA_URL}/models" >/dev/null 2>&1; then
      echo "[start] llama-server ready (pid $LLAMA_PID)"
      break
    fi
    sleep 1
    if ! kill -0 "$LLAMA_PID" 2>/dev/null; then
      echo "[start] llama-server died early — check VRAM (nvidia-smi) and try LLAMA_CTX=4096 LLAMA_MODELS_MAX=1 LLAMA_SPEC=0" >&2
      exit 1
    fi
    if [[ $i -eq 60 ]]; then
      echo "[start] timeout waiting for llama-server" >&2
      exit 1
    fi
  done
  # keep pid for cleanup on voxelforge exit (optional)
  trap 'kill $LLAMA_PID 2>/dev/null || true' EXIT
  # hint if VRAM already tight
  if command -v nvidia-smi >/dev/null 2>&1; then
    nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader 2>/dev/null | awk -F', ' '{printf "[start] VRAM %s / %s used after launch\n",$1,$2}' || true
  fi
  fi
 fi
fi

# 4) pick model that is actually available — prefer already-loaded to avoid OOM swap
MODELS_JSON="$(curl -sf "${LLAMA_URL}/models" 2>/dev/null || true)"
AVAILABLE=""
LOADED=""
if [[ -n "$MODELS_JSON" ]]; then
  AVAILABLE="$(echo "$MODELS_JSON" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("\n".join(m["id"] for m in d.get("data",[])))' 2>/dev/null || true)"
  LOADED="$(echo "$MODELS_JSON" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("\n".join(m["id"] for m in d.get("data",[]) if m.get("status",{}).get("value")=="loaded"))' 2>/dev/null || true)"
fi
if [[ -n "$AVAILABLE" ]]; then
  echo "[start] available models:"
  echo "$AVAILABLE" | sed 's/^/  - /'
  if [[ -n "$LOADED" ]]; then
    echo "[start] loaded models:"
    echo "$LOADED" | sed 's/^/  * /'
  fi
  if ! echo "$AVAILABLE" | grep -qx "$LLM_MODEL"; then
    # fall back to loaded model first, then first available
    FALLBACK=""
    if [[ -n "$LOADED" ]]; then FALLBACK="$(echo "$LOADED" | head -n1)"; fi
    if [[ -z "$FALLBACK" ]]; then FALLBACK="$(echo "$AVAILABLE" | head -n1)"; fi
    if [[ -n "$FALLBACK" ]]; then
      echo "[start] $LLM_MODEL not in list, using $FALLBACK"
      LLM_MODEL="$FALLBACK"
    fi
  elif [[ -n "$LOADED" ]] && ! echo "$LOADED" | grep -qx "$LLM_MODEL"; then
    # requested exists but is unloaded — another model already occupies VRAM (10GB).
    # Auto-pick resident to avoid on-demand swap that would OOM (needs 7–8GB extra).
    # This also enforces "never run second server": reuse existing instead of launching duplicate for other model.
    _RESIDENT="$(echo "$LOADED" | head -n1)"
    echo "[start] $LLM_MODEL is unloaded, but $_RESIDENT is already resident — using $_RESIDENT to avoid OOM/duplicate"
    echo "[start] hint: to force $LLM_MODEL, restart server: pkill -f llama-server; VF_LLM_MODEL=$LLM_MODEL LLAMA_CTX=$LLAMA_CTX LLAMA_MODELS_MAX=1 ./start.sh"
    LLM_MODEL="$_RESIDENT"
  fi
fi

echo "[start] launching voxelforge with $LLM_MODEL @ $LLAMA_URL"
echo "        Ctrl+LMB = pick voxel (bottom-center anchor) | Chat: 'put a 3x3 wood crate here'"
# auto-detect: voxelforge's OllamaClient tries VF_LLM_URL then Ollama; we force llama.cpp endpoint
export VF_LLM_URL="$LLAMA_URL"
export VF_LLM_MODEL="$LLM_MODEL"
# optional: keep Ollama as fallback if llama-server dies (client is sticky per AGENTS.md)
# export OLLAMA_HOST is not needed

exec "$ROOT/$BUILD_DIR/voxelforge" "$@"
