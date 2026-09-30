"""Declarative UI: the mod owns its interface, the Runtime only draws it.

The browser split, applied to a game overlay:

    a JSON file next to the mod   is the HTML/CSS - what exists and how it looks
    the mod, frame by frame       is the JS       - what the values are right now
    the native overlay            is the renderer - it draws and decides nothing

The mod never names a coordinate. It says `fish` is at 0.42; where that lands on screen is
the file's business. This is the whole point: a modder changes the look by editing a file,
without touching code, and the Runtime stops knowing anything about fishing.

Reading the file happens HERE, in Python, not in the overlay. Parsing text in C++ is
miserable, and keeping it on this side means the file format can change without touching the
renderer - only the wire messages below are a contract between the two.

Coordinates inside a screen are fractions of that screen's box, 0..1. The box itself is sized
in pixels and pinned to an edge, so a HUD stays the same physical size on a 4K monitor
instead of shrinking to nothing.
"""

from __future__ import annotations

import json
import logging
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

logger = logging.getLogger(__name__)

# Bumped only when a change would make an older overlay misread a newer file. The Runtime and
# the overlay ship in one wheel, so this exists to fail loudly on a mismatched install, not to
# support old versions.
#
# 2 added widgets. A file still saying 1 loads exactly as it always did - nothing in it changed
# meaning - it just cannot contain a widget, because a file that claims 1 was written before
# they existed and one that has them is lying about its version.
FORMAT_VERSION = 2
READABLE_FORMATS = (1, 2)

# Every shape the renderer knows. Deliberately short: these six draw the entire fishing panel
# today, and a small set that never has to be taken back is worth more than a large one that
# does. Anything not expressible here is what the raw drawing escape hatch is for.
SHAPES = ("text", "rect", "line", "circle", "quad", "diamond")

# Everything the player can press, drag or type into. Drawn by ImGui's own widgets inside the
# overlay; the file says where each one goes and how it looks, the mod hears what was done.
WIDGETS = ("button", "toggle", "slider", "dropdown", "input")

# What each widget can tell the mod. A button is pressed; everything else has a value that
# changes; an input can also be submitted with Enter, which is not the same as being typed into.
EVENTS = {
    "button": ("click",),
    "toggle": ("change",),
    "slider": ("change",),
    "dropdown": ("change",),
    "input": ("change", "submit"),
}

# A widget's look, under our own names. The file is a public format and has to outlive whichever
# library draws it, so nothing here is named after ImGui; the overlay maps these onto it in one
# place. Every one is optional - leave them out and ImGui's own look is used.
STYLE_COLOURS = ("background", "hover", "active", "text", "accent", "border", "popup", "hint",
                 "focus")
STYLE_SIZES = ("radius", "thickness")

# What the player is holding, as the overlay reports it: whichever was touched last. A mod uses
# it to show the right prompts - a pad button or a mouse click.
INPUT_DEVICES = ("gamepad", "mouse_keyboard")

# Names the Runtime itself supplies: what the overlay costs, frame by frame. A file may bind
# them like any other value; a mod may not write them, because the whole point is that these
# are measured rather than claimed. See `RESERVED` in the docs.
RUNTIME_PREFIX = "runtime."


class UiError(ValueError):
    """The UI file is wrong. Always names the path being complained about."""


def _fail(where: str, message: str) -> None:
    raise UiError(f"{where}: {message}")


class _Binding:
    """A field whose value the mod supplies at run time.

    Written in the file as `{"bind": "barY"}`, optionally with `from`/`to` to map the incoming
    0..1 onto part of the screen. Without a binding a field is a plain number and never moves.
    """

    __slots__ = ("name", "low", "high")

    def __init__(self, name: str, low: float, high: float):
        self.name = name
        self.low = low
        self.high = high

    def to_wire(self) -> dict[str, Any]:
        return {"bind": self.name, "from": self.low, "to": self.high}


def _number(raw: Any, where: str) -> float | _Binding:
    if isinstance(raw, bool):  # bool is an int in Python; a colour of `true` is a mistake
        _fail(where, "expected a number or a binding, got a boolean")
    if isinstance(raw, (int, float)):
        return float(raw)
    if isinstance(raw, dict):
        name = raw.get("bind")
        if not isinstance(name, str) or not name:
            _fail(where, "a binding needs a non-empty \"bind\" name")
        low = raw.get("from", 0.0)
        high = raw.get("to", 1.0)
        for label, value in (("from", low), ("to", high)):
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                _fail(where, f"\"{label}\" must be a number")
        return _Binding(name, float(low), float(high))
    _fail(where, f"expected a number or a binding, got {type(raw).__name__}")
    raise AssertionError("unreachable")


def _text_value(raw: Any, where: str) -> str | _Binding:
    if isinstance(raw, str):
        return raw
    if isinstance(raw, dict):
        name = raw.get("bind")
        if not isinstance(name, str) or not name:
            _fail(where, "a binding needs a non-empty \"bind\" name")
        return _Binding(name, 0.0, 1.0)
    _fail(where, f"expected a string or a binding, got {type(raw).__name__}")
    raise AssertionError("unreachable")


def _colour(raw: Any, palette: dict[str, str], where: str) -> str:
    """A CSS-style hex colour, or a name defined in the file's palette.

    `#rgb`, `#rgba`, `#rrggbb` and `#rrggbbaa` are all accepted, because those are the four
    forms anyone who has touched a stylesheet will write without thinking, and refusing the
    short ones buys nothing.

    Resolved here rather than in the overlay so a typo in a colour name is caught when the
    file loads, not silently drawn as black on the first frame that uses it.
    """
    if not isinstance(raw, str):
        _fail(where, f"expected a colour string, got {type(raw).__name__}")
    if raw in palette:
        raw = palette[raw]
    if not raw.startswith("#") or len(raw) not in (4, 5, 7, 9):
        _fail(where, "colour must be #rgb, #rgba, #rrggbb, #rrggbbaa or a palette name, "
                     f"got {raw!r}")
    try:
        int(raw[1:], 16)
    except ValueError:
        _fail(where, f"colour {raw!r} is not hexadecimal")
    return raw


