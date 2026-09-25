#!/usr/bin/env python3
"""Summarize Fallout 4 Faster Loadscreens timing and preload diagnostics.

This is a dependency-free, read-only parser for LoadingScreens.log.  It accepts
one or more logs (or directories containing LoadingScreens*.log), recognizes
concatenated game sessions, and can emit either a compact human report or JSON.

Examples:
    python tools/analyze_preload_log.py LoadingScreens.log
    python tools/analyze_preload_log.py run-a.log run-b.log --json
    python tools/analyze_preload_log.py --self-test
"""

from __future__ import annotations

import argparse
import io
import json
import math
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, TextIO, Tuple


SCHEMA_VERSION = 1

CROSSHAIR_RESULTS = {
    0: "null-ref-or-disabled",
    1: "not-a-load-door",
    2: "destination-already-loaded",
    3: "fired-interior",
    4: "interior-function-unavailable",
    5: "interior-cooldown",
    6: "exterior-function-unavailable",
    7: "destination-worldspace-missing",
    8: "same-worldspace",
    9: "exterior-cooldown",
    # Numeric-only result 10 is retained as the legacy name. Current logs emit
    # resultName=exterior-submitted, which takes precedence in the parser.
    10: "fired-exterior",
    11: "exterior-cell-layout-unavailable",
    12: "transition-active",
    13: "player-context-unavailable",
    14: "interior-origin-unsupported",
    15: "no-cold-work",
    16: "exterior-submitted",
    17: "source-worldspace-missing",
    18: "post-transition-quiet",
    19: "exterior-disabled",
}
# Numeric-only fallback is intentionally limited to established legacy codes.
# Current exterior outcomes always include resultName, which is authoritative;
# this avoids treating a new/changed numeric code as successful by accident.
FIRE_RESULTS = {3, 10}
FIRE_RESULT_NAMES = {
    "fired-interior",
    "fired-exterior",
    "exterior-submitted",
    "fired-exterior-engine",
}

DIAMOND_CITY_ROUTES = {
    "entry": {
        "label": "Commonwealth -> Diamond City",
        "door": "0012F822",
        "expected_linked_door": "000A8F9C",
        "expected_source_world": "0000003C",
        "expected_destination_world": "00000F94",
        "expected_computed_grid": [-1, -1],
    },
    "exit": {
        "label": "Diamond City -> Commonwealth",
        "door": "000A8F9C",
        "expected_linked_door": "0012F822",
        "expected_source_world": "00000F94",
        "expected_destination_world": "0000003C",
        "expected_computed_grid": [-4, -8],
    },
}

# Keep the original exit-only names for downstream readers which import this
# module or consume the additive diamond_city_exit compatibility view.
DIAMOND_CITY_GATE_DOOR = DIAMOND_CITY_ROUTES["exit"]["door"]
DIAMOND_CITY_GATE_LINKED_DOOR = DIAMOND_CITY_ROUTES["exit"][
    "expected_linked_door"
]
DIAMOND_CITY_GATE_EXPECTED_GRID = DIAMOND_CITY_ROUTES["exit"][
    "expected_computed_grid"
]

EVENT_RE = re.compile(r"\bPreloadDiag event=([a-z0-9-]+)\b")
SEQ_RE = re.compile(r"\bseq=(\d+)\b")
TMS_RE = re.compile(r"\btMs=(\d+)\b")
LOAD_RE = re.compile(r"\bload=(\d+)\b")
CONFIGURED_RE = re.compile(r"\bPreloadDiag configured=(true|false|0|1)\b", re.I)
SETTING_RE = re.compile(
    r"\bPreloadDiag native-setting when=(\S+)\s+([A-Za-z][A-Za-z0-9]*)=(\S+)"
)
MENU_CLOSE_RE = re.compile(
    r"\bLoadingMenu CLOSE \(load #(\d+)\)\s*[—–-]\s*([0-9]+(?:\.[0-9]+)?)s\b"
)
LOADTIME_RE = re.compile(
    r"\bLOADTIME:\s*(.*?)\s*[—–-]\s*([0-9]+(?:\.[0-9]+)?)s\b"
)


def _field(text: str, name: str) -> Optional[str]:
    match = re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"=([^\s]+)", text)
    return match.group(1) if match else None


def _integer_field(text: str, name: str) -> Optional[int]:
    value = _field(text, name)
    if value is None:
        return None
    try:
        return int(value, 10)
    except ValueError:
        return None


def _first_field(text: str, names: Sequence[str]) -> Optional[str]:
    for name in names:
        value = _field(text, name)
        if value is not None:
            return value
    return None


def _first_integer_field(text: str, names: Sequence[str]) -> Optional[int]:
    for name in names:
        value = _integer_field(text, name)
        if value is not None:
            return value
    return None


def _first_hex_field(text: str, names: Sequence[str]) -> Optional[str]:
    for name in names:
        value = _hex_field(text, name)
        if value is not None:
            return value
    return None


def _hex_field(text: str, name: str) -> Optional[str]:
    value = _field(text, name)
    if not value or not re.fullmatch(r"[0-9A-Fa-f]{1,8}", value):
        return None
    normalized = value.upper().zfill(8)
    return normalized if normalized != "00000000" else None


def _bool_value(value: Optional[str]) -> Optional[bool]:
    if value is None:
        return None
    lowered = value.lower()
    if lowered in {"true", "1", "yes", "on"}:
        return True
    if lowered in {"false", "0", "no", "off"}:
        return False
    return None


def _coords_field(text: str, name: str) -> Optional[List[int]]:
    match = re.search(
        r"(?<![A-Za-z0-9_])" + re.escape(name) + r"=\((-?\d+),\s*(-?\d+)\)",
        text,
    )
    return [int(match.group(1)), int(match.group(2))] if match else None


def _point3_field(text: str, name: str) -> Optional[List[float]]:
    number = r"-?(?:\d+(?:\.\d*)?|\.\d+)"
    match = re.search(
        r"(?<![A-Za-z0-9_])"
        + re.escape(name)
        + r"=\(("
        + number
        + r"),\s*("
        + number
        + r"),\s*("
        + number
        + r")\)",
        text,
    )
    if not match:
        return None
    return [float(match.group(index)) for index in range(1, 4)]


def _event_common(text: str, line_no: int, order: int) -> Dict[str, Any]:
    seq = SEQ_RE.search(text)
    t_ms = TMS_RE.search(text)
    load = LOAD_RE.search(text)
    return {
        "line": line_no,
        "order": order,
        "seq": int(seq.group(1)) if seq else None,
        "t_ms": int(t_ms.group(1)) if t_ms else None,
        "load": int(load.group(1)) if load else None,
    }


def _cell_record(
    common: Dict[str, Any],
    text: str,
    form_field: str,
    editor_field: str,
) -> Dict[str, Any]:
    record = dict(common)
    record.update(
        {
            "cell_id": _hex_field(text, form_field),
            "editor": _field(text, editor_field),
            "interior": _bool_value(_field(text, "interior")),
        }
    )
    return record


def _native_context_fields(text: str) -> Dict[str, Any]:
    return {
        "native_attempt_id": _integer_field(text, "nativeAttemptId"),
        "origin_observation": _bool_value(_field(text, "originObservation")),
        "origin_cell": _hex_field(text, "originCell"),
        "origin_editor": _field(text, "originEditor"),
        "origin_interior": _bool_value(_field(text, "originInterior")),
        "origin_world": _hex_field(text, "originWorld"),
        "origin_world_editor": _field(text, "originWorldEditor"),
        "direction": _field(text, "direction"),
    }


def _is_fire_result(record: Dict[str, Any]) -> bool:
    """Accept current named results and legacy numeric-only diagnostics."""
    result_name = record.get("result_name")
    if record.get("result_name_explicit"):
        return result_name in FIRE_RESULT_NAMES
    return record.get("result") in FIRE_RESULTS


@dataclass
class Session:
    source: str
    part: int
    start_line: int
    end_line: int = 0
    line_count: int = 0
    configured: Optional[bool] = None
    configured_seen: bool = False
    diagnostic_lines: int = 0
    hook_installed: bool = False
    hook_removed: bool = False
    hook_messages: List[str] = field(default_factory=list)
    settings_history: List[Dict[str, Any]] = field(default_factory=list)
    effective_settings: Dict[str, str] = field(default_factory=dict)
    menu_closes: List[Dict[str, Any]] = field(default_factory=list)
    loadtimes: List[Dict[str, Any]] = field(default_factory=list)
    menu_geometry: List[Dict[str, Any]] = field(default_factory=list)
    native_candidates: List[Dict[str, Any]] = field(default_factory=list)
    native_world_candidates: List[Dict[str, Any]] = field(default_factory=list)
    native_preloads: List[Dict[str, Any]] = field(default_factory=list)
    native_world_preloads: List[Dict[str, Any]] = field(default_factory=list)
    buffer_adds: List[Dict[str, Any]] = field(default_factory=list)
    buffer_removes: List[Dict[str, Any]] = field(default_factory=list)
    crosshair_candidates: List[Dict[str, Any]] = field(default_factory=list)
    plugin_world_preloads: List[Dict[str, Any]] = field(default_factory=list)
    exterior_preload_decisions: List[Dict[str, Any]] = field(
        default_factory=list
    )
    gate_enum_sources: List[Dict[str, Any]] = field(default_factory=list)
    gate_enum_candidates: List[Dict[str, Any]] = field(default_factory=list)
    gate_enum_summaries: List[Dict[str, Any]] = field(default_factory=list)
    legacy_gate_scan_sources: List[Dict[str, Any]] = field(default_factory=list)
    legacy_gate_scan_candidates: List[Dict[str, Any]] = field(default_factory=list)
    legacy_gate_scan_summaries: List[Dict[str, Any]] = field(default_factory=list)
    # Compatibility only: current builds use TES::PreloadWorld and no longer
    # emit plugin-exterior-grid/QueueCellLoad records.
    plugin_exterior_grids: List[Dict[str, Any]] = field(default_factory=list)
    player_cells: List[Dict[str, Any]] = field(default_factory=list)
    event_counts: Counter = field(default_factory=Counter)
    last_seq: Optional[int] = None
    _order: int = 0

    def meaningful(self) -> bool:
        return bool(
            self.line_count
            or self.diagnostic_lines
            or self.menu_closes
            or self.loadtimes
        )

    def next_order(self) -> int:
        self._order += 1
        return self._order


