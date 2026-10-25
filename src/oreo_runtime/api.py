"""Component-oriented entry point for Project Oreo Runtime."""

from __future__ import annotations

import os
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .controller import ControllerComponent
from .overlay import OverlayComponent
from .ui import UiError, UiHandle, encode_pad

_start_lock = threading.RLock()
_package_dir = Path(__file__).resolve().parent
_overlay = OverlayComponent(_package_dir)
_controller = ControllerComponent(_package_dir)
_overlay_requested = False
_controller_requested = False
_pad_linked = False
_ui_handles: dict[str, UiHandle] = {}


@dataclass(frozen=True)
class RuntimeSession:
    """References to the process-wide Runtime components."""

    overlay: OverlayComponent
    controller: ControllerComponent
    # Present only when the mod named a UI. None means the mod draws nothing, which is a
    # perfectly ordinary thing for a mod that only changes game logic to do.
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
    ui: str | None = None,
    on_overlay_connected: Callable[[], None] | None = None,
) -> RuntimeSession:
    """Start only the requested Runtime components.

    `ui` names the mod's interface: its RML documents bind to the data model of that name, and
    are read from `mod_dir`. Each mod gets a handle of its own:

        session = oreo_runtime.start(overlay=True, mod_dir=here, ui="fishing")
        session.ui.load("catch", "ui/catch.rml")
        session.ui.show("catch")
        session.ui.set("fishY", 0.42)
        session.ui.on("click", "save", lambda event: save())
        session.ui.flush()      # once a frame; handlers run here, on the mod's own thread

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

    handle = _ui_handle(ui, resolved_mod_dir) if ui is not None else None

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
                on_connected=on_overlay_connected,
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


def _ui_handle(name: str, mod_dir: str | None) -> UiHandle:
    """The mod's handle; the same one again for a repeated start() from the same folder."""
    if mod_dir is None:
        raise UiError("a UI needs mod_dir: its documents are read from the mod's folder")
    with _start_lock:
        handle = _ui_handles.get(name)
        if handle is not None:
            if handle.folder != Path(mod_dir).resolve():
                raise UiError(f"UI name {name!r} is already taken by the mod in {handle.folder}")
            return handle
        handle = UiHandle(name, mod_dir, _overlay.send)
        _ui_handles[name] = handle
    # What the player does comes back up the pipe; the device is only repeated on connect, so
    # a handle made later starts from what the overlay already said.
    _overlay.add_message_callback(handle.receive)
    if _overlay.input_device:
        handle.receive("INPUT " + _overlay.input_device)
    # Before the mod's own callback, so that one finds its documents already back. The overlay
    # comes up whenever it comes up, can drop and return, and closes a mod's documents when the
    # pipe drops; it keeps nothing of ours.
    _overlay.add_connect_callback(handle.resend)
    return handle


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
