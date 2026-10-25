"""A mod's interface: RML documents and RCSS styles in its own folder, drawn by the overlay.

RML is HTML and RCSS is CSS, as RmlUi reads them. The mod sends values by name and the
documents bind to them through the mod's data model, which has the mod's UI name:

    <body data-model="fishing">
        <div class="fish" data-style-top="fishY * 100 + '%'"></div>
        <span>{{ fishName }}</span>
        <button id="save">Save</button>
    </body>

    ui = session.ui
    ui.load("catch", "ui/catch.rml")
    ui.show("catch")
    ui.set("fishY", 0.42)
    ui.on("click", "save", lambda event: save())
    ui.flush()      # once a frame: sends what changed, runs the handlers

Several mods each have a handle of their own, and never see each other's documents or values.
"""

from __future__ import annotations

import json
import logging
import re
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

logger = logging.getLogger(__name__)

# The overlay greets with this; a mismatch is a broken install, not something to work around.
FORMAT_VERSION = 4

EVENTS = ("click", "change", "submit")
INPUT_DEVICES = ("gamepad", "mouse_keyboard")

# What RmlUi accepts as a data model or variable name; "oreo_" is the Runtime's own.
_NAME = re.compile(r"[A-Za-z][A-Za-z0-9_]{0,63}")
_DOC_ID = re.compile(r"[A-Za-z0-9_-]{1,64}")
_RESERVED = {"it", "it_index", "ev", "true", "false", "size", "literal"}


class UiError(ValueError):
    """The mod asked for something the UI cannot do. Raised where it asked."""


def check_name(name: str, what: str) -> str:
    if not isinstance(name, str) or not _NAME.fullmatch(name):
        raise UiError(f"{what} {name!r} must start with a letter and hold only letters, digits "
                      "and underscores, at most 64 of them")
    return name


def check_ui_name(name: str) -> str:
    check_name(name, "UI name")
    if name.startswith("oreo_"):
        raise UiError(f"UI name {name!r}: names starting with oreo_ belong to the Runtime")
    return name


def _check_doc(doc: str) -> str:
    if not isinstance(doc, str) or not _DOC_ID.fullmatch(doc):
        raise UiError(f"document id {doc!r} must be 1-64 letters, digits, '_' or '-'")
    return doc


def _check_value_name(name: str) -> str:
    check_name(name, "value name")
    if name.lower() in _RESERVED:
        raise UiError(f"value name {name!r} is a word RmlUi keeps for itself")
    return name


def _json(value: Any) -> str:
    # bool first: it is an int in Python, and True must not travel as 1.
    if isinstance(value, bool) or isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, (int, float)) and value == value and abs(value) != float("inf"):
        return json.dumps(float(value) if isinstance(value, float) else value)
    raise UiError(f"a UI value is a boolean, a finite number or a string, not {value!r}")


def encode_pad(buttons) -> str:
    return "PAD " + ",".join(sorted(buttons))


@dataclass(frozen=True)
class UiEvent:
    """What the player did: `kind` is click, change or submit, `element` the id it happened to.

    `value` is what a change left behind - a boolean for a checkbox, a number for a range, a
    string otherwise - or for a submit, the form's fields by name.
    """

    kind: str
    document: str
    element: str
    value: Any = None


