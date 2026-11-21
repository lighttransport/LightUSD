#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMSDK_DIR="${SCRIPT_DIR}/.emsdk"
EMSDK_VERSION="${EMSDK_VERSION:-7.0.0}"
EMSDK_REPO_URL="${EMSDK_REPO_URL:-https://github.com/emscripten-core/emsdk.git}"
EMSDK_REF="${EMSDK_REF:-${EMSDK_VERSION}}"
OFFLINE="${LIGHTUSD_VERIFY_OFFLINE:-0}"
LOCAL_NODE="${SCRIPT_DIR}/.nodejs/current/bin/node"

source "${SCRIPT_DIR}/setup-nodejs.sh"

if [[ ! -x "${LOCAL_NODE}" ]]; then
  echo "error: local Node.js not found at ${LOCAL_NODE}" >&2
  echo "run ./download-nodejs.sh first" >&2
  exit 1
fi

if [[ ! -d "${EMSDK_DIR}/.git" ]]; then
  [[ "${OFFLINE}" == 1 ]] && { echo "offline: missing emsdk checkout ${EMSDK_DIR}" >&2; exit 2; }
  echo "[download-emsdk] Cloning ${EMSDK_REPO_URL} into ${EMSDK_DIR}"
  git clone "${EMSDK_REPO_URL}" "${EMSDK_DIR}"
else
  echo "[download-emsdk] Reusing ${EMSDK_DIR}"
fi

if [[ "${OFFLINE}" != 1 ]]; then
  git -C "${EMSDK_DIR}" fetch --tags --quiet origin "${EMSDK_REF}"
fi
if ! git -C "${EMSDK_DIR}" cat-file -e "${EMSDK_REF}^{commit}" 2>/dev/null; then
  echo "error: emsdk revision is not cached: ${EMSDK_REF}" >&2
  exit 2
fi
git -C "${EMSDK_DIR}" checkout --quiet --detach "${EMSDK_REF}"

pushd "${EMSDK_DIR}" >/dev/null

echo "[download-emsdk] Installing Emscripten SDK ${EMSDK_VERSION}"
./emsdk install "${EMSDK_VERSION}"

echo "[download-emsdk] Activating Emscripten SDK ${EMSDK_VERSION}"
./emsdk activate "${EMSDK_VERSION}"

EM_CONFIG_PATH="${EMSDK_DIR}/.emscripten"
if [[ ! -f "${EM_CONFIG_PATH}" ]]; then
  echo "error: expected emsdk activation config at ${EM_CONFIG_PATH}" >&2
  exit 1
fi

node <<'NODE'
const fs = require('node:fs');
const path = require('node:path');

const emConfigPath = path.resolve(process.cwd(), '.emscripten');
const nodeExpr = "emsdk_path + '/../.nodejs/current/bin/node'";
let content = fs.readFileSync(emConfigPath, 'utf8');

if (/^NODE_JS = .*$/m.test(content)) {
  content = content.replace(/^NODE_JS = .*$/m, `NODE_JS = ${nodeExpr}`);
} else {
  content += `\nNODE_JS = ${nodeExpr}\n`;
}

fs.writeFileSync(emConfigPath, content, 'utf8');
NODE

popd >/dev/null

cat <<EOF
[download-emsdk] Ready:
  ${EMSDK_DIR}

[download-emsdk] Local Emscripten is configured to use:
  ${LOCAL_NODE}

To use this SDK in the current shell:
  source "${SCRIPT_DIR}/setup-emsdk.sh"
EOF
