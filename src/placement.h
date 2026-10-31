#pragma once

// Where on the taskbar the widget goes, from plain numbers (physical pixels,
// screen coordinates), so the rules can be tested without a real taskbar.
struct PlacementInput {
    long barLeft = 0, barRight = 0;
    long startLeft = 0;                 // the Start button, which leads the icon group
    bool hasWidgetsButton = false;
    long widgetsLeft = 0, widgetsRight = 0;
    bool hasIcons = false;              // whether the whole icon strip's extent is known
    long iconsRight = 0;
    long rightObstacleLeft = 0x7fffffff; // the tray, or another app's window, right of the icons
    int margin = 0;
    int preferredWidth = 0;
    int minWidth = 0;
};

struct Placement {
    bool fits = false;
    long left = 0;            // screen x of the widget's left edge
    int width = 0;
    bool rightOfIcons = false; // false = in the free space left of the icons
    int available = 0;         // room found on the side chosen (or on the left, if neither fits)
};

// Left of the icons when they're centered and leave room there; otherwise
// right of them, which is where the free space is when they're left-aligned
// (or centered so many that they crowd the left side).
Placement ChoosePlacement(const PlacementInput& in);
