import json
import unittest
from pathlib import Path
from unittest import mock

import oreo_runtime.api as api
from oreo_runtime import ui
from oreo_runtime.controller import ControllerComponent


def panel(*widgets, **overrides):
    """A format-2 document with one visible screen holding the given widgets.

    Called with no widgets it holds one of each kind, so every test can reach for any of them.
    """
    if not widgets:
        widgets = (
            {"id": "go", "widget": "button", "x": 0.1, "y": 0.1, "w": 0.3, "label": "Go"},
            {"id": "sound", "widget": "toggle", "x": 0.1, "y": 0.2, "w": 0.3, "label": "Sound"},
            {"id": "volume", "widget": "slider", "x": 0.1, "y": 0.3, "w": 0.8,
             "min": 0, "max": 100},
            {"id": "mode", "widget": "dropdown", "x": 0.1, "y": 0.4, "w": 0.5,
             "options": ["easy", "hard"]},
            {"id": "name", "widget": "input", "x": 0.1, "y": 0.5, "w": 0.8, "maxLength": 8},
        )
    document = {
        "format": 2,
        "palette": {"accent": "#6cf"},
        "screens": [{"id": "panel", "size": [400, 300], "elements": list(widgets)}],
    }
    document.update(overrides)
    return json.dumps(document)


def one(widget):
    """The loaded form of a single widget described on its own."""
    return ui.loads(panel(widget)).screens[0].elements[0]


class Recorder:
    def __init__(self):
        self.sent: list[str] = []

    def __call__(self, message: str) -> bool:
        self.sent.append(message)
        return True

    @property
    def lines(self) -> list[str]:
        return [line for write in self.sent for line in write.split(chr(10))]