class Element:
    """One drawable thing with a name the mod can address."""

    __slots__ = ("id", "shape", "fields", "visible")

    def __init__(self, element_id: str, shape: str, fields: dict[str, Any], visible: bool):
        self.id = element_id
        self.shape = shape
        self.fields = fields
        self.visible = visible

    def to_wire(self) -> dict[str, Any]:
        fields = {
            key: value.to_wire() if isinstance(value, _Binding) else value
            for key, value in self.fields.items()
        }
        wire = {"shape": self.shape, "visible": self.visible, **fields}
        if self.id:
            wire = {"id": self.id, **wire}
        return wire

    def bindings(self) -> list[str]:
        return [v.name for v in self.fields.values() if isinstance(v, _Binding)]


class Widget:
    """Something the player can use, with a name the mod listens to.

    Lives in a screen's `elements` alongside the shapes, in file order, which is also drawing
    order. `fields` always carries a `value` for widgets that hold one - the file's, or the
    default for that kind - so the overlay never has to guess what a widget starts at.
    """

    __slots__ = ("id", "kind", "fields", "visible", "style")

    def __init__(self, widget_id: str, kind: str, fields: dict[str, Any], visible: bool,
                 style: dict[str, Any]):
        self.id = widget_id
        self.kind = kind
        self.fields = fields
        self.visible = visible
        self.style = style

    @property
    def default(self) -> Any:
        """The value the file starts this widget at; None for a button, which holds none."""
        return self.fields.get("value")

    def to_wire(self) -> dict[str, Any]:
        fields = {
            key: value.to_wire() if isinstance(value, _Binding) else value
            for key, value in self.fields.items()
        }
        wire = {"id": self.id, "widget": self.kind, "visible": self.visible, **fields}
        if self.style:
            wire["style"] = dict(self.style)
        return wire

    def bindings(self) -> list[str]:
        return [v.name for v in self.fields.values() if isinstance(v, _Binding)]


class Screen:
    """A box pinned to a screen edge, holding elements.

    Screens exist from the start even though the fishing mod uses exactly one. An inventory
    and a map are separate screens, and a format that only ever knew about a single panel
    would have to be broken to add the second.
    """

    __slots__ = ("id", "size", "anchor", "valign", "inset", "elements", "visible")

    ANCHORS = ("left", "right", "center")
    # Vertical pin, independent of the horizontal one. Defaults to "center" - every screen
    # before this field existed centred vertically, and a document that never mentions it must
    # keep doing exactly that.
    VALIGNS = ("top", "center", "bottom")

    def __init__(self, screen_id: str, size, anchor: str, valign: str, inset, elements,
                visible: bool):
        self.id = screen_id
        self.size = size
        self.anchor = anchor
        self.valign = valign
        self.inset = inset
        self.elements = elements
        self.visible = visible

    def to_wire(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "size": list(self.size),
            "anchor": self.anchor,
            "valign": self.valign,
            "inset": list(self.inset),
            "visible": self.visible,
            "elements": [e.to_wire() for e in self.elements],
        }


class UiDocument:
    """A loaded, validated UI file, ready to be handed to the overlay."""

    __slots__ = ("screens", "path", "source", "palettes", "palette")

    def __init__(self, screens: list[Screen], path: str | None = None,
                 source: dict[str, Any] | None = None,
                 palettes: tuple[str, ...] = (), palette: str | None = None):
        self.screens = screens
        self.path = path
        # The document as it was parsed, kept so another palette can be resolved out of it
        # without going back to disk - the file may have been swapped or removed since.
        self.source = source
        self.palettes = palettes
        self.palette = palette

    def to_wire(self) -> dict[str, Any]:
        return {"format": FORMAT_VERSION, "screens": [s.to_wire() for s in self.screens]}

    def names(self) -> set[str]:
        """Every id the mod may address, screens and elements alike."""
        found = set()
        for screen in self.screens:
            found.add(screen.id)
            for element in screen.elements:
                found.add(element.id)
        return found

    def screen_names(self) -> set[str]:
        return {screen.id for screen in self.screens}

    def widgets(self) -> dict[str, Widget]:
        return {
            element.id: element
            for screen in self.screens
            for element in screen.elements
            if isinstance(element, Widget)
        }

    def bindings(self) -> set[str]:
        found = set()
        for screen in self.screens:
            for element in screen.elements:
                found.update(element.bindings())
        return found


# Which fields each shape takes, and which are required. Kept as data so adding a shape is one
# row here plus one branch in the renderer, and so the error messages can name what is missing.
_SHAPE_FIELDS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    #            required                          optional
    "text":    (("x", "y", "text", "color"),      ("size",)),
    "rect":    (("x", "y", "w", "h"),             ("fill", "border", "radius", "thickness")),
    "line":    (("x", "y", "x2", "y2", "color"),  ("thickness",)),
    "circle":  (("x", "y", "r"),                  ("fill", "border", "thickness")),
    "quad":    (("x", "y", "x2", "y2", "x3", "y3", "x4", "y4"), ("fill", "border", "thickness")),
    "diamond": (("x", "y", "r"),                  ("fill", "border", "thickness")),
}

_COLOUR_FIELDS = ("color", "fill", "border")

# What (x, y) means for a rectangle. Bars are the reason this exists, and both extra origins
# were found by describing the real fishing panel rather than imagined in advance:
#
#   center       a catch bar is described by its centre and its height, and the mod moves both.
#                "top-left" would make it work out `y - h / 2`;
#   bottom-left  a progress column is pinned at the bottom and grows upward. "top-left" would
#                make it send `1 - progress` as well as `progress`.
#
# Either way the mod ends up doing arithmetic on a coordinate, and at that moment layout has
# leaked back out of the file - which is the whole thing this design exists to prevent.
#
# Circles and diamonds are always centred, so they take no origin.
_ORIGINS = ("top-left", "center", "bottom-left")