class LogParser:
    def __init__(self) -> None:
        self.sessions: List[Session] = []
        self.current: Optional[Session] = None
        self._source: Optional[str] = None
        self._part_by_source: Counter = Counter()

    def _new_session(self, source: str, line_no: int) -> Session:
        self._part_by_source[source] += 1
        session = Session(
            source=source,
            part=self._part_by_source[source],
            start_line=line_no,
            end_line=line_no,
        )
        self.sessions.append(session)
        self.current = session
        self._source = source
        return session

    def begin_source(self, source: str) -> None:
        self.current = None
        self._source = source

    def feed(self, source: str, line_no: int, raw_line: str) -> None:
        text = raw_line.rstrip("\r\n")
        if self.current is None or self._source != source:
            session = self._new_session(source, line_no)
        else:
            session = self.current

        configured_match = CONFIGURED_RE.search(text)
        event_match = EVENT_RE.search(text)
        seq_match = SEQ_RE.search(text) if event_match else None
        seq = int(seq_match.group(1)) if seq_match else None

        # CommonLib truncates the live log, but archived logs are often
        # concatenated.  Configure is the reliable startup marker; seq=1 is a
        # fallback for partial concatenations without that line.
        starts_new = bool(
            configured_match
            and session.configured_seen
            and session.meaningful()
        )
        if (
            not starts_new
            and seq == 1
            and session.last_seq is not None
            and session.last_seq > 1
        ):
            starts_new = True
        if starts_new:
            session = self._new_session(source, line_no)

        session.end_line = line_no
        session.line_count += 1
        if "PreloadDiag" in text:
            session.diagnostic_lines += 1

        if configured_match:
            session.configured_seen = True
            session.configured = _bool_value(configured_match.group(1))

        if (
            "PreloadDiag installed " in text
            and " mutation-free observers" in text
        ):
            session.hook_installed = True
        if "PreloadDiag hooks removed" in text:
            session.hook_removed = True
        if (
            "PreloadDiag" in text
            and any(
                marker in text
                for marker in (
                    "hooks not installed",
                    "unavailable on runtime",
                    "prologue validation failed",
                    "MH_Initialize failed",
                    "MH_CreateHook",
                    "MH_EnableHook",
                    "rolling back",
                )
            )
        ):
            session.hook_messages.append(text.strip())

        setting_match = SETTING_RE.search(text)
        if setting_match:
            when, key, value = setting_match.groups()
            setting = {
                "line": line_no,
                "when": when,
                "name": key,
                "value": value,
            }
            session.settings_history.append(setting)
            session.effective_settings[key] = value

        close_match = MENU_CLOSE_RE.search(text)
        if close_match:
            session.menu_closes.append(
                {
                    "line": line_no,
                    "load": int(close_match.group(1)),
                    "seconds": float(close_match.group(2)),
                }
            )

        loadtime_match = LOADTIME_RE.search(text)
        if loadtime_match:
            session.loadtimes.append(
                {
                    "line": line_no,
                    "kind": loadtime_match.group(1).strip() or "<unknown>",
                    "seconds": float(loadtime_match.group(2)),
                }
            )

        if not event_match:
            return

        event_name = event_match.group(1)
        session.event_counts[event_name] += 1
        session.last_seq = seq if seq is not None else session.last_seq
        common = _event_common(text, line_no, session.next_order())

        if event_name == "menu-geometry-grid":
            event = dict(common)
            event.update(
                {
                    "world": _hex_field(text, "world"),
                    "lower": _coords_field(text, "lower"),
                    "upper": _coords_field(text, "upper"),
                    "side": _integer_field(text, "side"),
                    "cells": _integer_field(text, "cells"),
                    "current_cell": _hex_field(text, "currentCell"),
                    "current_editor": _field(text, "currentEditor"),
                }
            )
            session.menu_geometry.append(event)
            return

        if event_name == "native-linked-candidate":
            event = _cell_record(common, text, "dest", "editor")
            event.update(
                {
                    "candidate_kind": "interior-helper",
                    "selection_counts_valid": _bool_value(
                        _field(text, "selectionCountsValid")
                    ),
                    "selected_before": _integer_field(text, "selectedBefore"),
                    "selection_limit": _integer_field(text, "selectionLimit"),
                    "grid": _coords_field(text, "grid"),
                    "world": _hex_field(text, "world"),
                }
            )
            event.update(_native_context_fields(text))
            session.native_candidates.append(event)
            return

        if event_name == "native-linked-world-candidate":
            event = dict(common)
            event.update(
                {
                    "candidate_kind": "world-outer",
                    "stage": _field(text, "stage"),
                    "observation_only": _bool_value(
                        _field(text, "observationOnly")
                    ),
                    "candidate_decoded": _bool_value(
                        _field(text, "candidateDecoded")
                    ),
                }
            )
            event.update(_native_context_fields(text))
            session.native_world_candidates.append(event)
            return

        if event_name == "preload-interior":
            source_name = _field(text, "source")
            if source_name == "native-linked":
                event = _cell_record(common, text, "cell", "editor")
                event["source"] = source_name
                event["event_name"] = event_name
                event["inferred_from_candidate"] = False
                event.update(_native_context_fields(text))
                session.native_preloads.append(event)
            return

        if event_name == "preload-world":
            source_name = _field(text, "source")
            event = dict(common)
            event.update(
                {
                    "source": source_name,
                    "event_name": event_name,
                    "world": _hex_field(text, "world"),
                    "world_editor": _field(text, "worldEditor"),
                    "center": _coords_field(text, "center"),
                    "bool_flag": _bool_value(_field(text, "boolFlag")),
                    "queue_only": _bool_value(
                        _first_field(text, ("queueOnly", "boolFlag"))
                    ),
                    "branch": _field(text, "branch"),
                    "full_grid": _bool_value(_field(text, "fullGrid")),
                    "grid_side": _integer_field(text, "gridSide"),
                    "grid_cells": _integer_field(text, "gridCells"),
                    "cell_id": None,
                    "editor": None,
                    "inferred_from_candidate": False,
                }
            )
            event.update(_native_context_fields(text))
            if source_name == "native-linked":
                session.native_world_preloads.append(event)
            elif source_name in {
                "plugin-door-prefetch",
                "plugin-crosshair",
            }:
                session.plugin_world_preloads.append(event)
            return

        if event_name in {"interior-buffer-add", "interior-buffer-remove"}:
            event = _cell_record(common, text, "affected", "affectedEditor")
            event.update(
                {
                    "slots": _integer_field(text, "slots"),
                    "ring": text.split(" ring=", 1)[1] if " ring=" in text else None,
                }
            )
            if event_name == "interior-buffer-add":
                session.buffer_adds.append(event)
            else:
                session.buffer_removes.append(event)
            return

        if event_name == "crosshair-candidate":
            event = _cell_record(common, text, "dest", "destEditor")
            result = _integer_field(text, "result")
            distance = _field(text, "distance")
            result_name = _field(text, "resultName")
            if result_name in {"<null>", "<none>", ""}:
                result_name = None
            result_name_explicit = result_name is not None
            event.update(
                {
                    "source": _field(text, "source") or "plugin-crosshair",
                    "trigger": _field(text, "trigger") or "<unknown>",
                    "result": result,
                    "result_name_explicit": result_name_explicit,
                    "result_name": result_name
                    or CROSSHAIR_RESULTS.get(
                        result, "unknown-result-{}".format(result)
                    ),
                    "distance": _safe_float(distance),
                    "door": _hex_field(text, "door"),
                    "door_editor": _field(text, "doorEditor"),
                    "world": _hex_field(text, "world"),
                    "world_editor": _field(text, "worldEditor"),
                    "grid": _coords_field(text, "grid"),
                }
            )
            session.crosshair_candidates.append(event)
            return

        if event_name == "exterior-preload-decision":
            event = dict(common)
            event.update(
                {
                    "event_name": event_name,
                    "source": _field(text, "source")
                    or "plugin-door-prefetch",
                    "trigger": _field(text, "trigger") or "<unknown>",
                    "decision": _field(text, "decision"),
                    "door": _hex_field(text, "door"),
                    "linked_door": _hex_field(text, "linkedDoor"),
                    "source_cell": _hex_field(text, "sourceCell"),
                    "source_world": _hex_field(text, "sourceWorld"),
                    "destination_cell": _hex_field(text, "destCell"),
                    "destination_world": _hex_field(text, "destWorld"),
                    "center": _coords_field(text, "center"),
                    "mode": _field(text, "mode"),
                    "cooldown_ms": _integer_field(text, "cooldownMs"),
                    "engine_owns_resident_pending_dedup": _bool_value(
                        _field(text, "engineOwnsResidentPendingDedup")
                    ),
                    "completion_claim": _bool_value(
                        _field(text, "completionClaim")
                    ),
                }
            )
            session.exterior_preload_decisions.append(event)
            return

        if event_name in {"gate-enum-source", "gate-scan-source"}:
            is_enum = event_name == "gate-enum-source"
            event = dict(common)
            event.update(
                {
                    "event_name": event_name,
                    "family": "native-loaded-reference-enum"
                    if is_enum
                    else "legacy-cell-scan",
                    "scan": _integer_field(text, "scan"),
                    "backend": _field(text, "backend"),
                    "scope": _field(text, "scope"),
                    "resolved": _bool_value(_field(text, "resolved")),
                    "stable": _bool_value(_field(text, "stable")),
                    "collection_ptr": _field(text, "collectionPtr"),
                    "source_cell": _hex_field(text, "sourceCell"),
                    "source_editor": _field(text, "sourceEditor"),
                    "source_interior": _bool_value(
                        _field(text, "sourceInterior")
                    ),
                    "source_state": _integer_field(text, "sourceState"),
                    "loaded_data": _field(text, "loadedData"),
                    "source_world": _first_hex_field(
                        text, ("sourceWorld", "expectedWorld")
                    ),
                    "source_world_editor": _first_field(
                        text, ("sourceWorldEditor", "expectedWorldEditor")
                    ),
                    "cell_world": _hex_field(text, "cellWorld"),
                    "cell_world_editor": _field(text, "cellWorldEditor"),
                    "origin": _point3_field(text, "origin"),
                    "radius": _safe_float(_field(text, "radius")),
                }
            )
            if is_enum:
                session.gate_enum_sources.append(event)
            else:
                session.legacy_gate_scan_sources.append(event)
            return

        if event_name in {"gate-enum-candidate", "gate-scan-candidate"}:
            is_enum = event_name == "gate-enum-candidate"
            decision = _field(text, "decision")
            event = dict(common)
            event.update(
                {
                    "event_name": event_name,
                    "family": "native-loaded-reference-enum"
                    if is_enum
                    else "legacy-cell-scan",
                    "scan": _integer_field(text, "scan"),
                    "backend": _field(text, "backend"),
                    "scope": _field(text, "scope"),
                    "door": _hex_field(text, "door"),
                    "door_editor": _field(text, "doorEditor"),
                    "source_cell": _hex_field(text, "sourceCell"),
                    "source_cell_editor": _field(text, "sourceCellEditor"),
                    "source_world": _hex_field(text, "sourceWorld"),
                    "source_world_editor": _field(text, "sourceWorldEditor"),
                    "distance": _safe_float(_field(text, "distance")),
                    "position": _point3_field(text, "pos"),
                    "transition_cell": _hex_field(text, "transitionCell"),
                    "transition_editor": _field(text, "transitionEditor"),
                    "transition_grid": _coords_field(text, "transitionGrid"),
                    "linked_door": _hex_field(text, "linkedDoor"),
                    "linked_door_editor": _field(text, "linkedDoorEditor"),
                    "linked_cell": _hex_field(text, "linkedCell"),
                    "linked_cell_editor": _field(text, "linkedCellEditor"),
                    "linked_cell_source": _field(text, "linkedCellSource"),
                    "linked_world": _hex_field(text, "linkedWorld"),
                    "linked_world_editor": _field(text, "linkedWorldEditor"),
                    "xtel_position": _point3_field(text, "xtelPos"),
                    "linked_position": _point3_field(text, "linkedPos"),
                    "computed_grid": _coords_field(text, "computedGrid"),
                    "grid_valid": _bool_value(_field(text, "gridValid")),
                    "center_source": _field(text, "centerSource"),
                    "chosen_by": _field(text, "chosenBy"),
                    "destination": _hex_field(text, "destination"),
                    "destination_editor": _field(text, "destinationEditor"),
                    "destination_interior": _bool_value(
                        _field(text, "destinationInterior")
                    ),
                    "destination_world": _hex_field(text, "destinationWorld"),
                    "destination_world_editor": _field(
                        text, "destinationWorldEditor"
                    ),
                    "decision": decision,
                    "accepted": (
                        decision == "accepted-cross-world-exterior"
                        if decision is not None
                        else None
                    ),
                }
            )
            if is_enum:
                session.gate_enum_candidates.append(event)
            else:
                session.legacy_gate_scan_candidates.append(event)
            return

        if event_name in {"gate-enum-summary", "gate-scan-summary"}:
            is_enum = event_name == "gate-enum-summary"
            event = dict(common)
            event.update(
                {
                    "event_name": event_name,
                    "family": "native-loaded-reference-enum"
                    if is_enum
                    else "legacy-cell-scan",
                    "scan": _integer_field(text, "scan"),
                    "backend": _field(text, "backend"),
                    "scope": _field(text, "scope"),
                    "resolved": _bool_value(_field(text, "resolved")),
                    "collection_ptr": _field(text, "collectionPtr"),
                    "callbacks": _integer_field(text, "callbacks"),
                    "references": _integer_field(text, "references"),
                    "teleport_doors": _integer_field(text, "teleportDoors"),
                    "null_reference": _integer_field(text, "nullReference"),
                    "disabled_or_deleted": _integer_field(
                        text, "disabledOrDeleted"
                    ),
                    "missing_teleport_data": _integer_field(
                        text, "missingTeleportData"
                    ),
                    "source_world_missing": _integer_field(
                        text, "sourceWorldMissing"
                    ),
                    "source_world_mismatch": _integer_field(
                        text, "sourceWorldMismatch"
                    ),
                    "no_destination": _integer_field(text, "noDestination"),
                    "interior_destination": _integer_field(
                        text, "interiorDestination"
                    ),
                    "no_destination_world": _integer_field(
                        text, "noDestinationWorld"
                    ),
                    "same_world": _integer_field(text, "sameWorld"),
                    "accepted": _integer_field(text, "accepted"),
                    "duplicate": _integer_field(text, "duplicate"),
                    "capacity_dropped": _integer_field(
                        text, "capacityDropped"
                    ),
                    "candidate_logs": _integer_field(text, "candidateLogs"),
                    "candidate_logs_dropped": _integer_field(
                        text, "candidateLogsDropped"
                    ),
                    "cached": _integer_field(text, "cached"),
                }
            )
            if is_enum:
                session.gate_enum_summaries.append(event)
            else:
                session.legacy_gate_scan_summaries.append(event)
            return

        if event_name == "plugin-exterior-grid":
            event = _cell_record(common, text, "dest", "destEditor")
            # Legacy compatibility only. Current builds call TES::PreloadWorld
            # and emit event=preload-world source=plugin-door-prefetch instead.
            event["interior"] = False
            submission_attempts = _first_integer_field(
                text, ("submissionAttempts", "queued")
            )
            resident_or_busy_skipped = _first_integer_field(
                text, ("residentOrBusySkipped", "residentSkipped")
            )
            attempted_coordinates = _first_field(
                text, ("attemptedCoordinates", "queuedCoordinates")
            )
            event.update(
                {
                    "source": _field(text, "source") or "plugin-door-prefetch",
                    "trigger": _field(text, "trigger") or "<unknown>",
                    "distance": _safe_float(_field(text, "distance")),
                    "door": _hex_field(text, "door"),
                    "door_editor": _field(text, "doorEditor"),
                    "source_cell": _hex_field(text, "sourceCell"),
                    "source_cell_editor": _field(text, "sourceCellEditor"),
                    "source_interior": _bool_value(
                        _field(text, "sourceInterior")
                    ),
                    "source_world": _hex_field(text, "sourceWorld"),
                    "source_world_editor": _field(text, "sourceWorldEditor"),
                    "source_grid": _coords_field(text, "sourceGrid"),
                    "destination_world": _hex_field(text, "destWorld"),
                    "destination_world_editor": _field(
                        text, "destWorldEditor"
                    ),
                    "center": _coords_field(text, "center"),
                    "radius": _integer_field(text, "radius"),
                    "side": _integer_field(text, "side"),
                    "requested": _integer_field(text, "requested"),
                    "submission_attempts": submission_attempts,
                    "resident_or_busy_skipped": resident_or_busy_skipped,
                    "plugin_duplicate_skipped": _integer_field(
                        text, "pluginDuplicateSkipped"
                    ),
                    "attempted_coordinates": attempted_coordinates,
                    "used_legacy_submission_fields": bool(
                        (
                            _field(text, "submissionAttempts") is None
                            and _field(text, "queued") is not None
                        )
                        or (
                            _field(text, "residentOrBusySkipped") is None
                            and _field(text, "residentSkipped") is not None
                        )
                        or (
                            _field(text, "attemptedCoordinates") is None
                            and _field(text, "queuedCoordinates") is not None
                        )
                    ),
                }
            )
            session.plugin_exterior_grids.append(event)
            return

        if event_name == "player-cell":
            event = _cell_record(common, text, "cell", "editor")
            event.update(
                {
                    "reason": _field(text, "reason"),
                    "grid": _coords_field(text, "grid"),
                    "world": _hex_field(text, "world"),
                }
            )
            session.player_cells.append(event)


def _safe_float(value: Optional[str]) -> Optional[float]:
    if value is None:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def _duration_stats(values: Iterable[float]) -> Dict[str, Any]:
    numbers = list(values)
    if not numbers:
        return {
            "count": 0,
            "total_seconds": None,
            "mean_seconds": None,
            "median_seconds": None,
            "min_seconds": None,
            "max_seconds": None,
        }
    return {
        "count": len(numbers),
        "total_seconds": round(sum(numbers), 4),
        "mean_seconds": round(statistics.mean(numbers), 4),
        "median_seconds": round(statistics.median(numbers), 4),
        "min_seconds": round(min(numbers), 4),
        "max_seconds": round(max(numbers), 4),
    }


