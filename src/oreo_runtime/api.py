"""Component-oriented entry point for Project Oreo Runtime."""

from __future__ import annotations

import os
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .controller import ControllerComponent
from .overlay import OverlayComponent
from .ui import UiHandle, encode_pad, load as _load_ui

_start_lock = threading.RLock()
_package_dir = Path(__file__).resolve().parent
_overlay = OverlayComponent(_package_dir)
_controller = ControllerComponent(_package_dir)
_overlay_requested = False
_controller_requested = False
_pad_linked = False


@dataclass(frozen=True)
class RuntimeSession:
    """References to the process-wide Runtime components."""

    overlay: OverlayComponent
    controller: ControllerComponent
    # Present only when the mod handed start() a UI file. None means the mod draws nothing,
    # which is a perfectly ordinary thing for a mod that only changes game logic to do.
    ui: UiHandle | None = None


def start(
    *,
    overlay: bool = False,
    controller: bool = False,
    mod_dir: str | os.PathLike[str] | None = None,
    log_dir: str | os.PathLike[str] | None = None,
    log_mode: str = "normal",
    log_max_mb: int = 10,
    log_keep_previous: bool = True,
    log_trace_minutes: int = 10,
    ui: str | os.PathLike[str] | None = None,
    on_overlay_connected: Callable[[], None] | None = None,
) -> RuntimeSession:
    """Start only the requested Runtime components.

    `ui` is the mod's own interface description - a JSON file living next to the mod, which
    says what is drawn and how it looks. The mod then only ever supplies values by name:

        session = oreo_runtime.start(overlay=True, mod_dir=here, ui="fishing_ui.json")
        session.ui.set("fish", 0.42)
        session.ui.flush()

    A file with widgets is listened to the same way, by name:

        session.ui.on("click", "save", lambda event: save())
        session.ui.flush()      # also where handlers run, on the mod's own thread

    A relative path is resolved against `mod_dir`, so a mod names its own file without
    knowing where the player installed it.

    The four log_* arguments are the native components' logging settings. They are passed
    rather than read from a file because the native side used to guess at a filename -
    fishing_mod.ini, next to whatever FISHING_MOD_DIR pointed at - which quietly made a
    component shared by every mod belong to exactly one. Whoever starts the Runtime already
    knows where its logs go; saying how much of them to keep costs nothing extra.

    Repeated calls are safe. Components are singletons inside the current NMS process.
    """
    global _overlay_requested, _controller_requested

    if not overlay and not controller:
        return RuntimeSession(_overlay, _controller)

    resolved_mod_dir = str(Path(mod_dir).resolve()) if mod_dir is not None else None
    resolved_log_dir = str(Path(log_dir).resolve()) if log_dir is not None else None

    handle: UiHandle | None = None
    if ui is not None:
        ui_path = Path(ui)
        if not ui_path.is_absolute() and resolved_mod_dir is not None:
            ui_path = Path(resolved_mod_dir) / ui_path
        # Deliberately not caught: a broken UI file is a mistake in the mod, and a mod that
        # refuses to start saying which line is wrong costs the author a minute. One that
        # starts and draws nothing costs an evening.
        handle = UiHandle(_load_ui(ui_path), _overlay.send)
        # What the player does to a widget comes back up the pipe as EV lines, and which
        # device they are using as INPUT lines. The overlay only repeats the device on connect,
        # so a handle made after that starts from what it already said.
        _overlay.add_message_callback(handle.receive)
        if _overlay.input_device:
            handle.receive("INPUT " + _overlay.input_device)

    with _start_lock:
        _overlay_requested = _overlay_requested or overlay
        _controller_requested = _controller_requested or controller
        if overlay:
            _overlay.start(
                mod_dir=resolved_mod_dir,
                log_dir=resolved_log_dir,
                log_mode=log_mode,
                log_max_mb=log_max_mb,
                log_keep_previous=log_keep_previous,
                log_trace_minutes=log_trace_minutes,
                on_connected=_connected_callback(handle, on_overlay_connected),
            )
        if controller:
            _controller.start(
                mod_dir=resolved_mod_dir,
                log_dir=resolved_log_dir,
                log_mode=log_mode,
            )
        if _overlay_requested and _controller_requested:
            _link_pad()

    return RuntimeSession(_overlay, _controller, handle)


def _link_pad() -> None:
    """Hand the gamepad to the overlay, so widgets can be walked and pressed with it.

    The helper already reads Xbox and PlayStation pads alike; the overlay does not read the pad
    itself, it is told. Done here, by the Runtime, once both halves are running - no mod has to
    know it happens. The current state is repeated when the overlay (re)connects, so a button
    held across a reconnect is not stuck, or lost, on the other side.
    """
    global _pad_linked
    if _pad_linked:
        return
    _pad_linked = True

    def forward(buttons) -> None:
        _overlay.send(encode_pad(buttons))

    _controller.add_listener(forward)
    _overlay.add_connect_callback(lambda: forward(_controller.buttons))


def _connected_callback(handle, mod_callback):
    """Send the UI document the moment the overlay is reachable, and on every reconnect.

    Not at start(): the overlay is a DLL inside the game and its pipe comes up whenever it
    comes up, usually after this call has returned. It can also drop and come back. The
    overlay keeps the document in memory only, so anything that only sent it once would leave
    the player with a blank panel and nothing in any log to explain it.
    """
    if handle is None:
        return mod_callback

    def on_connected():
        handle.resend_document()
        if mod_callback is not None:
            mod_callback()

    return on_connected