# Which fields each widget takes. `x`, `y` and `w` place every one of them; the rest are what
# makes that kind of widget what it is. Kept as data for the same reason the shapes are.
_WIDGET_FIELDS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    #             required                            optional
    "button":   (("x", "y", "w", "label"),           ()),
    "toggle":   (("x", "y", "w"),                    ("label", "value")),
    "slider":   (("x", "y", "w", "min", "max"),      ("value", "step", "decimals", "suffix")),
    "dropdown": (("x", "y", "w", "options"),         ("value",)),
    "input":    (("x", "y", "w"),                    ("value", "hint", "maxLength")),
}
# Every widget may also say these.
_WIDGET_COMMON = ("h", "size", "enabled", "style")
_WIDGET_TEXT_FIELDS = ("label", "hint", "suffix")

MAX_INPUT_LENGTH = 4096
DEFAULT_INPUT_LENGTH = 256


def _plain_number(raw: Any, where: str) -> float:
    """A number that cannot be a binding: a range, a step, a length."""
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        _fail(where, "expected a number")
    return float(raw)


def _whole_number(raw: Any, where: str, low: int, high: int) -> int:
    if isinstance(raw, bool) or not isinstance(raw, int) or not low <= raw <= high:
        _fail(where, f"expected a whole number from {low} to {high}")
    return raw


def _utf8_cut(text: str, limit: int) -> str:
    """At most `limit` bytes of UTF-8, never half a character.

    The same cut the overlay makes, so both sides agree on what an over-long value became.
    """
    data = text.encode("utf-8")
    if len(data) <= limit:
        return text
    return data[:limit].decode("utf-8", "ignore")


def _load_style(raw: Any, palette: dict[str, str], where: str) -> dict[str, Any]:
    if not isinstance(raw, dict):
        _fail(where, "a style must be an object")
    style: dict[str, Any] = {}
    for key, value in raw.items():
        spot = f"{where}/{key}"
        if key in STYLE_COLOURS:
            style[key] = _colour(value, palette, spot)
        elif key in STYLE_SIZES:
            size = _plain_number(value, spot)
            if size < 0:
                _fail(spot, "must not be negative")
            style[key] = size
        else:
            _fail(spot, f"a style has no {key!r}; it knows "
                        f"{', '.join(STYLE_COLOURS + STYLE_SIZES)}")
    return style


def _load_widget(raw: dict[str, Any], palette: dict[str, str], where: str,
                 widget_style: dict[str, Any]) -> Widget:
    widget_id = raw["id"]
    kind = raw.get("widget")
    here = f"{where}/{widget_id}"
    if kind not in WIDGETS:
        _fail(here, f"unknown widget {kind!r}; known widgets are {', '.join(WIDGETS)}")
    # Ids travel on the wire between spaces - "EV change volume 0.5" - so a space in one would
    # split it in two. Shapes never went through this and are left as they were.
    if any(c.isspace() for c in widget_id):
        _fail(here, "a widget id cannot contain spaces")

    required, optional = _WIDGET_FIELDS[kind]
    for key in required:
        if key not in raw:
            _fail(here, f"a {kind} needs \"{key}\"")
    unknown = set(raw) - {"id", "widget", "visible"} - set(required + optional + _WIDGET_COMMON)
    if unknown:
        _fail(here, f"a {kind} has no field(s) {', '.join(sorted(unknown))}")

    fields: dict[str, Any] = {}
    for key in ("x", "y", "w", "h"):
        if key in raw:
            fields[key] = _number(raw[key], f"{here}/{key}")
    for key in _WIDGET_TEXT_FIELDS:
        if key in raw:
            if not isinstance(raw[key], str):
                _fail(f"{here}/{key}", "expected a string")
            fields[key] = raw[key]
    if "size" in raw:
        size = _plain_number(raw["size"], f"{here}/size")
        if size <= 0:
            _fail(f"{here}/size", "must be above zero")
        fields["size"] = size
    if "enabled" in raw:
        if not isinstance(raw["enabled"], bool):
            _fail(f"{here}/enabled", "must be true or false")
        fields["enabled"] = raw["enabled"]

    value = raw.get("value")
    if kind == "toggle":
        if value is None:
            value = False
        elif not isinstance(value, bool):
            _fail(f"{here}/value", "a toggle's value is true or false")
        fields["value"] = value
    elif kind == "slider":
        low = _plain_number(raw["min"], f"{here}/min")
        high = _plain_number(raw["max"], f"{here}/max")
        if not low < high:
            _fail(here, "\"min\" must be below \"max\"")
        fields["min"], fields["max"] = low, high
        if "step" in raw:
            step = _plain_number(raw["step"], f"{here}/step")
            if step <= 0:
                _fail(f"{here}/step", "must be above zero")
            fields["step"] = step
        if "decimals" in raw:
            fields["decimals"] = _whole_number(raw["decimals"], f"{here}/decimals", 0, 6)
        value = low if value is None else _plain_number(value, f"{here}/value")
        if not low <= value <= high:
            _fail(f"{here}/value", f"must be between {low:g} and {high:g}")
        fields["value"] = value
    elif kind == "dropdown":
        fields["options"] = _options(raw["options"], f"{here}/options")
        if value is None:
            value = fields["options"][0]
        elif value not in fields["options"]:
            _fail(f"{here}/value", f"{value!r} is not one of the options")
        fields["value"] = value
    elif kind == "input":
        limit = DEFAULT_INPUT_LENGTH
        if "maxLength" in raw:
            limit = _whole_number(raw["maxLength"], f"{here}/maxLength", 1, MAX_INPUT_LENGTH)
        fields["maxLength"] = limit
        if value is None:
            value = ""
        elif not isinstance(value, str):
            _fail(f"{here}/value", "an input's value is a string")
        elif len(value.encode("utf-8")) > limit:
            _fail(f"{here}/value", f"is longer than maxLength ({limit} bytes)")
        fields["value"] = value

    style = dict(widget_style)
    if "style" in raw:
        style.update(_load_style(raw["style"], palette, f"{here}/style"))

    visible = raw.get("visible", True)
    if not isinstance(visible, bool):
        _fail(here, "\"visible\" must be true or false")
    return Widget(widget_id, kind, fields, visible, style)


