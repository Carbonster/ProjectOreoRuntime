#include "input/rml_input.h"

#include <windowsx.h>

#include <RmlUi/Core/Input.h>

#include "input/pad_input.h"
#include "input/window_input.h"

namespace rml_input {

namespace {

using namespace Rml::Input;

struct KeyFor {
    WPARAM vk;
    KeyIdentifier key;
};

// What a UI needs: editing, moving around, and the shortcuts of a text field. Letters reach it
// as WM_CHAR instead.
const KeyFor kKeys[] = {
    {VK_BACK, KI_BACK},     {VK_TAB, KI_TAB},       {VK_RETURN, KI_RETURN},
    {VK_ESCAPE, KI_ESCAPE}, {VK_SPACE, KI_SPACE},   {VK_PRIOR, KI_PRIOR},
    {VK_NEXT, KI_NEXT},     {VK_END, KI_END},       {VK_HOME, KI_HOME},
    {VK_LEFT, KI_LEFT},     {VK_UP, KI_UP},         {VK_RIGHT, KI_RIGHT},
    {VK_DOWN, KI_DOWN},     {VK_INSERT, KI_INSERT}, {VK_DELETE, KI_DELETE},
    {'A', KI_A},            {'C', KI_C},            {'V', KI_V},
    {'X', KI_X},            {'Z', KI_Z},            {'Y', KI_Y},
};

struct PadKey {
    pad_input::Button button;
    KeyIdentifier key;
};
const PadKey kPadKeys[] = {
    {pad_input::Up, KI_UP},     {pad_input::Down, KI_DOWN},    {pad_input::Left, KI_LEFT},
    {pad_input::Right, KI_RIGHT}, {pad_input::A, KI_RETURN},   {pad_input::B, KI_ESCAPE},
};

KeyIdentifier key_for(WPARAM vk) {
    for (const KeyFor& entry : kKeys)
        if (entry.vk == vk) return entry.key;
    return KI_UNKNOWN;
}

struct TextState {
    wchar_t highSurrogate = 0;
};
TextState g_text;
uint32_t g_padDelivered = 0;

void deliver_char(Rml::Context* context, wchar_t c) {
    if (c >= 0xD800 && c < 0xDC00) {
        g_text.highSurrogate = c;
        return;
    }
    char32_t code = c;
    if (c >= 0xDC00 && c < 0xE000 && g_text.highSurrogate)
        code = 0x10000 + ((g_text.highSurrogate - 0xD800) << 10) + (c - 0xDC00);
    g_text.highSurrogate = 0;
    if (code == '\r') code = '\n';
    if ((code >= 32 || code == '\n') && code != 127) context->ProcessTextInput((Rml::Character)code);
}

void deliver_message(Rml::Context* context, const window_input::Message& m, bool keys) {
    switch (m.msg) {
        case WM_MOUSEMOVE:
            context->ProcessMouseMove(GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam), m.modifiers);
            break;
        case WM_LBUTTONDOWN: context->ProcessMouseButtonDown(0, m.modifiers); break;
        case WM_LBUTTONUP: context->ProcessMouseButtonUp(0, m.modifiers); break;
        case WM_RBUTTONDOWN: context->ProcessMouseButtonDown(1, m.modifiers); break;
        case WM_RBUTTONUP: context->ProcessMouseButtonUp(1, m.modifiers); break;
        case WM_MBUTTONDOWN: context->ProcessMouseButtonDown(2, m.modifiers); break;
        case WM_MBUTTONUP: context->ProcessMouseButtonUp(2, m.modifiers); break;
        case WM_MOUSEWHEEL:
            context->ProcessMouseWheel((float)GET_WHEEL_DELTA_WPARAM(m.wParam) / -WHEEL_DELTA,
                                       m.modifiers);
            break;
        case WM_MOUSEHWHEEL:
            context->ProcessMouseWheel(
                Rml::Vector2f((float)GET_WHEEL_DELTA_WPARAM(m.wParam) / WHEEL_DELTA, 0.0f),
                m.modifiers);
            break;
        case WM_MOUSELEAVE:
        case WM_KILLFOCUS:
            context->ProcessMouseLeave();
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (keys) context->ProcessKeyDown(key_for(m.wParam), m.modifiers);
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (keys) context->ProcessKeyUp(key_for(m.wParam), m.modifiers);
            break;
        case WM_CHAR:
            if (keys) deliver_char(context, (wchar_t)m.wParam);
            break;
    }
}

// Only changes become key events, so a held button is one press, as on a keyboard.
void deliver_pad(Rml::Context* context, bool keys) {
    uint32_t held = keys ? pad_input::held() : 0;
    uint32_t changed = held ^ g_padDelivered;
    for (const PadKey& entry : kPadKeys) {
        if (!(changed & entry.button)) continue;
        if (held & entry.button) context->ProcessKeyDown(entry.key, 0);
        else context->ProcessKeyUp(entry.key, 0);
    }
    g_padDelivered = held;
}

}  // namespace

void deliver(Rml::Context* context, bool keys) {
    for (const window_input::Message& message : window_input::take())
        deliver_message(context, message, keys);
    deliver_pad(context, keys);
}

}  // namespace rml_input