def _unique_cells(records: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    cells: Dict[str, Dict[str, Any]] = {}
    for record in records:
        cell_id = record.get("cell_id")
        if not cell_id:
            continue
        item = cells.setdefault(
            cell_id,
            {
                "form_id": cell_id,
                "editor_ids": [],
                "event_count": 0,
                "first_order": record.get("order"),
                "first_load": record.get("load"),
            },
        )
        item["event_count"] += 1
        editor = record.get("editor")
        if editor and editor not in item["editor_ids"]:
            item["editor_ids"].append(editor)
        order = record.get("order")
        if (
            order is not None
            and (item["first_order"] is None or order < item["first_order"])
        ):
            item["first_order"] = order
            item["first_load"] = record.get("load")
    return sorted(
        cells.values(),
        key=lambda item: (
            item["first_order"] if item["first_order"] is not None else 10**18,
            item["form_id"],
        ),
    )


def _records_after_first_matching_request(
    records: Sequence[Dict[str, Any]],
    requests: Sequence[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    first_request_order: Dict[str, int] = {}
    for request in requests:
        cell_id = request.get("cell_id")
        order = request.get("order")
        if not cell_id or not isinstance(order, int):
            continue
        previous = first_request_order.get(cell_id)
        if previous is None or order < previous:
            first_request_order[cell_id] = order

    return [
        record
        for record in records
        if record.get("cell_id") in first_request_order
        and isinstance(record.get("order"), int)
        and record["order"] > first_request_order[record["cell_id"]]
    ]


def _usefulness(
    records: Sequence[Dict[str, Any]],
    observations: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    first_requests: Dict[str, Dict[str, Any]] = {}
    for record in records:
        cell_id = record.get("cell_id")
        if not cell_id:
            continue
        existing = first_requests.get(cell_id)
        if existing is None or record.get("order", 10**18) < existing.get(
            "order", 10**18
        ):
            first_requests[cell_id] = record

    useful: List[str] = []
    wasted: List[str] = []
    indeterminate: List[str] = []
    for cell_id, request in first_requests.items():
        later = [
            observation
            for observation in observations
            if observation.get("order", -1) > request.get("order", -1)
        ]
        if any(observation.get("cell_id") == cell_id for observation in later):
            useful.append(cell_id)
        elif later:
            wasted.append(cell_id)
        else:
            indeterminate.append(cell_id)

    useful.sort()
    wasted.sort()
    indeterminate.sort()
    evaluable = len(useful) + len(wasted)
    ratio = round(len(useful) / evaluable, 4) if evaluable else None
    return {
        "definition": (
            "Useful means the requested destination was observed as the player's "
            "cell later in this log. The legacy 'wasted' bucket means later "
            "player-cell samples exist but never show that destination. This is "
            "request correlation only: "
            "it does not prove an engine submission was accepted, loaded, attached, "
            "or completed, or that the cell would never be used after recording "
            "stopped."
        ),
        "unique_destinations": len(first_requests),
        "evaluable_destinations": evaluable,
        "useful": len(useful),
        "wasted_within_recorded_window": len(wasted),
        "indeterminate_no_later_player_sample": len(indeterminate),
        "hit_ratio_among_evaluable": ratio,
        "useful_cells": useful,
        "wasted_cells": wasted,
        "indeterminate_cells": indeterminate,
    }


def _destination_hits(session: Session) -> List[Dict[str, Any]]:
    source_records = {
        "native-requested": session.native_candidates,
        "native-preload-call": session.native_preloads,
        "crosshair-candidate": session.crosshair_candidates,
        "crosshair-fired": [
            record
            for record in session.crosshair_candidates
            if _is_fire_result(record)
        ],
        "plugin-exterior-grid": session.plugin_exterior_grids,
    }
    merged: Dict[str, Dict[str, Any]] = {}
    for source_name, records in source_records.items():
        for request in records:
            cell_id = request.get("cell_id")
            if not cell_id:
                continue
            later_matches = [
                observation
                for observation in session.player_cells
                if observation.get("cell_id") == cell_id
                and observation.get("order", -1) > request.get("order", -1)
            ]
            if not later_matches:
                continue
            observation = min(
                later_matches, key=lambda item: item.get("order", 10**18)
            )
            hit = merged.setdefault(
                cell_id,
                {
                    "form_id": cell_id,
                    "editor_ids": [],
                    "sources": [],
                    "observed_load": observation.get("load"),
                    "observed_reason": observation.get("reason"),
                    "observed_seq": observation.get("seq"),
                },
            )
            editor = request.get("editor")
            if editor and editor not in hit["editor_ids"]:
                hit["editor_ids"].append(editor)
            if source_name not in hit["sources"]:
                hit["sources"].append(source_name)
    return sorted(merged.values(), key=lambda item: item["form_id"])


def _crosshair_by_trigger(
    records: Sequence[Dict[str, Any]],
) -> Dict[str, Dict[str, Any]]:
    grouped: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for record in records:
        grouped[record.get("trigger") or "<unknown>"].append(record)

    result: Dict[str, Dict[str, Any]] = {}
    for trigger in sorted(grouped):
        trigger_records = grouped[trigger]
        result_counts = Counter(
            "{}:{}".format(
                record.get("result"),
                record.get("result_name"),
            )
            for record in trigger_records
        )
        fired = [
            record
            for record in trigger_records
            if _is_fire_result(record)
        ]
        result[trigger] = {
            "candidate_events": len(trigger_records),
            "unique_candidate_cells": len(_unique_cells(trigger_records)),
            "fire_events": len(fired),
            "unique_fired_cells": len(_unique_cells(fired)),
            "results": dict(sorted(result_counts.items())),
        }
    return result


def _plugin_exterior_by_trigger(
    records: Sequence[Dict[str, Any]],
) -> Dict[str, Dict[str, Any]]:
    grouped: Dict[str, List[Dict[str, Any]]] = defaultdict(list)
    for record in records:
        grouped[record.get("trigger") or "<unknown>"].append(record)

    result: Dict[str, Dict[str, Any]] = {}
    for trigger in sorted(grouped):
        trigger_records = grouped[trigger]
        result[trigger] = {
            "event_count": len(trigger_records),
            "unique_destination_cells": len(_unique_cells(trigger_records)),
            "requested_positions_total": sum(
                record.get("requested") or 0 for record in trigger_records
            ),
            "submission_attempts_total": sum(
                record.get("submission_attempts") or 0
                for record in trigger_records
            ),
            "resident_or_busy_skipped_total": sum(
                record.get("resident_or_busy_skipped") or 0
                for record in trigger_records
            ),
            "plugin_duplicate_skipped_total": sum(
                record.get("plugin_duplicate_skipped") or 0
                for record in trigger_records
            ),
        }
    return result


def _native_attempt_correlation(session: Session) -> Dict[str, Any]:
    candidates = list(session.native_candidates) + list(
        session.native_world_candidates
    )
    calls = [
        record
        for record in session.native_preloads
        if record.get("event_name") == "preload-interior"
    ] + list(session.native_world_preloads)

    calls_by_attempt: Dict[int, List[Dict[str, Any]]] = defaultdict(list)
    for call in calls:
        attempt_id = call.get("native_attempt_id")
        if isinstance(attempt_id, int) and attempt_id > 0:
            calls_by_attempt[attempt_id].append(call)

    confirmed: List[Dict[str, Any]] = []
    unconfirmed: List[Dict[str, Any]] = []
    candidates_without_attempt_id = 0
    matched_call_orders = set()
    by_direction: Dict[str, Dict[str, int]] = defaultdict(
        lambda: {
            "candidate_events": 0,
            "confirmed_candidate_events": 0,
            "unconfirmed_candidate_events": 0,
            "preload_call_events": 0,
        }
    )

    for call in calls:
        direction = call.get("direction") or "<unknown>"
        by_direction[direction]["preload_call_events"] += 1

    for candidate in candidates:
        direction = candidate.get("direction") or "<unknown>"
        by_direction[direction]["candidate_events"] += 1
        attempt_id = candidate.get("native_attempt_id")
        if not isinstance(attempt_id, int) or attempt_id <= 0:
            candidates_without_attempt_id += 1
            by_direction[direction]["unconfirmed_candidate_events"] += 1
            unconfirmed.append(
                {
                    "attempt_id": None,
                    "candidate_kind": candidate.get("candidate_kind"),
                    "direction": direction,
                    "candidate_line": candidate.get("line"),
                    "reason": "no-nativeAttemptId",
                }
            )
            continue

        expected_event = (
            "preload-world"
            if candidate.get("candidate_kind") == "world-outer"
            else "preload-interior"
        )
        same_event_calls = [
            call
            for call in calls_by_attempt.get(attempt_id, [])
            if call.get("event_name") == expected_event
        ]
        if expected_event == "preload-interior":
            candidate_cell = candidate.get("cell_id")
            matching_calls = [
                call
                for call in same_event_calls
                if candidate_cell
                and call.get("cell_id") == candidate_cell
            ]
        else:
            # The outer world candidate payload is deliberately undecoded.
            # Attempt ID is therefore the strongest available correlation.
            matching_calls = same_event_calls

        if matching_calls:
            by_direction[direction]["confirmed_candidate_events"] += 1
            for call in matching_calls:
                matched_call_orders.add(call.get("order"))
            confirmed.append(
                {
                    "attempt_id": attempt_id,
                    "candidate_kind": candidate.get("candidate_kind"),
                    "direction": direction,
                    "candidate_line": candidate.get("line"),
                    "call_event": expected_event,
                    "call_lines": [
                        call.get("line") for call in matching_calls
                    ],
                    "call_orders": [
                        call.get("order") for call in matching_calls
                    ],
                    "destination_cell": candidate.get("cell_id"),
                    "destination_editor": candidate.get("editor"),
                    "destination_world": (
                        matching_calls[0].get("world")
                        if expected_event == "preload-world"
                        else candidate.get("world")
                    ),
                    "destination_center": matching_calls[0].get("center"),
                }
            )
        else:
            by_direction[direction]["unconfirmed_candidate_events"] += 1
            destination_mismatch = bool(
                expected_event == "preload-interior" and same_event_calls
            )
            unconfirmed.append(
                {
                    "attempt_id": attempt_id,
                    "candidate_kind": candidate.get("candidate_kind"),
                    "direction": direction,
                    "candidate_line": candidate.get("line"),
                    "reason": (
                        "destination-mismatch"
                        if destination_mismatch
                        else "no-matching-{}".format(expected_event)
                    ),
                    "candidate_destination_cell": candidate.get("cell_id"),
                    "observed_call_destinations": sorted(
                        {
                            call.get("cell_id")
                            for call in same_event_calls
                            if call.get("cell_id")
                        }
                    ),
                    "call_lines": [
                        call.get("line") for call in same_event_calls
                    ],
                }
            )

    orphan_calls = [
        {
            "attempt_id": call.get("native_attempt_id"),
            "event_name": call.get("event_name"),
            "direction": call.get("direction") or "<unknown>",
            "line": call.get("line"),
        }
        for call in calls
        if call.get("order") not in matched_call_orders
    ]
    return {
        "definition": (
            "Candidates are provisional observations. A candidate is confirmed "
            "as an interior request only when a nested preload-interior call has "
            "both the same nativeAttemptId and destination FormID. World outer "
            "payloads are deliberately undecoded, so a nested preload-world call "
            "is correlated by nativeAttemptId only. The hook proves invocation "
            "and arguments, not completion."
        ),
        "candidate_event_count": len(candidates),
        "interior_candidate_event_count": len(session.native_candidates),
        "world_candidate_event_count": len(session.native_world_candidates),
        "confirmed_candidate_event_count": len(confirmed),
        "unconfirmed_candidate_event_count": len(unconfirmed),
        "candidates_without_attempt_id": candidates_without_attempt_id,
        "by_direction": {
            direction: dict(counts)
            for direction, counts in sorted(by_direction.items())
        },
        "confirmed": confirmed,
        "unconfirmed": unconfirmed,
        "orphan_preload_calls": orphan_calls,
    }


def _confirmed_world_call_orders(
    correlation: Dict[str, Any],
) -> set:
    return {
        order
        for match in correlation["confirmed"]
        if match.get("call_event") == "preload-world"
        for order in match.get("call_orders", [])
        if isinstance(order, int)
    }


def _native_world_relation(
    session: Session,
    correlation: Dict[str, Any],
) -> Dict[str, Any]:
    confirmed_orders = _confirmed_world_call_orders(correlation)
    calls: List[Dict[str, Any]] = []
    for call in session.native_world_preloads:
        origin_world = call.get("origin_world")
        destination_world = call.get("world")
        if not origin_world or not destination_world:
            relation = "unknown"
        elif origin_world == destination_world:
            relation = "same-world"
        else:
            relation = "cross-world"
        calls.append(
            {
                "line": call.get("line"),
                "order": call.get("order"),
                "load": call.get("load"),
                "attempt_id": call.get("native_attempt_id"),
                "direction": call.get("direction") or "<unknown>",
                "origin_world_snapshot": origin_world,
                "destination_world": destination_world,
                "relation": relation,
                "confirmed_by_outer_candidate": (
                    call.get("order") in confirmed_orders
                ),
            }
        )

    relation_names = ("same-world", "cross-world", "unknown")
    counts = Counter(call["relation"] for call in calls)
    confirmed_counts = Counter(
        call["relation"]
        for call in calls
        if call["confirmed_by_outer_candidate"]
    )
    return {
        "definition": (
            "Relation compares destination world with originWorld from the "
            "current-player snapshot captured at native outer-visitor time. "
            "That origin is not decoded from the candidate and does not by "
            "itself identify a door or authoritative source context."
        ),
        "call_event_count": len(calls),
        "relation_counts": {
            name: counts.get(name, 0) for name in relation_names
        },
        "confirmed_call_event_count": sum(
            1 for call in calls if call["confirmed_by_outer_candidate"]
        ),
        "confirmed_relation_counts": {
            name: confirmed_counts.get(name, 0) for name in relation_names
        },
        "calls": calls,
    }


def _world_request_geometry(
    call: Dict[str, Any],
) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    world = call.get("world")
    center = call.get("center")
    if not world:
        return None, "missing-destination-world"
    if (
        not isinstance(center, list)
        or len(center) != 2
        or not all(isinstance(value, int) for value in center)
    ):
        return None, "missing-arrival-center"

    branch = call.get("branch")
    full_grid = call.get("full_grid")
    if full_grid is None:
        if branch == "full-arrival-grid":
            full_grid = True
        elif branch == "queue-single-cell":
            full_grid = False
        elif call.get("queue_only") is not None:
            full_grid = not call["queue_only"]

    if full_grid is False:
        side = 1
    elif full_grid is True:
        side = call.get("grid_side")
        if not isinstance(side, int) or side <= 0:
            grid_cells = call.get("grid_cells")
            if isinstance(grid_cells, int) and grid_cells > 0:
                inferred_side = math.isqrt(grid_cells)
                side = (
                    inferred_side
                    if inferred_side * inferred_side == grid_cells
                    else None
                )
        if not isinstance(side, int) or side <= 0:
            return None, "missing-full-grid-side"
    else:
        return None, "missing-full-grid-flag"

    half = side // 2
    lower = [center[0] - half, center[1] - half]
    upper = [lower[0] + side - 1, lower[1] + side - 1]
    return (
        {
            "world": world,
            "center": list(center),
            "full_grid": full_grid,
            "grid_side": side,
            "lower": lower,
            "upper": upper,
        },
        None,
    )


def _world_preload_mode(call: Dict[str, Any]) -> str:
    if call.get("full_grid") is True:
        return "native-grid"
    if call.get("full_grid") is False:
        return "single-cell"
    if call.get("branch") == "full-arrival-grid":
        return "native-grid"
    if call.get("branch") == "queue-single-cell":
        return "single-cell"
    if call.get("queue_only") is False:
        return "native-grid"
    if call.get("queue_only") is True:
        return "single-cell"
    return "unknown"


def _world_grid_usefulness(
    calls: Sequence[Dict[str, Any]],
    player_cells: Sequence[Dict[str, Any]],
    definition: str,
) -> Dict[str, Any]:
    details: List[Dict[str, Any]] = []
    hit_calls = 0
    wasted_calls = 0
    missing_geometry = 0
    no_later_comparable = 0

    for call in calls:
        geometry, geometry_error = _world_request_geometry(call)
        later_comparable = [
            observation
            for observation in player_cells
            if observation.get("order", -1) > call.get("order", -1)
            and observation.get("world")
            and isinstance(observation.get("grid"), list)
            and len(observation["grid"]) == 2
        ]
        matching: List[Dict[str, Any]] = []
        if geometry:
            matching = [
                observation
                for observation in later_comparable
                if observation.get("world") == geometry["world"]
                and geometry["lower"][0]
                <= observation["grid"][0]
                <= geometry["upper"][0]
                and geometry["lower"][1]
                <= observation["grid"][1]
                <= geometry["upper"][1]
            ]

        if geometry is None:
            status = "indeterminate-missing-request-geometry"
            missing_geometry += 1
        elif matching:
            status = "hit"
            hit_calls += 1
        elif later_comparable:
            status = "waste-within-recorded-window"
            wasted_calls += 1
        else:
            status = "indeterminate-no-later-comparable-player-cell"
            no_later_comparable += 1

        first_match = min(
            matching,
            key=lambda item: item.get("order", 10**18),
            default=None,
        )
        details.append(
            {
                "line": call.get("line"),
                "order": call.get("order"),
                "load": call.get("load"),
                "attempt_id": call.get("native_attempt_id"),
                "direction": call.get("direction") or "<unknown>",
                "branch": call.get("branch"),
                "geometry": geometry,
                "geometry_error": geometry_error,
                "status": status,
                "later_comparable_player_cell_samples": len(later_comparable),
                "later_same_world_samples": sum(
                    1
                    for observation in later_comparable
                    if geometry
                    and observation.get("world") == geometry["world"]
                ),
                "later_in_requested_square_samples": len(matching),
                "first_hit": (
                    {
                        "line": first_match.get("line"),
                        "order": first_match.get("order"),
                        "load": first_match.get("load"),
                        "cell": first_match.get("cell_id"),
                        "editor": first_match.get("editor"),
                        "world": first_match.get("world"),
                        "grid": first_match.get("grid"),
                    }
                    if first_match
                    else None
                ),
            }
        )

    evaluable = hit_calls + wasted_calls
    return {
        "definition": definition,
        "call_events": len(calls),
        "evaluable_calls": evaluable,
        "hit_calls": hit_calls,
        "wasted_within_recorded_window": wasted_calls,
        "indeterminate_calls": missing_geometry + no_later_comparable,
        "indeterminate_missing_request_geometry": missing_geometry,
        "indeterminate_no_later_comparable_player_cell": no_later_comparable,
        "hit_ratio_among_evaluable": (
            round(hit_calls / evaluable, 4) if evaluable else None
        ),
        "calls": details,
    }


def _native_world_grid_usefulness(
    session: Session,
    correlation: Dict[str, Any],
) -> Dict[str, Any]:
    confirmed_orders = _confirmed_world_call_orders(correlation)
    confirmed_calls = [
        call
        for call in session.native_world_preloads
        if call.get("order") in confirmed_orders
    ]
    result = _world_grid_usefulness(
        confirmed_calls,
        session.player_cells,
        (
            "Per confirmed native preload-world call, a hit requires a later "
            "player-cell sample in the same world and requested grid square. "
            "queue-single-cell means the exact center. For fullGrid, lower is "
            "center-floor(gridSide/2) and upper is lower+gridSide-1 on each "
            "axis. Waste means at least one later world/grid player sample was "
            "recorded but none matched; it does not rule out a visit after the "
            "recording ended or prove preload completion."
        ),
    )
    result["confirmed_call_events"] = result.pop("call_events")
    return result


def _plugin_world_grid_usefulness(session: Session) -> Dict[str, Any]:
    return _world_grid_usefulness(
        session.plugin_world_preloads,
        session.player_cells,
        (
            "Per plugin-triggered engine PreloadWorld invocation, a hit requires "
            "a later player-cell sample in the same world and requested grid "
            "square. This is destination-usefulness correlation only: the event "
            "proves the engine path was invoked with these arguments, not that "
            "the engine accepted, loaded, attached, or completed any cell."
        ),
    )


def _setting_as_bool(settings: Dict[str, str], name: str) -> Optional[bool]:
    return _bool_value(settings.get(name))


GATE_SUMMARY_COUNT_FIELDS = (
    "callbacks",
    "references",
    "teleport_doors",
    "null_reference",
    "disabled_or_deleted",
    "missing_teleport_data",
    "source_world_missing",
    "source_world_mismatch",
    "no_destination",
    "interior_destination",
    "no_destination_world",
    "same_world",
    "accepted",
    "duplicate",
    "capacity_dropped",
    "candidate_logs",
    "candidate_logs_dropped",
    "cached",
)


def _gate_family_summary(
    sources: Sequence[Dict[str, Any]],
    candidates: Sequence[Dict[str, Any]],
    summaries: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    accepted = [event for event in candidates if event.get("accepted") is True]
    rejected = [event for event in candidates if event.get("accepted") is False]
    unknown = [event for event in candidates if event.get("accepted") is None]
    summary_totals = {}
    for name in GATE_SUMMARY_COUNT_FIELDS:
        values = [
            event[name]
            for event in summaries
            if event.get(name) is not None
        ]
        if values:
            summary_totals[name] = sum(values)
    return {
        "source_event_count": len(sources),
        "resolved_source_event_count": sum(
            event.get("resolved") is True for event in sources
        ),
        "unresolved_source_event_count": sum(
            event.get("resolved") is False for event in sources
        ),
        "stable_source_event_count": sum(
            event.get("stable") is True for event in sources
        ),
        "unstable_source_event_count": sum(
            event.get("stable") is False for event in sources
        ),
        "candidate_event_count": len(candidates),
        "accepted_candidate_event_count": len(accepted),
        "rejected_candidate_event_count": len(rejected),
        "unknown_decision_candidate_event_count": len(unknown),
        "decision_counts": dict(
            sorted(
                Counter(
                    event.get("decision") or "<unknown>"
                    for event in candidates
                ).items()
            )
        ),
        "summary_event_count": len(summaries),
        "summary_totals": summary_totals,
        "source_events": list(sources),
        "candidate_events": list(candidates),
        "summary_events": list(summaries),
    }


def _gate_candidate_status(records: Sequence[Dict[str, Any]]) -> str:
    if not records:
        return "not-observed"
    accepted = any(record.get("accepted") is True for record in records)
    rejected = any(record.get("accepted") is False for record in records)
    if accepted and rejected:
        return "mixed"
    if accepted:
        return "accepted"
    if rejected:
        return "rejected"
    return "unknown-decision"


def _count_gate_grids(records: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    counts = Counter(
        tuple(record["computed_grid"])
        for record in records
        if record.get("computed_grid") is not None
    )
    return [
        {"grid": list(grid), "event_count": count}
        for grid, count in sorted(counts.items())
    ]


def _gate_candidate_destination_world(
    event: Dict[str, Any],
) -> Optional[str]:
    return event.get("linked_world") or event.get("destination_world")


def _count_optional_values(
    records: Sequence[Dict[str, Any]], name: str
) -> Dict[str, int]:
    return dict(
        sorted(
            Counter(
                record.get(name) or "<missing>" for record in records
            ).items()
        )
    )


def _correlate_exterior_decisions_with_crosshair(
    decisions: Sequence[Dict[str, Any]],
    crosshair: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    """Pair each decision with the next same-door gate-proximity result.

    DoorPrefetch emits the decision inside TryPreloadFromRef and the
    crosshair-candidate result immediately after that function returns. A pair
    is bounded by the next decision for the same route so a missing diagnostic
    cannot accidentally consume a result from a later poll.
    """
    ordered_decisions = sorted(
        decisions, key=lambda event: event.get("order", 10**18)
    )
    ordered_crosshair = sorted(
        crosshair, key=lambda event: event.get("order", 10**18)
    )
    used_crosshair_orders = set()
    pairs: List[Dict[str, Any]] = []
    expected_result_by_decision = {
        "engine-call-returned": 16,
        "recent-plugin-request": 9,
    }

    for index, decision in enumerate(ordered_decisions):
        decision_order = decision.get("order", -1)
        next_order = (
            ordered_decisions[index + 1].get("order", 10**18)
            if index + 1 < len(ordered_decisions)
            else 10**18
        )
        matching = [
            event
            for event in ordered_crosshair
            if event.get("order") not in used_crosshair_orders
            and decision_order < event.get("order", -1) < next_order
        ]
        result_event = matching[0] if matching else None
        if result_event is not None:
            used_crosshair_orders.add(result_event.get("order"))
        expected_result = expected_result_by_decision.get(
            decision.get("decision")
        )
        observed_result = (
            result_event.get("result") if result_event is not None else None
        )
        pairs.append(
            {
                "decision": decision.get("decision"),
                "decision_line": decision.get("line"),
                "decision_order": decision.get("order"),
                "expected_crosshair_result": expected_result,
                "crosshair_result": observed_result,
                "crosshair_result_name": (
                    result_event.get("result_name")
                    if result_event is not None
                    else None
                ),
                "crosshair_line": (
                    result_event.get("line")
                    if result_event is not None
                    else None
                ),
                "matches_expected": (
                    observed_result == expected_result
                    if expected_result is not None
                    and result_event is not None
                    else None
                ),
            }
        )

    exact_pairs = [
        pair for pair in pairs if pair.get("matches_expected") is True
    ]
    engine_pairs = [
        pair
        for pair in exact_pairs
        if pair.get("decision") == "engine-call-returned"
    ]
    cooldown_pairs = [
        pair
        for pair in exact_pairs
        if pair.get("decision") == "recent-plugin-request"
    ]
    ordered_cycle = any(
        engine.get("decision_order", 10**18)
        < cooldown.get("decision_order", -1)
        for engine in engine_pairs
        for cooldown in cooldown_pairs
    )
    return {
        "pair_count": len(pairs),
        "matched_expected_pair_count": len(exact_pairs),
        "mismatched_pair_count": sum(
            pair.get("matches_expected") is False for pair in pairs
        ),
        "unmatched_decision_count": sum(
            pair.get("crosshair_result") is None for pair in pairs
        ),
        "engine_result_16_pair_count": len(engine_pairs),
        "cooldown_result_9_pair_count": len(cooldown_pairs),
        "preload_then_cooldown_sequence_observed": ordered_cycle,
        "pairs": pairs,
    }


def _diamond_city_route_summary(
    session: Session,
    direction: str,
    specification: Dict[str, Any],
) -> Dict[str, Any]:
    door = specification["door"]
    expected_linked = specification["expected_linked_door"]
    expected_source_world = specification["expected_source_world"]
    expected_destination_world = specification[
        "expected_destination_world"
    ]
    expected_grid = specification["expected_computed_grid"]

    operational = [
        event
        for event in session.gate_enum_candidates
        if event.get("door") == door
    ]
    legacy = [
        event
        for event in session.legacy_gate_scan_candidates
        if event.get("door") == door
    ]
    selected = operational or legacy
    status = _gate_candidate_status(selected)
    if not operational and legacy:
        status = "legacy-only-" + status

    linked_counts = _count_optional_values(selected, "linked_door")
    linked_cell_source_counts = _count_optional_values(
        selected, "linked_cell_source"
    )
    gate_decision_counts = _count_optional_values(selected, "decision")
    destination_world_counts = dict(
        sorted(
            Counter(
                _gate_candidate_destination_world(event) or "<missing>"
                for event in selected
            ).items()
        )
    )

    matching_gate_events = [
        event
        for event in selected
        if event.get("accepted") is True
        and event.get("linked_door") == expected_linked
        and _gate_candidate_destination_world(event)
        == expected_destination_world
        and event.get("computed_grid") == expected_grid
    ]

    preload_decisions = [
        event
        for event in session.exterior_preload_decisions
        if event.get("door") == door
    ]
    exact_preload_decisions = [
        event
        for event in preload_decisions
        if event.get("linked_door") == expected_linked
        and event.get("source_world") == expected_source_world
        and event.get("destination_world") == expected_destination_world
        and event.get("center") == expected_grid
    ]
    preload_decision_counts = _count_optional_values(
        preload_decisions, "decision"
    )
    mode_counts = _count_optional_values(preload_decisions, "mode")

    gate_crosshair = [
        event
        for event in session.crosshair_candidates
        if event.get("door") == door
        and event.get("trigger") == "gate-proximity"
    ]
    crosshair_result_counts = dict(
        sorted(
            Counter(
                "{}:{}".format(
                    event.get("result"),
                    event.get("result_name") or "<unknown>",
                )
                for event in gate_crosshair
            ).items()
        )
    )
    correlation = _correlate_exterior_decisions_with_crosshair(
        preload_decisions, gate_crosshair
    )

    exact_engine_decisions = [
        event
        for event in exact_preload_decisions
        if event.get("decision") == "engine-call-returned"
    ]
    route_anchors = exact_engine_decisions or matching_gate_events
    first_anchor_order = min(
        (
            event.get("order")
            for event in route_anchors
            if isinstance(event.get("order"), int)
        ),
        default=None,
    )
    later_player_samples = (
        [
            event
            for event in session.player_cells
            if event.get("order", -1) > first_anchor_order
        ]
        if first_anchor_order is not None
        else []
    )
    destination_hits = [
        event
        for event in later_player_samples
        if event.get("world") == expected_destination_world
        and event.get("grid") == expected_grid
    ]
    if first_anchor_order is None:
        hit_status = "no-route-evidence"
    elif destination_hits:
        hit_status = "observed"
    elif later_player_samples:
        hit_status = "not-observed-within-recorded-window"
    else:
        hit_status = "indeterminate-no-later-player-sample"

    return {
        "direction": direction,
        "label": specification["label"],
        "door": door,
        "expected_linked_door": expected_linked,
        "expected_source_world": expected_source_world,
        "expected_destination_world": expected_destination_world,
        "expected_computed_grid": list(expected_grid),
        "status": status,
        "operational_candidate_event_count": len(operational),
        "legacy_candidate_event_count": len(legacy),
        "accepted_event_count": sum(
            event.get("accepted") is True for event in selected
        ),
        "rejected_event_count": sum(
            event.get("accepted") is False for event in selected
        ),
        "unknown_decision_event_count": sum(
            event.get("accepted") is None for event in selected
        ),
        "fully_matching_accepted_event_count": len(matching_gate_events),
        "linked_door_counts": linked_counts,
        "linked_door_match_count": sum(
            event.get("linked_door") == expected_linked for event in selected
        ),
        "linked_door_mismatch_count": sum(
            event.get("linked_door") not in {None, expected_linked}
            for event in selected
        ),
        "linked_door_missing_count": sum(
            event.get("linked_door") is None for event in selected
        ),
        "linked_cell_source_counts": linked_cell_source_counts,
        "live_parent_resolution_count": sum(
            event.get("linked_cell_source") == "live-parent"
            for event in selected
        ),
        "save_parent_resolution_count": sum(
            event.get("linked_cell_source") == "save-parent"
            for event in selected
        ),
        "linked_cell_source_missing_count": sum(
            event.get("linked_cell_source") is None for event in selected
        ),
        "destination_world_counts": destination_world_counts,
        "expected_destination_world_match_count": sum(
            _gate_candidate_destination_world(event)
            == expected_destination_world
            for event in selected
        ),
        "destination_world_mismatch_count": sum(
            _gate_candidate_destination_world(event) is not None
            and _gate_candidate_destination_world(event)
            != expected_destination_world
            for event in selected
        ),
        "destination_world_missing_count": sum(
            _gate_candidate_destination_world(event) is None
            for event in selected
        ),
        "computed_grids": _count_gate_grids(selected),
        "expected_grid_match_count": sum(
            event.get("computed_grid") == expected_grid for event in selected
        ),
        "computed_grid_mismatch_count": sum(
            event.get("computed_grid") is not None
            and event.get("computed_grid") != expected_grid
            for event in selected
        ),
        "computed_grid_missing_count": sum(
            event.get("computed_grid") is None for event in selected
        ),
        "decision_counts": gate_decision_counts,
        "events": list(selected),
        "operational_events": list(operational),
        "legacy_events": list(legacy),
        "preload_decisions": {
            "event_count": len(preload_decisions),
            "exact_route_event_count": len(exact_preload_decisions),
            "decision_counts": preload_decision_counts,
            "engine_call_returned_count": sum(
                event.get("decision") == "engine-call-returned"
                for event in preload_decisions
            ),
            "recent_plugin_request_count": sum(
                event.get("decision") == "recent-plugin-request"
                for event in preload_decisions
            ),
            "mode_counts": mode_counts,
            "events": list(preload_decisions),
        },
        "gate_proximity_crosshair": {
            "event_count": len(gate_crosshair),
            "result_counts": crosshair_result_counts,
            "result_16_count": sum(
                event.get("result") == 16 for event in gate_crosshair
            ),
            "result_9_count": sum(
                event.get("result") == 9 for event in gate_crosshair
            ),
            "events": list(gate_crosshair),
        },
        "decision_crosshair_correlation": correlation,
        "destination_visit": {
            "status": hit_status,
            "hit": bool(destination_hits),
            "first_route_evidence_order": first_anchor_order,
            "later_player_sample_count": len(later_player_samples),
            "hit_event_count": len(destination_hits),
            "hit_events": destination_hits,
        },
    }


def _gate_diagnostics(session: Session) -> Dict[str, Any]:
    operational = _gate_family_summary(
        session.gate_enum_sources,
        session.gate_enum_candidates,
        session.gate_enum_summaries,
    )
    legacy = _gate_family_summary(
        session.legacy_gate_scan_sources,
        session.legacy_gate_scan_candidates,
        session.legacy_gate_scan_summaries,
    )
    routes = {
        direction: _diamond_city_route_summary(
            session, direction, specification
        )
        for direction, specification in DIAMOND_CITY_ROUTES.items()
    }
    return {
        "semantics": (
            "gate-enum records are the operational native loaded-reference "
            "enumeration. gate-scan records are diagnostics-only legacy "
            "current/persistent-cell comparison evidence. Diamond City routes "
            "correlate gate enumeration, destination-key decisions, the next "
            "same-door gate-proximity result, and later player world/grid "
            "observations without claiming preload completion."
        ),
        "operational_enumeration": operational,
        "legacy_cell_scan": legacy,
        "diamond_city_routes": routes,
        # Additive compatibility alias retained for existing JSON consumers.
        "diamond_city_exit": routes["exit"],
    }


def summarize_session(session: Session, session_id: int) -> Dict[str, Any]:
    requested_cells = _unique_cells(session.native_candidates)
    preloaded_cells = _unique_cells(session.native_preloads)
    requested_adds = _records_after_first_matching_request(
        session.buffer_adds, session.native_candidates
    )
    requested_removes = _records_after_first_matching_request(
        session.buffer_removes, session.native_candidates
    )
    crosshair_fires = [
        record
        for record in session.crosshair_candidates
        if _is_fire_result(record)
    ]
    legacy_plugin_exterior_cells = _unique_cells(session.plugin_exterior_grids)
    native_attempt_correlation = _native_attempt_correlation(session)
    native_world_relation = _native_world_relation(
        session, native_attempt_correlation
    )
    native_world_grid_usefulness = _native_world_grid_usefulness(
        session, native_attempt_correlation
    )
    plugin_world_grid_usefulness = _plugin_world_grid_usefulness(session)
    plugin_world_modes = Counter(
        _world_preload_mode(record)
        for record in session.plugin_world_preloads
    )
    gate_diagnostics = _gate_diagnostics(session)

    menu_grid_keys = {
        (
            event.get("world"),
            tuple(event["lower"]) if event.get("lower") else None,
            tuple(event["upper"]) if event.get("upper") else None,
        )
        for event in session.menu_geometry
    }
    loadtime_by_kind: Dict[str, Dict[str, Any]] = {}
    kinds = sorted({record["kind"] for record in session.loadtimes})
    for kind in kinds:
        loadtime_by_kind[kind] = _duration_stats(
            record["seconds"]
            for record in session.loadtimes
            if record["kind"] == kind
        )

    warnings: List[str] = []
    if session.diagnostic_lines == 0:
        warnings.append(
            "No PreloadDiag records were found; preload hooks/settings cannot be assessed."
        )
    elif session.configured is False:
        warnings.append(
            "Preload diagnostics were configured off; only read-only native-setting "
            "snapshots may be present."
        )
    elif not session.hook_installed:
        warnings.append(
            "The seven-observer installation marker is missing; diagnostic "
            "observers may not have been installed or this is a partial log."
        )
    for message in session.hook_messages:
        warnings.append("Hook diagnostic: " + _strip_log_prefix(message))

    native_enabled = _setting_as_bool(
        session.effective_settings, "bPreloadLinkedAreas"
    )
    native_activity = bool(
        session.native_candidates
        or session.native_world_candidates
        or session.native_preloads
        or session.native_world_preloads
    )
    plugin_prefetch_activity = bool(
        session.plugin_world_preloads
        or session.plugin_exterior_grids
        or session.exterior_preload_decisions
    ) or any(
        record.get("result") not in {None, 0}
        for record in session.crosshair_candidates
    )
    if (native_enabled is True or native_activity) and plugin_prefetch_activity:
        warnings.append(
            "Native linked-area preload and plugin door prefetch were both "
            "enabled or active. Treat speed/usefulness attribution as contaminated."
        )
    exterior_attempt_evidence = bool(
        session.plugin_world_preloads
        or session.plugin_exterior_grids
        or any(
            event.get("decision") == "engine-call-returned"
            for event in session.exterior_preload_decisions
        )
    ) or any(
        record.get("result_name")
        in {
            "fired-exterior",
            "exterior-submitted",
            "fired-exterior-engine",
        }
        for record in session.crosshair_candidates
    )
    if exterior_attempt_evidence:
        warnings.append(
            "Plugin exterior records prove only that the engine PreloadWorld path "
            "was invoked with the logged arguments (or, for legacy logs, that raw "
            "QueueCellLoad calls were attempted). They do not prove acceptance, "
            "loading, attachment, or completion."
        )

    gate_enum = gate_diagnostics["operational_enumeration"]
    if gate_enum["unresolved_source_event_count"]:
        warnings.append(
            "The native loaded-reference gate enumerator was unresolved for one "
            "or more scans; missing gate candidates in those scans are not "
            "evidence that no gate was loaded."
        )
    if gate_enum["summary_totals"].get("candidate_logs_dropped", 0):
        warnings.append(
            "Gate candidate diagnostics were log-capped; an unlogged door cannot "
            "be classified from this run."
        )
    for route in gate_diagnostics["diamond_city_routes"].values():
        if not route["operational_candidate_event_count"]:
            continue
        if (
            route["linked_door_mismatch_count"]
            or route["linked_door_missing_count"]
        ):
            warnings.append(
                "Diamond City {} gate {} did not consistently resolve linked "
                "door {}.".format(
                    route["direction"],
                    route["door"],
                    route["expected_linked_door"],
                )
            )
        if (
            route["destination_world_mismatch_count"]
            or route["destination_world_missing_count"]
        ):
            warnings.append(
                "Diamond City {} gate {} did not consistently resolve expected "
                "destination world {}.".format(
                    route["direction"],
                    route["door"],
                    route["expected_destination_world"],
                )
            )
        if (
            route["computed_grid_mismatch_count"]
            or route["computed_grid_missing_count"]
        ):
            warnings.append(
                "Diamond City {} gate {} did not consistently compute expected "
                "arrival grid ({}, {}).".format(
                    route["direction"],
                    route["door"],
                    route["expected_computed_grid"][0],
                    route["expected_computed_grid"][1],
                )
            )

    menu_enabled = _setting_as_bool(
        session.effective_settings, "bUseMenuLoadCellPreload"
    )
    if menu_enabled is False and session.menu_geometry:
        warnings.append(
            "Menu geometry preload events were recorded although the latest setting "
            "snapshot is off; inspect settings_history for a mid-session change."
        )
    if session.hook_installed and not session.player_cells:
        warnings.append(
            "No player-cell observations were recorded, so destination usefulness "
            "cannot be determined."
        )

    return {
        "session_id": session_id,
        "source": session.source,
        "source_part": session.part,
        "line_range": [session.start_line, session.end_line],
        "diagnostics": {
            "configured": session.configured,
            "hook_installed": session.hook_installed,
            "hook_removed": session.hook_removed,
            "preload_diag_lines": session.diagnostic_lines,
            "event_counts": dict(sorted(session.event_counts.items())),
            "hook_messages": [_strip_log_prefix(item) for item in session.hook_messages],
        },
        "native_settings": {
            "effective": dict(session.effective_settings),
            "history": list(session.settings_history),
        },
        "load_durations": {
            "loading_menu_close": {
                "stats": _duration_stats(
                    record["seconds"] for record in session.menu_closes
                ),
                "events": list(session.menu_closes),
            },
            "loadtime": {
                "stats": _duration_stats(
                    record["seconds"] for record in session.loadtimes
                ),
                "by_kind": loadtime_by_kind,
                "events": list(session.loadtimes),
            },
        },
        "menu_geometry": {
            "event_count": len(session.menu_geometry),
            "unique_grids": len(menu_grid_keys),
            "declared_cells_total": sum(
                event.get("cells") or 0 for event in session.menu_geometry
            ),
            "events": list(session.menu_geometry),
        },
        "native_linked": {
            "requested": {
                "event_count": len(session.native_candidates),
                "unique_cells": requested_cells,
                "selection_limit_counts": dict(
                    sorted(
                        Counter(
                            str(record.get("selection_limit"))
                            for record in session.native_candidates
                            if record.get("selection_limit") is not None
                        ).items()
                    )
                ),
                "events": list(session.native_candidates),
            },
            "world_candidates": {
                "semantics": (
                    "Observation-only outer candidates. A matching preload-world "
                    "call with the same nativeAttemptId is required before this "
                    "counts as a request."
                ),
                "event_count": len(session.native_world_candidates),
                "events": list(session.native_world_candidates),
            },
            "preloaded": {
                "semantics": (
                    "Observed preload function calls and arguments only. "
                    "For interiors, a later interior-buffer-add plus cell state/"
                    "loadedData fields provide temporal post-call/ring context; "
                    "they do not prove target insertion, full 3D, or Havok "
                    "attachment."
                ),
                "event_count": len(session.native_preloads),
                "unique_cells": preloaded_cells,
                "world_grid_events": list(session.native_world_preloads),
            },
            "preload_calls": {
                "total_event_count": (
                    len(session.native_preloads)
                    + len(session.native_world_preloads)
                ),
                "interior_event_count": len(session.native_preloads),
                "world_event_count": len(session.native_world_preloads),
            },
            "attempt_correlation": native_attempt_correlation,
            "world_relation": native_world_relation,
            "world_grid_usefulness": native_world_grid_usefulness,
            "buffer_added_for_requested_cells": {
                "semantics": (
                    "Only buffer events recorded after the first matching native "
                    "candidate for that FormID are counted."
                ),
                "event_count": len(requested_adds),
                "unique_cells": _unique_cells(requested_adds),
            },
            "buffer_removed_for_requested_cells": {
                "semantics": (
                    "Only buffer events recorded after the first matching native "
                    "candidate for that FormID are counted."
                ),
                "event_count": len(requested_removes),
                "unique_cells": _unique_cells(requested_removes),
            },
            "all_interior_buffer_activity": {
                "add_event_count": len(session.buffer_adds),
                "added_unique_cells": _unique_cells(session.buffer_adds),
                "remove_event_count": len(session.buffer_removes),
                "removed_unique_cells": _unique_cells(session.buffer_removes),
                "add_events": list(session.buffer_adds),
                "remove_events": list(session.buffer_removes),
            },
            "requested_usefulness": _usefulness(
                session.native_candidates, session.player_cells
            ),
            "preloaded_usefulness": _usefulness(
                session.native_preloads, session.player_cells
            ),
        },
        "crosshair": {
            "candidate_event_count": len(session.crosshair_candidates),
            "unique_candidate_cells": _unique_cells(session.crosshair_candidates),
            "fire_event_count": len(crosshair_fires),
            "unique_fired_cells": _unique_cells(crosshair_fires),
            "source_counts": dict(
                sorted(
                    Counter(
                        record.get("source") or "<unknown>"
                        for record in session.crosshair_candidates
                    ).items()
                )
            ),
            "by_trigger": _crosshair_by_trigger(session.crosshair_candidates),
            "fired_usefulness": _usefulness(crosshair_fires, session.player_cells),
            "events": list(session.crosshair_candidates),
        },
        "gate_diagnostics": gate_diagnostics,
        "plugin_exterior_preload": {
            "semantics": (
                "Counts plugin-triggered TES::PreloadWorld invocations. The "
                "single-cell mode passes queueOnly=true; native-grid mode passes "
                "queueOnly=false and lets the engine use uGridsToLoad. An event "
                "proves invocation and arguments only, not acceptance, loading, "
                "attachment, or completion. exterior-preload-decision records "
                "separately expose exact destination-key submission/cooldown "
                "decisions."
            ),
            "invocation_event_count": len(session.plugin_world_preloads),
            "decision_event_count": len(
                session.exterior_preload_decisions
            ),
            "decision_counts": dict(
                sorted(
                    Counter(
                        record.get("decision") or "<unknown>"
                        for record in session.exterior_preload_decisions
                    ).items()
                )
            ),
            "decision_events": list(session.exterior_preload_decisions),
            "single_cell_invocation_count": plugin_world_modes["single-cell"],
            "native_grid_invocation_count": plugin_world_modes["native-grid"],
            "unknown_mode_invocation_count": plugin_world_modes["unknown"],
            "destination_world_counts": dict(
                sorted(
                    Counter(
                        record.get("world") or "<unknown>"
                        for record in session.plugin_world_preloads
                    ).items()
                )
            ),
            "source_counts": dict(
                sorted(
                    Counter(
                        record.get("source") or "<unknown>"
                        for record in session.plugin_world_preloads
                    ).items()
                )
            ),
            "world_grid_usefulness": plugin_world_grid_usefulness,
            "events": list(session.plugin_world_preloads),
        },
        "legacy_plugin_exterior_grid": {
            "semantics": (
                "Compatibility view for old plugin-exterior-grid records. These "
                "were raw QueueCellLoad call attempts, not current engine "
                "PreloadWorld invocations, and never proved acceptance or "
                "completion."
            ),
            "event_count": len(session.plugin_exterior_grids),
            "unique_destination_cells": legacy_plugin_exterior_cells,
            "source_counts": dict(
                sorted(
                    Counter(
                        record.get("source") or "<unknown>"
                        for record in session.plugin_exterior_grids
                    ).items()
                )
            ),
            "requested_positions_total": sum(
                record.get("requested") or 0
                for record in session.plugin_exterior_grids
            ),
            "submission_attempts_total": sum(
                record.get("submission_attempts") or 0
                for record in session.plugin_exterior_grids
            ),
            "resident_or_busy_skipped_total": sum(
                record.get("resident_or_busy_skipped") or 0
                for record in session.plugin_exterior_grids
            ),
            "plugin_duplicate_skipped_total": sum(
                record.get("plugin_duplicate_skipped") or 0
                for record in session.plugin_exterior_grids
            ),
            "legacy_field_event_count": sum(
                1
                for record in session.plugin_exterior_grids
                if record.get("used_legacy_submission_fields")
            ),
            "by_trigger": _plugin_exterior_by_trigger(
                session.plugin_exterior_grids
            ),
            "usefulness": _usefulness(
                session.plugin_exterior_grids, session.player_cells
            ),
            "events": list(session.plugin_exterior_grids),
        },
        "player_cells": {
            "event_count": len(session.player_cells),
            "unique_cells": _unique_cells(session.player_cells),
            "events": list(session.player_cells),
        },
        "destination_cells_later_observed_as_player": _destination_hits(session),
        "warnings": _dedupe(warnings),
    }


def summarize(parser: LogParser, inputs: Sequence[str]) -> Dict[str, Any]:
    sessions = [
        summarize_session(session, index)
        for index, session in enumerate(parser.sessions, start=1)
        if session.meaningful()
    ]
    menu_seconds = [
        event["seconds"]
        for session in sessions
        for event in session["load_durations"]["loading_menu_close"]["events"]
    ]
    loadtime_seconds = [
        event["seconds"]
        for session in sessions
        for event in session["load_durations"]["loadtime"]["events"]
    ]
    world_usefulness_confirmed = sum(
        session["native_linked"]["world_grid_usefulness"][
            "confirmed_call_events"
        ]
        for session in sessions
    )
    world_usefulness_evaluable = sum(
        session["native_linked"]["world_grid_usefulness"]["evaluable_calls"]
        for session in sessions
    )
    world_usefulness_hits = sum(
        session["native_linked"]["world_grid_usefulness"]["hit_calls"]
        for session in sessions
    )
    world_usefulness_waste = sum(
        session["native_linked"]["world_grid_usefulness"][
            "wasted_within_recorded_window"
        ]
        for session in sessions
    )
    plugin_world_calls = sum(
        session["plugin_exterior_preload"]["world_grid_usefulness"][
            "call_events"
        ]
        for session in sessions
    )
    plugin_world_evaluable = sum(
        session["plugin_exterior_preload"]["world_grid_usefulness"][
            "evaluable_calls"
        ]
        for session in sessions
    )
    plugin_world_hits = sum(
        session["plugin_exterior_preload"]["world_grid_usefulness"]["hit_calls"]
        for session in sessions
    )
    plugin_world_waste = sum(
        session["plugin_exterior_preload"]["world_grid_usefulness"][
            "wasted_within_recorded_window"
        ]
        for session in sessions
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "inputs": list(inputs),
        "session_count": len(sessions),
        "aggregate": {
            "loading_menu_close": _duration_stats(menu_seconds),
            "loadtime": _duration_stats(loadtime_seconds),
            "menu_geometry_events": sum(
                session["menu_geometry"]["event_count"] for session in sessions
            ),
            "native_requested_events": sum(
                session["native_linked"]["requested"]["event_count"]
                for session in sessions
            ),
            "native_preload_events": sum(
                session["native_linked"]["preloaded"]["event_count"]
                for session in sessions
            ),
            "native_preload_call_events": sum(
                session["native_linked"]["preload_calls"]["total_event_count"]
                for session in sessions
            ),
            "native_world_candidate_events": sum(
                session["native_linked"]["world_candidates"]["event_count"]
                for session in sessions
            ),
            "native_confirmed_candidate_events": sum(
                session["native_linked"]["attempt_correlation"][
                    "confirmed_candidate_event_count"
                ]
                for session in sessions
            ),
            "native_unconfirmed_candidate_events": sum(
                session["native_linked"]["attempt_correlation"][
                    "unconfirmed_candidate_event_count"
                ]
                for session in sessions
            ),
            "native_world_relation": {
                "call_event_count": sum(
                    session["native_linked"]["world_relation"][
                        "call_event_count"
                    ]
                    for session in sessions
                ),
                "relation_counts": {
                    relation: sum(
                        session["native_linked"]["world_relation"][
                            "relation_counts"
                        ][relation]
                        for session in sessions
                    )
                    for relation in ("same-world", "cross-world", "unknown")
                },
                "confirmed_call_event_count": sum(
                    session["native_linked"]["world_relation"][
                        "confirmed_call_event_count"
                    ]
                    for session in sessions
                ),
                "confirmed_relation_counts": {
                    relation: sum(
                        session["native_linked"]["world_relation"][
                            "confirmed_relation_counts"
                        ][relation]
                        for session in sessions
                    )
                    for relation in ("same-world", "cross-world", "unknown")
                },
            },
            "native_world_grid_usefulness": {
                "confirmed_call_events": world_usefulness_confirmed,
                "evaluable_calls": world_usefulness_evaluable,
                "hit_calls": world_usefulness_hits,
                "wasted_within_recorded_window": world_usefulness_waste,
                "indeterminate_calls": (
                    world_usefulness_confirmed - world_usefulness_evaluable
                ),
                "hit_ratio_among_evaluable": (
                    round(
                        world_usefulness_hits / world_usefulness_evaluable,
                        4,
                    )
                    if world_usefulness_evaluable
                    else None
                ),
            },
            "crosshair_candidate_events": sum(
                session["crosshair"]["candidate_event_count"] for session in sessions
            ),
            "crosshair_fire_events": sum(
                session["crosshair"]["fire_event_count"] for session in sessions
            ),
            "gate_enum_candidate_events": sum(
                session["gate_diagnostics"]["operational_enumeration"][
                    "candidate_event_count"
                ]
                for session in sessions
            ),
            "gate_enum_accepted_candidate_events": sum(
                session["gate_diagnostics"]["operational_enumeration"][
                    "accepted_candidate_event_count"
                ]
                for session in sessions
            ),
            "gate_enum_rejected_candidate_events": sum(
                session["gate_diagnostics"]["operational_enumeration"][
                    "rejected_candidate_event_count"
                ]
                for session in sessions
            ),
            "diamond_gate_candidate_events": sum(
                session["gate_diagnostics"]["diamond_city_exit"][
                    "operational_candidate_event_count"
                ]
                for session in sessions
            ),
            "diamond_gate_accepted_events": sum(
                session["gate_diagnostics"]["diamond_city_exit"][
                    "accepted_event_count"
                ]
                for session in sessions
                if session["gate_diagnostics"]["diamond_city_exit"][
                    "operational_candidate_event_count"
                ]
            ),
            "diamond_gate_rejected_events": sum(
                session["gate_diagnostics"]["diamond_city_exit"][
                    "rejected_event_count"
                ]
                for session in sessions
                if session["gate_diagnostics"]["diamond_city_exit"][
                    "operational_candidate_event_count"
                ]
            ),
            "diamond_city_routes": {
                direction: {
                    "operational_candidate_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["operational_candidate_event_count"]
                        for session in sessions
                    ),
                    "accepted_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["accepted_event_count"]
                        for session in sessions
                    ),
                    "fully_matching_accepted_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["fully_matching_accepted_event_count"]
                        for session in sessions
                    ),
                    "engine_call_returned_decisions": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["preload_decisions"][
                            "engine_call_returned_count"
                        ]
                        for session in sessions
                    ),
                    "recent_plugin_request_decisions": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["preload_decisions"][
                            "recent_plugin_request_count"
                        ]
                        for session in sessions
                    ),
                    "result_16_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["gate_proximity_crosshair"]["result_16_count"]
                        for session in sessions
                    ),
                    "result_9_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["gate_proximity_crosshair"]["result_9_count"]
                        for session in sessions
                    ),
                    "destination_hit_events": sum(
                        session["gate_diagnostics"]["diamond_city_routes"][
                            direction
                        ]["destination_visit"]["hit_event_count"]
                        for session in sessions
                    ),
                }
                for direction in DIAMOND_CITY_ROUTES
            },
            "plugin_preload_world_invocation_events": sum(
                session["plugin_exterior_preload"]["invocation_event_count"]
                for session in sessions
            ),
            "plugin_exterior_preload_decision_events": sum(
                session["plugin_exterior_preload"]["decision_event_count"]
                for session in sessions
            ),
            "plugin_preload_world_single_cell_invocations": sum(
                session["plugin_exterior_preload"][
                    "single_cell_invocation_count"
                ]
                for session in sessions
            ),
            "plugin_preload_world_native_grid_invocations": sum(
                session["plugin_exterior_preload"][
                    "native_grid_invocation_count"
                ]
                for session in sessions
            ),
            "plugin_preload_world_unknown_mode_invocations": sum(
                session["plugin_exterior_preload"][
                    "unknown_mode_invocation_count"
                ]
                for session in sessions
            ),
            "plugin_world_grid_usefulness": {
                "call_events": plugin_world_calls,
                "evaluable_calls": plugin_world_evaluable,
                "hit_calls": plugin_world_hits,
                "wasted_within_recorded_window": plugin_world_waste,
                "indeterminate_calls": plugin_world_calls - plugin_world_evaluable,
                "hit_ratio_among_evaluable": (
                    round(plugin_world_hits / plugin_world_evaluable, 4)
                    if plugin_world_evaluable
                    else None
                ),
            },
            "legacy_plugin_exterior_grid_events": sum(
                session["legacy_plugin_exterior_grid"]["event_count"]
                for session in sessions
            ),
            "sessions_with_attribution_contamination": [
                session["session_id"]
                for session in sessions
                if any(
                    "attribution as contaminated" in warning
                    for warning in session["warnings"]
                )
            ],
        },
        "sessions": sessions,
    }