def _options(raw: Any, where: str) -> list[str]:
    if not isinstance(raw, list) or not raw:
        _fail(where, "expected a non-empty list of strings")
    if not all(isinstance(option, str) and option for option in raw):
        _fail(where, "every option must be a non-empty string")
    if len(set(raw)) != len(raw):
        _fail(where, "options must not repeat; the mod is told which one was picked by its text")
    return list(raw)


def _load_element(raw: Any, palette: dict[str, str], where: str,
                  seen: set[str] | None,
                  widget_style: dict[str, Any] | None = None) -> Element | Widget:
    """One shape or widget. `seen` is None for escape-hatch shapes, which carry no id to collide.

    `widget_style` is the file's `widgetStyle`, what every widget looks like unless it says
    otherwise.
    """
    if not isinstance(raw, dict):
        _fail(where, f"an element must be an object, got {type(raw).__name__}")
    element_id = raw.get("id", "")
    if seen is not None:
        if not isinstance(element_id, str) or not element_id:
            _fail(where, "every element needs a non-empty \"id\" - it is how the mod addresses it")
        if element_id in seen:
            _fail(where, f"duplicate id {element_id!r}; the mod could not tell the two apart")
        seen.add(element_id)
    elif not isinstance(element_id, str):
        _fail(where, "\"id\" must be a string")

    if "widget" in raw:
        if seen is None:
            # A widget drawn by hand would vanish, value and all, the next time the mod redrew
            # the layer. Controls belong in the file, where they persist.
            _fail(f"{where}/{element_id or '?'}",
                  "widgets cannot be drawn through the escape hatch; describe them in the file")
        if "shape" in raw:
            _fail(f"{where}/{element_id}", "an element is a shape or a widget, not both")
        return _load_widget(raw, palette, where, widget_style or {})

    shape = raw.get("shape")
    if shape not in SHAPES:
        _fail(f"{where}/{element_id or '?'}",
              f"unknown shape {shape!r}; known shapes are {', '.join(SHAPES)}")

    required, optional = _SHAPE_FIELDS[shape]
    fields: dict[str, Any] = {}
    for key in required:
        if key not in raw:
            _fail(f"{where}/{element_id or '?'}", f"a {shape} needs \"{key}\"")
    for key in required + optional:
        if key not in raw:
            continue
        spot = f"{where}/{element_id or '?'}/{key}"
        if key == "text":
            fields[key] = _text_value(raw[key], spot)
        elif key in _COLOUR_FIELDS:
            fields[key] = _colour(raw[key], palette, spot)
        else:
            fields[key] = _number(raw[key], spot)

    if shape == "rect":
        origin = raw.get("origin", "top-left")
        if origin not in _ORIGINS:
            _fail(f"{where}/{element_id or '?'}",
                  f"origin must be one of {', '.join(_ORIGINS)}, got {origin!r}")
        fields["origin"] = origin

    unknown = set(raw) - {"id", "shape", "visible"} - set(required) - set(optional)
    if shape == "rect":
        unknown -= {"origin"}
    if unknown:
        _fail(f"{where}/{element_id or '?'}",
              f"a {shape} has no field(s) {', '.join(sorted(unknown))}")

    visible = raw.get("visible", True)
    if not isinstance(visible, bool):
        _fail(f"{where}/{element_id or '?'}", "\"visible\" must be true or false")
    return Element(element_id, shape, fields, visible)


def _pair(raw: Any, where: str, default=None):
    if raw is None and default is not None:
        return default
    if not isinstance(raw, (list, tuple)) or len(raw) != 2:
        _fail(where, "expected two numbers, like [400, 600]")
    for value in raw:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            _fail(where, "expected two numbers")
    return (float(raw[0]), float(raw[1]))


def _read_palettes(raw: dict[str, Any], where: str, wanted: str | None):
    """Work out the colour names in force, and which named theme they came from.

    Two keys, and they stack:

        "palette":  {"accent": "#6cf"}                 colours shared by every theme
        "palettes": {"blue": {...}, "sand": {...}}     one entry per theme, layered on top

    A file with only `palette` has one look and no themes, which is what the fishing panel
    shipped with. A file with `palettes` can be switched whole at run time - `ui.palette("sand")`
    - rather than by making every individual colour a binding. Themes change together or not at
    all, so switching the set is the honest unit; forty separate colour bindings would be forty
    chances to leave one behind.

    Every named theme must define exactly the same colour names. Otherwise switching to the one
    that forgot `accent` fails at load, and a mod that has already started is a mod whose panel
    goes blank in front of the player.
    """
    base = raw.get("palette", {})
    if not isinstance(base, dict):
        raise UiError(f"{where}: \"palette\" must be an object of name -> colour")
    for name, value in base.items():
        if not isinstance(value, str):
            raise UiError(f"{where}: palette entry {name!r} must be a colour string")

    themes = raw.get("palettes")
    if themes is None:
        if wanted is not None:
            raise UiError(f"{where}: asked for palette {wanted!r} but the file defines none")
        return dict(base), (), None
    if not isinstance(themes, dict) or not themes:
        raise UiError(f"{where}: \"palettes\" must be a non-empty object of name -> palette")

    expected: set[str] | None = None
    for theme_name, colours in themes.items():
        if not isinstance(colours, dict):
            raise UiError(f"{where}: palette {theme_name!r} must be an object of name -> colour")
        for name, value in colours.items():
            if not isinstance(value, str):
                raise UiError(
                    f"{where}: palette {theme_name!r} entry {name!r} must be a colour string")
        if expected is None:
            expected = set(colours)
        elif set(colours) != expected:
            missing = sorted(expected - set(colours)) or sorted(set(colours) - expected)
            raise UiError(
                f"{where}: palette {theme_name!r} does not define the same colours as the "
                f"others; they differ by {', '.join(missing)}. Every palette must define the "
                "same names, or switching to this one would leave a colour undefined")

    order = list(themes)
    active = wanted if wanted is not None else raw.get("defaultPalette", order[0])
    if not isinstance(active, str) or active not in themes:
        raise UiError(f"{where}: no palette named {active!r}; the file has "
                      f"{', '.join(repr(n) for n in order)}")
    resolved = dict(base)
    resolved.update(themes[active])
    return resolved, tuple(order), active


