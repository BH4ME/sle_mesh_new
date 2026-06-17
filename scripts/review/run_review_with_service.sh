#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/review/run_review_with_service.sh [--scope "<scope>"] [--goal-doc <path>] [--model <model>] [--dry-run]

Options:
  --scope      审查范围。默认: 全量审查（新仓库/大版本）
  --goal-doc   特性验证目标文档（可选），如 docs/v2/networking-goal.md
  --model      模型名。默认: review-model
  --dry-run    仅打印 prompt，不请求 API

Environment:
  REVIEW_API_KEY  review service API key
  REVIEW_API_URL  review service chat-completions URL
  REVIEW_CONTEXT_BYTES  review context byte limit (optional, default 200000)

Behavior:
  1) 把 docs/v2/review_framework.md、目标文档、git 变更注入 prompt
  2) 按 Stage 执行审查
  3) 输出覆盖写入 meta/review_feedback.md
EOF
}

SCOPE="全量审查（新仓库/大版本）"
GOAL_DOC=""
MODEL="${REVIEW_MODEL:-review-model}"
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scope)
      SCOPE="${2:-}"
      shift 2
      ;;
    --goal-doc)
      GOAL_DOC="${2:-}"
      shift 2
      ;;
    --model)
      MODEL="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  echo "Not inside a git repository." >&2
  exit 1
fi
cd "$ROOT"

if [[ ! -f "docs/v2/review_framework.md" ]]; then
  echo "Missing docs/v2/review_framework.md" >&2
  exit 1
fi

if [[ -n "$GOAL_DOC" && ! -f "$GOAL_DOC" ]]; then
  echo "Goal doc not found: $GOAL_DOC" >&2
  exit 1
fi

if [[ $DRY_RUN -eq 0 ]] && ! command -v curl >/dev/null 2>&1; then
  echo "curl not found." >&2
  exit 1
fi

if [[ $DRY_RUN -eq 0 ]] && ! command -v jq >/dev/null 2>&1; then
  echo "jq not found. Please install jq first." >&2
  exit 1
fi

API_KEY="${REVIEW_API_KEY:-}"
if [[ -z "$API_KEY" && $DRY_RUN -eq 0 ]]; then
  echo "Missing API key. Set REVIEW_API_KEY." >&2
  exit 1
fi

API_URL="${REVIEW_API_URL:-}"
if [[ -z "$API_URL" && $DRY_RUN -eq 0 ]]; then
  echo "Missing review service URL. Set REVIEW_API_URL." >&2
  exit 1
fi
BRANCH="$(git branch --show-current)"
TODAY="$(date +%F)"
MAX_CONTEXT_BYTES="${REVIEW_CONTEXT_BYTES:-200000}"

PROMPT_FILE="$(mktemp)"
RESP_FILE="$(mktemp)"
FRAMEWORK_FILE="$(mktemp)"
VERSIONS_FILE="$(mktemp)"
STATUS_FILE="$(mktemp)"
CACHED_DIFF_FILE="$(mktemp)"
WORKTREE_DIFF_FILE="$(mktemp)"
UNTRACKED_DIFF_FILE="$(mktemp)"
GOAL_FILE="$(mktemp)"
trap 'rm -f "$PROMPT_FILE" "$RESP_FILE" "$FRAMEWORK_FILE" "$VERSIONS_FILE" "$STATUS_FILE" "$CACHED_DIFF_FILE" "$WORKTREE_DIFF_FILE" "$UNTRACKED_DIFF_FILE" "$GOAL_FILE"' EXIT

cat docs/v2/review_framework.md > "$FRAMEWORK_FILE"
cat versions/README.md > "$VERSIONS_FILE"
git status --short > "$STATUS_FILE"
git diff --cached --no-color > "$CACHED_DIFF_FILE"
git diff --no-color > "$WORKTREE_DIFF_FILE"
: > "$UNTRACKED_DIFF_FILE"

while IFS= read -r file; do
  if [[ -z "$file" || ! -f "$file" ]]; then
    continue
  fi
  if LC_ALL=C grep -Iq . "$file"; then
    {
      echo "### UNTRACKED FILE: $file"
      git diff --no-color --no-index -- /dev/null "$file" || true
      echo
    } >> "$UNTRACKED_DIFF_FILE"
  else
    echo "### UNTRACKED FILE: $file (binary skipped)" >> "$UNTRACKED_DIFF_FILE"
  fi
done < <(git ls-files --others --exclude-standard)

if [[ -n "$GOAL_DOC" ]]; then
  cat "$GOAL_DOC" > "$GOAL_FILE"
fi

