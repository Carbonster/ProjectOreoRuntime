"""External SDL controller component and named-pipe state reader."""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import subprocess
import threading
import time
from pathlib import Path
from typing import Callable

logger = logging.getLogger(__name__)

PIPE_NAME = r"\\.\pipe\nms_controller"
PIPE_ACCESS_INBOUND = 0x00000001
PIPE_TYPE_BYTE = 0x00000000
PIPE_READMODE_BYTE = 0x00000000
PIPE_WAIT = 0x00000000
ERROR_PIPE_CONNECTED = 535
INVALID_HANDLE_VALUE = wt.HANDLE(-1).value


class ControllerComponent:
    """Own the controller helper and normalized current button state."""

    def __init__(self, package_dir: Path):
        self._package_dir = package_dir
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._kernel32.CreateNamedPipeW.restype = wt.HANDLE
        self._kernel32.ConnectNamedPipe.argtypes = [wt.HANDLE, ctypes.c_void_p]
        self._kernel32.ReadFile.argtypes = [
            wt.HANDLE,
            ctypes.c_void_p,
            wt.DWORD,
            ctypes.POINTER(wt.DWORD),
            ctypes.c_void_p,
        ]
        self._kernel32.CloseHandle.argtypes = [wt.HANDLE]
        self._buttons: set[str] = set()
        self._buttons_lock = threading.RLock()
        self._listeners: list[Callable[[frozenset[str]], None]] = []
        self._pipe_started = False
        self._start_attempted = False
        self._started = False
        self._helper_connected = False
        self._error = ""

    @property
    def available(self) -> bool:
        # Explorer forwards the request and exits, so there is no child process to poll. This
        # only says the helper was asked to start; `helper_connected` is the live signal, true
        # while the helper holds the pipe.
        return self._started

    @property
    def helper_connected(self) -> bool:
        return self._helper_connected

    @property
    def error(self) -> str:
        return self._error

    @property
    def buttons(self) -> frozenset[str]:
        with self._buttons_lock:
            return frozenset(self._buttons)

    def is_pressed(self, name: str) -> bool:
        with self._buttons_lock:
            return name.lower() in self._buttons

    def add_listener(self, callback: Callable[[frozenset[str]], None]) -> None:
        """Call `callback(buttons)` whenever the set of held buttons changes.

        On the helper's pipe thread, and only on a change: the helper repeats itself a few times
        a second as a keepalive, and a listener has no use for hearing the same set again.
        """
        with self._buttons_lock:
            if callback not in self._listeners:
                self._listeners.append(callback)

    def _set_buttons(self, names: set[str]) -> None:
        with self._buttons_lock:
            if names == self._buttons:
                return
            self._buttons = set(names)
            held = frozenset(names)
            listeners = list(self._listeners)
        for listener in listeners:
            try:
                listener(held)
            except Exception:
                logger.exception("Controller listener failed.")

    def start(self, *, mod_dir: str | None, log_dir: str | None, log_mode: str) -> bool:
        """Start the controller helper outside the game's process tree.

        The settings arguments are accepted but not delivered. Explorer passes neither
        arguments nor environment to what it starts, so the helper falls back to its own
        defaults and logs beside its executable. Handing these over needs the return channel
        planned for v2 - see docs/TODO.md.
        """

        self._start_pipe_reader()
        if self.available:
            return True
        if self._start_attempted:
            return False
        self._start_attempted = True

        executable = self._package_dir / "bin" / "oreo_controller_support.exe"
        sdl = self._package_dir / "bin" / "SDL2.dll"
        if not executable.is_file() or not sdl.is_file():
            self._error = (
                "Controller component is incomplete: "
                f"exe={executable.is_file()} SDL2={sdl.is_file()}"
            )
            logger.error(self._error)
            return False

        try:
            # Explorer keeps the helper outside Steam's game process tree. Started directly it
            # is a child of NMS.exe, inherits Steam's hooks, and Steam Input answers "no
            # controllers" - SDL and XInput then both stay empty for its whole lifetime.
            # Explorer hands the request to the already running shell process, which starts the
            # helper as its own child instead. This is what the fishing mod did before the
            # Runtime was split out; the line was lost in the move.
            subprocess.Popen(
                ["explorer.exe", str(executable)],
                cwd=str(executable.parent),
                close_fds=True,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            self._started = True
            self._error = ""
            logger.info("Project Oreo controller support started.")
            return True
        except Exception as exc:
            self._error = f"Failed to start controller support: {exc}"
            logger.exception(self._error)
            return False

    def _start_pipe_reader(self) -> None:
        if self._pipe_started:
            return
        self._pipe_started = True
        threading.Thread(target=self._pipe_loop, name="OreoControllerPipe", daemon=True).start()

    def _pipe_loop(self) -> None:
        while True:
            pipe = self._kernel32.CreateNamedPipeW(
                PIPE_NAME,
                PIPE_ACCESS_INBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                None,
            )
            if pipe == INVALID_HANDLE_VALUE or not pipe:
                time.sleep(0.5)
                continue

            connected = self._kernel32.ConnectNamedPipe(pipe, None)
            if not connected and ctypes.get_last_error() != ERROR_PIPE_CONNECTED:
                self._kernel32.CloseHandle(pipe)
                time.sleep(0.2)
                continue

            self._helper_connected = True
            logger.info("Controller helper connected.")
            buffer = ctypes.create_string_buffer(512)
            accumulated = b""
            while True:
                read = wt.DWORD(0)
                if not self._kernel32.ReadFile(
                    pipe, buffer, 511, ctypes.byref(read), None
                ) or read.value == 0:
                    break
                accumulated += buffer.raw[: read.value]
                while b"\n" in accumulated:
                    line, accumulated = accumulated.split(b"\n", 1)
                    self._parse_line(line.decode("ascii", "replace"))

            self._set_buttons(set())
            self._helper_connected = False
            self._kernel32.CloseHandle(pipe)
            logger.info("Controller helper disconnected.")

    def _parse_line(self, line: str) -> None:
        line = line.strip()
        if line.startswith("BTN"):
            payload = line[3:].strip()
            names = {name for name in payload.split(",") if name} if payload else set()
            self._set_buttons(names)
        elif line:
            logger.warning("Unknown controller pipe message: %r", line[:80])