def loads(text: str, path: str | None = None, palette: str | None = None) -> UiDocument:
    """Parse and validate a UI document.

    Everything is checked here, at load, rather than on the first frame that happens to use a
    broken element - a mod that starts is far easier to debug than one that draws nothing
    twenty minutes in.

    `palette` names which of the file's themes to resolve colours from; the file's default is
    used when it is not given.
    """
    try:
        raw = json.loads(text)
    except json.JSONDecodeError as exc:
        raise UiError(f"{path or 'ui'}: not valid JSON - {exc}") from exc
    if not isinstance(raw, dict):
        raise UiError(f"{path or 'ui'}: the document must be an object")
    return _build(raw, path, palette)


def _build(raw: dict[str, Any], path: str | None, palette: str | None) -> UiDocument:
    where = path or "ui"
    version = raw.get("format")
    if isinstance(version, bool) or version not in READABLE_FORMATS:
        raise UiError(f"{where}: \"format\" is {version!r}, this Runtime reads "
                      f"{', '.join(str(v) for v in READABLE_FORMATS)}")

    colours, palette_names, active = _read_palettes(raw, where, palette)

    widget_style: dict[str, Any] = {}
    if "widgetStyle" in raw:
        widget_style = _load_style(raw["widgetStyle"], colours, f"{where}/widgetStyle")

    screens_raw = raw.get("screens")
    if not isinstance(screens_raw, list) or not screens_raw:
        raise UiError(f"{where}: \"screens\" must be a non-empty list")

    screens: list[Screen] = []
    seen_ids: set[str] = set()
    for index, screen_raw in enumerate(screens_raw):
        spot = f"{where}/screens[{index}]"
        if not isinstance(screen_raw, dict):
            _fail(spot, "a screen must be an object")
        screen_id = screen_raw.get("id")
        if not isinstance(screen_id, str) or not screen_id:
            _fail(spot, "every screen needs a non-empty \"id\"")
        if screen_id in seen_ids:
            _fail(spot, f"duplicate id {screen_id!r}")
        seen_ids.add(screen_id)

        anchor = screen_raw.get("anchor", "right")
        if anchor not in Screen.ANCHORS:
            _fail(f"{spot}/{screen_id}",
                  f"anchor must be one of {', '.join(Screen.ANCHORS)}, got {anchor!r}")
        valign = screen_raw.get("valign", "center")
        if valign not in Screen.VALIGNS:
            _fail(f"{spot}/{screen_id}",
                  f"valign must be one of {', '.join(Screen.VALIGNS)}, got {valign!r}")

        size = _pair(screen_raw.get("size"), f"{spot}/{screen_id}/size")
        inset = _pair(screen_raw.get("inset"), f"{spot}/{screen_id}/inset", default=(0.0, 0.0))

        elements_raw = screen_raw.get("elements", [])
        if not isinstance(elements_raw, list):
            _fail(f"{spot}/{screen_id}", "\"elements\" must be a list")
        elements = [
            _load_element(e, colours, f"{spot}/{screen_id}/elements[{i}]", seen_ids,
                          widget_style)
            for i, e in enumerate(elements_raw)
        ]
        if version < 2 and any(isinstance(e, Widget) for e in elements):
            _fail(f"{spot}/{screen_id}", "widgets need \"format\": 2")

        visible = screen_raw.get("visible", False)
        if not isinstance(visible, bool):
            _fail(f"{spot}/{screen_id}", "\"visible\" must be true or false")
        screens.append(Screen(screen_id, size, anchor, valign, inset, elements, visible))

    return UiDocument(screens, path, raw, palette_names, active)


def load(path: str | Path) -> UiDocument:
    path = Path(path)
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise UiError(f"{path}: cannot be read - {exc}") from exc
    return loads(text, str(path))


# ---- the wire ---------------------------------------------------------------------------
# Newline-terminated messages, mod -> overlay. This, not the file, is what the renderer has to
# understand, and it is deliberately small.
#
#   UI <json>            the whole document; sent on connect and on every reconnect
#   SET <name> <number>  a bound value moved
#   TXT <name> <text>    a bound string changed
#   VIS <name> <0|1>     an element or screen was shown or hidden
#   POS <screen> <x> <y> <anchor>   a screen moved
#   RAW <screen> <json>  free-hand shapes drawn above that screen
#   VAL <id> <json>      a widget's value: true, 0.5, "text"
#   OPT <id> <json>      a dropdown's choices
#   ENA <id> <0|1>       a widget can or cannot be used
#   PAD <a,up,...>       the gamepad buttons held; sent by the Runtime itself, never by a mod
#
# And back, overlay -> mod: HELLO on connect, OK/ERR for the two messages a mod does not send
# every frame, and what the player did:
#
#   EV click <id>           a button was pressed
#   EV change <id> <json>   a toggle, slider, dropdown or input has a new value
#   EV submit <id> <json>   Enter was pressed in an input
#   INPUT <device>          the player switched to "gamepad" or "mouse_keyboard"; also sent on
#                           every connect, once the player has touched anything at all
#
# Numbers are formatted with repr(float), which is locale-independent by definition in Python.
# The overlay must parse them the same way: strtod under a Russian locale wants a comma and
# would read "0.42" as 0, which is the kind of bug that only appears on someone else's
# machine.


def encode_document(document: UiDocument) -> str:
    return "UI " + json.dumps(document.to_wire(), ensure_ascii=False, separators=(",", ":"))


def encode_set(name: str, value: float) -> str:
    return f"SET {name} {float(value)!r}"


def encode_text(name: str, value: str) -> str:
    # A newline in the text would be read as the end of the message and the rest as a command.
    return "TXT {} {}".format(name, value.replace("\r", " ").replace("\n", " "))


def encode_visible(name: str, visible: bool) -> str:
    return f"VIS {name} {1 if visible else 0}"


