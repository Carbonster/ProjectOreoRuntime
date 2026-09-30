# /// script
# dependencies = ["pymhf>=0.2.4"]
#
# [tool.pymhf]
# exe = "NMS.exe"
# steam_gameid = 275850
# start_paused = false
# interactive_console = false
#
# [tool.pymhf.gui]
# shown = false
#
# [tool.pymhf.logging]
# log_dir = "logs"
# log_level = "info"
# shown = false
# ///
"""Every widget Project Oreo Runtime has, on one panel. F9 opens and closes it.

The whole interface is widgets_demo_ui.json: where each widget sits, what it looks like, what
it starts at. This file only listens. Press, drag, pick and type, and the text under the widgets
says what the mod heard; the log says it too, one line per event.

"Reset" goes the other way: the mod sets every widget back, and the panel follows.

The icon in the top-right corner is the device the player last touched - a pad, or a mouse and
keyboard - as the Runtime reports it. Both icons are plain shapes in the file; the mod only
says which set is shown.

No NMSpy: nothing here touches the game, so nothing here needs to know its structures. The
Runtime only needs flush() called regularly, and a thread of our own does that - which also
means the demo keeps working the day a game update leaves NMSpy behind.
"""

import logging
import os
import threading
import time

from pymhf import Mod
from pymhf.core.hooking import on_key_pressed

logger = logging.getLogger("WidgetsDemo")

UI_FILE = "widgets_demo_ui.json"
TOGGLE_KEY = "f9"
TICK_SECONDS = 1 / 60

# The two icons in the file, one set of shapes each.
GAMEPAD_ICON = ("gp_body", "gp_dpad_h", "gp_dpad_v", "gp_btn_a", "gp_btn_b")
MOUSE_KEYBOARD_ICON = ("kb_body", "kb_k1", "kb_k2", "kb_k3", "kb_k4", "kb_k5", "kb_k6", "kb_k7",
                       "kb_k8", "kb_space", "ms_body", "ms_split")


def _mod_dir():
    return os.path.dirname(os.path.abspath(__file__))


class WidgetsDemo(Mod):
    __author__ = "Carbonster"
    __description__ = "Every Project Oreo Runtime widget on one panel"
    __version__ = "0.1.0"

    def __init__(self):
        super().__init__()
        self.ui = None
        self._start_attempted = False
        self._open = False
        self._clicks = 0
        threading.Thread(target=self._tick, name="WidgetsDemoTick", daemon=True).start()

    def _ensure_runtime(self):
        if self._start_attempted:
            return self.ui is not None
        self._start_attempted = True
        try:
            import oreo_runtime

            session = oreo_runtime.start(
                overlay=True,
                controller=True,
                mod_dir=_mod_dir(),
                log_dir=os.path.join(_mod_dir(), "logs"),
                ui=UI_FILE,
                on_overlay_connected=self._on_overlay_connected,
            )
        except Exception:
            logger.exception("Project Oreo Runtime did not start; the demo is off.")
            return False

        self.ui = session.ui
        self.ui.on("click", "go", self._on_go)
        self.ui.on("click", "reset", self._on_reset)
        for widget in ("sound", "volume", "mode", "name"):
            self.ui.on("change", widget, self._on_change)
        self.ui.on("submit", "name", self._on_submit)
        self.ui.on_input(self._on_input)
        self._show_input_icon(self.ui.input)
        self._say_state()
        self.ui.text("clicks", "not pressed yet")
        self.ui.text("last", "")
        logger.info("Widgets demo ready. %s opens the panel.", TOGGLE_KEY.upper())
        return True

    def _on_overlay_connected(self):
        # The Runtime replays visibility and widget values after a reconnect, but not bound
        # text: that is this mod's to say again. Queued only - the next frame's flush sends it.
        # Calling flush() here would also run event handlers on the pipe's thread.
        if self.ui is not None:
            self._say_state()
            self.ui.text("clicks", self._clicks_text())

    # ---- what the player did ------------------------------------------------------------

    def _on_go(self, event):
        self._clicks += 1
        self.ui.text("clicks", self._clicks_text())
        self._heard(event)

    def _on_change(self, event):
        self._say_state()
        self._heard(event)

    def _on_submit(self, event):
        self._heard(event)

    def _on_input(self, device):
        self._show_input_icon(device)
        logger.info("input device: %s", device)

    def _on_reset(self, event):
        # The mod setting values, the other direction of the same channel.
        self.ui.set_value("sound", True)
        self.ui.set_value("volume", 40)
        self.ui.set_value("mode", "Normal")
        self.ui.set_value("name", "")
        self._clicks = 0
        self.ui.text("clicks", self._clicks_text())
        self._say_state()
        self._heard(event)

    # ---- saying it back -----------------------------------------------------------------

    def _show_input_icon(self, device):
        # None until the player has touched anything: then neither icon is shown.
        for element in GAMEPAD_ICON:
            self.ui.show(element, device == "gamepad")
        for element in MOUSE_KEYBOARD_ICON:
            self.ui.show(element, device == "mouse_keyboard")

    def _clicks_text(self):
        return "not pressed yet" if self._clicks == 0 else f"pressed {self._clicks}x"

    def _say_state(self):
        ui = self.ui
        self.ui.text("state", "sound {}   volume {:.0f}%   mode {}   name {}".format(
            "on" if ui.value("sound") else "off", ui.value("volume"), ui.value("mode"),
            ui.value("name") or "-"))

    def _heard(self, event):
        shown = "" if event.value is None else f" = {event.value!r}"
        self.ui.text("last", f"heard: {event.kind} {event.id}{shown}")
        logger.info("event: %s %s%s", event.kind, event.id, shown)

    # ---- hooks --------------------------------------------------------------------------

    @on_key_pressed(TOGGLE_KEY)
    def toggle_panel(self):
        if self.ui is None:
            return
        self._open = not self._open
        self.ui.show("demo", self._open)
        logger.info("panel %s", "open" if self._open else "closed")

    def _tick(self):
        if not self._ensure_runtime():
            return
        while True:
            # Sends what was queued, then runs the handlers above for whatever the player did
            # since the last tick - on this thread, the one that calls flush().
            try:
                self.ui.flush()
            except Exception:
                logger.exception("flush failed")
            time.sleep(TICK_SECONDS)
