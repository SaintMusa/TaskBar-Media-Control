#include "placement.h"
#include <algorithm>

Placement ChoosePlacement(const PlacementInput& in) {
    Placement result;

    // Left side: after the Widgets button, if that sits left of Start.
    const bool widgetsLeftOfStart = in.hasWidgetsButton && in.widgetsRight <= in.startLeft;
    const long leftFrom = widgetsLeftOfStart ? in.widgetsRight : in.barLeft;
    const int leftRoom = static_cast<int>(in.startLeft - leftFrom) - 2 * in.margin;
    result.available = leftRoom;
    if (leftRoom >= in.minWidth) {
        result.fits = true;
        result.width = std::min(in.preferredWidth, leftRoom);
        result.left = leftFrom + in.margin;
        return result;
    }

    if (!in.hasIcons) return result;
    const long rightTo = std::min(in.rightObstacleLeft, in.barRight);
    const int rightRoom = static_cast<int>(rightTo - in.iconsRight) - 2 * in.margin;
    if (rightRoom < in.minWidth) {
        result.available = std::max(leftRoom, rightRoom);
        return result;
    }
    result.fits = true;
    result.rightOfIcons = true;
    result.available = rightRoom;
    result.width = std::min(in.preferredWidth, rightRoom);
    result.left = in.iconsRight + in.margin;
    return result;
}
