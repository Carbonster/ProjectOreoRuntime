"""Vulkan overlay loading and named-pipe transport.

The pipe is bidirectional. It carries the interface out, and back: a greeting when the overlay
accepts a connection, an answer to the two messages a mod does not send every frame, and what
the player did to a widget.

The return path was laid before anything travelled on it, so that the first event would be new
content on a channel that already worked rather than a protocol that had to be opened up. Widget
events are that content.
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import logging
import os
import threading
import time
from pathlib import Path
from typing import Callable

logger = logging.getLogger(__name__)

PIPE_NAME = r"\\.\pipe\nms_overlay"
GENERIC_READ_WRITE = 0xC0000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = wt.HANDLE(-1).value
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0

# How long a single write may take before the overlay is treated as wedged. A healthy overlay
# drains its pipe every two milliseconds, so anything near this means it has stopped reading
# altogether - and the mod is inside the game process under the GIL, where waiting any longer
# is a frame the player watches go by. Dropping the connection instead costs one reconnect and
# one resent document.
WRITE_TIMEOUT_MS = 50


class _Overlapped(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_void_p),
        ("InternalHigh", ctypes.c_void_p),
        ("Offset", wt.DWORD),
        ("OffsetHigh", wt.DWORD),
        ("hEvent", wt.HANDLE),
    ]


class OverlayComponent:
    """Own the native overlay lifetime and reconnecting pipe client."""

    def __init__(self, package_dir: Path):
        self._package_dir = package_dir
        self._dll_handle = None
        self._load_attempted = False
        self._handle = None
        self._handle_lock = threading.RLock()
        self._callbacks: list[Callable[[], None]] = []
        self._message_callbacks: list[Callable[[str], None]] = []
        self._transport_started = False
        self._last_error = 0
        self._error = ""
        self._overlay_format: int | None = None
        self._input_device: str | None = None
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self._kernel32
        k.CreateFileW.restype = wt.HANDLE
        k.CreateFileW.argtypes = [
            wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE,
        ]
        k.WriteFile.argtypes = [
            wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p,
        ]
        k.WriteFile.restype = wt.BOOL
        k.ReadFile.argtypes = [
            wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p,
        ]
        k.ReadFile.restype = wt.BOOL
        k.CreateEventW.restype = wt.HANDLE
        k.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
        k.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
        k.WaitForSingleObject.restype = wt.DWORD
        k.GetOverlappedResult.argtypes = [
            wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(wt.DWORD), wt.BOOL,
        ]
        k.GetOverlappedResult.restype = wt.BOOL
        k.CancelIoEx.argtypes = [wt.HANDLE, ctypes.c_void_p]
        k.CancelIoEx.restype = wt.BOOL
        k.ResetEvent.argtypes = [wt.HANDLE]
        k.ResetEvent.restype = wt.BOOL
        k.CloseHandle.argtypes = [wt.HANDLE]
        # One event per direction. Writes are serialised by _handle_lock, so they can share
        # theirs; the reader blocks on its own and must not share anything.
        self._write_event = k.CreateEventW(None, True, False, None)
        self._read_event = k.CreateEventW(None, True, False, None)

    @property
    def available(self) -> bool:
        return self._dll_handle is not None

    @property
    def connected(self) -> bool:
        with self._handle_lock:
            return self._handle is not None

    @property
    def last_error(self) -> int:
        return self._last_error

    @property
    def error(self) -> str:
        return self._error

    @property
    def input_device(self) -> str | None:
        """The last INPUT line the overlay sent: "gamepad", "mouse_keyboard", or None yet."""
        return self._input_device

    @property
    def overlay_format(self) -> int | None:
        """The UI format version the overlay greeted us with, once it has connected."""
        return self._overlay_format

    def start(
        self,
        *,
        mod_dir: str | None,
        log_dir: str | None,
        log_mode: str,
        log_max_mb: int = 10,
        log_keep_previous: bool = True,
        log_trace_minutes: int = 10,
        on_connected: Callable[[], None] | None = None,
    ) -> bool:
        if on_connected is not None:
            self.add_connect_callback(on_connected)

        # Every setting the native side needs, handed over before the DLL is loaded. It used to
        # read most of these out of a fishing_mod.ini whose name was compiled into it, which
        # made a component shared by every mod belong to exactly one; see log.h.
        if mod_dir:
            os.environ["FISHING_MOD_DIR"] = mod_dir
        if log_dir:
            os.environ["FISHING_MOD_LOG_DIR"] = log_dir
        os.environ["FISHING_MOD_LOG_MODE"] = log_mode
        os.environ["FISHING_MOD_LOG_MAX_MB"] = str(int(log_max_mb))
        os.environ["FISHING_MOD_LOG_KEEP_PREVIOUS"] = "true" if log_keep_previous else "false"
        os.environ["FISHING_MOD_LOG_TRACE_MINUTES"] = str(int(log_trace_minutes))
        os.environ["OREO_RUNTIME_DIR"] = str(self._package_dir)

        self._start_transport()
        if self._dll_handle is not None:
            return True
        if self._load_attempted:
            return False

        self._load_attempted = True
        source = self._package_dir / "bin" / "oreo_overlay.dll"
        if not source.is_file():
            self._error = f"Native overlay is missing: {source}"
            logger.error(self._error)
            return False

        # Loaded straight out of the package. It used to be copied to
        # %TEMP%\oreo_overlay_run_<pid>_<ms>.dll first and loaded from there, which was wrong
        # three times over: the copy was never deleted, so every game launch left another one
        # behind; "writes a DLL into TEMP under a generated name and loads it" is what malware
        # does, and antivirus software reads it that way; and anything running as this user
        # could swap the file in the gap between the copy and the load.
        #
        # The reason for the copy was that a loaded DLL cannot be overwritten while the game is
        # running, so the Runtime could not be updated mid-session. That is no longer a problem:
        # Project Oreo Launcher updates the Runtime before the game starts.
        try:
            self._dll_handle = ctypes.WinDLL(str(source))
            self._error = ""
            logger.info("Project Oreo overlay loaded from %s", source)
            return True
        except Exception as exc:
            self._error = f"Failed to load Project Oreo overlay: {exc}"
            logger.exception(self._error)
            return False

    def add_connect_callback(self, callback: Callable[[], None]) -> None:
        with self._handle_lock:
            if callback not in self._callbacks:
                self._callbacks.append(callback)
            connected = self._handle is not None
        if connected:
            self._invoke_callback(callback)

    def add_message_callback(self, callback: Callable[[str], None]) -> None:
        """Called with every line the overlay sends back, on the pipe's reader thread.

        The Runtime handles the greeting and the errors itself. Widget events are the reason
        this exists: the UI handle listens here and keeps them for the mod's next flush().
        """
        with self._handle_lock:
            if callback not in self._message_callbacks:
                self._message_callbacks.append(callback)

    def send(self, message: str) -> bool:
        """Write one batch. Never blocks for longer than WRITE_TIMEOUT_MS."""
        data = (message + "\n").encode("utf-8")
        with self._handle_lock:
            handle = self._handle
            if handle is None:
                return False
            written = wt.DWORD(0)
            overlapped = _Overlapped()
            overlapped.hEvent = self._write_event
            self._kernel32.ResetEvent(self._write_event)
            buffer = ctypes.create_string_buffer(data)
            ok = self._kernel32.WriteFile(
                handle, buffer, len(data), ctypes.byref(written), ctypes.byref(overlapped)
            )
            if not ok:
                error = ctypes.get_last_error()
                if error != ERROR_IO_PENDING:
                    self._last_error = error
                    self._close_locked()
                    logger.warning("Overlay pipe write failed: winerror=%d", error)
                    return False
                waited = self._kernel32.WaitForSingleObject(self._write_event, WRITE_TIMEOUT_MS)
                if waited != WAIT_OBJECT_0:
                    # The overlay has stopped reading. Cancel rather than wait it out: this is
                    # a game frame, and a stalled overlay is worth less than a smooth one.
                    self._kernel32.CancelIoEx(handle, ctypes.byref(overlapped))
                    self._last_error = ctypes.get_last_error()
                    self._close_locked()
                    logger.warning(
                        "Overlay pipe write timed out after %d ms; dropping the connection.",
                        WRITE_TIMEOUT_MS)
                    return False
                ok = self._kernel32.GetOverlappedResult(
                    handle, ctypes.byref(overlapped), ctypes.byref(written), False)
            if ok and written.value == len(data):
                return True
            self._last_error = ctypes.get_last_error()
            self._close_locked()
        logger.warning(
            "Overlay pipe write incomplete: winerror=%d bytes=%d/%d",
            self._last_error, written.value, len(data))
        return False

    def _start_transport(self) -> None:
        if self._transport_started:
            return
        self._transport_started = True
        threading.Thread(target=self._connect_loop, name="OreoOverlayPipe", daemon=True).start()
        threading.Thread(target=self._read_loop, name="OreoOverlayPipeIn", daemon=True).start()

    def _connect_loop(self) -> None:
        failed_attempts = 0
        while True:
            with self._handle_lock:
                connected = self._handle is not None
            if connected:
                time.sleep(1.0)
                continue

            handle = self._kernel32.CreateFileW(
                PIPE_NAME, GENERIC_READ_WRITE, 0, None, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED, None
            )
            if handle == INVALID_HANDLE_VALUE or not handle:
                self._last_error = ctypes.get_last_error()
                failed_attempts += 1
                if failed_attempts == 10 or failed_attempts % 30 == 0:
                    logger.warning(
                        "Overlay pipe unavailable: attempts=%d winerror=%d",
                        failed_attempts,
                        self._last_error,
                    )
                time.sleep(1.0)
                continue

            with self._handle_lock:
                if self._handle is None:
                    self._handle = handle
                    callbacks = list(self._callbacks)
                else:
                    self._kernel32.CloseHandle(handle)
                    callbacks = []
            failed_attempts = 0
            self._last_error = 0
            logger.info("Connected to Project Oreo overlay pipe.")
            for callback in callbacks:
                self._invoke_callback(callback)

    def _read_loop(self) -> None:
        """Collect what the overlay says back, a line at a time.

        A byte stream, so a line can arrive split across two reads and two lines can arrive in
        one. The overlay had this bug in the other direction and it went unnoticed for months
        because the traffic was short; the same mistake is not worth making twice.
        """
        pending = b""
        buffer = ctypes.create_string_buffer(4096)
        while True:
            with self._handle_lock:
                handle = self._handle
            if handle is None:
                time.sleep(0.2)
                pending = b""
                continue

            read = wt.DWORD(0)
            overlapped = _Overlapped()
            overlapped.hEvent = self._read_event
            self._kernel32.ResetEvent(self._read_event)
            ok = self._kernel32.ReadFile(
                handle, buffer, len(buffer), ctypes.byref(read), ctypes.byref(overlapped))
            if not ok:
                if ctypes.get_last_error() != ERROR_IO_PENDING:
                    time.sleep(0.2)
                    continue
                # No timeout here on purpose: this thread has nothing else to do, and waiting
                # is what it is for. A dropped connection wakes it through the failed result.
                self._kernel32.WaitForSingleObject(self._read_event, 0xFFFFFFFF)
                ok = self._kernel32.GetOverlappedResult(
                    handle, ctypes.byref(overlapped), ctypes.byref(read), False)
            if not ok or read.value == 0:
                time.sleep(0.2)
                continue

            pending += buffer.raw[:read.value]
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text:
                    self._handle_incoming(text)

    def _handle_incoming(self, line: str) -> None:
        if line.startswith("HELLO "):
            # "HELLO ui-format <n>". A mismatch means the wheel was assembled out of a Python
            # side and a DLL from different builds - possible only on a development machine,
            # and worth saying out loud there rather than debugging a blank panel.
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "ui-format" and parts[2].isdigit():
                self._overlay_format = int(parts[2])
                from .ui import FORMAT_VERSION

                if self._overlay_format != FORMAT_VERSION:
                    logger.error(
                        "Overlay speaks UI format %d, this Runtime speaks %d. The install is "
                        "mismatched: reinstall Project Oreo Runtime.",
                        self._overlay_format, FORMAT_VERSION)
                else:
                    logger.info("Overlay ready, UI format %d.", self._overlay_format)
            else:
                logger.info("Overlay greeting: %s", line)
        elif line.startswith("ERR "):
            # The overlay rejected something. Said here, in the mod's own log, because that is
            # where the author is looking - overlay.log is a second file nobody thinks to open.
            logger.error("Overlay rejected a message: %s", line[4:])
        elif line.startswith("INPUT "):
            # Kept here as well as passed on, so a UI handle made after the overlay connected
            # can still start out knowing the device; see api.start().
            self._input_device = line[6:].strip() or None
        elif line.startswith("OK "):
            logger.debug("Overlay accepted: %s", line[3:])
        else:
            logger.debug("Overlay says: %s", line)

        with self._handle_lock:
            callbacks = list(self._message_callbacks)
        for callback in callbacks:
            try:
                callback(line)
            except Exception:
                logger.exception("Overlay message callback failed.")

    @staticmethod
    def _invoke_callback(callback: Callable[[], None]) -> None:
        try:
            callback()
        except Exception:
            logger.exception("Overlay connection callback failed.")

    def _close_locked(self) -> None:
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None