def _strip_log_prefix(text: str) -> str:
    marker = text.find("PreloadDiag")
    return text[marker:] if marker >= 0 else text.strip()


def _dedupe(items: Sequence[str]) -> List[str]:
    seen = set()
    result = []
    for item in items:
        if item not in seen:
            seen.add(item)
            result.append(item)
    return result


def _format_stats(stats: Dict[str, Any]) -> str:
    if not stats["count"]:
        return "none recorded"
    return (
        "{count} loads, total {total_seconds:.2f}s, mean {mean_seconds:.2f}s, "
        "median {median_seconds:.2f}s, range {min_seconds:.2f}-{max_seconds:.2f}s"
    ).format(**stats)


def _format_cell_list(cells: Sequence[Dict[str, Any]], limit: int = 12) -> str:
    if not cells:
        return "none"
    rendered = []
    for cell in cells[:limit]:
        editors = [
            editor
            for editor in cell.get("editor_ids", [])
            if editor not in {"<none>", "<null>"}
        ]
        rendered.append(
            "{}{}".format(
                cell["form_id"],
                " ({})".format("/".join(editors)) if editors else "",
            )
        )
    if len(cells) > limit:
        rendered.append("+{} more".format(len(cells) - limit))
    return ", ".join(rendered)


def _format_usefulness(usefulness: Dict[str, Any]) -> str:
    ratio = usefulness["hit_ratio_among_evaluable"]
    if ratio is None:
        return (
            "indeterminate ({} destinations; {} had no later player-cell sample)"
        ).format(
            usefulness["unique_destinations"],
            usefulness["indeterminate_no_later_player_sample"],
        )
    return (
        "{:.1f}% useful among {} evaluable "
        "({} useful, {} not observed in later samples, {} indeterminate)"
    ).format(
        ratio * 100.0,
        usefulness["evaluable_destinations"],
        usefulness["useful"],
        usefulness["wasted_within_recorded_window"],
        usefulness["indeterminate_no_later_player_sample"],
    )