class WidgetLoadingTests(unittest.TestCase):
    def test_every_kind_loads(self):
        doc = ui.loads(panel())

        self.assertEqual({"go", "sound", "volume", "mode", "name"}, set(doc.widgets()))
        self.assertEqual({"panel", "go", "sound", "volume", "mode", "name"}, doc.names())

    def test_each_kind_starts_at_a_sensible_value_when_the_file_gives_none(self):
        widgets = ui.loads(panel()).widgets()

        self.assertIsNone(widgets["go"].default)
        self.assertIs(False, widgets["sound"].default)
        self.assertEqual(0.0, widgets["volume"].default)          # the bottom of the range
        self.assertEqual("easy", widgets["mode"].default)         # the first option
        self.assertEqual("", widgets["name"].default)

    def test_a_value_from_the_file_is_kept(self):
        slider = one({"id": "v", "widget": "slider", "x": 0, "y": 0, "w": 1,
                      "min": 0, "max": 10, "value": 7})

        self.assertEqual(7.0, slider.default)

    def test_widgets_and_shapes_share_a_screen_in_file_order(self):
        doc = ui.loads(panel(
            {"id": "bg", "shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1, "fill": "#000"},
            {"id": "go", "widget": "button", "x": 0, "y": 0, "w": 1, "label": "Go"},
        ))

        self.assertEqual(["bg", "go"], [e.id for e in doc.screens[0].elements])

    def test_a_format_1_file_still_loads(self):
        doc = ui.loads(json.dumps({"format": 1, "screens": [{"id": "s", "size": [1, 1]}]}))

        self.assertEqual(["s"], [s.id for s in doc.screens])

    def test_the_wire_carries_format_2(self):
        wire = json.loads(ui.encode_document(ui.loads(panel()))[3:])

        self.assertEqual(2, wire["format"])

    def test_a_widget_travels_with_its_value(self):
        wire = one({"id": "v", "widget": "slider", "x": 0, "y": 0, "w": 1,
                    "min": 0, "max": 10}).to_wire()

        self.assertEqual("slider", wire["widget"])
        self.assertEqual(0.0, wire["value"])
        self.assertNotIn("shape", wire)


class WidgetValidationTests(unittest.TestCase):
    """Checked at load, like the shapes: a mod that will not start says which line is wrong."""

    def error(self, *widgets, **overrides):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(panel(*widgets, **overrides))
        return str(caught.exception)

    def slider(self, **fields):
        return {"id": "v", "widget": "slider", "x": 0, "y": 0, "w": 1, "min": 0, "max": 10,
                **fields}

    def test_widgets_in_a_format_1_file_are_refused(self):
        message = self.error(format=1)
        self.assertIn("format", message)
        self.assertIn("2", message)

    def test_an_unknown_widget_lists_the_known_ones(self):
        message = self.error({"id": "k", "widget": "knob", "x": 0, "y": 0, "w": 1})
        self.assertIn("knob", message)
        self.assertIn("slider", message)

    def test_a_missing_field_is_named(self):
        message = self.error({"id": "v", "widget": "slider", "x": 0, "y": 0, "w": 1, "min": 0})
        self.assertIn("max", message)

    def test_a_field_the_widget_does_not_have_is_refused(self):
        message = self.error({"id": "go", "widget": "button", "x": 0, "y": 0, "w": 1,
                              "label": "Go", "min": 3})
        self.assertIn("min", message)

    def test_an_element_cannot_be_both(self):
        self.assertIn("both", self.error({"id": "go", "widget": "button", "shape": "rect",
                                          "x": 0, "y": 0, "w": 1, "label": "Go"}))

    def test_a_widget_id_with_a_space_is_refused(self):
        # It would split the event line "EV click my button" in the wrong place.
        self.assertIn("space", self.error({"id": "my button", "widget": "button",
                                           "x": 0, "y": 0, "w": 1, "label": "Go"}))

    def test_a_slider_range_must_go_upward(self):
        self.assertIn("min", self.error(self.slider(min=5, max=5)))

    def test_a_slider_value_outside_its_range_is_refused(self):
        self.assertIn("value", self.error(self.slider(value=11)))

    def test_a_slider_step_must_be_positive(self):
        self.assertIn("step", self.error(self.slider(step=0)))

    def test_decimals_are_a_small_whole_number(self):
        self.assertIn("decimals", self.error(self.slider(decimals=7)))
        self.assertIn("decimals", self.error(self.slider(decimals=1.5)))

    def test_a_toggle_value_is_a_boolean(self):
        self.assertIn("value", self.error({"id": "t", "widget": "toggle", "x": 0, "y": 0,
                                           "w": 1, "value": 1}))

    def test_a_dropdown_value_must_be_an_option(self):
        self.assertIn("options", self.error({"id": "m", "widget": "dropdown", "x": 0, "y": 0,
                                             "w": 1, "options": ["a"], "value": "b"}))

    def test_dropdown_options_must_not_repeat(self):
        self.assertIn("repeat", self.error({"id": "m", "widget": "dropdown", "x": 0, "y": 0,
                                            "w": 1, "options": ["a", "a"]}))

    def test_a_dropdown_needs_options(self):
        self.assertIn("options", self.error({"id": "m", "widget": "dropdown", "x": 0, "y": 0,
                                             "w": 1, "options": []}))

    def test_an_input_value_must_fit(self):
        message = self.error({"id": "n", "widget": "input", "x": 0, "y": 0, "w": 1,
                              "maxLength": 3, "value": "abcd"})
        self.assertIn("maxLength", message)

    def test_an_input_length_is_bounded(self):
        self.assertIn("maxLength", self.error({"id": "n", "widget": "input", "x": 0, "y": 0,
                                               "w": 1, "maxLength": 0}))


class WidgetStyleTests(unittest.TestCase):
    def test_palette_names_resolve_in_a_style(self):
        button = one({"id": "go", "widget": "button", "x": 0, "y": 0, "w": 1, "label": "Go",
                      "style": {"background": "accent", "radius": 4}})

        self.assertEqual({"background": "#6cf", "radius": 4.0}, button.style)

    def test_the_files_widget_style_applies_to_every_widget(self):
        doc = ui.loads(panel(widgetStyle={"text": "#fff", "radius": 6}))

        for widget in doc.widgets().values():
            self.assertEqual({"text": "#fff", "radius": 6.0}, widget.style)

    def test_a_widgets_own_style_wins(self):
        doc = ui.loads(panel(
            {"id": "go", "widget": "button", "x": 0, "y": 0, "w": 1, "label": "Go",
             "style": {"radius": 0}},
            widgetStyle={"text": "#fff", "radius": 6}))

        self.assertEqual({"text": "#fff", "radius": 0.0}, doc.widgets()["go"].style)

    def test_an_unknown_style_key_lists_the_known_ones(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(panel(widgetStyle={"ImGuiCol_Button": "#fff"}))
        self.assertIn("background", str(caught.exception))

    def test_a_negative_size_is_refused(self):
        with self.assertRaises(ui.UiError):
            ui.loads(panel(widgetStyle={"radius": -1}))

    def test_a_widget_with_no_style_sends_none(self):
        wire = one({"id": "go", "widget": "button", "x": 0, "y": 0, "w": 1,
                    "label": "Go"}).to_wire()

        self.assertNotIn("style", wire)


class WidgetHandleTests(unittest.TestCase):
    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(panel()), self.recorder)

    def sent(self):
        self.handle.flush()
        return self.recorder.lines

    def test_a_value_goes_out_as_json(self):
        self.handle.set_value("volume", 42)
        self.handle.set_value("sound", True)
        self.handle.set_value("mode", "hard")

        self.assertEqual(["VAL volume 42.0", "VAL sound true", 'VAL mode "hard"'], self.sent())

    def test_text_never_carries_a_newline(self):
        self.handle.set_value("name", "a\nb")

        self.assertEqual(['VAL name "a\\nb"'], self.sent())

    def test_the_mod_reads_back_what_it_set(self):
        self.assertEqual(0.0, self.handle.value("volume"))
        self.handle.set_value("volume", 30)
        self.assertEqual(30.0, self.handle.value("volume"))

    def test_a_slider_value_is_kept_in_range(self):
        self.handle.set_value("volume", 500)
        self.assertEqual(100.0, self.handle.value("volume"))

    def test_input_text_is_cut_to_its_length_without_splitting_a_letter(self):
        self.handle.set_value("name", "aабвг")      # nine bytes into eight
        self.assertEqual("aабв", self.handle.value("name"))

    def test_a_value_of_the_wrong_kind_is_dropped(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.set_value("sound", 1)
            self.handle.set_value("volume", "loud")
            self.handle.set_value("volume", True)

        self.assertEqual([], self.sent())
        self.assertIs(False, self.handle.value("sound"))

    def test_a_dropdown_takes_only_its_options(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.set_value("mode", "nightmare")
        self.assertEqual("easy", self.handle.value("mode"))

    def test_a_button_holds_nothing(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.set_value("go", True)
        self.assertIsNone(self.handle.value("go"))

    def test_an_unknown_widget_is_reported_once(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING") as logs:
            self.handle.set_value("nobody", 1)
            self.handle.set_value("nobody", 2)
        self.assertEqual(1, len(logs.records))

    def test_new_options_go_out_and_become_choosable(self):
        self.handle.options("mode", ["easy", "hard", "nightmare"])
        self.handle.set_value("mode", "nightmare")

        self.assertEqual(['OPT mode ["easy","hard","nightmare"]', 'VAL mode "nightmare"'],
                         self.sent())

    def test_bad_options_are_dropped(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.options("mode", ["a", "a"])
            self.handle.options("volume", ["a"])
        self.assertEqual([], self.sent())

    def test_a_widget_can_be_disabled_and_enabled(self):
        self.handle.disable("go")
        self.handle.enable("sound")

        self.assertEqual(["ENA go 0", "ENA sound 1"], self.sent())

    def test_widgets_can_be_shown_and_hidden_like_anything_else(self):
        self.handle.show("go", False)

        self.assertEqual(["VIS go 0"], self.sent())


class EventTests(unittest.TestCase):
    """What the player did, delivered on the mod's own thread, inside flush()."""

    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(panel()), self.recorder)
        self.heard: list[ui.UiEvent] = []

    def test_a_click_reaches_its_handler_at_flush(self):
        self.handle.on("click", "go", self.heard.append)
        self.handle.receive("EV click go")

        self.assertEqual([], self.heard, "nothing runs on the pipe's thread")
        self.handle.flush()
        self.assertEqual([ui.UiEvent("click", "go")], self.heard)

    def test_a_change_updates_the_value_before_the_handler_runs(self):
        seen = []
        self.handle.on("change", "volume", lambda e: seen.append(self.handle.value("volume")))
        self.handle.receive("EV change volume 42.5")
        self.handle.flush()

        self.assertEqual([42.5], seen)

    def test_strings_arrive_whole(self):
        self.handle.on("submit", "name", self.heard.append)
        self.handle.receive('EV submit name "Bob Smith"')
        self.handle.flush()

        self.assertEqual([ui.UiEvent("submit", "name", "Bob Smith")], self.heard)
        self.assertEqual("Bob Smith", self.handle.value("name"))

    def test_a_handler_runs_once_per_event(self):
        self.handle.on("click", "go", self.heard.append)
        self.handle.receive("EV click go")
        self.handle.receive("EV click go")
        self.handle.flush()
        self.handle.flush()

        self.assertEqual(2, len(self.heard))

    def test_a_handler_that_throws_does_not_stop_the_rest(self):
        def broken(event):
            raise RuntimeError("mod bug")

        self.handle.on("click", "go", broken)
        self.handle.on("click", "go", self.heard.append)
        self.handle.receive("EV click go")
        with self.assertLogs("oreo_runtime.ui", "ERROR"):
            self.handle.flush()

        self.assertEqual(1, len(self.heard))

    def test_listening_to_an_unknown_widget_stops_the_mod(self):
        with self.assertRaises(ui.UiError):
            self.handle.on("click", "nobody", self.heard.append)

    def test_listening_for_an_event_the_widget_cannot_send_stops_the_mod(self):
        with self.assertRaises(ui.UiError) as caught:
            self.handle.on("click", "volume", self.heard.append)
        self.assertIn("change", str(caught.exception))

    def test_an_event_for_a_widget_the_file_does_not_have_is_dropped(self):
        self.handle.receive("EV click ghost")
        self.handle.flush()     # nothing to hear, nothing raised

    def test_a_malformed_event_is_logged_not_raised(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.receive("EV change volume {not json")

    def test_other_lines_are_not_events(self):
        self.handle.receive("OK ui")
        self.handle.receive("HELLO ui-format 2")
        self.handle.flush()

    def test_events_are_delivered_even_when_nothing_was_sent(self):
        self.handle.on("click", "go", self.heard.append)
        self.handle.receive("EV click go")

        self.handle.flush()

        self.assertEqual([], self.recorder.sent)
        self.assertEqual(1, len(self.heard))


class EventDecodingTests(unittest.TestCase):
    def test_a_click_carries_no_value(self):
        self.assertEqual(ui.UiEvent("click", "go", None), ui.decode_event("EV click go"))

    def test_values_are_json(self):
        self.assertEqual(ui.UiEvent("change", "sound", True),
                         ui.decode_event("EV change sound true"))
        self.assertEqual(ui.UiEvent("change", "mode", "a b"),
                         ui.decode_event('EV change mode "a b"'))

    def test_a_line_that_is_not_an_event_is_none(self):
        self.assertIsNone(ui.decode_event("OK ui"))

    def test_a_truncated_event_raises(self):
        with self.assertRaises(ValueError):
            ui.decode_event("EV click")


class WidgetReplayTests(unittest.TestCase):
    """The overlay puts widgets back at the file's values on every document; the rest is ours."""

    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(panel()), self.recorder)

    def replayed(self):
        self.recorder.sent.clear()
        self.handle.resend_document()
        return self.recorder.lines[1:]     # everything after the document itself

    def test_nothing_is_replayed_for_widgets_nobody_touched(self):
        self.assertEqual([], self.replayed())

    def test_what_the_mod_set_is_replayed(self):
        self.handle.set_value("volume", 30)
        self.handle.options("mode", ["x", "y"])
        self.handle.disable("go")

        self.assertEqual(['OPT mode ["x","y"]', "ENA go 0", "VAL volume 30.0"], self.replayed())

    def test_what_the_player_did_is_replayed_too(self):
        self.handle.receive("EV change sound true")
        self.handle.flush()

        self.assertEqual(["VAL sound true"], self.replayed())

    def test_a_palette_switch_keeps_the_values(self):
        doc = panel(palettes={"a": {"c": "#000"}, "b": {"c": "#fff"}})
        handle = ui.UiHandle(ui.loads(doc), self.recorder)
        handle.set_value("volume", 30)

        handle.use_palette("b")

        self.assertEqual(30.0, handle.value("volume"))
        self.assertIn("VAL volume 30.0", self.recorder.lines)


class EscapeHatchWidgetTests(unittest.TestCase):
    def test_widgets_cannot_be_drawn_by_hand(self):
        handle = ui.UiHandle(ui.loads(panel()), Recorder())

        with self.assertRaises(ui.UiError) as caught:
            handle.draw("panel", [{"widget": "button", "x": 0, "y": 0, "w": 1, "label": "x"}])
        self.assertIn("escape hatch", str(caught.exception))


class InputDeviceTests(unittest.TestCase):
    """Which device the player is using, so a mod can show the right prompts."""

    def setUp(self):
        self.handle = ui.UiHandle(ui.loads(panel()), Recorder())
        self.heard: list[str] = []
        self.handle.on_input(self.heard.append)

    def test_nothing_is_known_until_the_player_touches_something(self):
        self.assertIsNone(self.handle.input)

    def test_a_switch_is_heard_at_flush(self):
        self.handle.receive("INPUT gamepad")
        self.assertIsNone(self.handle.input, "nothing changes on the pipe's thread")

        self.handle.flush()

        self.assertEqual("gamepad", self.handle.input)
        self.assertEqual(["gamepad"], self.heard)

    def test_the_same_device_again_is_not_a_switch(self):
        # The overlay repeats the device on every reconnect.
        self.handle.receive("INPUT mouse_keyboard")
        self.handle.receive("INPUT mouse_keyboard")
        self.handle.flush()
        self.handle.receive("INPUT gamepad")
        self.handle.flush()

        self.assertEqual(["mouse_keyboard", "gamepad"], self.heard)

    def test_an_unknown_device_is_logged_and_ignored(self):
        with self.assertLogs("oreo_runtime.ui", "WARNING"):
            self.handle.receive("INPUT steering_wheel")
        self.handle.flush()

        self.assertIsNone(self.handle.input)

    def test_a_handler_that_throws_does_not_stop_the_switch(self):
        def broken(device):
            raise RuntimeError("mod bug")

        handle = ui.UiHandle(ui.loads(panel()), Recorder())
        handle.on_input(broken)
        handle.receive("INPUT gamepad")
        with self.assertLogs("oreo_runtime.ui", "ERROR"):
            handle.flush()

        self.assertEqual("gamepad", handle.input)

    def test_the_overlay_component_remembers_the_last_device(self):
        overlay = api.OverlayComponent(Path("."))
        overlay._handle_incoming("INPUT gamepad")

        self.assertEqual("gamepad", overlay.input_device)

    def test_a_handle_made_after_the_overlay_spoke_starts_from_it(self):
        overlay = api.OverlayComponent(Path("."))
        overlay._input_device = "mouse_keyboard"
        package = Path(__file__).resolve().parent
        with mock.patch.object(api, "_overlay", overlay), \
                mock.patch.object(api, "_overlay_requested", False), \
                mock.patch.object(overlay, "start", return_value=True), \
                mock.patch("oreo_runtime.api._load_ui", return_value=ui.loads(panel())):
            session = api.start(overlay=True, mod_dir=package, ui="unused.json")
        session.ui.flush()

        self.assertEqual("mouse_keyboard", session.ui.input)


class PadTests(unittest.TestCase):
    """The gamepad reaches the overlay through the Runtime, not through any mod."""

    def test_the_line_is_the_same_for_the_same_buttons(self):
        self.assertEqual("PAD a,up", ui.encode_pad({"up", "a"}))
        self.assertEqual("PAD ", ui.encode_pad(set()))

    def test_listeners_hear_changes_only(self):
        controller = ControllerComponent(Path("."))
        heard = []
        controller.add_listener(heard.append)

        controller._parse_line("BTN a")
        controller._parse_line("BTN a")       # the helper's keepalive
        controller._parse_line("BTN a,up")
        controller._parse_line("BTN")

        self.assertEqual([{"a"}, {"a", "up"}, set()], [set(h) for h in heard])

    def test_the_runtime_forwards_the_pad_to_the_overlay(self):
        controller = ControllerComponent(Path("."))
        overlay = mock.Mock()
        with mock.patch.object(api, "_controller", controller), \
                mock.patch.object(api, "_overlay", overlay), \
                mock.patch.object(api, "_pad_linked", False):
            api._link_pad()
            api._link_pad()                     # twice is once
            controller._parse_line("BTN a")

        overlay.send.assert_called_once_with("PAD a")
        overlay.add_connect_callback.assert_called_once()


if __name__ == "__main__":
    unittest.main()
