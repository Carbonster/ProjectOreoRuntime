import logging
import shutil
import tempfile
import unittest
from pathlib import Path

from oreo_runtime.ui import UiError, UiEvent, UiHandle, check_ui_name


class Wire:
    """Stands in for the pipe: every write, as the lines it held."""

    def __init__(self):
        self.writes: list[list[str]] = []

    def send(self, batch: str) -> bool:
        self.writes.append(batch.split("\n"))
        return True

    @property
    def last(self) -> list[str]:
        return self.writes[-1] if self.writes else []


class UiTest(unittest.TestCase):
    def setUp(self):
        logging.disable(logging.CRITICAL)
        self.addCleanup(logging.disable, logging.NOTSET)
        self.dir = Path(tempfile.mkdtemp(prefix="oreo-ui-test-")).resolve()
        self.addCleanup(shutil.rmtree, self.dir, True)
        (self.dir / "ui").mkdir()
        (self.dir / "ui" / "catch.rml").write_text("<rml><body/></rml>")
        self.wire = Wire()
        self.ui = UiHandle("fishing", self.dir, self.wire.send)

    def document(self) -> str:
        return str(self.dir / "ui" / "catch.rml")


class TestNames(UiTest):
    def test_a_ui_name_is_what_rmlui_takes_as_a_model_name(self):
        self.assertEqual(check_ui_name("fishing_2"), "fishing_2")
        for bad in ("", "2fish", "fish-ing", "fish.ing", "a" * 65, "oreo_runtime"):
            with self.subTest(name=bad), self.assertRaises(UiError):
                check_ui_name(bad)

    def test_a_value_name_may_not_be_a_word_rmlui_keeps(self):
        for bad in ("size", "IT", "fish.y", "1x"):
            with self.subTest(name=bad), self.assertRaises(UiError):
                self.ui.set(bad, 1)


class TestDocuments(UiTest):
    def test_a_document_is_loaded_by_its_full_path(self):
        self.ui.load("catch", "ui/catch.rml")
        self.ui.flush()
        self.assertEqual(self.wire.last, [f"LOAD fishing catch {self.document()}"])

    def test_documents_live_inside_the_mods_folder(self):
        outside = self.dir.parent / "elsewhere.rml"
        with self.assertRaises(UiError):
            self.ui.load("catch", "../elsewhere.rml")
        with self.assertRaises(UiError):
            self.ui.load("catch", str(outside))

    def test_a_missing_document_is_refused_where_it_was_asked_for(self):
        with self.assertRaises(UiError):
            self.ui.load("catch", "ui/nope.rml")

    def test_only_a_loaded_document_can_be_shown(self):
        with self.assertRaises(UiError):
            self.ui.show("catch")

    def test_show_modal_and_hide_say_their_mode(self):
        self.ui.load("catch", "ui/catch.rml")
        self.ui.show("catch", modal=True)
        self.ui.flush()
        self.assertEqual(self.wire.last[-1], "SHOW fishing catch modal")
        self.ui.hide("catch")
        self.ui.flush()
        self.assertEqual(self.wire.last, ["SHOW fishing catch hide"])

    def test_closing_forgets_what_was_waiting_for_that_document(self):
        self.ui.load("catch", "ui/catch.rml")
        self.ui.show("catch")
        self.ui.close("catch")
        self.ui.flush()
        self.assertEqual(self.wire.last, ["CLOSE fishing catch"])


class TestValues(UiTest):
    def test_values_travel_as_json(self):
        self.ui.set("hooked", True)
        self.ui.set("depth", 0.42)
        self.ui.set("count", 3)
        self.ui.set("name", 'Pike "the big one"')
        self.ui.flush()
        self.assertEqual(self.wire.last, [
            "SET fishing hooked true",
            "SET fishing depth 0.42",
            "SET fishing count 3",
            'SET fishing name "Pike \\"the big one\\""',
        ])

    def test_only_the_last_value_before_a_flush_is_sent(self):
        for y in (0.1, 0.2, 0.3):
            self.ui.set("fishY", y)
        self.ui.flush()
        self.assertEqual(self.wire.last, ["SET fishing fishY 0.3"])

    def test_text_keeps_its_letters(self):
        self.ui.set("name", "Щука")
        self.ui.flush()
        self.assertEqual(self.wire.last, ['SET fishing name "Щука"'])

    def test_only_scalars_cross_the_pipe(self):
        for bad in (float("nan"), float("inf"), None, [1], {"a": 1}):
            with self.subTest(value=bad), self.assertRaises(UiError):
                self.ui.set("x", bad)

    def test_nothing_queued_means_nothing_written(self):
        self.ui.flush()
        self.assertEqual(self.wire.writes, [])


class TestEvents(UiTest):
    def test_a_click_reaches_its_handler_at_flush(self):
        seen = []
        self.ui.on("click", "save", seen.append)
        self.ui.receive("EV fishing settings click save null")
        self.assertEqual(seen, [])
        self.ui.flush()
        self.assertEqual(seen, [UiEvent("click", "settings", "save", None)])

    def test_a_change_carries_its_value_spaces_and_all(self):
        seen = []
        self.ui.on("change", "title", seen.append)
        self.ui.receive('EV fishing settings change title "a b c"')
        self.ui.flush()
        self.assertEqual(seen[0].value, "a b c")

    def test_another_mods_events_are_not_ours(self):
        seen = []
        self.ui.on("click", "save", seen.append)
        self.ui.receive("EV hunting settings click save null")
        self.ui.flush()
        self.assertEqual(seen, [])

    def test_a_failing_handler_does_not_stop_the_next(self):
        seen = []
        self.ui.on("click", "save", lambda event: 1 / 0)
        self.ui.on("click", "save", seen.append)
        self.ui.receive("EV fishing settings click save null")
        self.ui.flush()
        self.assertEqual(len(seen), 1)

    def test_a_value_the_player_changed_is_remembered(self):
        self.ui.set("volume", 0.5)
        self.ui.receive("VAR fishing volume 0.8")
        self.ui.flush()
        self.assertEqual(self.ui.value("volume"), 0.8)

    def test_the_input_device_is_heard_once_per_switch(self):
        seen = []
        self.ui.on_input(seen.append)
        for line in ("INPUT gamepad", "INPUT gamepad", "INPUT mouse_keyboard"):
            self.ui.receive(line)
        self.ui.flush()
        self.assertEqual(seen, ["gamepad", "mouse_keyboard"])
        self.assertEqual(self.ui.input, "mouse_keyboard")

    def test_garbage_from_the_overlay_is_dropped(self):
        self.ui.receive("EV fishing settings click save {not json")
        self.ui.receive("INPUT keyboard")
        self.ui.flush()
        self.assertIsNone(self.ui.input)

    def test_an_unknown_event_kind_is_refused(self):
        with self.assertRaises(UiError):
            self.ui.on("hover", "save", print)


class TestResend(UiTest):
    def test_everything_is_said_again_after_a_reconnect(self):
        self.ui.load("catch", "ui/catch.rml")
        self.ui.show("catch")
        self.ui.set("fishY", 0.5)
        self.ui.flush()
        self.ui.resend()
        self.assertEqual(self.wire.last, [
            f"ROOT fishing {self.dir}",
            f"LOAD fishing catch {self.document()}",
            "SHOW fishing catch show",
            "SET fishing fishY 0.5",
        ])

    def test_a_resend_covers_what_was_waiting(self):
        self.ui.set("fishY", 0.5)
        self.ui.resend()
        self.ui.flush()
        self.assertEqual(len(self.wire.writes), 1)


if __name__ == "__main__":
    unittest.main()
