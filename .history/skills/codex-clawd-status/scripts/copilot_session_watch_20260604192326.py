#!/usr/bin/env python3
"""Watch GitHub Copilot Chat session JSONL and drive Clawd status animations.

The watcher tails VS Code Copilot Chat session files under workspaceStorage,
maps request/tool activity to animations, and forwards them through the same
Hub transport stack used by the Codex watcher.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

import codex_clawd_hook as hook


APPDATA = Path(os.environ.get("APPDATA", str(Path.home() / "AppData" / "Roaming")))
WORKSPACE_STORAGE = APPDATA / "Code" / "User" / "workspaceStorage"
WATCH_PID_PATH = hook.LOG_DIR / "copilot-session-watch.pid"


def candidate_session_files() -> list[Path]:
    files: list[Path] = []
    if not WORKSPACE_STORAGE.exists():
        return files
    try:
        files.extend(p for p in WORKSPACE_STORAGE.rglob("chatSessions/*.jsonl") if p.is_file())
        files.extend(p for p in WORKSPACE_STORAGE.rglob("chatEditingSessions/*/*.jsonl") if p.is_file())
    except OSError:
        return []
    return files


def latest_session_file() -> Path | None:
    files = candidate_session_files()
    return max(files, key=lambda p: p.stat().st_mtime, default=None)


def client_id_from_snapshot(snapshot: dict[str, Any], fallback: str) -> str:
    responder = str(snapshot.get("responderUsername") or "").lower()
    location = str(snapshot.get("initialLocation") or "").lower()
    input_state = snapshot.get("inputState") if isinstance(snapshot.get("inputState"), dict) else {}
    model = input_state.get("selectedModel") if isinstance(input_state.get("selectedModel"), dict) else {}
    metadata = model.get("metadata") if isinstance(model.get("metadata"), dict) else {}
    extension = metadata.get("extension") if isinstance(metadata.get("extension"), dict) else {}

    ext_value = str(extension.get("value") or extension.get("_lower") or "").lower()
    if "copilot-chat" in ext_value or "copilot" in responder:
        if "editing" in location:
            return "copilot-editing"
        if "terminal" in location:
            return "copilot-terminal"
        return "copilot-chat"
    return fallback


def session_client_id(path: Path, fallback: str) -> str:
    try:
        first = path.open("r", encoding="utf-8", errors="replace").readline()
        item = json.loads(first)
    except (OSError, json.JSONDecodeError):
        return fallback
    if not isinstance(item, dict) or item.get("kind") != 0:
        return fallback
    snapshot = item.get("v") if isinstance(item.get("v"), dict) else {}
    return client_id_from_snapshot(snapshot, fallback)


def tool_invocation_to_anim(item: dict[str, Any]) -> str | None:
    tool_id = str(item.get("toolId") or item.get("name") or item.get("toolName") or "").lower()
    tool_data = item.get("toolSpecificData") if isinstance(item.get("toolSpecificData"), dict) else {}
    invocation = item.get("invocationMessage") if isinstance(item.get("invocationMessage"), dict) else {}
    text = str(invocation.get("value") or item.get("value") or "").lower()

    if tool_data.get("kind") == "terminal":
        return "building"

    if any(token in tool_id for token in ("edit", "write", "patch", "replace", "apply", "multi")):
        return "typing"
    if any(token in tool_id for token in ("read", "find", "list", "search", "glob", "inspect", "diagnostic")):
        return "debugger"
    if any(token in tool_id for token in ("ask", "question", "followup", "permission")):
        return "confused"
    if any(token in tool_id for token in ("agent", "task", "subagent", "plan")):
        return "conducting"
    if any(token in tool_id for token in ("web", "browse", "image")):
        return "wizard"

    if any(token in text for token in ("terminal", "powershell", "shell", "cmd")):
        return "building"
    if any(token in text for token in ("read", "list", "find", "search", "glob", "inspect")):
        return "debugger"

    return None


def request_to_anim(request: dict[str, Any]) -> str | None:
    response = request.get("response") if isinstance(request.get("response"), list) else []
    model_state = request.get("modelState") if isinstance(request.get("modelState"), dict) else {}
    model_value = model_state.get("value")

    # Debug: log model_state and response kinds for diagnosis
    try:
        kinds = [it.get("kind") if isinstance(it, dict) else str(type(it)) for it in response]
    except Exception:
        kinds = [str(type(it)) for it in response]
    if model_state or response:
        hook.log(f"copilot request model_state={model_state} response_kinds={kinds}")

    for item in response:
        if not isinstance(item, dict):
            continue
        if item.get("kind") == "toolInvocationSerialized":
            anim = tool_invocation_to_anim(item)
            if anim:
                return anim
        if item.get("kind") == "thinking":
            return "thinking"

    if model_value == 1:
        return hook.TASK_COMPLETE_ANIM
    if model_value == 4:
        return "thinking"
    if response:
        return "thinking"
    return None


def line_to_anim(item: dict[str, Any]) -> tuple[str | None, str]:
    if item.get("kind") != 2:
        return None, ""

    path = item.get("k") if isinstance(item.get("k"), list) else []
    if path != ["requests"]:
        return None, ""

    requests = item.get("v") if isinstance(item.get("v"), list) else []
    if not requests:
        return None, ""

    request = requests[-1]
    if not isinstance(request, dict):
        return None, ""

    anim = request_to_anim(request)
    if not anim:
        return None, ""

    request_id = str(request.get("requestId") or request.get("timestamp") or "")
    return anim, f"copilot request {request_id}".strip()


def send_watched_anim(anim: str, reason: str, args: argparse.Namespace) -> None:
    event_time = hook.touch_last_event()
    hook.log(f"copilot watch mapped {reason} anim={anim}")
    payload = {"hook_event_name": "CopilotSessionWatch", "tool_name": reason}
    hook.deliver_anim(anim, args, payload=payload, event_time=event_time)
    if anim == hook.TASK_COMPLETE_ANIM:
        hook.spawn_timed_transition(event_time, args)


def follow_file(path: Path, args: argparse.Namespace) -> None:
    session_args = argparse.Namespace(**vars(args))
    session_args.client_id = session_client_id(path, args.client_id)
    hook.log(f"copilot watch following session={path} client_id={session_args.client_id}")

    with path.open("r", encoding="utf-8", errors="replace") as fh:
        if not args.replay:
            fh.seek(0, os.SEEK_END)

        while True:
            line = fh.readline()
            if not line:
                if args.follow_latest or args.session is None:
                    latest = latest_session_file()
                    if latest and latest != path and latest.stat().st_mtime > path.stat().st_mtime:
                        hook.log(f"copilot watch switching session={latest}")
                        return
                time.sleep(args.poll)
                continue

            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(item, dict):
                continue

            anim, reason = line_to_anim(item)
            if anim:
                send_watched_anim(anim, reason, session_args)


def write_pid() -> None:
    try:
        hook.LOG_DIR.mkdir(parents=True, exist_ok=True)
        WATCH_PID_PATH.write_text(str(os.getpid()), encoding="utf-8")
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", type=Path, help="Copilot chat JSONL to follow; defaults to newest")
    parser.add_argument("--follow-latest", action="store_true", help="switch to newer session files")
    parser.add_argument("--replay", action="store_true", help="process existing lines before tailing")
    parser.add_argument("--poll", type=float, default=0.25)
    parser.add_argument("--transport")
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=None)
    parser.add_argument("--ble-address")
    parser.add_argument("--ble-name", default=None)
    parser.add_argument("--hub-url", default=None)
    parser.add_argument("--no-hub", action="store_true")
    parser.add_argument("--hub-required", action="store_true")
    parser.add_argument("--client-id", default=os.environ.get("CLAWD_TANK_WATCH_CLIENT_ID", "copilot-chat"))
    args = parser.parse_args()

    write_pid()
    hook.log("copilot watch started")

    while True:
        session = args.session or latest_session_file()
        if not session:
            time.sleep(args.poll)
            continue
        follow_file(session, args)
        if args.session:
            return 0


if __name__ == "__main__":
    raise SystemExit(main())