append_section_with_limit() {
  local title="$1"
  local section_file="$2"
  local section_size remaining

  if [[ ! -s "$section_file" ]]; then
    return
  fi
  remaining=$((MAX_CONTEXT_BYTES - CONTEXT_BYTES))
  if (( remaining <= 0 )); then
    return
  fi
  section_size=$(( $(wc -c < "$section_file") ))
  {
    echo
    echo "===== ${title} ====="
  } >> "$PROMPT_FILE"
  if (( section_size <= remaining )); then
    cat "$section_file" >> "$PROMPT_FILE"
    CONTEXT_BYTES=$((CONTEXT_BYTES + section_size))
    return
  fi
  head -c "$remaining" "$section_file" >> "$PROMPT_FILE"
  {
    echo
    echo "[TRUNCATED] section exceeded REVIEW_CONTEXT_BYTES=${MAX_CONTEXT_BYTES}"
  } >> "$PROMPT_FILE"
  CONTEXT_BYTES=$MAX_CONTEXT_BYTES
}

{
  echo "你现在是代码审查执行器。请严格遵循以下要求："
  echo
  echo "1. 你无法直接访问本地仓库；必须仅基于下方提供的上下文进行审查。"
  echo "2. 优先使用 'REVIEW_FRAMEWORK' 执行 Stage，不要跳步。"
  echo "3. Scope 使用：${SCOPE}"
  echo "4. 生成完整 Markdown 审查报告正文（不要解释过程，不要代码块包裹）。"
  echo "5. 报告头信息必须包含："
  echo "   - Reviewer: Review Service (configured-provider)"
  echo "   - Date: ${TODAY}"
  echo "   - Version: 按 framework 规则从 versions/README.md 读取"
  echo "   - Branch: ${BRANCH}"
  echo "   - Scope: ${SCOPE}"
  if [[ -n "$GOAL_DOC" ]]; then
    echo "6. 本次特性验证目标文档路径：${GOAL_DOC}"
  fi
  echo
  echo "输出仅包含最终审查 Markdown，不要包含多余说明。"
} > "$PROMPT_FILE"

CONTEXT_BYTES=0
append_section_with_limit "REVIEW_FRAMEWORK (docs/v2/review_framework.md)" "$FRAMEWORK_FILE"
append_section_with_limit "VERSIONS_INDEX (versions/README.md)" "$VERSIONS_FILE"
if [[ -n "$GOAL_DOC" ]]; then
  append_section_with_limit "GOAL_DOC (${GOAL_DOC})" "$GOAL_FILE"
fi
append_section_with_limit "GIT_STATUS_SHORT" "$STATUS_FILE"
append_section_with_limit "GIT_DIFF_CACHED" "$CACHED_DIFF_FILE"
append_section_with_limit "GIT_DIFF_WORKTREE" "$WORKTREE_DIFF_FILE"
append_section_with_limit "GIT_DIFF_UNTRACKED" "$UNTRACKED_DIFF_FILE"

if [[ $DRY_RUN -eq 1 ]]; then
  echo "===== Prompt Begin ====="
  cat "$PROMPT_FILE"
  echo "===== Prompt End ====="
  exit 0
fi

REQUEST_JSON="$(mktemp)"
trap 'rm -f "$PROMPT_FILE" "$RESP_FILE" "$REQUEST_JSON" "$FRAMEWORK_FILE" "$VERSIONS_FILE" "$STATUS_FILE" "$CACHED_DIFF_FILE" "$WORKTREE_DIFF_FILE" "$UNTRACKED_DIFF_FILE" "$GOAL_FILE"' EXIT

jq -n \
  --arg model "$MODEL" \
  --arg prompt "$(cat "$PROMPT_FILE")" \
  '{
    model: $model,
    messages: [
      {role: "system", content: "你是严谨的代码审查助手。"},
      {role: "user", content: $prompt}
    ],
    temperature: 0.1,
    stream: false
  }' > "$REQUEST_JSON"

curl -sS "$API_URL" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $API_KEY" \
  -d @"$REQUEST_JSON" > "$RESP_FILE"

if jq -e '.error' "$RESP_FILE" >/dev/null 2>&1; then
  echo "Review service error:" >&2
  jq '.error' "$RESP_FILE" >&2
  exit 1
fi

CONTENT="$(jq -r '.choices[0].message.content // empty' "$RESP_FILE")"
if [[ -z "$CONTENT" ]]; then
  echo "Review service returned empty content." >&2
  jq '.' "$RESP_FILE" >&2
  exit 1
fi

printf '%s\n' "$CONTENT" > meta/review_feedback.md

if ! rg -q "^# Code Review Feedback" meta/review_feedback.md; then
  echo "meta/review_feedback.md updated, but header is unexpected. Please verify manually." >&2
  exit 1
fi

echo "meta/review_feedback.md updated by review service"