def _format_world_grid_usefulness(
    usefulness: Dict[str, Any],
    call_count_key: str = "confirmed_call_events",
    call_label: str = "confirmed call(s)",
) -> str:
    ratio = usefulness["hit_ratio_among_evaluable"]
    prefix = (
        "indeterminate"
        if ratio is None
        else "{:.1f}% hit rate".format(ratio * 100.0)
    )
    return (
        "{}; {} {}, {} evaluable, {} hit, {} waste within "
        "recorded window, {} indeterminate"
    ).format(
        prefix,
        usefulness[call_count_key],
        call_label,
        usefulness["evaluable_calls"],
        usefulness["hit_calls"],
        usefulness["wasted_within_recorded_window"],
        usefulness["indeterminate_calls"],
    )


def render_human(report: Dict[str, Any], stream: TextIO = sys.stdout) -> None:
    print(
        "LoadingScreens preload report: {} session(s)".format(
            report["session_count"]
        ),
        file=stream,
    )
    if report["session_count"] > 1:
        print(
            "Overall LoadingMenu CLOSE: "
            + _format_stats(report["aggregate"]["loading_menu_close"]),
            file=stream,
        )
        print(
            "Overall LOADTIME: "
            + _format_stats(report["aggregate"]["loadtime"]),
            file=stream,
        )

    for session in report["sessions"]:
        print(file=stream)
        print(
            "Session {session_id}: {source} (part {source_part}, lines "
            "{line_range[0]}-{line_range[1]})".format(**session),
            file=stream,
        )
        diagnostics = session["diagnostics"]
        print(
            "  Diagnostics: configured={}, hooks={}".format(
                _display_optional_bool(diagnostics["configured"]),
                "installed" if diagnostics["hook_installed"] else "not confirmed",
            ),
            file=stream,
        )

        settings = session["native_settings"]["effective"]
        if settings:
            order = (
                "bUseMenuLoadCellPreload",
                "bPreloadLinkedAreas",
                "fTeleportPreloadDistance",
                "uInteriorCellBuffer",
                "uGridsToLoad",
            )
            parts = [
                "{}={}".format(name, settings[name])
                for name in order
                if name in settings
            ]
            parts.extend(
                "{}={}".format(name, value)
                for name, value in sorted(settings.items())
                if name not in order
            )
            print("  Effective native settings: " + ", ".join(parts), file=stream)
        else:
            print("  Effective native settings: none recorded", file=stream)

        durations = session["load_durations"]
        print(
            "  LoadingMenu CLOSE: "
            + _format_stats(durations["loading_menu_close"]["stats"]),
            file=stream,
        )
        print(
            "  LOADTIME: " + _format_stats(durations["loadtime"]["stats"]),
            file=stream,
        )
        if durations["loadtime"]["by_kind"]:
            for kind, stats in durations["loadtime"]["by_kind"].items():
                print(
                    "    {}: {}".format(kind, _format_stats(stats)),
                    file=stream,
                )

        menu = session["menu_geometry"]
        print(
            "  Native menu geometry: {} event(s), {} unique grid(s), "
            "{} declared geometry cells".format(
                menu["event_count"],
                menu["unique_grids"],
                menu["declared_cells_total"],
            ),
            file=stream,
        )

        native = session["native_linked"]
        requested = native["requested"]
        preloaded = native["preloaded"]
        preload_calls = native["preload_calls"]
        added = native["buffer_added_for_requested_cells"]
        removed = native["buffer_removed_for_requested_cells"]
        print(
            "  Native linked requested: {} event(s), {} unique: {}".format(
                requested["event_count"],
                len(requested["unique_cells"]),
                _format_cell_list(requested["unique_cells"]),
            ),
            file=stream,
        )
        selection_limits = ", ".join(
            "limit {}={}".format(limit, count)
            for limit, count in requested["selection_limit_counts"].items()
        )
        print(
            "  Native interior selection limits: "
            + (selection_limits or "none recorded"),
            file=stream,
        )
        print(
            "  Native linked preload calls: {} event(s) "
            "({} interior, {} world), {} unique interior: {}".format(
                preload_calls["total_event_count"],
                preload_calls["interior_event_count"],
                preload_calls["world_event_count"],
                len(preloaded["unique_cells"]),
                _format_cell_list(preloaded["unique_cells"]),
            ),
            file=stream,
        )
        correlation = native["attempt_correlation"]
        print(
            "  Native candidate correlation: {} confirmed / {} unconfirmed "
            "({} interior candidates, {} provisional world candidates)".format(
                correlation["confirmed_candidate_event_count"],
                correlation["unconfirmed_candidate_event_count"],
                correlation["interior_candidate_event_count"],
                correlation["world_candidate_event_count"],
            ),
            file=stream,
        )
        for direction, counts in correlation["by_direction"].items():
            print(
                "    {}: {} candidate(s), {} confirmed, {} unconfirmed, "
                "{} preload call(s)".format(
                    direction,
                    counts["candidate_events"],
                    counts["confirmed_candidate_events"],
                    counts["unconfirmed_candidate_events"],
                    counts["preload_call_events"],
                ),
                file=stream,
            )
        world_relation = native["world_relation"]
        relation_counts = world_relation["relation_counts"]
        confirmed_relation_counts = world_relation[
            "confirmed_relation_counts"
        ]
        print(
            "  Native preload-world relation (all/confirmed calls): "
            "same-world={}/{}, cross-world={}/{}, unknown={}/{}; "
            "origin is a current-player snapshot at outer-visitor time".format(
                relation_counts["same-world"],
                confirmed_relation_counts["same-world"],
                relation_counts["cross-world"],
                confirmed_relation_counts["cross-world"],
                relation_counts["unknown"],
                confirmed_relation_counts["unknown"],
            ),
            file=stream,
        )
        print(
            "  Native preload-world grid usefulness: "
            + _format_world_grid_usefulness(
                native["world_grid_usefulness"]
            ),
            file=stream,
        )
        print(
            "  Requested cells with later buffer add/remove-call events: "
            "{}/{} unique: {} / {}".format(
                len(added["unique_cells"]),
                len(removed["unique_cells"]),
                _format_cell_list(added["unique_cells"]),
                _format_cell_list(removed["unique_cells"]),
            ),
            file=stream,
        )
        print(
            "  Native preload-call destination usefulness: "
            + _format_usefulness(native["preloaded_usefulness"]),
            file=stream,
        )

        crosshair = session["crosshair"]
        candidate_sources = ", ".join(
            "{}={}".format(name, count)
            for name, count in crosshair["source_counts"].items()
        )
        print(
            "  Plugin door candidates: {} event(s), {} fire event(s), "
            "{} unique fired: {}; sources: {}".format(
                crosshair["candidate_event_count"],
                crosshair["fire_event_count"],
                len(crosshair["unique_fired_cells"]),
                _format_cell_list(crosshair["unique_fired_cells"]),
                candidate_sources or "none",
            ),
            file=stream,
        )
        for trigger, details in crosshair["by_trigger"].items():
            results = ", ".join(
                "{}={}".format(name, count)
                for name, count in details["results"].items()
            )
            print(
                "    {}: {} candidate(s), {} fire(s); {}".format(
                    trigger,
                    details["candidate_events"],
                    details["fire_events"],
                    results or "no results",
                ),
                file=stream,
            )
        print(
            "  Plugin door-candidate fire usefulness: "
            + _format_usefulness(crosshair["fired_usefulness"]),
            file=stream,
        )

        gate_diagnostics = session["gate_diagnostics"]
        gate_enum = gate_diagnostics["operational_enumeration"]
        gate_enum_totals = gate_enum["summary_totals"]
        print(
            "  Gate discovery via native loaded-reference enumeration: "
            "{} source event(s) ({} resolved, {} unresolved), "
            "{} candidate(s) ({} accepted, {} rejected, {} unknown), "
            "{} summary event(s); summary callbacks={}, accepted={}, cached={}, "
            "candidateLogsDropped={}".format(
                gate_enum["source_event_count"],
                gate_enum["resolved_source_event_count"],
                gate_enum["unresolved_source_event_count"],
                gate_enum["candidate_event_count"],
                gate_enum["accepted_candidate_event_count"],
                gate_enum["rejected_candidate_event_count"],
                gate_enum["unknown_decision_candidate_event_count"],
                gate_enum["summary_event_count"],
                gate_enum_totals.get("callbacks", "unknown"),
                gate_enum_totals.get("accepted", "unknown"),
                gate_enum_totals.get("cached", "unknown"),
                gate_enum_totals.get("candidate_logs_dropped", "unknown"),
            ),
            file=stream,
        )
        legacy_gate = gate_diagnostics["legacy_cell_scan"]
        print(
            "  Legacy diagnostics-only gate cell scans: "
            "{} source event(s) ({} stable, {} unstable), "
            "{} candidate(s), {} summary event(s)".format(
                legacy_gate["source_event_count"],
                legacy_gate["stable_source_event_count"],
                legacy_gate["unstable_source_event_count"],
                legacy_gate["candidate_event_count"],
                legacy_gate["summary_event_count"],
            ),
            file=stream,
        )
        diamond_gate = gate_diagnostics["diamond_city_exit"]
        linked_doors = ", ".join(
            "{}={}".format(form_id, count)
            for form_id, count in diamond_gate["linked_door_counts"].items()
        )
        computed_grids = ", ".join(
            "({}, {})={}".format(
                item["grid"][0], item["grid"][1], item["event_count"]
            )
            for item in diamond_gate["computed_grids"]
        )
        decisions = ", ".join(
            "{}={}".format(name, count)
            for name, count in diamond_gate["decision_counts"].items()
        )
        print(
            "  Diamond gate {} -> expected linked {}: status={}; "
            "linkedDoor(s)={} ({} match); computedGrid(s)={} "
            "(expected ({}, {}), {} match); decision(s)={}".format(
                diamond_gate["door"],
                diamond_gate["expected_linked_door"],
                diamond_gate["status"],
                linked_doors or "none",
                diamond_gate["linked_door_match_count"],
                computed_grids or "none",
                diamond_gate["expected_computed_grid"][0],
                diamond_gate["expected_computed_grid"][1],
                diamond_gate["expected_grid_match_count"],
                decisions or "none",
            ),
            file=stream,
        )
        for direction in ("entry", "exit"):
            route = gate_diagnostics["diamond_city_routes"][direction]
            linked_sources = ", ".join(
                "{}={}".format(name, count)
                for name, count in route[
                    "linked_cell_source_counts"
                ].items()
            )
            destination_worlds = ", ".join(
                "{}={}".format(world, count)
                for world, count in route[
                    "destination_world_counts"
                ].items()
            )
            preload_decisions = ", ".join(
                "{}={}".format(name, count)
                for name, count in route["preload_decisions"][
                    "decision_counts"
                ].items()
            )
            crosshair_results = ", ".join(
                "{}={}".format(name, count)
                for name, count in route["gate_proximity_crosshair"][
                    "result_counts"
                ].items()
            )
            print(
                "  Diamond City {} ({} -> {}): enum={}; "
                "fullyMatchedAccepted={}; linkedCellSource(s)={}; "
                "destWorld(s)={} (expected {}, {} match); "
                "computedGrid expected=({}, {}) ({} match)".format(
                    direction,
                    route["door"],
                    route["expected_linked_door"],
                    route["status"],
                    route["fully_matching_accepted_event_count"],
                    linked_sources or "none",
                    destination_worlds or "none",
                    route["expected_destination_world"],
                    route["expected_destination_world_match_count"],
                    route["expected_computed_grid"][0],
                    route["expected_computed_grid"][1],
                    route["expected_grid_match_count"],
                ),
                file=stream,
            )
            print(
                "    exterior decisions={}; gate-proximity results={}; "
                "decision/result 16->9 cycle={}; destination visit={}".format(
                    preload_decisions or "none",
                    crosshair_results or "none",
                    "observed"
                    if route["decision_crosshair_correlation"][
                        "preload_then_cooldown_sequence_observed"
                    ]
                    else "not-observed",
                    route["destination_visit"]["status"],
                ),
                file=stream,
            )

        exterior = session["plugin_exterior_preload"]
        exterior_sources = ", ".join(
            "{}={}".format(name, count)
            for name, count in exterior["source_counts"].items()
        )
        print(
            "  Plugin exterior engine PreloadWorld: {} invocation(s), "
            "{} single-cell / {} native-grid / {} unknown; sources: {}".format(
                exterior["invocation_event_count"],
                exterior["single_cell_invocation_count"],
                exterior["native_grid_invocation_count"],
                exterior["unknown_mode_invocation_count"],
                exterior_sources or "none",
            ),
            file=stream,
        )
        world_counts = ", ".join(
            "{}={}".format(world, count)
            for world, count in exterior["destination_world_counts"].items()
        )
        print(
            "    Destination worlds: " + (world_counts or "none"),
            file=stream,
        )
        exterior_decisions = ", ".join(
            "{}={}".format(name, count)
            for name, count in exterior["decision_counts"].items()
        )
        print(
            "    Destination-key decisions: {} event(s); {}".format(
                exterior["decision_event_count"],
                exterior_decisions or "none",
            ),
            file=stream,
        )
        print(
            "  Plugin exterior world/grid usefulness: "
            + _format_world_grid_usefulness(
                exterior["world_grid_usefulness"],
                call_count_key="call_events",
                call_label="invocation(s)",
            ),
            file=stream,
        )

        legacy_exterior = session["legacy_plugin_exterior_grid"]
        if legacy_exterior["event_count"]:
            print(
                "  Legacy raw exterior-grid records: {} event(s), "
                "{} QueueCellLoad call attempt(s); not current behavior".format(
                    legacy_exterior["event_count"],
                    legacy_exterior["submission_attempts_total"],
                ),
                file=stream,
            )

        hits = session["destination_cells_later_observed_as_player"]
        if hits:
            print("  Destinations later observed as player cells:", file=stream)
            for hit in hits:
                editors = "/".join(hit["editor_ids"])
                print(
                    "    {}{} via {}; observed load={} reason={}".format(
                        hit["form_id"],
                        " ({})".format(editors) if editors else "",
                        ", ".join(hit["sources"]),
                        hit["observed_load"],
                        hit["observed_reason"],
                    ),
                    file=stream,
                )
        else:
            print(
                "  Destinations later observed as player cells: none determinable",
                file=stream,
            )

        if session["warnings"]:
            print("  Warnings:", file=stream)
            for warning in session["warnings"]:
                print("    - " + warning, file=stream)
        else:
            print("  Warnings: none", file=stream)