class UiHandle:
    """One mod's documents and values. Safe to call from the mod's thread at any time."""

    def __init__(self, name: str, mod_dir: str | Path, send: Callable[[str], bool]):
        self._name = check_ui_name(name)
        self._dir = Path(mod_dir).resolve()
        self._send = send
        self._lock = threading.Lock()
        self._pending: dict[tuple, str] = {}
        self._documents: dict[str, Path] = {}
        self._shown: dict[str, str] = {}
        self._values: dict[str, Any] = {}
        self._handlers: dict[tuple[str, str], list[Callable[[UiEvent], None]]] = {}
        self._inbox: list[tuple[str, Any]] = []
        self._input: str | None = None
        self._input_handlers: list[Callable[[str], None]] = []

    @property
    def name(self) -> str:
        return self._name

    @property
    def folder(self) -> Path:
        return self._dir

    def _queue(self, key: tuple, line: str) -> None:
        with self._lock:
            self._pending[key] = line

    # ---- documents ----------------------------------------------------------------------

    def load(self, doc: str, path: str | Path) -> None:
        """Load an RML document from the mod's folder, or load it again after an edit."""
        _check_doc(doc)
        resolved = (self._dir / path).resolve()
        if not resolved.is_relative_to(self._dir):
            raise UiError(f"{path}: documents live inside the mod's folder, {self._dir}")
        if not resolved.is_file():
            raise UiError(f"{resolved}: no such document")
        if any(c in str(resolved) for c in "\r\n"):
            raise UiError(f"{resolved!r}: a line break in a path cannot cross the pipe")
        with self._lock:
            self._documents[doc] = resolved
        self._queue(("load", doc), f"LOAD {self._name} {doc} {resolved}")

    def show(self, doc: str, modal: bool = False) -> None:
        """Show a document. A modal one takes the pointer, the keys and the pad from the game."""
        self._show(doc, "modal" if modal else "show")

    def hide(self, doc: str) -> None:
        self._show(doc, "hide")

    def _show(self, doc: str, mode: str) -> None:
        with self._lock:
            if doc not in self._documents:
                raise UiError(f"document {doc!r} is not loaded")
            self._shown[doc] = mode
        self._queue(("show", doc), f"SHOW {self._name} {doc} {mode}")

    def close(self, doc: str) -> None:
        with self._lock:
            self._documents.pop(doc, None)
            self._shown.pop(doc, None)
            for key in (("load", doc), ("show", doc)):
                self._pending.pop(key, None)
        self._queue(("close", doc), f"CLOSE {self._name} {doc}")

    # ---- values -------------------------------------------------------------------------

    def set(self, name: str, value: bool | int | float | str) -> None:
        """A value the documents bind to. Only the last one set before a flush is sent."""
        _check_value_name(name)
        line = f"SET {self._name} {name} {_json(value)}"
        with self._lock:
            self._values[name] = value
            self._pending[("set", name)] = line

    def value(self, name: str) -> Any:
        """The last value set, or changed by the player through a data-value binding."""
        with self._lock:
            return self._values.get(name)

    # ---- what the player does -----------------------------------------------------------

    def on(self, kind: str, element: str, callback: Callable[[UiEvent], None]) -> None:
        """Call `callback` from flush() when the player does `kind` to the element `element`."""
        if kind not in EVENTS:
            raise UiError(f"event {kind!r} is not one of {', '.join(EVENTS)}")
        with self._lock:
            self._handlers.setdefault((kind, element), []).append(callback)

    @property
    def input(self) -> str | None:
        """"gamepad" or "mouse_keyboard": what the player touched last, or None so far."""
        with self._lock:
            return self._input

    def on_input(self, callback: Callable[[str], None]) -> None:
        """Called from flush() whenever the player switches between the pad and mouse."""
        with self._lock:
            self._input_handlers.append(callback)

    def receive(self, line: str) -> None:
        """One line from the overlay, on the pipe's reader thread. Kept for the next flush()."""
        parts = line.split(" ", 5)
        verb = parts[0]
        try:
            if verb == "EV" and len(parts) == 6 and parts[1] == self._name:
                _, _, doc, kind, element, value = parts
                item = ("event", UiEvent(kind, doc, element, json.loads(value)))
            elif verb == "VAR" and len(parts) >= 4 and parts[1] == self._name:
                name, value = parts[2], line.split(" ", 3)[3]
                item = ("var", (name, json.loads(value)))
            elif verb == "INPUT" and len(parts) == 2 and parts[1] in INPUT_DEVICES:
                item = ("input", parts[1])
            else:
                return
        except ValueError:
            logger.warning("UI %s: unreadable line from the overlay: %.80s", self._name, line)
            return
        with self._lock:
            self._inbox.append(item)

    def _dispatch(self) -> None:
        with self._lock:
            items, self._inbox = self._inbox, []
        for kind, payload in items:
            if kind == "var":
                with self._lock:
                    self._values[payload[0]] = payload[1]
                continue
            if kind == "input":
                with self._lock:
                    changed = payload != self._input
                    self._input = payload
                    handlers = list(self._input_handlers) if changed else []
                for handler in handlers:
                    self._run(handler, payload)
                continue
            with self._lock:
                handlers = list(self._handlers.get((payload.kind, payload.element), ()))
            for handler in handlers:
                self._run(handler, payload)

    @staticmethod
    def _run(handler: Callable, argument: Any) -> None:
        # flush() runs in the middle of the mod's frame; one bad handler must not end it.
        try:
            handler(argument)
        except Exception:
            logger.exception("UI: a handler failed")

    def flush(self) -> bool:
        """Send everything queued as one write, then run the handlers for what the player did."""
        with self._lock:
            batch = "\n".join(self._pending.values())
            self._pending.clear()
        sent = self._send(batch) if batch else True
        self._dispatch()
        return sent

    def resend(self) -> bool:
        """Everything again, after the overlay (re)connects: it keeps nothing of ours."""
        with self._lock:
            lines = [f"ROOT {self._name} {self._dir}"]
            lines += [f"LOAD {self._name} {doc} {path}" for doc, path in self._documents.items()]
            lines += [f"SHOW {self._name} {doc} {mode}" for doc, mode in self._shown.items()]
            lines += [f"SET {self._name} {name} {_json(value)}"
                      for name, value in self._values.items()]
            self._pending.clear()
        return self._send("\n".join(lines))
