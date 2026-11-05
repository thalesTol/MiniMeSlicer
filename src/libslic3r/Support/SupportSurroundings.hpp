#ifndef slic3r_SupportSurroundings_hpp_
#define slic3r_SupportSurroundings_hpp_

// SliceOfMe: what lies around an object's tree supports - the plate areas painted in the support painting tool
// and the other objects on the same plate.

#include "../ExPolygon.hpp"
#include "../Model.hpp"

#include <vector>

namespace Slic3r {

class PrintObject;
class PrintConfig;

// Painted (support painting tool) on an object to let other objects' tree supports rest there.
constexpr EnforcerBlockerType SUPPORT_LANDING_STATE = EnforcerBlockerType(3);

struct SupportSurroundings
{
    // Where the object's supports may not start on the plate, in object coordinates (all instances).
    ExPolygons              floor_forbidden;
    // Per object layer: the other objects on the plate, in object coordinates. Supports go around them.
    std::vector<ExPolygons> foreign;
    // Per object layer: tops of other objects painted as support base. Supports may rest there even when
    // they are limited to the build plate.
    std::vector<ExPolygons> landing;
    // Per object layer: where supports starting on this layer must stand on what lies below them (the plate areas
    // painted "stand on objects", where some object is below). They never go to the plate.
    std::vector<ExPolygons> forced_landing;
    bool                    has_foreign{false};
    bool                    has_landing{false};
    bool                    has_forced_landing{false};
};

// The plate areas painted in the support painting tool, in plate coordinates (scaled); empty when none.
ExPolygons support_plate_forbidden_area(const PrintConfig &config);
bool       support_plate_zones_set(const PrintConfig &config);

// True when the object's tree supports depend on the plate zones or on other objects: such supports cannot be
// shared between identical objects and must be recalculated when the neighbours move.
bool support_depends_on_surroundings(const PrintObject &object);

// Collects the surroundings of an object for its tree supports. Other objects must already be sliced.
SupportSurroundings support_surroundings(const PrintObject &object);

} // namespace Slic3r

#endif // slic3r_SupportSurroundings_hpp_
