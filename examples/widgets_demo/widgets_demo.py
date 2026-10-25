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
"""Every kind of widget on one RML panel. F9 opens and closes it.

The interface is ui/demo.rml and ui/demo.rcss - HTML and CSS as RmlUi reads them. The widgets
bind to this mod's values, so the panel and the mod agree on them without either asking:
toggle the checkbox and `sound` changes here; "Reset" sets the values back and the panel
follows. The text under the widgets is written in the RML itself, from the same values.

No NMSpy: nothing here touches the game. The Runtime only needs flush() called regularly, and
a thread of our own does that.
"""

import logging
import os
import threading
import time

from pymhf import Mod
from pymhf.core.hooking import on_key_pressed

logger = logging.getLogger("WidgetsDemo")

TOGGLE_KEY = "f9"
TICK_SECONDS = 1 / 60
DEFAULTS = {"sound": True, "volume": 40, "mode": "Normal", "name": ""}


def _mod_dir():
    return os.path.dirname(os.path.abspath(__file__))


class WidgetsDemo(Mod):
    __author__ = "Carbonster"
    __description__ = "Every Project Oreo Runtime widget on one panel"
    __version__ = "0.2.0"

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
                ui="widgets_demo",
            )
        except Exception:
            logger.exception("Project Oreo Runtime did not start; the demo is off.")
            return False

        self.ui = session.ui
        self.ui.load("demo", "ui/demo.rml")
        for name, value in DEFAULTS.items():
            self.ui.set(name, value)
        self.ui.set("clicks", "not pressed yet")
        self.ui.set("last", "")
        self.ui.set("device", self.ui.input or "")
        self.ui.on("click", "go", self._on_go)
        self.ui.on("click", "reset", self._on_reset)
        for element in ("sound", "volume", "mode", "name"):
            self.ui.on("change", element, self._heard)
        self.ui.on_input(lambda device: self.ui.set("device", device))
        logger.info("Widgets demo ready. %s opens the panel.", TOGGLE_KEY.upper())
        return True

    def _on_go(self, event):
        self._clicks += 1
        self.ui.set("clicks", f"pressed {self._clicks}x")
        self._heard(event)

    def _on_reset(self, event):
        for name, value in DEFAULTS.items():
            self.ui.set(name, value)
        self._clicks = 0
        self.ui.set("clicks", "not pressed yet")
        self._heard(event)

    def _heard(self, event):
        shown = "" if event.value is None else f" = {event.value!r}"
        self.ui.set("last", f"heard: {event.kind} {event.element}{shown}")
        logger.info("event: %s %s%s", event.kind, event.element, shown)

    @on_key_pressed(TOGGLE_KEY)
    def toggle_panel(self):
        if self.ui is None:
            return
        self._open = not self._open
        if self._open:
            self.ui.show("demo", modal=True)
        else:
            self.ui.hide("demo")
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