def _display_optional_bool(value: Optional[bool]) -> str:
    if value is None:
        return "unknown"
    return "true" if value else "false"


def _expand_inputs(arguments: Sequence[str]) -> List[Path]:
    paths: List[Path] = []
    for argument in arguments:
        path = Path(argument).expanduser()
        if path.is_dir():
            paths.extend(sorted(path.glob("LoadingScreens*.log")))
        else:
            paths.append(path)
    seen = set()
    result = []
    for path in paths:
        key = str(path.resolve()) if path.exists() else str(path)
        if key not in seen:
            seen.add(key)
            result.append(path)
    return result


def _default_input() -> Optional[Path]:
    home = Path.home()
    candidates = [
        home / "Documents/My Games/Fallout4/F4SE/LoadingScreens.log",
        home / "Documents/My Games/Fallout4VR/F4SE/LoadingScreens.log",
    ]
    existing = [path for path in candidates if path.is_file()]
    return max(existing, key=lambda path: path.stat().st_mtime) if existing else None


def parse_files(paths: Sequence[Path]) -> Tuple[LogParser, List[str]]:
    parser = LogParser()
    labels: List[str] = []
    for path in paths:
        label = str(path.resolve())
        labels.append(label)
        parser.begin_source(label)
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line_no, line in enumerate(handle, start=1):
                parser.feed(label, line_no, line)
    return parser, labels


def parse_text(source: str, text: str) -> LogParser:
    parser = LogParser()
    parser.begin_source(source)
    for line_no, line in enumerate(text.splitlines(), start=1):
        parser.feed(source, line_no, line)
    return parser


def self_test() -> None:
    fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag native-setting when=install bUseMenuLoadCellPreload=true
