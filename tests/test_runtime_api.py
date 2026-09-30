import ctypes
import logging
import os
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

import oreo_runtime.api as api
from oreo_runtime.controller import ControllerComponent
from oreo_runtime.overlay import OverlayComponent

LOG_DIR_ENV = "FISHING_MOD_LOG_DIR"
MOD_DIR_ENV = "FISHING_MOD_DIR"
NATIVE_FILES = ("oreo_overlay.dll", "oreo_controller_support.exe", "SDL2.dll")


def tree(root):
    """Every path under `root`, so a test can say "nothing appeared here"."""
    return sorted(str(p) for p in Path(root).rglob("*"))


class StandInPackage(unittest.TestCase):
    """A Runtime package on disk, with nothing in it that can actually run.

    Almost everything worth testing here happens on the way to loading the overlay and starting
    the controller helper - which folder is named, what is written where, how many times a
    component agrees to start. None of it can be reached without a package that looks complete:
    `start()` gives up at the first missing file, and every assertion below would then pass for
    the wrong reason. So the native files are present and empty, and the three things that would
    make an empty file dangerous are replaced:

      ctypes.WinDLL   - nothing is loaded;
      subprocess.Popen - no helper is started;
      threading.Thread - no pipe loop runs.

    Thread rather than the private `_start_transport` / `_start_pipe_reader` on purpose: those
    methods carry the "only once" guards, and patching them out would hide exactly what
    `test_repeated_start_*` is there to check.
    """

    missing_from_package = ()

    def setUp(self):
        # Several of these tests take a component down a path it is meant to complain about,
        # and the complaint goes to the logger. Nothing here asserts on log output, so silence
        # it: a passing run that prints "Native overlay is missing" reads like a failing one.
        logging.disable(logging.CRITICAL)
        self.addCleanup(logging.disable, logging.NOTSET)

        package_dir = Path(tempfile.mkdtemp(prefix="oreo-runtime-test-"))
        self.addCleanup(shutil.rmtree, package_dir, True)
        binaries = package_dir / "bin"
        binaries.mkdir()
        for name in NATIVE_FILES:
            if name not in self.missing_from_package:
                (binaries / name).write_bytes(b"")
        self.package_dir = package_dir

        # Built before WinDLL is patched: both components load kernel32 in their constructor,
        # and a mocked one would give them a Mock to call CreateFileW on.
        #
        # Fresh instances also keep the tests apart. api's components are module-level
        # singletons that remember having been started, so shared ones would let whichever test
        # ran first decide the outcome of the rest.
        self.overlay = OverlayComponent(package_dir)
        self.controller = ControllerComponent(package_dir)

        patches = [
            mock.patch.object(api, "_overlay", self.overlay),
            mock.patch.object(api, "_controller", self.controller),
            mock.patch("ctypes.WinDLL"),
            mock.patch("subprocess.Popen"),
            mock.patch("threading.Thread"),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

        # Read back off the modules rather than from patch.start(): the tests below ask about
        # the replacement that is actually in place, and this is the same object either way.
        self.win_dll = ctypes.WinDLL
        self.popen = subprocess.Popen
        self.thread = threading.Thread

        # A previous call in this process may have left these behind; every question below is
        # about what start() does, not about what was lying around beforehand.
        for name in (LOG_DIR_ENV, MOD_DIR_ENV):
            self.addCleanup(os.environ.pop, name, None)
            os.environ.pop(name, None)


class RuntimeApiTests(unittest.TestCase):
    def test_import_does_not_start_components(self):
        self.assertFalse(api._overlay._load_attempted)
        self.assertFalse(api._controller._start_attempted)

    def test_start_requests_only_selected_components(self):
        with mock.patch.object(api._overlay, "start", return_value=True) as overlay_start:
            with mock.patch.object(api._controller, "start", return_value=True) as controller_start:
                api.start(overlay=True)
        overlay_start.assert_called_once()
        controller_start.assert_not_called()


class LogDirTests(StandInPackage):
    """The Runtime must never invent a place to write logs.

    It does not open log files itself - it hands a folder to the native overlay and the
    controller helper through the environment. So "no log_dir" has to mean the variable is not
    set at all: a component that finds it missing falls back to its own behaviour, while a
    component handed a guessed path would quietly start writing there.
    """

    def test_start_without_log_dir_names_no_folder(self):
        api.start(overlay=True, controller=True)

        self.assertNotIn(
            LOG_DIR_ENV,
            os.environ,
            "without log_dir the Runtime must leave the log folder unnamed, not guess one",
        )

    def test_start_without_log_dir_writes_nothing(self):
        before_package = tree(self.package_dir)
        # Only the top level of %TEMP%: the stand-in package lives there too, and walking into
        # it would report our own fixture as a change.
        before_temp = sorted(os.listdir(tempfile.gettempdir()))

        api.start(overlay=True, controller=True)

        self.assertEqual(
            before_package,
            tree(self.package_dir),
            "start() created something inside the installed package",
        )
        self.assertEqual(
            before_temp,
            sorted(os.listdir(tempfile.gettempdir())),
            "start() left something in %TEMP% - the copied-DLL habit must not come back",
        )

    def test_start_with_log_dir_passes_it_through_untouched(self):
        # The other half of the same promise: named a folder, the Runtime uses that one. Without
        # this, a component that simply never sets the variable would pass the test above.
        with tempfile.TemporaryDirectory() as chosen:
            api.start(overlay=True, controller=True, log_dir=chosen)

            self.assertEqual(
                os.path.realpath(os.environ[LOG_DIR_ENV]),
                os.path.realpath(chosen),
                "the folder handed to the Runtime is the folder its components should be told",
            )

    def test_start_with_log_dir_still_writes_nowhere_else(self):
        """"And only there" - the half of the promise the pass-through case does not cover.

        Naming a folder is permission to write in that folder, not permission to start writing
        in general. A component that took the log_dir AND kept a second habit somewhere else
        would satisfy every other test in this class.
        """
        before_package = tree(self.package_dir)

        with tempfile.TemporaryDirectory() as chosen:
            # Snapshot inside the block: the chosen folder is itself in %TEMP%, and taking the
            # listing before it exists would report this test's own fixture as a change.
            before_temp = sorted(os.listdir(tempfile.gettempdir()))

            api.start(overlay=True, controller=True, log_dir=chosen)

            self.assertEqual(
                before_package,
                tree(self.package_dir),
                "with a log folder named, start() still wrote inside the installed package",
            )
            self.assertEqual(
                before_temp,
                sorted(os.listdir(tempfile.gettempdir())),
                "with a log folder named, start() still left something in %TEMP%",
            )


class LogSettingsTests(StandInPackage):
    """How much to log is told, not guessed.

    The native side used to read [Logging] out of a fishing_mod.ini it found by gluing a
    hardcoded filename onto the mod folder. That made a component shared by every mod belong to
    exactly one: a second mod would have had to name its settings file fishing_mod.ini, and if
    it did not, the overlay silently read nothing - the path fell back to the game's working
    directory, where no such file has ever existed.

    Whoever starts the Runtime already says where the logs go. These say how big to let them
    get, which costs nothing extra and takes the guessed filename out of the binary.
    """

    def test_the_defaults_are_passed_explicitly(self):
        """Not left unset for the native side to default on its own.

        Two places holding the same default is two places to change it, and the one nobody
        changed wins silently.
        """
        api.start(overlay=True)

        self.assertEqual("10", os.environ["FISHING_MOD_LOG_MAX_MB"])
        self.assertEqual("true", os.environ["FISHING_MOD_LOG_KEEP_PREVIOUS"])
        self.assertEqual("10", os.environ["FISHING_MOD_LOG_TRACE_MINUTES"])

    def test_the_mods_settings_are_passed_through(self):
        api.start(overlay=True, log_mode="debug", log_max_mb=25,
                  log_keep_previous=False, log_trace_minutes=3)

        self.assertEqual("debug", os.environ["FISHING_MOD_LOG_MODE"])
        self.assertEqual("25", os.environ["FISHING_MOD_LOG_MAX_MB"])
        self.assertEqual("false", os.environ["FISHING_MOD_LOG_KEEP_PREVIOUS"])
        self.assertEqual("3", os.environ["FISHING_MOD_LOG_TRACE_MINUTES"])

    def test_nothing_names_a_settings_file(self):
        """The whole point of the change: no variable the native side could turn into a path
        to one mod's ini."""
        api.start(overlay=True, controller=True)

        named = [value for key, value in os.environ.items()
                 if key.startswith("FISHING_MOD") or key.startswith("OREO_RUNTIME")]
        self.assertFalse([value for value in named if value.endswith(".ini")])


class OverlayLoadTests(StandInPackage):
    def test_overlay_is_loaded_from_the_package(self):
        """The DLL loaded must be the one inside the installed package.

        It used to be copied to %TEMP%\\oreo_overlay_run_<pid>_<ms>.dll and loaded from there,
        which left a copy behind on every launch, looked like malware to antivirus software,
        and left a gap in which anything running as this user could swap the file. The fix was
        to load `bin/oreo_overlay.dll` directly - so what this test pins down is the path
        handed to WinDLL, not whether a load succeeded.
        """
        api.start(overlay=True)

        self.win_dll.assert_called_once()
        loaded = Path(self.win_dll.call_args.args[0])
        self.assertEqual(
            loaded.resolve(),
            (self.package_dir / "bin" / "oreo_overlay.dll").resolve(),
            "the overlay must be loaded straight out of the package, never from a copy",
        )

    def test_start_does_not_reach_for_a_temporary_directory(self):
        """Nobody asks the tempfile module where to put things.

        A weak test on its own - the Runtime does not import tempfile today, so there is
        nothing to call. It is here as a tripwire: the copy-to-%TEMP% habit is the one thing
        this component is known to have done wrong, and re-adding it would almost certainly go
        through one of these three functions.
        """
        with mock.patch("tempfile.gettempdir") as gettempdir:
            with mock.patch("tempfile.mkdtemp") as mkdtemp:
                with mock.patch("tempfile.NamedTemporaryFile") as named:
                    api.start(overlay=True, controller=True)

        gettempdir.assert_not_called()
        mkdtemp.assert_not_called()
        named.assert_not_called()

    def test_runtime_sources_do_not_mention_temp(self):
        """The tripwire above only fires on tempfile; this one covers the rest of the ways in.

        Reading source text in a test earns its place exactly once, and this is it: "the
        Runtime never writes to %TEMP%" is an invariant about code that is not supposed to
        exist, and there is no call to intercept when it does not.
        """
        package_root = Path(api.__file__).resolve().parent
        offenders = []
        for module in sorted(package_root.glob("*.py")):
            text = module.read_text(encoding="utf-8")
            for line_number, line in enumerate(text.splitlines(), start=1):
                stripped = line.strip()
                if stripped.startswith("#") or stripped.startswith('"""'):
                    continue  # the history is written down in comments, and must stay readable
                if "tempfile" in line or "gettempdir" in line or '"TEMP"' in line:
                    offenders.append(f"{module.name}:{line_number}: {stripped}")

        self.assertEqual(
            [],
            offenders,
            "the Runtime must not reach for %TEMP% - see the v1 note about the copied DLL",
        )


class MissingNativeFilesTests(StandInPackage):
    """A broken Runtime must not take the mod down with it.

    The mod is a guest in the game's process. If the Runtime cannot find its own native files,
    the honest outcome is a component that reports itself unavailable with a readable reason -
    not an exception travelling up into somebody else's game.
    """

    missing_from_package = NATIVE_FILES

    def test_start_returns_instead_of_raising(self):
        session = api.start(overlay=True, controller=True)

        self.assertIs(session.overlay, self.overlay)
        self.assertIs(session.controller, self.controller)

    def test_missing_overlay_is_reported_not_raised(self):
        api.start(overlay=True)

        self.assertFalse(self.overlay.available)
        self.assertIn(
            "oreo_overlay.dll",
            self.overlay.error,
            "the error has to name the file that is missing, or it explains nothing",
        )

    def test_missing_controller_files_are_reported_not_raised(self):
        api.start(controller=True)

        self.assertFalse(self.controller.available)
        self.assertTrue(self.controller.error, "a component that failed must say why")
        self.popen.assert_not_called()

    def test_overlay_load_failure_is_caught(self):
        """Present but unloadable is a different path from missing, and it is the likelier one.

        A truncated or architecture-mismatched DLL exists on disk, passes the is_file() check,
        and then throws out of WinDLL.
        """
        (self.package_dir / "bin" / "oreo_overlay.dll").write_bytes(b"")
        self.win_dll.side_effect = OSError("not a valid Win32 application")

        api.start(overlay=True)

        self.assertFalse(self.overlay.available)
        self.assertIn("not a valid Win32 application", self.overlay.error)


class RepeatedStartTests(StandInPackage):
    """Components are singletons inside the game process, and start() says repeats are safe.

    Two overlays in one process means two DLLs loaded and two pipe clients competing for the
    same handle; two controller helpers means two processes fighting over one named pipe. The
    guards that prevent it are three separate flags, which is three chances to get it wrong.
    """

    def test_repeated_start_loads_the_overlay_once(self):
        api.start(overlay=True)
        api.start(overlay=True)

        self.assertEqual(
            1,
            self.win_dll.call_count,
            "a second start() loaded the overlay DLL again",
        )

    def test_repeated_start_launches_the_helper_once(self):
        api.start(controller=True)
        api.start(controller=True)

        self.assertEqual(
            1,
            self.popen.call_count,
            "a second start() launched a second controller helper",
        )

    def test_repeated_start_creates_one_thread_per_component(self):
        api.start(overlay=True, controller=True)
        api.start(overlay=True, controller=True)

        # Three: the overlay connects on one thread and listens for what the overlay says back
        # on another, and the controller helper has its own. Whichever way that number moves,
        # it must not move because start() was called twice.
        self.assertEqual(
            3,
            self.thread.call_count,
            "the pipe loops must be started once each, not once per start() call",
        )


class ControllerLineParsingTests(unittest.TestCase):
    """The one piece of the controller path that is a pure function, and worth testing as one.

    Everything the helper says arrives as text through a pipe. A line that is not understood
    must leave the button state alone: a controller does not un-press its buttons because a
    byte went missing.
    """

    def setUp(self):
        # No package and no patches: _parse_line touches nothing but its own state.
        self.controller = ControllerComponent(Path("."))

    def test_btn_line_gives_every_name(self):
        self.controller._parse_line("BTN a,x,up")

        self.assertEqual({"a", "x", "up"}, set(self.controller.buttons))

    def test_btn_line_replaces_the_previous_state(self):
        self.controller._parse_line("BTN a,x")
        self.controller._parse_line("BTN up")

        self.assertEqual({"up"}, set(self.controller.buttons))

    def test_bare_btn_line_means_nothing_is_pressed(self):
        self.controller._parse_line("BTN a,x")
        self.controller._parse_line("BTN")

        self.assertEqual(set(), set(self.controller.buttons))

    def test_empty_line_is_ignored(self):
        self.controller._parse_line("BTN a")
        self.controller._parse_line("")
        self.controller._parse_line("   ")

        self.assertEqual(
            {"a"},
            set(self.controller.buttons),
            "a blank line is noise on the pipe, not a controller with nothing pressed",
        )

    def test_unknown_line_leaves_the_state_alone(self):
        self.controller._parse_line("BTN a")
        self.controller._parse_line("hello")
        self.controller._parse_line("STATUS connected")

        self.assertEqual({"a"}, set(self.controller.buttons))

    def test_btn_prefix_is_matched_loosely(self):
        """Documents what the parser does today, which is not quite what it looks like.

        The check is `startswith("BTN")`, so `BTNX garbage` is accepted as a button line and
        yields one button named "X garbage" rather than being ignored as noise. Harmless in
        practice - the only thing writing to this pipe is our own helper - but it is a
        difference between the code and the sentence "anything unrecognised is ignored", and a
        test that pretends otherwise would be the wrong kind of green. Flagged, not fixed:
        tightening it to `BTN ` or a bare `BTN` is a behaviour change, not a test change.
        """
        self.controller._parse_line("BTNX garbage")

        self.assertEqual({"X garbage"}, set(self.controller.buttons))

    def test_trailing_whitespace_and_empty_names_are_dropped(self):
        self.controller._parse_line("  BTN a,,x,  \n")

        self.assertEqual({"a", "x"}, set(self.controller.buttons))

    def test_is_pressed_answers_for_a_reported_button(self):
        self.controller._parse_line("BTN a,x")

        self.assertTrue(self.controller.is_pressed("a"))
        self.assertFalse(self.controller.is_pressed("b"))


if __name__ == "__main__":
    unittest.main()
