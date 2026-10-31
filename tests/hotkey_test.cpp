// Tests for reading and writing hotkeys like "Ctrl+Alt+Up".
#include "hotkey.h"
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                               \
    do {                                                               \
        if (!(condition)) {                                            \
            std::printf("FAILED line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                \
        }                                                              \
    } while (0)

bool Parses(const wchar_t* text, UINT expectedModifiers, UINT expectedKey) {
    UINT modifiers = 0, key = 0;
    return ParseHotkey(text, modifiers, key) && modifiers == expectedModifiers && key == expectedKey;
}

bool Rejects(const wchar_t* text) {
    UINT modifiers = 0, key = 0;
    return !ParseHotkey(text, modifiers, key);
}

void TestParse() {
    CHECK(Parses(L"Ctrl+Alt+Up", MOD_CONTROL | MOD_ALT, VK_UP));
    CHECK(Parses(L"ctrl + shift + a", MOD_CONTROL | MOD_SHIFT, 'A')); // case and spaces don't matter
    CHECK(Parses(L"Win+Space", MOD_WIN, VK_SPACE));
    CHECK(Parses(L"Alt+F5", MOD_ALT, VK_F5));
    CHECK(Parses(L"Alt+F24", MOD_ALT, VK_F24));
    CHECK(Parses(L"Control+9", MOD_CONTROL, '9'));
    CHECK(Parses(L"Ctrl+PageDown", MOD_CONTROL, VK_NEXT));
    CHECK(Parses(L"Windows+Home", MOD_WIN, VK_HOME));
}

void TestParseRejects() {
    CHECK(Rejects(L""));
    CHECK(Rejects(L"Up"));            // no modifier: a bare key would swallow typing
    CHECK(Rejects(L"Ctrl"));          // no key
    CHECK(Rejects(L"Ctrl+Alt+A+B"));  // two keys
    CHECK(Rejects(L"Ctrl++Up"));      // an empty part
    CHECK(Rejects(L"Ctrl+Up+"));      // trailing "+"
    CHECK(Rejects(L"Ctrl+F25"));      // out of range
    CHECK(Rejects(L"Ctrl+F0"));
    CHECK(Rejects(L"Ctrl+Banana"));
    CHECK(Rejects(L"Ctrl+;"));        // punctuation isn't supported
}

void TestFormat() {
    CHECK(FormatHotkey(MOD_CONTROL | MOD_ALT, VK_UP) == L"Ctrl+Alt+Up");
    CHECK(FormatHotkey(MOD_WIN | MOD_SHIFT | MOD_CONTROL | MOD_ALT, 'Z') == L"Ctrl+Alt+Shift+Win+Z"); // fixed order
    CHECK(FormatHotkey(MOD_ALT, VK_F12) == L"Alt+F12");
    CHECK(FormatHotkey(MOD_CONTROL, VK_NEXT) == L"Ctrl+PageDown");
    CHECK(FormatHotkey(MOD_CONTROL, VK_OEM_1).empty());   // ";" has no name
    CHECK(FormatHotkey(MOD_CONTROL, VK_ESCAPE).empty());
}

// Anything FormatHotkey writes, ParseHotkey must read back to the same keys.
void TestRoundTrip() {
    const UINT keys[] = {VK_SPACE, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, 'A', 'Z', '0', '9', VK_F1, VK_F10, VK_F24};
    for (UINT modifiers = 1; modifiers < 16; ++modifiers) {
        UINT m = 0;
        if (modifiers & 1) m |= MOD_CONTROL;
        if (modifiers & 2) m |= MOD_ALT;
        if (modifiers & 4) m |= MOD_SHIFT;
        if (modifiers & 8) m |= MOD_WIN;
        for (UINT key : keys) {
            const std::wstring text = FormatHotkey(m, key);
            CHECK(!text.empty());
            CHECK(Parses(text.c_str(), m, key));
        }
    }
}

} // namespace

int main() {
    TestParse();
    TestParseRejects();
    TestFormat();
    TestRoundTrip();
    std::printf(failures ? "%d check(s) failed\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}