[info] PreloadDiag native-setting when=install bPreloadLinkedAreas=true
[info] PreloadDiag native-setting when=install fTeleportPreloadDistance=4096.000
[info] PreloadDiag native-setting when=install uInteriorCellBuffer=3
[info] PreloadDiag native-setting when=install uGridsToLoad=5
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163 (menu hook is combined geometry only, not cell loading)
[info] PreloadDiag event=load-phase seq=1 tMs=10 load=1 loading=true
[info] PreloadDiag event=menu-geometry-grid seq=2 tMs=11 load=1 loading=true source=native-menu geometryOnly=true world=0000003C lower=(-2, -2) upper=(2, 2) side=5 cells=25 currentCell=00000000 currentEditor=<null>
[info] PreloadDiag event=interior-buffer-add seq=3 tMs=11 load=1 loading=false source=engine-interior-buffer affected=010000AA affectedEditor=UsefulInterior slots=3 ring=[0]=010000AA:UsefulInterior:loaded:ld
[info] PreloadDiag event=interior-buffer-remove seq=4 tMs=11 load=1 loading=false source=engine-interior-buffer affected=010000AA affectedEditor=UsefulInterior slots=3 ring=
[info] PreloadDiag event=native-linked-candidate seq=5 tMs=12 load=1 loading=false source=native-linked nativeAttemptId=101 originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-interior candidateValid=true dest=010000AA editor=UsefulInterior interior=true world=00000000 selectionCountsValid=true selectedBefore=1 selectionLimit=3
[info] PreloadDiag event=preload-interior seq=6 tMs=13 load=1 loading=false source=native-linked nativeAttemptId=101 originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-interior cellValid=true cell=010000AA editor=UsefulInterior interior=true
[info] PreloadDiag event=native-linked-world-candidate seq=7 tMs=13 load=1 loading=false source=native-linked nativeAttemptId=102 stage=before-call observationOnly=true candidateDecoded=false originObservation=true originCell=010000AA originEditor=UsefulInterior originInterior=true originWorld=00000000 originWorldEditor=<none> direction=interior-to-exterior
[info] PreloadDiag event=preload-world seq=8 tMs=14 load=1 loading=false source=native-linked world=0000003C worldEditor=Commonwealth center=(10, 11) branch=full-arrival-grid fullGrid=true gridSide=5 gridCells=25 nativeAttemptId=102 originObservation=true originCell=010000AA originEditor=UsefulInterior originInterior=true originWorld=00000000 originWorldEditor=<none> direction=interior-to-exterior
[info] PreloadDiag event=native-linked-world-candidate seq=9 tMs=14 load=1 loading=false source=native-linked nativeAttemptId=103 stage=before-call observationOnly=true candidateDecoded=false originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-exterior
[info] PreloadDiag event=native-linked-world-candidate seq=10 tMs=14 load=1 loading=false source=native-linked nativeAttemptId=104 stage=before-call observationOnly=true candidateDecoded=false originObservation=true originCell=0000BEEF originEditor=DiamondCityExterior originInterior=false originWorld=0000BBBB originWorldEditor=DiamondCityWorld direction=exterior-to-exterior
[info] PreloadDiag event=preload-world seq=11 tMs=15 load=1 loading=false source=native-linked world=0000003C worldEditor=Commonwealth center=(-4, 7) branch=queue-single-cell fullGrid=false gridSide=1 gridCells=1 nativeAttemptId=104 originObservation=true originCell=0000BEEF originEditor=DiamondCityExterior originInterior=false originWorld=0000BBBB originWorldEditor=DiamondCityWorld direction=exterior-to-exterior
[info] PreloadDiag event=native-linked-world-candidate seq=12 tMs=15 load=1 loading=false source=native-linked nativeAttemptId=106 stage=before-call observationOnly=true candidateDecoded=false originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-exterior
[info] PreloadDiag event=preload-world seq=13 tMs=15 load=1 loading=false source=native-linked world=0000003C worldEditor=Commonwealth center=(2, 2) branch=queue-single-cell fullGrid=false gridSide=1 gridCells=1 nativeAttemptId=106 originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-exterior
[info] PreloadDiag event=native-linked-candidate seq=14 tMs=15 load=1 loading=false source=native-linked nativeAttemptId=105 originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-interior candidateValid=true dest=010000AC editor=ExpectedInterior interior=true world=00000000 selectionCountsValid=false
[info] PreloadDiag event=preload-interior seq=15 tMs=15 load=1 loading=false source=native-linked nativeAttemptId=105 originObservation=true originCell=0000CAFE originEditor=CommonwealthExterior originInterior=false originWorld=0000003C originWorldEditor=Commonwealth direction=exterior-to-interior cellValid=true cell=010000AB editor=DifferentInterior interior=true
[info] PreloadDiag event=interior-buffer-add seq=16 tMs=15 load=1 loading=false source=engine-interior-buffer affected=010000AA affectedEditor=UsefulInterior slots=3 ring=[0]=010000AA:UsefulInterior:loaded:ld
[info] PreloadDiag event=preload-interior seq=17 tMs=15 load=1 loading=false source=plugin-door-prefetch cellValid=true cell=020000BB editor=UnusedInterior interior=true
[info] PreloadDiag event=crosshair-candidate seq=18 tMs=16 load=1 loading=false source=plugin-door-prefetch trigger=event-pick distance=100.000 result=3 resultName=fired-interior door=00001234 doorEditor=DoorA dest=020000BB destEditor=UnusedInterior interior=true
[info] PreloadDiag event=preload-world seq=19 tMs=17 load=1 loading=false source=plugin-door-prefetch world=0000003C worldEditor=Commonwealth center=(12, 13) boolFlag=true branch=queue-single-cell fullGrid=false gridSide=1 gridCells=1
[info] PreloadDiag event=crosshair-candidate seq=20 tMs=18 load=1 loading=false source=plugin-door-prefetch trigger=gate-proximity distance=2048.000 result=16 resultName=fired-exterior-engine door=00005678 doorEditor=DiamondCityGate dest=040000DD destEditor=CommonwealthArrival interior=false
[info] LoadingMenu CLOSE (load #1) — 2.50s
[info] LOADTIME: interior — 2.25s
[info] PreloadDiag event=player-cell seq=21 tMs=20 load=1 loading=false reason=loading-menu-close cellValid=true cell=010000AA editor=UsefulInterior interior=true world=00000000 grid=(0, 0)
[info] PreloadDiag event=player-cell seq=22 tMs=25 load=2 loading=false reason=loading-menu-close cellValid=true cell=040000DD editor=CommonwealthArrival interior=false world=0000003C grid=(12, 13)
[info] PreloadDiag event=player-cell seq=23 tMs=30 load=3 loading=false reason=loading-menu-close cellValid=true cell=030000CC editor=Elsewhere interior=true world=00000000 grid=(0, 0)
[info] PreloadDiag event=player-cell seq=24 tMs=31 load=4 loading=false reason=loading-menu-close cellValid=true cell=050000EE editor=CommonwealthGridTwo interior=false world=0000003C grid=(2, 2)
[info] PreloadDiag event=interior-buffer-remove seq=25 tMs=32 load=4 loading=false source=engine-interior-buffer affected=010000AA affectedEditor=UsefulInterior slots=3 ring=
[info] PreloadDiag configured=false
[info] PreloadDiag native-setting when=install bPreloadLinkedAreas=false
[info] PreloadDiag hooks not installed (diagnostics disabled); native settings above were read only
[info] LoadingMenu CLOSE (load #1) - 3.00s
"""
    parsed = parse_text("<self-test>", fixture)
    report = summarize(parsed, ["<self-test>"])
    assert report["session_count"] == 2
    first = report["sessions"][0]
    second = report["sessions"][1]
    assert first["diagnostics"]["hook_installed"] is True
    assert first["menu_geometry"]["event_count"] == 1
    assert first["native_linked"]["requested"]["event_count"] == 2
    assert first["native_linked"]["requested"]["selection_limit_counts"] == {
        "3": 1
    }
    correlation = first["native_linked"]["attempt_correlation"]
    assert correlation["world_candidate_event_count"] == 4
    assert correlation["confirmed_candidate_event_count"] == 4
    assert correlation["unconfirmed_candidate_event_count"] == 2
    assert (
        correlation["by_direction"]["exterior-to-interior"][
            "confirmed_candidate_events"
        ]
        == 1
    )
    assert (
        correlation["by_direction"]["interior-to-exterior"][
            "confirmed_candidate_events"
        ]
        == 1
    )
    assert (
        correlation["by_direction"]["exterior-to-exterior"][
            "confirmed_candidate_events"
        ]
        == 2
    )
    assert (
        correlation["by_direction"]["exterior-to-exterior"][
            "unconfirmed_candidate_events"
        ]
        == 1
    )
    mismatch = next(
        item
        for item in correlation["unconfirmed"]
        if item["attempt_id"] == 105
    )
    assert mismatch["reason"] == "destination-mismatch"
    assert mismatch["candidate_destination_cell"] == "010000AC"
    assert mismatch["observed_call_destinations"] == ["010000AB"]
    assert first["native_linked"]["preload_calls"]["world_event_count"] == 3
    assert len(first["native_linked"]["preloaded"]["unique_cells"]) == 2
    assert (
        first["native_linked"]["preloaded_usefulness"]["hit_ratio_among_evaluable"]
        == 0.5
    )
    assert (
        first["native_linked"]["buffer_added_for_requested_cells"][
            "event_count"
        ]
        == 1
    )
    assert (
        first["native_linked"]["buffer_removed_for_requested_cells"][
            "event_count"
        ]
        == 1
    )
    assert (
        first["native_linked"]["all_interior_buffer_activity"][
            "add_event_count"
        ]
        == 2
    )
    assert (
        first["native_linked"]["all_interior_buffer_activity"][
            "remove_event_count"
        ]
        == 2
    )
    world_relation = first["native_linked"]["world_relation"]
    assert world_relation["relation_counts"] == {
        "same-world": 1,
        "cross-world": 1,
        "unknown": 1,
    }
    assert world_relation["confirmed_relation_counts"] == {
        "same-world": 1,
        "cross-world": 1,
        "unknown": 1,
    }
    world_usefulness = first["native_linked"]["world_grid_usefulness"]
    assert world_usefulness["confirmed_call_events"] == 3
    assert world_usefulness["evaluable_calls"] == 3
    assert world_usefulness["hit_calls"] == 2
    assert world_usefulness["wasted_within_recorded_window"] == 1
    assert world_usefulness["indeterminate_calls"] == 0
    assert world_usefulness["hit_ratio_among_evaluable"] == 0.6667
    assert world_usefulness["calls"][0]["geometry"]["lower"] == [8, 9]
    assert world_usefulness["calls"][0]["geometry"]["upper"] == [12, 13]
    assert world_usefulness["calls"][0]["status"] == "hit"
    assert world_usefulness["calls"][1]["geometry"]["lower"] == [-4, 7]
    assert world_usefulness["calls"][1]["geometry"]["upper"] == [-4, 7]
    assert world_usefulness["calls"][1]["status"] == (
        "waste-within-recorded-window"
    )
    assert world_usefulness["calls"][2]["status"] == "hit"
    assert first["crosshair"]["fire_event_count"] == 2
    assert first["crosshair"]["source_counts"] == {"plugin-door-prefetch": 2}
    assert first["crosshair"]["events"][0]["result_name"] == "fired-interior"
    assert (
        first["crosshair"]["events"][1]["result_name"]
        == "fired-exterior-engine"
    )
    assert (
        first["crosshair"]["fired_usefulness"]["wasted_within_recorded_window"]
        == 1
    )
    exterior = first["plugin_exterior_preload"]
    assert exterior["invocation_event_count"] == 1
    assert exterior["source_counts"] == {"plugin-door-prefetch": 1}
    assert exterior["single_cell_invocation_count"] == 1
    assert exterior["native_grid_invocation_count"] == 0
    assert exterior["events"][0]["center"] == [12, 13]
    assert exterior["world_grid_usefulness"]["hit_ratio_among_evaluable"] == 1.0
    assert len(first["destination_cells_later_observed_as_player"]) == 2
    assert report["aggregate"]["loading_menu_close"]["count"] == 2
    assert report["aggregate"]["native_world_relation"][
        "relation_counts"
    ] == {
        "same-world": 1,
        "cross-world": 1,
        "unknown": 1,
    }
    assert report["aggregate"]["native_world_grid_usefulness"][
        "evaluable_calls"
    ] == 3
    assert report["aggregate"]["native_world_grid_usefulness"][
        "hit_calls"
    ] == 2
    assert report["aggregate"]["native_world_grid_usefulness"][
        "wasted_within_recorded_window"
    ] == 1
    assert report["aggregate"]["plugin_preload_world_invocation_events"] == 1
    assert report["aggregate"]["plugin_preload_world_single_cell_invocations"] == 1
    assert second["diagnostics"]["configured"] is False
    assert second["warnings"]
    assert any(
        "do not prove acceptance" in warning
        for warning in first["warnings"]
    )
    rendered = io.StringIO()
    render_human(report, rendered)
    assert "1 single-cell / 0 native-grid / 0 unknown" in rendered.getvalue()
    assert "3 evaluable, 2 hit, 1 waste" in rendered.getvalue()
    assert "same-world=1/1, cross-world=1/1, unknown=1/1" in rendered.getvalue()
    assert "cold queued" not in rendered.getvalue()

    gate_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=gate-enum-source scan=40 backend=native-loaded-reference-collection resolved=true collectionPtr=0x1111 sourceCellPtr=0x2222 sourceCell=00018AA2 sourceEditor=CommonwealthPersistent sourceState=4 loadedData=0x8888 sourceWorld=0000003C sourceWorldEditor=Commonwealth origin=(-14500.0,-28800.0,1400.0) radius=4096.0
[info] PreloadDiag event=gate-enum-candidate scan=40 backend=native-loaded-reference-collection door=0012F822 doorEditor=DiamondCityExtEntranceLoadDoorREF sourceCell=00018AA2 sourceCellEditor=CommonwealthPersistent sourceWorld=0000003C sourceWorldEditor=Commonwealth distance=400.0 transitionCell=00000FEF transitionEditor=DiamondCityPersistent transitionGrid=(0, 0) linkedDoor=000A8F9C linkedDoorEditor=DiamondCityIntLoadDoorREF linkedCell=00000FEF linkedCellEditor=DiamondCityPersistent linkedCellSource=save-parent linkedWorld=00000F94 linkedWorldEditor=DiamondCity xtelPos=(-100.0,-200.0,20.0) linkedPos=(-90.0,-190.0,20.0) computedGrid=(-1, -1) gridValid=true centerSource=xtel-position decision=accepted-cross-world-exterior
[info] PreloadDiag event=gate-enum-summary scan=40 backend=native-loaded-reference-collection resolved=true collectionPtr=0x1111 callbacks=12 nullReference=0 disabledOrDeleted=0 missingTeleportData=0 sourceWorldMissing=0 sourceWorldMismatch=0 noDestination=0 interiorDestination=11 noDestinationWorld=0 sameWorld=0 accepted=1 duplicate=0 capacityDropped=0 candidateLogs=1 candidateLogsDropped=0 cached=1
[info] PreloadDiag event=exterior-preload-decision source=plugin-door-prefetch trigger=gate-proximity decision=engine-call-returned door=0012F822 linkedDoor=000A8F9C sourceCell=00018AA2 sourceWorld=0000003C destCell=00000FEF destWorld=00000F94 center=(-1, -1) mode=single-arrival-cell destinationKey=(00000F94,-1,-1,false) cooldownMs=8000 engineOwnsResidentPendingDedup=true completionClaim=false
[info] PreloadDiag event=crosshair-candidate seq=1 tMs=100 load=0 loading=false source=plugin-door-prefetch trigger=gate-proximity distance=400.000 result=16 resultName=exterior-submitted door=0012F822 dest=00000FEF destEditor=DiamondCityPersistent interior=false world=00000F94 worldEditor=DiamondCity grid=(0, 0)
[info] PreloadDiag event=exterior-preload-decision source=plugin-door-prefetch trigger=gate-proximity decision=recent-plugin-request door=0012F822 linkedDoor=000A8F9C sourceCell=00018AA2 sourceWorld=0000003C destCell=00000FEF destWorld=00000F94 center=(-1, -1) mode=single-arrival-cell destinationKey=(00000F94,-1,-1,false) cooldownMs=8000 engineOwnsResidentPendingDedup=true completionClaim=false
[info] PreloadDiag event=crosshair-candidate seq=2 tMs=200 load=0 loading=false source=plugin-door-prefetch trigger=gate-proximity distance=400.000 result=9 resultName=exterior-cooldown door=0012F822 dest=00000FEF destEditor=DiamondCityPersistent interior=false world=00000F94 worldEditor=DiamondCity grid=(0, 0)
[info] PreloadDiag event=player-cell seq=3 tMs=300 load=1 loading=false reason=loading-menu-close cellValid=true cell=00000FEB editor=DiamondCityExterior interior=false world=00000F94 grid=(-1, -1)
[info] PreloadDiag event=gate-enum-source scan=41 backend=native-loaded-reference-collection resolved=true collectionPtr=0x1234 sourceCellPtr=0x5678 sourceCell=00000FEB sourceEditor=DiamondCityExterior sourceState=4 loadedData=0x9999 sourceWorld=00000F94 sourceWorldEditor=DiamondCity origin=(100.0,200.0,300.0) radius=4096.0
[info] PreloadDiag event=gate-enum-candidate scan=41 backend=native-loaded-reference-collection door=000A8F9C doorEditor=DiamondCityIntLoadDoorREF sourceCell=00000FEF sourceCellEditor=DiamondCityPersistent sourceWorld=00000F94 sourceWorldEditor=DiamondCity distance=512.0 transitionCell=00018AA2 transitionEditor=CommonwealthPersistent transitionGrid=(0, 0) linkedDoor=0012F822 linkedDoorEditor=DiamondCityExtEntranceLoadDoorREF linkedCell=00018AA2 linkedCellEditor=CommonwealthPersistent linkedCellSource=save-parent linkedWorld=0000003C linkedWorldEditor=Commonwealth xtelPos=(-14466.8,-28763.5,1471.0) linkedPos=(-14317.1,-28398.7,1650.1) computedGrid=(-4, -8) gridValid=true centerSource=xtel-position decision=accepted-cross-world-exterior
[info] PreloadDiag event=gate-enum-candidate scan=41 backend=native-loaded-reference-collection door=000C4486 doorEditor=DiamondCityElevator sourceCell=00000FEF sourceCellEditor=DiamondCityPersistent sourceWorld=00000F94 sourceWorldEditor=DiamondCity distance=9000.0 transitionCell=00000FEF transitionEditor=DiamondCityPersistent transitionGrid=(0, 0) linkedDoor=000C447C linkedDoorEditor=ElevatorTarget linkedCell=00000FEF linkedCellEditor=DiamondCityPersistent linkedCellSource=live-parent linkedWorld=00000F94 linkedWorldEditor=DiamondCity xtelPos=(0.0,0.0,0.0) linkedPos=(0.0,0.0,0.0) computedGrid=(0, 0) gridValid=true centerSource=xtel-position decision=same-worldspace
[info] PreloadDiag event=gate-enum-summary scan=41 backend=native-loaded-reference-collection resolved=true collectionPtr=0x1234 callbacks=47 nullReference=0 disabledOrDeleted=0 missingTeleportData=0 sourceWorldMissing=0 sourceWorldMismatch=0 noDestination=0 interiorDestination=45 noDestinationWorld=0 sameWorld=1 accepted=1 duplicate=0 capacityDropped=0 candidateLogs=2 candidateLogsDropped=0 cached=1
[info] PreloadDiag event=exterior-preload-decision source=plugin-door-prefetch trigger=gate-proximity decision=engine-call-returned door=000A8F9C linkedDoor=0012F822 sourceCell=00000FEF sourceWorld=00000F94 destCell=00018AA2 destWorld=0000003C center=(-4, -8) mode=single-arrival-cell destinationKey=(0000003C,-4,-8,false) cooldownMs=8000 engineOwnsResidentPendingDedup=true completionClaim=false
[info] PreloadDiag event=crosshair-candidate seq=4 tMs=400 load=1 loading=false source=plugin-door-prefetch trigger=gate-proximity distance=512.000 result=16 resultName=exterior-submitted door=000A8F9C dest=00018AA2 destEditor=CommonwealthPersistent interior=false world=0000003C worldEditor=Commonwealth grid=(0, 0)
[info] PreloadDiag event=exterior-preload-decision source=plugin-door-prefetch trigger=gate-proximity decision=recent-plugin-request door=000A8F9C linkedDoor=0012F822 sourceCell=00000FEF sourceWorld=00000F94 destCell=00018AA2 destWorld=0000003C center=(-4, -8) mode=single-arrival-cell destinationKey=(0000003C,-4,-8,false) cooldownMs=8000 engineOwnsResidentPendingDedup=true completionClaim=false
[info] PreloadDiag event=crosshair-candidate seq=5 tMs=500 load=1 loading=false source=plugin-door-prefetch trigger=gate-proximity distance=512.000 result=9 resultName=exterior-cooldown door=000A8F9C dest=00018AA2 destEditor=CommonwealthPersistent interior=false world=0000003C worldEditor=Commonwealth grid=(0, 0)
[info] PreloadDiag event=player-cell seq=6 tMs=600 load=2 loading=false reason=loading-menu-close cellValid=true cell=00018AA3 editor=DiamondCityGateExterior interior=false world=0000003C grid=(-4, -8)
[info] PreloadDiag event=gate-scan-source scan=42 scope=worldspace-persistent-cell stable=false sourceCellPtr=0x1111 sourceCell=00000FEF sourceEditor=DiamondCityPersistent sourceInterior=false sourceState=2 loadedData=0x0 cellWorld=00000F94 cellWorldEditor=DiamondCity expectedWorld=00000F94 expectedWorldEditor=DiamondCity
[info] PreloadDiag event=gate-scan-source scan=43 scope=player-current-cell stable=true sourceCellPtr=0x2222 sourceCell=00000FEB sourceEditor=DiamondCityExterior sourceInterior=false sourceState=4 loadedData=0x3333 cellWorld=00000F94 cellWorldEditor=DiamondCity expectedWorld=00000F94 expectedWorldEditor=DiamondCity
[info] PreloadDiag event=gate-scan-candidate scan=43 scope=player-current-cell sourceCell=00000FEB sourceWorld=00000F94 door=000C4486 doorEditor=DiamondCityElevator pos=(1.0,2.0,3.0) transitionCell=00000FEF transitionEditor=DiamondCityPersistent linkedDoor=000C447C linkedDoorEditor=ElevatorTarget linkedCell=00000FEF linkedCellEditor=DiamondCityPersistent chosenBy=transition-cell destination=00000FEF destinationEditor=DiamondCityPersistent destinationInterior=false destinationWorld=00000F94 destinationWorldEditor=DiamondCity decision=same-worldspace
[info] PreloadDiag event=gate-scan-summary scan=43 scope=player-current-cell references=24 teleportDoors=12 disabledOrDeleted=0 noDestination=0 interiorDestination=11 noDestinationWorld=0 sameWorld=1 accepted=0
"""
    gate_report = summarize(
        parse_text("<gate-self-test>", gate_fixture),
        ["<gate-self-test>"],
    )
    gate_session = gate_report["sessions"][0]
    gate = gate_session["gate_diagnostics"]
    gate_enum = gate["operational_enumeration"]
    assert gate_enum["source_event_count"] == 2
    assert gate_enum["resolved_source_event_count"] == 2
    assert gate_enum["candidate_event_count"] == 3
    assert gate_enum["accepted_candidate_event_count"] == 2
    assert gate_enum["rejected_candidate_event_count"] == 1
    assert gate_enum["decision_counts"] == {
        "accepted-cross-world-exterior": 2,
        "same-worldspace": 1,
    }
    assert gate_enum["summary_totals"]["callbacks"] == 59
    assert gate_enum["summary_totals"]["interior_destination"] == 56
    assert gate_enum["summary_totals"]["accepted"] == 2
    assert gate_enum["summary_totals"]["cached"] == 2
    legacy_gate = gate["legacy_cell_scan"]
    assert legacy_gate["source_event_count"] == 2
    assert legacy_gate["stable_source_event_count"] == 1
    assert legacy_gate["unstable_source_event_count"] == 1
    assert legacy_gate["candidate_event_count"] == 1
    assert legacy_gate["summary_totals"]["references"] == 24
    assert legacy_gate["summary_totals"]["teleport_doors"] == 12
    diamond = gate["diamond_city_exit"]
    assert diamond["status"] == "accepted"
    assert diamond["operational_candidate_event_count"] == 1
    assert diamond["legacy_candidate_event_count"] == 0
    assert diamond["linked_door_counts"] == {"0012F822": 1}
    assert diamond["linked_door_match_count"] == 1
    assert diamond["linked_cell_source_counts"] == {"save-parent": 1}
    assert diamond["save_parent_resolution_count"] == 1
    assert diamond["destination_world_counts"] == {"0000003C": 1}
    assert diamond["expected_destination_world_match_count"] == 1
    assert diamond["computed_grids"] == [{"grid": [-4, -8], "event_count": 1}]
    assert diamond["expected_grid_match_count"] == 1
    assert diamond["fully_matching_accepted_event_count"] == 1
    assert diamond["decision_counts"] == {
        "accepted-cross-world-exterior": 1
    }
    assert diamond["preload_decisions"]["event_count"] == 2
    assert diamond["preload_decisions"]["exact_route_event_count"] == 2
    assert diamond["preload_decisions"]["decision_counts"] == {
        "engine-call-returned": 1,
        "recent-plugin-request": 1,
    }
    assert diamond["preload_decisions"]["mode_counts"] == {
        "single-arrival-cell": 2
    }
    assert diamond["gate_proximity_crosshair"]["result_16_count"] == 1
    assert diamond["gate_proximity_crosshair"]["result_9_count"] == 1
    assert diamond["decision_crosshair_correlation"][
        "preload_then_cooldown_sequence_observed"
    ] is True
    assert diamond["destination_visit"]["status"] == "observed"
    assert diamond["destination_visit"]["hit_event_count"] == 1
    assert diamond["events"][0]["transition_grid"] == [0, 0]
    assert diamond["events"][0]["xtel_position"] == [
        -14466.8,
        -28763.5,
        1471.0,
    ]
    entry = gate["diamond_city_routes"]["entry"]
    assert entry["status"] == "accepted"
    assert entry["door"] == "0012F822"
    assert entry["expected_linked_door"] == "000A8F9C"
    assert entry["linked_door_match_count"] == 1
    assert entry["linked_cell_source_counts"] == {"save-parent": 1}
    assert entry["save_parent_resolution_count"] == 1
    assert entry["destination_world_counts"] == {"00000F94": 1}
    assert entry["expected_destination_world_match_count"] == 1
    assert entry["computed_grids"] == [
        {"grid": [-1, -1], "event_count": 1}
    ]
    assert entry["expected_grid_match_count"] == 1
    assert entry["fully_matching_accepted_event_count"] == 1
    assert entry["preload_decisions"]["decision_counts"] == {
        "engine-call-returned": 1,
        "recent-plugin-request": 1,
    }
    assert entry["gate_proximity_crosshair"]["result_16_count"] == 1
    assert entry["gate_proximity_crosshair"]["result_9_count"] == 1
    assert entry["decision_crosshair_correlation"][
        "preload_then_cooldown_sequence_observed"
    ] is True
    assert entry["destination_visit"]["status"] == "observed"
    assert entry["destination_visit"]["hit_event_count"] == 1
    assert gate_session["plugin_exterior_preload"][
        "decision_event_count"
    ] == 4
    assert gate_session["plugin_exterior_preload"]["decision_counts"] == {
        "engine-call-returned": 2,
        "recent-plugin-request": 2,
    }
    assert gate_report["aggregate"]["gate_enum_candidate_events"] == 3
    assert gate_report["aggregate"]["gate_enum_accepted_candidate_events"] == 2
    assert gate_report["aggregate"]["gate_enum_rejected_candidate_events"] == 1
    assert gate_report["aggregate"]["diamond_gate_candidate_events"] == 1
    assert gate_report["aggregate"]["diamond_gate_accepted_events"] == 1
    assert gate_report["aggregate"]["diamond_city_routes"]["entry"][
        "fully_matching_accepted_events"
    ] == 1
    assert gate_report["aggregate"]["diamond_city_routes"]["exit"][
        "fully_matching_accepted_events"
    ] == 1
    assert gate_report["aggregate"][
        "plugin_exterior_preload_decision_events"
    ] == 4
    gate_rendered = io.StringIO()
    render_human(gate_report, gate_rendered)
    assert (
        "Diamond gate 000A8F9C -> expected linked 0012F822: "
        "status=accepted"
        in gate_rendered.getvalue()
    )
    assert "computedGrid(s)=(-4, -8)=1" in gate_rendered.getvalue()
    assert (
        "Diamond City entry (0012F822 -> 000A8F9C): enum=accepted"
        in gate_rendered.getvalue()
    )
    assert (
        "Diamond City exit (000A8F9C -> 0012F822): enum=accepted"
        in gate_rendered.getvalue()
    )
    assert (
        gate_rendered.getvalue().count(
            "decision/result 16->9 cycle=observed"
        )
        == 2
    )
    assert gate_rendered.getvalue().count(
        "destination visit=observed"
    ) == 2
    assert "1 stable, 1 unstable" in gate_rendered.getvalue()

    rejected_gate_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=gate-enum-candidate scan=51 backend=native-loaded-reference-collection door=000A8F9C doorEditor=DiamondCityIntLoadDoorREF sourceCell=00000FEF sourceWorld=00000F94 transitionCell=00018AA2 transitionGrid=(0, 0) linkedDoor=0012F822 linkedCell=00018AA2 linkedWorld=0000003C computedGrid=(0, 0) gridValid=true centerSource=xtel-position decision=same-worldspace
[info] PreloadDiag event=gate-enum-summary scan=51 backend=native-loaded-reference-collection resolved=true callbacks=1 nullReference=0 disabledOrDeleted=0 missingTeleportData=0 sourceWorldMissing=0 sourceWorldMismatch=0 noDestination=0 interiorDestination=0 noDestinationWorld=0 sameWorld=1 accepted=0 duplicate=0 capacityDropped=0 candidateLogs=1 candidateLogsDropped=0 cached=0
"""
    rejected_gate = summarize(
        parse_text("<rejected-gate-self-test>", rejected_gate_fixture),
        ["<rejected-gate-self-test>"],
    )["sessions"][0]["gate_diagnostics"]["diamond_city_exit"]
    assert rejected_gate["status"] == "rejected"
    assert rejected_gate["accepted_event_count"] == 0
    assert rejected_gate["rejected_event_count"] == 1
    assert rejected_gate["linked_door_match_count"] == 1
    assert rejected_gate["computed_grid_mismatch_count"] == 1
    assert rejected_gate["expected_grid_match_count"] == 0

    legacy_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=crosshair-candidate seq=1 tMs=1 load=0 loading=false source=plugin-crosshair trigger=event-pick distance=64.000 result=10 door=00001234 dest=050000EE destEditor=LegacyExterior interior=false
[info] PreloadDiag event=plugin-exterior-grid seq=2 tMs=2 load=0 loading=false source=plugin-crosshair trigger=event-pick distance=64.000 door=00001234 sourceCell=0000AAAA sourceInterior=false sourceWorld=0000BBBB dest=050000EE destEditor=LegacyExterior destWorld=0000003C center=(7, 8) radius=1 side=3 requested=9 queued=4 residentSkipped=5 queuedCoordinates=[(7,8)]
"""
    legacy_report = summarize(
        parse_text("<legacy-self-test>", legacy_fixture),
        ["<legacy-self-test>"],
    )
    legacy_event = legacy_report["sessions"][0]["crosshair"]["events"][0]
    assert legacy_event["source"] == "plugin-crosshair"
    assert legacy_event["result_name"] == "fired-exterior"
    legacy_exterior = legacy_report["sessions"][0][
        "legacy_plugin_exterior_grid"
    ]
    assert legacy_exterior["event_count"] == 1
    assert legacy_exterior["submission_attempts_total"] == 4
    assert legacy_exterior["resident_or_busy_skipped_total"] == 5
    assert legacy_exterior["plugin_duplicate_skipped_total"] == 0
    assert legacy_exterior["legacy_field_event_count"] == 1

    world_only_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=native-linked-world-candidate seq=1 tMs=1 load=0 loading=false source=native-linked nativeAttemptId=201 stage=before-call observationOnly=true candidateDecoded=false originObservation=true originCell=0000BEEF originEditor=OtherExterior originInterior=false originWorld=0000BBBB originWorldEditor=OtherWorld direction=exterior-to-exterior
[info] PreloadDiag event=preload-world seq=2 tMs=2 load=0 loading=false source=native-linked world=0000003C worldEditor=Commonwealth center=(0, 0) branch=full-arrival-grid gridCells=9 nativeAttemptId=201 originObservation=true originCell=0000BEEF originEditor=OtherExterior originInterior=false originWorld=0000BBBB originWorldEditor=OtherWorld direction=exterior-to-exterior
[info] PreloadDiag event=crosshair-candidate seq=3 tMs=3 load=0 loading=false source=plugin-door-prefetch trigger=event-pick result=3 resultName=fired-interior dest=07000001 destEditor=PluginInterior interior=true
[info] PreloadDiag event=player-cell seq=4 tMs=4 load=1 loading=false reason=loading-menu-close cellValid=true cell=07000002 editor=GridEdge interior=false world=0000003C grid=(1, 1)
"""
    world_only_report = summarize(
        parse_text("<world-only-self-test>", world_only_fixture),
        ["<world-only-self-test>"],
    )
    world_only_session = world_only_report["sessions"][0]
    assert world_only_session["native_linked"]["world_grid_usefulness"][
        "hit_calls"
    ] == 1
    assert world_only_session["native_linked"]["world_grid_usefulness"][
        "calls"
    ][0]["geometry"]["lower"] == [-1, -1]
    assert any(
        "attribution as contaminated" in warning
        for warning in world_only_session["warnings"]
    )

    plugin_native_grid_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=preload-world seq=1 tMs=1 load=0 loading=false source=plugin-door-prefetch world=0000003C worldEditor=Commonwealth center=(0, 0) boolFlag=false branch=full-arrival-grid fullGrid=true gridSide=5 gridCells=25
[info] PreloadDiag event=player-cell seq=2 tMs=2 load=1 loading=false reason=loading-menu-close cellValid=true cell=07000002 editor=GridEdge interior=false world=0000003C grid=(2, 2)
"""
    plugin_native_grid_report = summarize(
        parse_text("<plugin-native-grid-self-test>", plugin_native_grid_fixture),
        ["<plugin-native-grid-self-test>"],
    )
    plugin_native_grid = plugin_native_grid_report["sessions"][0][
        "plugin_exterior_preload"
    ]
    assert plugin_native_grid["single_cell_invocation_count"] == 0
    assert plugin_native_grid["native_grid_invocation_count"] == 1
    assert plugin_native_grid["world_grid_usefulness"]["hit_calls"] == 1
    assert plugin_native_grid["world_grid_usefulness"]["calls"][0][
        "geometry"
    ]["upper"] == [2, 2]

    safety_fixture = """\
[info] PreloadDiag configured=true
[info] PreloadDiag installed seven mutation-free observers for Fallout4 1.10.163
[info] PreloadDiag event=crosshair-candidate seq=1 result=12 trigger=gate-proximity dest=06000001
[info] PreloadDiag event=crosshair-candidate seq=2 result=13 trigger=gate-proximity dest=06000002
[info] PreloadDiag event=crosshair-candidate seq=3 result=14 trigger=gate-proximity dest=06000003
[info] PreloadDiag event=crosshair-candidate seq=4 result=15 trigger=gate-proximity dest=06000004
[info] PreloadDiag event=crosshair-candidate seq=5 result=16 trigger=gate-proximity dest=06000005
[info] PreloadDiag event=crosshair-candidate seq=6 result=17 trigger=gate-proximity dest=06000006
[info] PreloadDiag event=crosshair-candidate seq=7 result=18 trigger=gate-proximity dest=06000007
[info] PreloadDiag event=crosshair-candidate seq=8 result=19 trigger=gate-proximity dest=06000008
"""
    safety_report = summarize(
        parse_text("<safety-self-test>", safety_fixture),
        ["<safety-self-test>"],
    )
    safety_crosshair = safety_report["sessions"][0]["crosshair"]
    assert safety_crosshair["fire_event_count"] == 0
    assert [
        event["result_name"] for event in safety_crosshair["events"]
    ] == [
        "transition-active",
        "player-context-unavailable",
        "interior-origin-unsupported",
        "no-cold-work",
        "exterior-submitted",
        "source-worldspace-missing",
        "post-transition-quiet",
        "exterior-disabled",
    ]


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read LoadingScreens.log and summarize load times, native/menu/"
            "plugin door-prefetch activity (including engine PreloadWorld "
            "invocations), interior-buffer changes, and whether requested "
            "FormID or world/grid destinations were later visited."
        ),
        epilog=(
            "With no LOG argument, the newest live Fallout4 or Fallout4VR "
            "LoadingScreens.log under Documents/My Games is used. Input files "
            "are never modified."
        ),
    )
    parser.add_argument(
        "logs",
        metavar="LOG",
        nargs="*",
        help=(
            "log file, or a directory whose LoadingScreens*.log files should be "
            "analyzed"
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON instead of the human report",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the built-in in-memory parser smoke test and exit",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if args.self_test:
        self_test()
        print("analyze_preload_log.py self-test: PASS")
        return 0

    if args.logs:
        paths = _expand_inputs(args.logs)
    else:
        default = _default_input()
        paths = [default] if default else []

    if not paths:
        print(
            "error: no log supplied and no default LoadingScreens.log was found",
            file=sys.stderr,
        )
        return 2
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        print(
            "error: input is not a readable file: " + ", ".join(missing),
            file=sys.stderr,
        )
        return 2

    try:
        parsed, labels = parse_files(paths)
    except OSError as exc:
        print("error: could not read log: {}".format(exc), file=sys.stderr)
        return 2
    report = summarize(parsed, labels)
    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        print()
    else:
        render_human(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
