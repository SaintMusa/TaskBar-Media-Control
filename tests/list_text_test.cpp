// Tests for the ";"-separated lists in settings.ini.
#include "list_text.h"
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

using List = std::vector<std::wstring>;

void TestSplit() {
    CHECK(SplitList(L"") == List{});
    CHECK(SplitList(L"Spotify") == List{L"Spotify"});
    CHECK(SplitList(L"A;B;C") == (List{L"A", L"B", L"C"}));
    CHECK(SplitList(L"  A ; B\t;C  ") == (List{L"A", L"B", L"C"})); // trimmed
    CHECK(SplitList(L"A;;B;") == (List{L"A", L"B"}));               // empty items dropped
    CHECK(SplitList(L";") == List{});
    CHECK(SplitList(L" ; ") == List{});
    CHECK(SplitList(L"Apple Music;YouTube Music") == (List{L"Apple Music", L"YouTube Music"})); // inner spaces stay
    CHECK(SplitList(L"Müzik;音楽") == (List{L"Müzik", L"音楽"}));
}

void TestJoin() {
    CHECK(JoinList({}) == L"");
    CHECK(JoinList({L"A"}) == L"A");
    CHECK(JoinList({L"A", L"B", L"C"}) == L"A;B;C");
}

void TestRoundTrip() {
    const List items = {L"Spotify", L"msedge.exe", L"Apple Music"};
    CHECK(SplitList(JoinList(items)) == items);
    // What a user types is tidied up by one trip through both.
    CHECK(JoinList(SplitList(L" A ;; B ")) == L"A;B");
}

} // namespace

int main() {
    TestSplit();
    TestJoin();
    TestRoundTrip();
    std::printf(failures ? "%d check(s) failed\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}
