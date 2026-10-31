// Tests for where the widget goes. The numbers come from a real 1920 px wide
// Windows 11 taskbar (dumped with UI Automation), centered and left-aligned.
#include "placement.h"
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

PlacementInput Taskbar() {
    PlacementInput in;
    in.barLeft = 0;
    in.barRight = 1920;
    in.margin = 8;
    in.preferredWidth = 300;
    in.minWidth = 160;
    return in;
}

// Start at 696 and ten more buttons: the icons run 696..1225.
PlacementInput Centered() {
    PlacementInput in = Taskbar();
    in.startLeft = 696;
    in.hasIcons = true;
    in.iconsRight = 1225;
    in.rightObstacleLeft = 1713; // the tray
    return in;
}

// The same buttons moved to the left edge: 0..529.
PlacementInput LeftAligned() {
    PlacementInput in = Taskbar();
    in.startLeft = 0;
    in.hasIcons = true;
    in.iconsRight = 529;
    in.rightObstacleLeft = 1713;
    return in;
}

void TestCentered() {
    const Placement p = ChoosePlacement(Centered());
    CHECK(p.fits);
    CHECK(!p.rightOfIcons);
    CHECK(p.left == 8);
    CHECK(p.width == 300);
    CHECK(p.available == 680); // matches "free 680 px" in the real log
}

void TestCenteredWithWidgetsButton() {
    PlacementInput in = Centered();
    in.hasWidgetsButton = true;
    in.widgetsLeft = 0;
    in.widgetsRight = 100;
    const Placement p = ChoosePlacement(in);
    CHECK(p.fits && !p.rightOfIcons);
    CHECK(p.left == 108);
}

void TestLeftAligned() {
    const Placement p = ChoosePlacement(LeftAligned());
    CHECK(p.fits);
    CHECK(p.rightOfIcons);
    CHECK(p.left == 537);
    CHECK(p.width == 300);
}

void TestAnotherAppsWindowEndsTheFreeSpace() {
    PlacementInput in = LeftAligned();
    in.rightObstacleLeft = 1412; // TrafficMonitor's window: still room for the full 300
    Placement p = ChoosePlacement(in);
    CHECK(p.fits && p.rightOfIcons && p.width == 300);

    in.rightObstacleLeft = 720; // closer: 720 - 529 - 16 = 175 px, so it shrinks to stop short
    p = ChoosePlacement(in);
    CHECK(p.fits && p.rightOfIcons);
    CHECK(p.left == 537);
    CHECK(p.width == 175);
}

void TestCrowdedLeftAlignedHides() {
    PlacementInput in = LeftAligned();
    in.iconsRight = 1600; // many apps open: 1713 - 1600 - 16 = 97 px, under the 160 minimum
    const Placement p = ChoosePlacement(in);
    CHECK(!p.fits);
    CHECK(p.available == 97);

    in.iconsRight = 1500; // 197 px is enough
    CHECK(ChoosePlacement(in).fits);
}

void TestCenteredButCrowdedMovesRight() {
    PlacementInput in = Centered();
    in.startLeft = 100; // 84 px left of the icons: too small
    in.iconsRight = 700;
    const Placement p = ChoosePlacement(in);
    CHECK(p.fits && p.rightOfIcons);
    CHECK(p.left == 708);
}

void TestWidgetsButtonRightOfIconsIsAnObstacle() {
    PlacementInput in = LeftAligned();
    in.hasWidgetsButton = true; // the watcher folds it into rightObstacleLeft
    in.widgetsLeft = 1600;
    in.widgetsRight = 1650;
    in.rightObstacleLeft = 1600;
    const Placement p = ChoosePlacement(in);
    CHECK(p.fits && p.rightOfIcons);
    CHECK(p.width == 300);
    // It must not be used as the left-side gap even though it's the Widgets button.
    CHECK(p.left == 537);
}

void TestNoIconExtentKnown() {
    PlacementInput in = LeftAligned();
    in.hasIcons = false;
    CHECK(!ChoosePlacement(in).fits); // can't place right of icons we can't find
}

void TestObstacleBeyondTheBar() {
    PlacementInput in = LeftAligned();
    in.rightObstacleLeft = 0x7fffffff; // nothing found at all
    const Placement p = ChoosePlacement(in);
    CHECK(p.fits && p.width == 300); // the bar's own edge still limits it
    in.iconsRight = 1900;
    CHECK(!ChoosePlacement(in).fits);
}

} // namespace

int main() {
    TestCentered();
    TestCenteredWithWidgetsButton();
    TestLeftAligned();
    TestAnotherAppsWindowEndsTheFreeSpace();
    TestCrowdedLeftAlignedHides();
    TestCenteredButCrowdedMovesRight();
    TestWidgetsButtonRightOfIconsIsAnObstacle();
    TestNoIconExtentKnown();
    TestObstacleBeyondTheBar();
    std::printf(failures ? "%d check(s) failed\n" : "All checks passed\n", failures);
    return failures ? 1 : 0;
}