def encode_place(screen: str, inset_x: float, inset_y: float, anchor: str) -> str:
    return f"POS {screen} {float(inset_x)!r} {float(inset_y)!r} {anchor}"


def encode_raw(screen: str, shapes: list[Element]) -> str:
    payload = json.dumps([shape.to_wire() for shape in shapes],
                         ensure_ascii=False, separators=(",", ":"))
    return f"RAW {screen} {payload}"


def _json(value: Any) -> str:
    # JSON rather than a bare word: a string may hold spaces and newlines, and json.dumps escapes
    # the newline that would otherwise end the message. Floats go through repr, which does not
    # follow the locale.
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def encode_value(widget: str, value: Any) -> str:
    return f"VAL {widget} {_json(value)}"


def encode_options(widget: str, options: list[str]) -> str:
    return f"OPT {widget} {_json(options)}"


def encode_enabled(widget: str, enabled: bool) -> str:
    return f"ENA {widget} {1 if enabled else 0}"


def encode_pad(buttons) -> str:
    """The gamepad buttons held right now, as the controller helper names them.

    Sorted so the same set is always the same line.
    """
    return "PAD " + ",".join(sorted(buttons))


@dataclass(frozen=True)
class UiEvent:
    """Something the player did to a widget.

    `kind` is "click", "change" or "submit"; `id` names the widget; `value` is what it holds
    now - None for a click, which carries nothing.
    """

    kind: str
    id: str
    value: Any = None


def decode_input(line: str) -> str | None:
    """The device named by an `INPUT ...` line, or None when the line is not one.

    Raises ValueError for a device this Runtime does not know.
    """
    if not line.startswith("INPUT "):
        return None
    device = line[6:].strip()
    if device not in INPUT_DEVICES:
        raise ValueError(f"unknown input device: {device[:40]!r}")
    return device


def decode_event(line: str) -> UiEvent | None:
    """An `EV ...` line from the overlay, or None when the line is not an event.

    Raises ValueError for an event line that does not parse; the caller decides how loudly.
    """
    if not line.startswith("EV "):
        return None
    parts = line.split(" ", 3)
    if len(parts) < 3 or not parts[1] or not parts[2]:
        raise ValueError(f"malformed event: {line[:80]!r}")
    value = json.loads(parts[3]) if len(parts) == 4 else None
    return UiEvent(parts[1], parts[2], value)


