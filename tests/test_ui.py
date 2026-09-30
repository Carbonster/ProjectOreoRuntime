import json
import locale
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import oreo_runtime.api as api
from oreo_runtime import ui
from oreo_runtime.overlay import OverlayComponent


def minimal(**overrides):
    """The smallest document that loads, so each test can break exactly one thing."""
    document = {
        "format": 1,
        "screens": [
            {
                "id": "catch",
                "size": [400, 600],
                "elements": [
                    {"id": "bar", "shape": "rect", "x": 0.4, "y": {"bind": "barY"},
                     "w": 0.2, "h": 0.1, "fill": "#6cf"},
                ],
            }
        ],
    }
    document.update(overrides)
    return json.dumps(document)


class LoadingTests(unittest.TestCase):
    def test_a_minimal_document_loads(self):
        doc = ui.loads(minimal())

        self.assertEqual(1, len(doc.screens))
        self.assertEqual("catch", doc.screens[0].id)
        self.assertEqual({"catch", "bar"}, doc.names())
        self.assertEqual({"barY"}, doc.bindings())

    def test_screens_start_hidden_unless_the_file_says_otherwise(self):
        """A screen the mod has not asked for must not be on screen at load.

        The mod decides when to show things - that is game state. A screen that defaulted to
        visible would flash up the moment the game starts, before the mod has said anything.
        """
        self.assertFalse(ui.loads(minimal()).screens[0].visible)

    def test_a_broken_file_names_the_place(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.load(Path("no-such-file.json"))
        self.assertIn("no-such-file.json", str(caught.exception))

    def test_wrong_format_version_is_refused(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(minimal(format=99))
        self.assertIn("99", str(caught.exception))

    def test_invalid_json_says_so(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads("{not json", path="broken.json")
        self.assertIn("broken.json", str(caught.exception))


class ValidationTests(unittest.TestCase):
    """Everything is checked at load, not on the frame that happens to use it.

    A mod that refuses to start with a readable reason is far cheaper to fix than one that
    silently draws nothing.
    """

    def load_expecting_error(self, document):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(json.dumps(document))
        return str(caught.exception)

    def base(self):
        return json.loads(minimal())

    def test_an_element_without_an_id_is_refused(self):
        doc = self.base()
        del doc["screens"][0]["elements"][0]["id"]
        self.assertIn("id", self.load_expecting_error(doc))

    def test_duplicate_ids_are_refused(self):
        doc = self.base()
        doc["screens"][0]["elements"].append(dict(doc["screens"][0]["elements"][0]))
        self.assertIn("duplicate", self.load_expecting_error(doc))

    def test_an_element_id_may_not_collide_with_a_screen_id(self):
        # Both are addressed by the same show()/hide() call, so one name has to mean one thing.
        doc = self.base()
        doc["screens"][0]["elements"][0]["id"] = "catch"
        self.assertIn("duplicate", self.load_expecting_error(doc))

    def test_unknown_shape_lists_the_known_ones(self):
        doc = self.base()
        doc["screens"][0]["elements"][0]["shape"] = "hexagon"
        message = self.load_expecting_error(doc)
        self.assertIn("hexagon", message)
        self.assertIn("rect", message)

    def test_a_missing_required_field_names_it(self):
        doc = self.base()
        del doc["screens"][0]["elements"][0]["w"]
        self.assertIn('"w"', self.load_expecting_error(doc))

    def test_a_field_the_shape_does_not_have_is_refused(self):
        # Catches the typo that would otherwise be ignored in silence.
        doc = self.base()
        doc["screens"][0]["elements"][0]["radiuss"] = 4
        self.assertIn("radiuss", self.load_expecting_error(doc))

    def test_an_unknown_anchor_is_refused(self):
        doc = self.base()
        doc["screens"][0]["anchor"] = "middle-ish"
        self.assertIn("anchor", self.load_expecting_error(doc))

    def test_an_unknown_valign_is_refused(self):
        doc = self.base()
        doc["screens"][0]["valign"] = "middle-ish"
        self.assertIn("valign", self.load_expecting_error(doc))

    def test_a_bad_colour_is_caught_at_load(self):
        doc = self.base()
        doc["screens"][0]["elements"][0]["fill"] = "sort of blue"
        self.assertIn("colour", self.load_expecting_error(doc))

    def test_a_binding_without_a_name_is_refused(self):
        doc = self.base()
        doc["screens"][0]["elements"][0]["y"] = {"bind": ""}
        self.assertIn("bind", self.load_expecting_error(doc))

    def test_a_boolean_where_a_number_belongs_is_refused(self):
        # bool is an int in Python, so this passes a naive isinstance check.
        doc = self.base()
        doc["screens"][0]["elements"][0]["w"] = True
        self.assertIn("boolean", self.load_expecting_error(doc))


class OriginTests(unittest.TestCase):
    """A bar is described by its centre, not its top edge.

    The fishing catch bar moves and changes height at the same time. If (x, y) always meant
    the top-left corner, the mod would have to send `y - h / 2`, and layout arithmetic would
    be back in the mod - exactly what moving the UI into a file is meant to stop.
    """

    def rect(self, **extra):
        element = {"id": "bar", "shape": "rect", "x": 0.5, "y": 0.5, "w": 0.2, "h": 0.1,
                   "fill": "#6cf"}
        element.update(extra)
        return json.dumps({"format": 1, "screens": [
            {"id": "s", "size": [10, 10], "elements": [element]}]})

    def test_a_rect_is_anchored_top_left_unless_it_says_otherwise(self):
        doc = ui.loads(self.rect())
        self.assertEqual("top-left", doc.screens[0].elements[0].fields["origin"])

    def test_a_rect_can_be_anchored_by_its_centre(self):
        doc = ui.loads(self.rect(origin="center"))
        self.assertEqual("center", doc.screens[0].elements[0].fields["origin"])

    def test_an_unknown_origin_is_refused(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(self.rect(origin="middle"))
        self.assertIn("origin", str(caught.exception))

    def test_shapes_that_are_always_centred_take_no_origin(self):
        # A circle has nothing but a centre; accepting the field would suggest it does
        # something.
        document = json.dumps({"format": 1, "screens": [{"id": "s", "size": [10, 10], "elements": [
            {"id": "dot", "shape": "circle", "x": 0.5, "y": 0.5, "r": 0.1,
             "origin": "center"}]}]})
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(document)
        self.assertIn("origin", str(caught.exception))


class VAlignTests(unittest.TestCase):
    """Vertical pinning, independent of the horizontal anchor.

    Added so a screen can sit in a screen corner - top-left, say - rather than only ever being
    centred top-to-bottom. Every document written before this field existed never set it, so
    the default has to keep meaning exactly what it always meant: centred.
    """

    def screen(self, **extra):
        document = {"format": 1, "screens": [
            {"id": "s", "size": [10, 10], "elements": [
                {"id": "e", "shape": "rect", "x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0,
                 "fill": "#6cf"}]}]}
        document["screens"][0].update(extra)
        return json.dumps(document)

    def test_valign_defaults_to_center(self):
        self.assertEqual("center", ui.loads(self.screen()).screens[0].valign)

    def test_valign_can_be_top_or_bottom(self):
        self.assertEqual("top", ui.loads(self.screen(valign="top")).screens[0].valign)
        self.assertEqual("bottom", ui.loads(self.screen(valign="bottom")).screens[0].valign)

    def test_valign_travels_on_the_wire(self):
        wire = ui.loads(self.screen(valign="top")).to_wire()
        self.assertEqual("top", wire["screens"][0]["valign"])


class PaletteTests(unittest.TestCase):
    def test_a_palette_name_resolves_to_its_colour(self):
        doc = ui.loads(json.dumps({
            "format": 1,
            "palette": {"accent": "#6cf"},
            "screens": [{"id": "s", "size": [10, 10], "elements": [
                {"id": "e", "shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1, "fill": "accent"},
            ]}],
        }))
        self.assertEqual("#6cf", doc.screens[0].elements[0].fields["fill"])

    def test_an_undefined_palette_name_is_caught_at_load(self):
        """Otherwise a mistyped colour name is drawn as black and nobody knows why."""
        doc = {
            "format": 1,
            "palette": {"accent": "#6cf"},
            "screens": [{"id": "s", "size": [10, 10], "elements": [
                {"id": "e", "shape": "rect", "x": 0, "y": 0, "w": 1, "h": 1, "fill": "acccent"},
            ]}],
        }
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(json.dumps(doc))
        self.assertIn("acccent", str(caught.exception))


class WireTests(unittest.TestCase):
    def test_the_document_travels_as_one_line(self):
        """The overlay reads the pipe line by line; a document split over several would be
        applied in pieces, and a half-built screen would be drawn."""
        message = ui.encode_document(ui.loads(minimal()))

        self.assertTrue(message.startswith("UI "))
        self.assertNotIn("\n", message)
        json.loads(message[3:])  # and it is still valid JSON on the other side

    def test_numbers_do_not_follow_the_system_locale(self):
        """Under a Russian locale a comma is the decimal separator.

        Python's repr() is locale-independent by definition, but the failure this guards
        against is real and only shows up on someone else's machine: 0.42 written as "0,42"
        is read by the overlay as 0, and the bar sits at the top of the screen for that
        player alone.
        """
        previous = locale.setlocale(locale.LC_NUMERIC)
        try:
            for candidate in ("ru_RU.UTF-8", "Russian_Russia.1251", "de_DE.UTF-8"):
                try:
                    locale.setlocale(locale.LC_NUMERIC, candidate)
                    break
                except locale.Error:
                    continue
            self.assertEqual("SET fish 0.42", ui.encode_set("fish", 0.42))
        finally:
            locale.setlocale(locale.LC_NUMERIC, previous)

    def test_text_never_carries_a_newline(self):
        """A newline in a fish name would end the message and turn the rest into a command."""
        message = ui.encode_text("name", "Bass\nSET evil 1")

        self.assertNotIn("\n", message)
        self.assertTrue(message.startswith("TXT name "))

    def test_non_ascii_text_survives(self):
        # The pipe carries UTF-8 end to end; the old protocol decoded it as ASCII.
        self.assertEqual("TXT name Окунь", ui.encode_text("name", "Окунь"))

    def test_visibility_is_zero_or_one(self):
        self.assertEqual("VIS bar 1", ui.encode_visible("bar", True))
        self.assertEqual("VIS bar 0", ui.encode_visible("bar", False))


class Recorder:
    """Stands in for the pipe. Returns True the way a real send does - a handle that reads the
    result, as resending the document does, must see the same thing it would in the game."""

    def __init__(self):
        self.sent: list[str] = []
        self.ok = True

    def __call__(self, message: str) -> bool:
        self.sent.append(message)
        return self.ok

    @property
    def lines(self) -> list[str]:
        """Every message, with batched writes split back out."""
        return [line for write in self.sent for line in write.split(chr(10))]


class HandleTests(unittest.TestCase):
    def setUp(self):
        self.recorder = Recorder()
        self.sent = self.recorder.sent
        self.handle = ui.UiHandle(ui.loads(minimal()), self.recorder)

    def test_nothing_is_sent_until_the_frame_is_flushed(self):
        """One write per frame, not one per change: the mod runs inside the game under the
        GIL, and a blocking pipe write stalls a frame the player is watching."""
        self.handle.set("barY", 0.4)
        self.handle.set("barY", 0.5)

        self.assertEqual([], self.sent)

    def test_a_value_set_twice_in_one_frame_is_sent_once(self):
        self.handle.set("barY", 0.4)
        self.handle.set("barY", 0.5)
        self.handle.flush()

        self.assertEqual(["SET barY 0.5"], self.sent)

    def test_a_frame_is_one_write(self):
        self.handle.set("barY", 0.5)
        self.handle.show("bar", False)
        self.handle.flush()

        self.assertEqual(1, len(self.sent))
        self.assertEqual({"SET barY 0.5", "VIS bar 0"}, set(self.sent[0].split("\n")))

    def test_flushing_nothing_sends_nothing(self):
        self.assertTrue(self.handle.flush())
        self.assertEqual([], self.sent)

    def test_an_unknown_binding_is_dropped_not_sent(self):
        """A typo must not reach the overlay: it would be a command the renderer cannot place,
        every frame, forever."""
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.set("barZ", 0.5)
        self.handle.flush()

        self.assertEqual([], self.sent)

    def test_an_unknown_name_is_reported_once_not_every_frame(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING") as logs:
            for _ in range(100):
                self.handle.set("barZ", 0.5)

        self.assertEqual(1, len(logs.output))

    def test_show_addresses_screens_as_well_as_elements(self):
        self.handle.show("catch")
        self.handle.flush()

        self.assertEqual(["VIS catch 1"], self.sent)

    def test_the_document_can_be_sent_again(self):
        # The overlay holds the document in memory only, so a reconnect has to start over.
        self.handle.resend_document()

        self.assertEqual(1, len(self.sent))
        self.assertTrue(self.sent[0].startswith("UI "))


class FileTests(unittest.TestCase):
    def test_loading_from_disk_reads_utf8(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "ui.json"
            path.write_text(minimal().replace('"bar"', '"полоска"'), encoding="utf-8")

            doc = ui.load(path)

            self.assertIn("полоска", doc.names())
            self.assertEqual(str(path), doc.path)


class ApiWiringTests(unittest.TestCase):
    """How the UI file reaches the overlay, without loading a DLL to find out."""

    def setUp(self):
        self.package = Path(tempfile.mkdtemp(prefix="oreo-ui-test-"))
        self.addCleanup(shutil.rmtree, self.package, True)
        (self.package / "bin").mkdir()
        for name in ("oreo_overlay.dll", "oreo_controller_support.exe", "SDL2.dll"):
            (self.package / "bin" / name).write_bytes(b"")
        (self.package / "ui.json").write_text(minimal(), encoding="utf-8")

        self.overlay = OverlayComponent(self.package)
        patches = [
            mock.patch.object(api, "_overlay", self.overlay),
            mock.patch("ctypes.WinDLL"),
            mock.patch("threading.Thread"),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_a_relative_ui_path_is_resolved_against_the_mod_folder(self):
        """A mod names its own file; it has no idea where the player installed it."""
        session = api.start(overlay=True, mod_dir=self.package, ui="ui.json")

        self.assertIsNotNone(session.ui)
        self.assertEqual({"catch", "bar"}, session.ui.document.names())

    def test_no_ui_argument_leaves_the_session_without_one(self):
        # Most Python mods change game logic and draw nothing at all.
        session = api.start(overlay=True, mod_dir=self.package)

        self.assertIsNone(session.ui)

    def test_a_broken_ui_file_stops_the_mod_rather_than_starting_blind(self):
        (self.package / "broken.json").write_text("{oops", encoding="utf-8")

        with self.assertRaises(ui.UiError):
            api.start(overlay=True, mod_dir=self.package, ui="broken.json")

    def test_the_document_is_sent_when_the_overlay_connects(self):
        """Not at start(): the overlay's pipe comes up later, and can drop and come back."""
        sent = []
        handle = ui.UiHandle(ui.loads(minimal()), sent.append)

        api._connected_callback(handle, None)()

        self.assertEqual(1, len(sent))
        self.assertTrue(sent[0].startswith("UI "))

    def test_the_mods_own_connect_callback_still_runs(self):
        sent, called = [], []
        handle = ui.UiHandle(ui.loads(minimal()), sent.append)

        api._connected_callback(handle, lambda: called.append(True))()

        self.assertEqual(1, len(sent))
        self.assertEqual([True], called)


if __name__ == "__main__":
    unittest.main()


def themed(**overrides):
    """A document with two named palettes, for the theme tests."""
    document = {
        "format": 1,
        "palette": {"ink": "#111"},
        "palettes": {
            "blue": {"accent": "#6cf"},
            "sand": {"accent": "#fc6"},
        },
        "screens": [
            {
                "id": "catch",
                "size": [400, 600],
                "inset": [200, 0],
                "anchor": "right",
                "elements": [
                    {"id": "bar", "shape": "rect", "x": 0.4, "y": 0.1, "w": 0.2, "h": 0.1,
                     "fill": "accent", "border": "ink"},
                ],
            }
        ],
    }
    document.update(overrides)
    return json.dumps(document)


class NamedPaletteTests(unittest.TestCase):
    """Themes switch whole or not at all.

    The alternative was making every colour a binding, so a theme change would be forty
    separate messages - forty chances to leave one behind and show a panel half in one scheme
    and half in the other.
    """

    def test_the_first_palette_is_used_unless_the_file_names_a_default(self):
        doc = ui.loads(themed())

        self.assertEqual("blue", doc.palette)
        self.assertEqual(("blue", "sand"), doc.palettes)
        self.assertEqual("#6cf", doc.screens[0].elements[0].fields["fill"])

    def test_a_named_default_wins(self):
        self.assertEqual("sand", ui.loads(themed(defaultPalette="sand")).palette)

    def test_the_shared_palette_still_applies(self):
        """Colours that do not change between themes live in `palette` and are not repeated."""
        self.assertEqual("#111", ui.loads(themed()).screens[0].elements[0].fields["border"])

    def test_asking_for_a_palette_resolves_that_one(self):
        doc = ui.loads(themed(), palette="sand")

        self.assertEqual("#fc6", doc.screens[0].elements[0].fields["fill"])

    def test_palettes_that_define_different_colours_are_refused(self):
        """Caught at load, where it costs a minute.

        A theme missing `accent` would load happily and only fail on the keypress that switched
        to it - in front of the player, mid-game, with the panel already on screen.
        """
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(themed(palettes={"blue": {"accent": "#6cf"}, "sand": {"other": "#fc6"}}))

        self.assertIn("accent", str(caught.exception))

    def test_asking_for_a_palette_a_file_does_not_have_is_refused(self):
        with self.assertRaises(ui.UiError) as caught:
            ui.loads(themed(), palette="mauve")

        self.assertIn("mauve", str(caught.exception))

    def test_a_file_without_palettes_has_none_to_switch(self):
        doc = ui.loads(minimal())

        self.assertIsNone(doc.palette)
        self.assertEqual((), doc.palettes)


class PaletteSwitchTests(unittest.TestCase):
    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(themed()), self.recorder)

    def test_switching_resends_the_whole_document(self):
        self.handle.use_palette("sand")

        self.assertTrue(self.recorder.sent[0].startswith("UI "))
        self.assertIn("#fc6", self.recorder.sent[0])
        self.assertEqual("sand", self.handle.palette)

    def test_switching_to_the_current_palette_sends_nothing(self):
        self.handle.use_palette("blue")

        self.assertEqual([], self.recorder.sent)

    def test_an_unknown_palette_is_reported_and_changes_nothing(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.use_palette("mauve")

        self.assertEqual([], self.recorder.sent)
        self.assertEqual("blue", self.handle.palette)

    def test_what_the_mod_had_changed_survives_the_switch(self):
        """A theme change reloads the document, which arrives at the file's defaults. Anything
        the player had already done would silently undo itself."""
        self.handle.show("catch")
        self.handle.place("catch", inset=(40, 10), anchor="left")
        self.handle.flush()
        self.recorder.sent.clear()

        self.handle.use_palette("sand")

        replayed = self.recorder.lines
        self.assertIn("VIS catch 1", replayed)
        self.assertIn("POS catch 40.0 10.0 left", replayed)


class PlacementTests(unittest.TestCase):
    """Where a panel sits is the player's business, not the file's.

    The file cannot know which monitor it will end up on, so the mod reads the key, remembers
    the position in its own settings, and moves the box. What is inside the box never moves.
    """

    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(themed()), self.recorder)

    def test_moving_a_screen_sends_its_new_placement(self):
        self.handle.place("catch", inset=(120, -30))
        self.handle.flush()

        self.assertEqual(["POS catch 120.0 -30.0 right"], self.recorder.sent)

    def test_the_anchor_can_be_flipped_without_touching_the_inset(self):
        """Mirroring keeps the margin. A panel 200px from the right edge belongs 200px from the
        left one, not at whatever coordinate 200 happens to mean there."""
        self.handle.place("catch", anchor="left")
        self.handle.flush()

        self.assertEqual(["POS catch 200.0 0.0 left"], self.recorder.sent)

    def test_an_unknown_anchor_is_refused(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.place("catch", anchor="sideways")
        self.handle.flush()

        self.assertEqual([], self.recorder.sent)

    def test_an_unknown_screen_is_reported(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.place("inventory", inset=(0, 0))
        self.handle.flush()

        self.assertEqual([], self.recorder.sent)

    def test_moving_twice_in_one_frame_sends_the_last_position(self):
        self.handle.place("catch", inset=(10, 0))
        self.handle.place("catch", inset=(20, 0))
        self.handle.flush()

        self.assertEqual(["POS catch 20.0 0.0 right"], self.recorder.sent)


class EscapeHatchTests(unittest.TestCase):
    """Free-hand shapes, checked exactly as hard as the ones in the file.

    The pipe is reachable by any process running as this user, and the overlay is inside
    somebody's game. Nothing gets a shortcut through validation for being convenient.
    """

    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(themed()), self.recorder)

    def test_shapes_go_out_addressed_to_a_screen(self):
        self.handle.draw("catch", [{"shape": "circle", "x": 0.5, "y": 0.5, "r": 0.1,
                                    "fill": "accent"}])
        self.handle.flush()

        self.assertEqual(1, len(self.recorder.sent))
        message = self.recorder.sent[0]
        self.assertTrue(message.startswith("RAW catch "))
        self.assertIn("#6cf", message, "palette names resolve here, not in the overlay")

    def test_raw_shapes_need_no_id(self):
        """Nothing addresses them, so demanding a unique name for each would be paperwork."""
        self.handle.draw("catch", [{"shape": "circle", "x": 0.5, "y": 0.5, "r": 0.1,
                                    "fill": "#fff"}] * 3)
        self.handle.flush()

        self.assertEqual(1, len(self.recorder.sent))

    def test_a_broken_raw_shape_is_refused(self):
        with self.assertRaises(ui.UiError):
            self.handle.draw("catch", [{"shape": "blob", "x": 0.5, "y": 0.5}])

    def test_a_raw_shape_missing_a_required_field_is_refused(self):
        with self.assertRaises(ui.UiError) as caught:
            self.handle.draw("catch", [{"shape": "circle", "x": 0.5, "y": 0.5}])

        self.assertIn("r", str(caught.exception))

    def test_an_empty_list_clears_the_layer(self):
        self.handle.draw("catch", [])
        self.handle.flush()

        self.assertEqual(["RAW catch []"], self.recorder.sent)

    def test_drawing_on_an_unknown_screen_is_reported(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.draw("inventory", [])
        self.handle.flush()

        self.assertEqual([], self.recorder.sent)

    def test_raw_shapes_may_bind_values_like_any_other(self):
        self.handle.draw("catch", [{"shape": "circle", "x": {"bind": "barY"}, "y": 0.5,
                                    "r": 0.1, "fill": "#fff"}])
        self.handle.flush()

        self.assertIn("barY", self.recorder.sent[0])


class ReservedNameTests(unittest.TestCase):
    """`runtime.*` is measured by the Runtime, and a mod may not write it.

    A cost readout a mod could set to whatever it liked would not be a cost readout.
    """

    def setUp(self):
        self.recorder = Recorder()
        document = ui.loads(json.dumps({
            "format": 1,
            "screens": [{
                "id": "debug",
                "size": [300, 100],
                "elements": [
                    {"id": "cost", "shape": "text", "x": 0.1, "y": 0.1, "color": "#fff",
                     "text": {"bind": "runtime.frameMs"}},
                ],
            }],
        }))
        self.handle = ui.UiHandle(document, self.recorder)

    def test_a_file_may_bind_a_runtime_value(self):
        self.assertIn("runtime.frameMs", self.handle.document.bindings())

    def test_a_mod_may_not_set_one(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.set("runtime.frameMs", 999.0)
        self.handle.flush()

        self.assertEqual([], self.recorder.sent)

    def test_a_mod_may_not_write_one_as_text_either(self):
        with self.assertLogs("oreo_runtime.ui", level="WARNING"):
            self.handle.text("runtime.frameMs", "0.001")
        self.handle.flush()

        self.assertEqual([], self.recorder.sent)


class ReplayTests(unittest.TestCase):
    """A reconnect must not undo what the player has done.

    The overlay keeps the document in memory only. It comes back knowing the file's defaults
    and nothing else: panel at its shipped position, parts shown or hidden as shipped. Every
    mod would otherwise have to remember all of that itself, and get it right.
    """

    def setUp(self):
        self.recorder = Recorder()
        self.handle = ui.UiHandle(ui.loads(themed()), self.recorder)

    def test_visibility_is_replayed(self):
        self.handle.show("catch")
        self.handle.hide("bar")
        self.handle.flush()
        self.recorder.sent.clear()

        self.handle.resend_document()

        replayed = self.recorder.lines
        self.assertIn("VIS catch 1", replayed)
        self.assertIn("VIS bar 0", replayed)

    def test_placement_is_replayed(self):
        self.handle.place("catch", inset=(64, 8), anchor="left")
        self.handle.flush()
        self.recorder.sent.clear()

        self.handle.resend_document()

        self.assertIn("POS catch 64.0 8.0 left", self.recorder.lines)

    def test_raw_shapes_are_replayed(self):
        self.handle.draw("catch", [{"shape": "circle", "x": 0.5, "y": 0.5, "r": 0.1,
                                    "fill": "#fff"}])
        self.handle.flush()
        self.recorder.sent.clear()

        self.handle.resend_document()

        self.assertTrue(any(line.startswith("RAW catch ") for line in self.recorder.lines))

    def test_the_document_goes_first(self):
        """Replaying before the document arrives would address elements the overlay has not
        heard of yet, and every one of them would be logged as a typo."""
        self.handle.show("catch")
        self.handle.flush()
        self.recorder.sent.clear()

        self.handle.resend_document()

        self.assertTrue(self.recorder.sent[0].startswith("UI "))

    def test_a_document_that_did_not_arrive_is_not_followed_by_a_replay(self):
        self.handle.show("catch")
        self.handle.flush()
        self.recorder.sent.clear()
        self.recorder.ok = False

        self.assertFalse(self.handle.resend_document())
        self.assertEqual(1, len(self.recorder.sent))


class ReverseChannelTests(unittest.TestCase):
    """What the overlay says back.

    Nothing in v1 depends on it. It exists so that the first input event in v2 is content on a
    channel that already works rather than a change to the protocol - and, today, so that an
    overlay refusing a document says so in the log the mod author is actually reading.
    """

    def setUp(self):
        self.component = OverlayComponent(Path("."))

    def test_a_matching_greeting_is_recorded(self):
        with self.assertLogs("oreo_runtime.overlay", level="INFO"):
            self.component._handle_incoming(f"HELLO ui-format {ui.FORMAT_VERSION}")

        self.assertEqual(ui.FORMAT_VERSION, self.component.overlay_format)

    def test_a_mismatched_greeting_is_an_error(self):
        """Only possible on a development machine, where the DLL and the Python side came from
        different builds. Loud there beats a blank panel and no explanation."""
        with self.assertLogs("oreo_runtime.overlay", level="ERROR") as logs:
            self.component._handle_incoming("HELLO ui-format 99")

        self.assertIn("mismatched", logs.output[0])

    def test_a_rejected_document_reaches_the_mods_log(self):
        with self.assertLogs("oreo_runtime.overlay", level="ERROR") as logs:
            self.component._handle_incoming("ERR ui no \"screens\" array")

        self.assertIn("screens", logs.output[0])

    def test_a_listener_hears_every_line(self):
        heard: list[str] = []
        self.component.add_message_callback(heard.append)

        self.component._handle_incoming("OK ui")

        self.assertEqual(["OK ui"], heard)

    def test_a_listener_that_throws_does_not_take_the_reader_down(self):
        def explode(_line):
            raise RuntimeError("mod bug")

        self.component.add_message_callback(explode)

        with self.assertLogs("oreo_runtime.overlay", level="ERROR"):
            self.component._handle_incoming("OK ui")