class UiHandle:
    """What the mod holds: names in, drawing out.

    Every call is cheap and does not block. Values are collected and flushed as one write per
    frame rather than one per change: the mod runs inside the game process under the GIL, and
    a pipe write that blocks stalls a frame the player is looking at.
    """

    def __init__(self, document: UiDocument, send: Callable[[str], bool]):
        self._document = document
        self._send = send
        self._lock = threading.RLock()
        self._pending: dict[str, str] = {}
        self._known = document.names()
        self._screens = document.screen_names()
        self._bindings = document.bindings()
        self._unknown_reported: set[str] = set()
        # Everything the mod has changed since the document was loaded. The overlay keeps the
        # document in memory only, so a reconnect - or a palette switch, which reloads it -
        # would otherwise put the panel back to the file's defaults with the player watching.
        # Replaying is the Runtime's job rather than every mod's.
        self._shown: dict[str, bool] = {}
        self._placed: dict[str, tuple[float, float, str]] = {}
        self._raw: dict[str, list[Element]] = {}

        # Widgets. The value of each is kept here as well as in the overlay, and this copy is
        # the one the mod reads: it is updated from the mod's own set_value() and from the
        # player's events, and replayed after every document, so the two cannot drift apart.
        self._widgets = document.widgets()
        self._values: dict[str, Any] = {
            wid: w.default for wid, w in self._widgets.items() if w.kind != "button"}
        self._options: dict[str, list[str]] = {}
        self._enabled: dict[str, bool] = {}
        self._handlers: dict[tuple[str, str], list[Callable[[UiEvent], None]]] = {}
        # Events arrive on the pipe's reader thread and wait here for the mod's own thread -
        # see flush(). A mod's handler must never run somewhere the mod did not expect it.
        self._inbox: list[UiEvent] = []
        self._inbox_lock = threading.Lock()
        # The device the player last used, as of the last flush(); None until they touch one.
        self._input: str | None = None
        self._input_handlers: list[Callable[[str], None]] = []

    @property
    def document(self) -> UiDocument:
        return self._document

    @property
    def palette(self) -> str | None:
        """Which of the file's palettes is in force, or None for a file without themes."""
        return self._document.palette

    @property
    def palettes(self) -> tuple[str, ...]:
        return self._document.palettes

    def _queue(self, key: str, message: str) -> None:
        with self._lock:
            # Keyed, so a value set five times in one frame is sent once, with its last value.
            self._pending[key] = message

    def _warn_once(self, name: str, message: str, *args) -> None:
        # Warned once per name, not once per frame: a typo at 60 Hz would bury the log.
        if name not in self._unknown_reported:
            self._unknown_reported.add(name)
            logger.warning(message, *args)

    def _check(self, name: str, pool: set[str], what: str) -> bool:
        if name in pool:
            return True
        self._warn_once(name, "UI: no %s named %r in %s", what, name,
                        self._document.path or "the UI document")
        return False

    def _writable(self, name: str) -> bool:
        """`runtime.*` is measured by the Runtime, not claimed by a mod.

        A file is free to bind these names and draw them; letting a mod write them as well
        would mean a panel could show whatever frame cost it liked, which is the one thing a
        cost readout must not be able to do.
        """
        if not name.startswith(RUNTIME_PREFIX):
            return True
        self._warn_once(name, "UI: %r is supplied by the Runtime and cannot be set by a mod",
                        name)
        return False

    def set(self, name: str, value: float) -> None:
        """Move a bound value. `ui.set("fish", 0.42)`."""
        if self._writable(name) and self._check(name, self._bindings, "binding"):
            self._queue("set:" + name, encode_set(name, value))

    def text(self, name: str, value: str) -> None:
        """Change a bound string."""
        if self._writable(name) and self._check(name, self._bindings, "binding"):
            self._queue("txt:" + name, encode_text(name, value))

    def show(self, name: str, visible: bool = True) -> None:
        """Show or hide a screen or a single element, by id."""
        if self._check(name, self._known, "screen or element"):
            with self._lock:
                self._shown[name] = visible
            self._queue("vis:" + name, encode_visible(name, visible))

    def hide(self, name: str) -> None:
        self.show(name, False)

    def place(self, screen: str, inset: tuple[float, float] | None = None,
              anchor: str | None = None) -> None:
        """Move a screen's box, without touching anything inside it.

        This is what a "move the panel" hotkey ends up calling. The mod reads the key and
        remembers the position in its own settings; the file still owns the layout. Where a HUD
        sits depends on the player's monitor, which is not something a file shipped with a mod
        can know.
        """
        if not self._check(screen, self._screens, "screen"):
            return
        current = self._placed.get(screen)
        if current is None:
            described = next(s for s in self._document.screens if s.id == screen)
            current = (described.inset[0], described.inset[1], described.anchor)
        inset_x, inset_y = (current[0], current[1]) if inset is None else inset
        chosen = current[2] if anchor is None else anchor
        if chosen not in Screen.ANCHORS:
            self._warn_once("anchor:" + str(chosen),
                            "UI: anchor must be one of %s, got %r",
                            ", ".join(Screen.ANCHORS), chosen)
            return
        with self._lock:
            self._placed[screen] = (float(inset_x), float(inset_y), chosen)
        self._queue("pos:" + screen, encode_place(screen, inset_x, inset_y, chosen))

    def draw(self, screen: str, shapes: list[dict[str, Any]]) -> None:
        """The escape hatch: shapes sent straight through, drawn above the described ones.

        For what a file cannot say - a plotted curve, a scribble over a debug build, anything
        whose count is not known until it runs. Same six shapes, same fractions of the same
        screen box, checked the same way; what it skips is having to name each shape in the
        file first.

        It replaces whatever was drawn there last; an empty list clears it.

        Second in the documentation on purpose. Handed to a modder alongside the file rather
        than after it, this is the one they would use, and they would never find out that an
        interface here can be a file at all.
        """
        if not self._check(screen, self._screens, "screen"):
            return
        colours, _, _ = _read_palettes(self._document.source or {},
                                       self._document.path or "ui", self._document.palette)
        parsed = [
            _load_element(shape, colours, f"{self._document.path or 'ui'}/raw[{i}]", None)
            for i, shape in enumerate(shapes)
        ]
        with self._lock:
            self._raw[screen] = parsed
        self._queue("raw:" + screen, encode_raw(screen, parsed))

    def use_palette(self, name: str) -> None:
        """Switch the whole colour scheme.

        Resolved here and the document resent, rather than making every colour a binding the
        overlay would have to look up per shape per frame. A theme changes when a player
        presses a key, which is roughly never in frame terms, so paying a few kilobytes then
        costs less than paying a lookup always.
        """
        if name == self._document.palette:
            return
        if name not in self._document.palettes:
            self._warn_once("palette:" + name, "UI: no palette named %r; the file has %s",
                            name, ", ".join(self._document.palettes) or "none")
            return
        rebuilt = _build(self._document.source or {}, self._document.path, name)
        with self._lock:
            self._document = rebuilt
            self._known = rebuilt.names()
            self._screens = rebuilt.screen_names()
            self._bindings = rebuilt.bindings()
            # Same widgets, new colours. The values are the player's and stay.
            self._widgets = rebuilt.widgets()
        self.resend_document()

    # ---- widgets ------------------------------------------------------------------------

    def _widget(self, widget_id: str) -> Widget | None:
        widget = self._widgets.get(widget_id)
        if widget is None:
            self._warn_once(widget_id, "UI: no widget named %r in %s", widget_id,
                            self._document.path or "the UI document")
        return widget

    def on(self, kind: str, widget_id: str, callback: Callable[[UiEvent], None]) -> None:
        """Call `callback(event)` when the player does `kind` to a widget.

            ui.on("click", "save", lambda e: save())
            ui.on("change", "volume", lambda e: set_volume(e.value))

        Handlers run inside flush(), on the thread that calls it - the mod's own - never on the
        pipe's. Raises for a widget the file does not have or an event it cannot send: this is
        set up once, at start, where a typo should stop the mod rather than leave a button that
        silently does nothing.
        """
        widget = self._widgets.get(widget_id)
        if widget is None:
            raise UiError(f"{self._document.path or 'ui'}: no widget named {widget_id!r}")
        if kind not in EVENTS[widget.kind]:
            raise UiError(f"{self._document.path or 'ui'}: a {widget.kind} sends "
                          f"{' or '.join(EVENTS[widget.kind])}, not {kind!r}")
        with self._lock:
            self._handlers.setdefault((kind, widget_id), []).append(callback)

    def value(self, widget_id: str) -> Any:
        """What a widget holds now: a bool, a float or a string. None for a button."""
        if self._widget(widget_id) is None:
            return None
        with self._lock:
            return self._values.get(widget_id)

    def set_value(self, widget_id: str, value: Any) -> None:
        """Change what a widget holds, as if the player had.

        A slider's number is kept inside its range and an input's text cut to its length, the
        same as the overlay would. Ignored by the overlay while the player is holding that very
        widget - their hand wins - and the next event from it says where they left it.
        """
        widget = self._widget(widget_id)
        if widget is None:
            return
        checked = self._checked_value(widget, value)
        if checked is None:
            return
        with self._lock:
            self._values[widget_id] = checked
        self._queue("val:" + widget_id, encode_value(widget_id, checked))

    def _checked_value(self, widget: Widget, value: Any) -> Any:
        wrong = f"UI: {widget.kind} {widget.id!r} cannot hold {value!r}"
        if widget.kind == "button":
            self._warn_once("button:" + widget.id, "UI: button %r holds no value", widget.id)
            return None
        if widget.kind == "toggle":
            if isinstance(value, bool):
                return value
        elif widget.kind == "slider":
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return min(max(float(value), widget.fields["min"]), widget.fields["max"])
        elif widget.kind == "dropdown":
            if isinstance(value, str):
                with self._lock:
                    options = self._options.get(widget.id, widget.fields["options"])
                if value in options:
                    return value
                wrong = f"UI: {value!r} is not one of the options of dropdown {widget.id!r}"
        elif widget.kind == "input":
            if isinstance(value, str):
                return _utf8_cut(value, widget.fields["maxLength"])
        self._warn_once("value:" + widget.id, wrong)
        return None

    def options(self, widget_id: str, options: list[str]) -> None:
        """Replace a dropdown's choices.

        What it holds is left alone even when it is no longer among them: whether that means
        "pick the first" or "keep showing the old one" is the mod's decision, made with
        set_value().
        """
        widget = self._widget(widget_id)
        if widget is None:
            return
        if widget.kind != "dropdown":
            self._warn_once("options:" + widget_id, "UI: %s %r has no options",
                            widget.kind, widget_id)
            return
        try:
            checked = _options(list(options), f"{self._document.path or 'ui'}/{widget_id}")
        except UiError as exc:
            self._warn_once("options:" + widget_id, "UI: %s", exc)
            return
        with self._lock:
            self._options[widget_id] = checked
        self._queue("opt:" + widget_id, encode_options(widget_id, checked))

    def enable(self, widget_id: str, enabled: bool = True) -> None:
        """Let the player use a widget, or grey it out."""
        if self._widget(widget_id) is None:
            return
        with self._lock:
            self._enabled[widget_id] = bool(enabled)
        self._queue("ena:" + widget_id, encode_enabled(widget_id, enabled))

    def disable(self, widget_id: str) -> None:
        self.enable(widget_id, False)

    # ---- the player's input device ------------------------------------------------------

    @property
    def input(self) -> str | None:
        """"gamepad" or "mouse_keyboard" - whichever the player touched last.

        None until they have touched either. Updated inside flush(), like widget values.
        """
        with self._lock:
            return self._input

    def on_input(self, callback: Callable[[str], None]) -> None:
        """Call `callback(device)` when the player switches between pad and mouse/keyboard.

            ui.on_input(lambda device: show_prompts_for(device))

        Runs inside flush(), on the mod's thread, the same as widget handlers. Not called for
        the device already in use when this is registered - read `input` for that.
        """
        with self._lock:
            self._input_handlers.append(callback)

    def receive(self, line: str) -> None:
        """One line from the overlay. Events are kept for the next flush(); the rest ignored.

        Called on the pipe's reader thread, which is why nothing here touches the mod.
        """
        try:
            device = decode_input(line)
            event = UiEvent("input", "", device) if device else decode_event(line)
        except ValueError as exc:
            logger.warning("UI: %s", exc)
            return
        if event is None:
            return
        with self._inbox_lock:
            self._inbox.append(event)

    def _switch_input(self, device: str) -> None:
        with self._lock:
            if device == self._input:
                return
            self._input = device
            handlers = list(self._input_handlers)
        for handler in handlers:
            try:
                handler(device)
            except Exception:
                logger.exception("UI: an input-device handler failed")

    def _dispatch(self) -> None:
        with self._inbox_lock:
            if not self._inbox:
                return
            events, self._inbox = self._inbox, []
        for event in events:
            if event.kind == "input":
                self._switch_input(event.value)
                continue
            widget = self._widgets.get(event.id)
            if widget is None or event.kind not in EVENTS[widget.kind]:
                # A document swapped out from under an event still in flight, or a stray
                # writer on the pipe. Neither is the mod's to hear about.
                logger.debug("UI: dropped %r", event)
                continue
            with self._lock:
                if event.kind != "click":
                    self._values[event.id] = event.value
                handlers = list(self._handlers.get((event.kind, event.id), ()))
            for handler in handlers:
                try:
                    handler(event)
                except Exception:
                    # Logged rather than raised: flush() is called from the middle of the mod's
                    # frame, and one bad handler must not take the rest of that frame with it.
                    logger.exception("UI: the handler for %s on %r failed", event.kind, event.id)

    def flush(self) -> bool:
        """Send everything queued as one write, then hand the mod what the player did.

        Call once per frame. Handlers registered with on() run here, on this thread, after the
        write - so a handler that sets values queues them for the next frame's flush.
        """
        with self._lock:
            batch = "\n".join(self._pending.values())
            self._pending.clear()
        sent = self._send(batch) if batch else True
        self._dispatch()
        return sent

    def resend_document(self) -> bool:
        """Hand the overlay the whole document again, then everything said since.

        Called when the pipe connects, and again after every reconnect. The overlay keeps the
        document in memory only; if it restarts - or the game is alt-tabbed hard enough to drop
        the pipe - it comes back knowing nothing, and a Runtime that only sent this once at
        startup would leave the player with a blank screen and no error anywhere.

        The replay is the same story one level down. A document arrives at the file's defaults:
        panel back at its shipped position, in its shipped theme, with whichever parts the file
        marks visible. Everything the player had changed would quietly undo itself.
        """
        if not self._send(encode_document(self._document)):
            return False
        with self._lock:
            replay = [encode_visible(name, visible) for name, visible in self._shown.items()]
            replay += [encode_place(screen, x, y, anchor)
                       for screen, (x, y, anchor) in self._placed.items()]
            replay += [encode_raw(screen, shapes) for screen, shapes in self._raw.items()]
            # Options before values: a dropdown's value may be one of the options the mod added.
            replay += [encode_options(wid, options) for wid, options in self._options.items()]
            replay += [encode_enabled(wid, on) for wid, on in self._enabled.items()]
            # The overlay puts every widget back at the file's value on each document. Any that
            # the player or the mod has moved since is said again, or the panel would quietly
            # disagree with what value() returns.
            replay += [encode_value(wid, value) for wid, value in self._values.items()
                       if value != self._widgets[wid].default]
        if not replay:
            return True
        return self._send("\n".join(replay))
