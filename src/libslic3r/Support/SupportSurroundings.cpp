#include "SupportSurroundings.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../TriangleMeshSlicer.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>

namespace Slic3r {

namespace {

// Painted polygons, clockwise ones being holes, as areas.
ExPolygons plate_areas(const ConfigOptionPointsGroups &opt)
{
    Polygons polys;
    for (const Vec2ds &pts : opt.values) {
        if (pts.size() < 3)
            continue;
        Polygon poly;
        poly.points.reserve(pts.size());
        for (const Vec2d &pt : pts)
            poly.points.emplace_back(coord_t(scale_(pt.x())), coord_t(scale_(pt.y())));
        polys.emplace_back(std::move(poly));
    }
    // non-zero filling: a clockwise polygon inside a counter-clockwise one cancels it out
    return polys.empty() ? ExPolygons() : union_ex(polys, ClipperLib::pftNonZero);
}

// Index of the layer of 'obj' that contains height 'z', or -1.
int layer_at(const PrintObject &obj, double z)
{
    int lo = 0, hi = int(obj.layer_count()); // first layer with print_z >= z
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (obj.get_layer(mid)->print_z < z)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= int(obj.layer_count()) || obj.get_layer(lo)->print_z - obj.get_layer(lo)->height > z + EPSILON)
        return -1;
    return lo;
}

ExPolygons shifted(const ExPolygons &src, const Point &shift)
{
    ExPolygons out = src;
    for (ExPolygon &expoly : out)
        expoly.translate(shift);
    return out;
}

} // namespace

ExPolygons support_plate_forbidden_area(const PrintConfig &config)
{
    ExPolygons       forbidden = plate_areas(config.support_plate_block_area);
    const ExPolygons allowed   = plate_areas(config.support_plate_allow_area);
    if (!allowed.empty()) {
        // only the allowed areas: everything else on (and around) the plate is forbidden
        BoundingBox box = get_extents(allowed);
        for (const Vec2d &pt : config.printable_area.values)
            box.merge(Point(coord_t(scale_(pt.x())), coord_t(scale_(pt.y()))));
        box.offset(scale_(50.));
        forbidden = union_ex(forbidden, diff_ex(ExPolygons{ExPolygon(box.polygon())}, allowed));
    }
    return forbidden;
}

bool support_plate_zones_set(const PrintConfig &config)
{
    auto set = [](const ConfigOptionPointsGroups &opt) {
        return std::any_of(opt.values.begin(), opt.values.end(), [](const Vec2ds &pts) { return pts.size() >= 3; });
    };
    return set(config.support_plate_block_area) || set(config.support_plate_allow_area) || set(config.support_plate_land_area);
}

bool support_depends_on_surroundings(const PrintObject &object)
{
    if (!is_tree(object.config().support_type.value))
        return false;
    // no supports at all (neither enabled nor painted): nothing to recalculate or keep apart
    if (!object.has_support_material() && (object.model_object() == nullptr || !object.model_object()->is_fdm_support_painted()))
        return false;
    const Print *print = object.print();
    if (print == nullptr)
        return false;
    if (support_plate_zones_set(print->config()))
        return true;
    return print->objects().size() > 1 && print->config().print_sequence.value != PrintSequence::ByObject;
}

SupportSurroundings support_surroundings(const PrintObject &object)
{
    SupportSurroundings out;
    const Print *print = object.print();
    if (print == nullptr || object.instances().empty())
        return out;
    const size_t num_layers = object.layer_count();
    out.foreign.assign(num_layers, ExPolygons());
    out.landing.assign(num_layers, ExPolygons());

    // 1) plate zones, seen from every instance of this object (the supports are shared by the instances)
    const ExPolygons plate_forbidden = support_plate_forbidden_area(print->config());
    if (!plate_forbidden.empty()) {
        const Vec3d origin = print->get_plate_origin();
        const Point plate_shift(coord_t(scale_(origin.x())), coord_t(scale_(origin.y())));
        for (const PrintInstance &inst : object.instances())
            append(out.floor_forbidden, shifted(plate_forbidden, plate_shift - inst.shift));
        out.floor_forbidden = union_ex(out.floor_forbidden);
    }

    // the plate areas where supports stand on the objects below them, in object coordinates
    ExPolygons land_zone;
    if (const ExPolygons land = plate_areas(print->config().support_plate_land_area); !land.empty()) {
        const Vec3d origin = print->get_plate_origin();
        const Point plate_shift(coord_t(scale_(origin.x())), coord_t(scale_(origin.y())));
        for (const PrintInstance &inst : object.instances())
            append(land_zone, shifted(land, plate_shift - inst.shift));
        land_zone = union_ex(land_zone);
    }

    // 2) the other objects: obstacles, and tops painted as support base
    if (print->config().print_sequence.value != PrintSequence::ByObject)
    for (const PrintObject *other : print->objects()) {
        if (other == &object || other->instances().empty())
            continue;
        // identical objects share their slices with the first one
        const PrintObject *src = other->get_shared_object() != nullptr ? other->get_shared_object() : other;
        if (src->layer_count() == 0)
            continue; // not sliced (yet)
        const size_t src_count = src->layer_count();

        // other object -> this object: one shift per pair of instances, only for instances close enough to matter
        // (the branches of this object stay within its footprint plus how far they may lean)
        std::vector<Point> shifts;
        {
            BoundingBox mine_box = object.bounding_box();
            mine_box.offset(scale_(40.));
            const BoundingBox theirs_box = src->bounding_box();
            for (const PrintInstance &mine : object.instances())
                for (const PrintInstance &theirs : other->instances()) {
                    const Point shift = theirs.shift - mine.shift;
                    BoundingBox moved = theirs_box;
                    moved.translate(double(shift.x()), double(shift.y()));
                    if (moved.overlap(mine_box))
                        shifts.emplace_back(shift);
                }
        }
        if (shifts.empty())
            continue;

        // painted support base of the other object, per layer of the other object
        std::vector<ExPolygons> painted;
        {
            const std::vector<float> zs     = zs_from_layers(src->layers());
            const double             shrink = src->support_shrinkage_scale();
            for (const ModelVolume *mv : other->model_object()->volumes) {
                // nothing painted: skip, reading the painting rebuilds the triangle structure of the whole part
                if (!mv->is_model_part() || mv->supported_facets.empty() || !mv->supported_facets.has_facets(*mv, SUPPORT_LANDING_STATE))
                    continue;
                const indexed_triangle_set facets = mv->supported_facets.get_facets_strict(*mv, SUPPORT_LANDING_STATE);
                if (facets.indices.empty())
                    continue;
                std::vector<Polygons> top;
                // upward facing painted areas, projected onto their slicing planes
                slice_mesh_slabs(facets, zs, src->trafo_centered() * mv->get_matrix(), &top, nullptr, nullptr, []() {});
                if (painted.empty())
                    painted.assign(src_count, ExPolygons());
                for (size_t k = 0; k < top.size() && k < painted.size(); ++k) {
                    if (top[k].empty())
                        continue;
                    if (shrink != 1.)
                        for (Polygon &poly : top[k])
                            poly.scale(shrink);
                    append(painted[k], union_ex(top[k]));
                }
            }
            for (ExPolygons &layer : painted)
                if (!layer.empty())
                    layer = offset_ex(union_ex(layer), scale_(0.3));
        }

        for (size_t i = 0; i < num_layers; ++i) {
            const Layer *layer = object.get_layer(int(i));
            const int    k     = layer_at(*src, layer->print_z - 0.5 * layer->height);
            if (k < 0)
                continue;
            const ExPolygons &slices = src->get_layer(k)->lslices;
            if (!slices.empty()) {
                for (const Point &shift : shifts)
                    append(out.foreign[i], shifted(slices, shift));
                out.has_foreign = true;
            }
            if (!painted.empty()) {
                // the painted top may be projected onto the plane of a neighbouring layer
                ExPolygons here;
                for (int kk = std::max(0, k - 1); kk <= std::min(int(painted.size()) - 1, k + 1); ++kk)
                    append(here, painted[kk]);
                if (!here.empty()) {
                    for (const Point &shift : shifts)
                        append(out.landing[i], shifted(here, shift));
                    out.has_landing = true;
                }
            }
        }
    }
    if (out.has_foreign || out.has_landing)
        for (size_t i = 0; i < num_layers; ++i) {
            if (out.foreign[i].size() > 1)
                out.foreign[i] = union_ex(out.foreign[i]);
            if (out.landing[i].size() > 1)
                out.landing[i] = union_ex(out.landing[i]);
        }

    // 3) "stand on objects": inside the painted areas, wherever this or another object lies below a layer, the
    // supports starting there must stand on it; they may rest there even when limited to the build plate
    if (!land_zone.empty()) {
        out.forced_landing.assign(num_layers, ExPolygons());
        ExPolygons below; // what lies below the current layer, inside the zone
        for (size_t i = 0; i < num_layers; ++i) {
            if (!below.empty()) {
                out.forced_landing[i] = below;
                append(out.landing[i], below);
                if (out.landing[i].size() > 1)
                    out.landing[i] = union_ex(out.landing[i]);
                out.has_landing        = true;
                out.has_forced_landing = true;
            }
            ExPolygons here = object.get_layer(int(i))->lslices;
            if (i < out.foreign.size())
                append(here, out.foreign[i]);
            here = intersection_ex(here, land_zone);
            if (!here.empty())
                below = union_ex(below, here);
        }
    }

    // diagnostics: what the supports of this object get to see
    {
        auto mm2 = [](const ExPolygons &e) { double a = 0.; for (const ExPolygon &x : e) a += x.area(); return a * SCALING_FACTOR * SCALING_FACTOR; };
        size_t forced_layers = 0, landing_layers = 0, foreign_layers = 0;
        for (const ExPolygons &l : out.forced_landing) forced_layers += !l.empty();
        for (const ExPolygons &l : out.landing) landing_layers += !l.empty();
        for (const ExPolygons &l : out.foreign) foreign_layers += !l.empty();
        const Vec3d origin = print->get_plate_origin();
        BOOST_LOG_TRIVIAL(warning) << "SliceOfMe supports: object '" << (object.model_object() ? object.model_object()->name : std::string()) << "' layers "
                                   << num_layers << ", plate origin " << origin.x() << "," << origin.y() << ", instance shift "
                                   << unscale<double>(object.instances().front().shift.x()) << "," << unscale<double>(object.instances().front().shift.y())
                                   << ", plate zones block/allow/land groups " << print->config().support_plate_block_area.values.size() << "/"
                                   << print->config().support_plate_allow_area.values.size() << "/" << print->config().support_plate_land_area.values.size()
                                   << ", floor forbidden " << mm2(out.floor_forbidden) << " mm2, land zone " << mm2(land_zone) << " mm2, forced layers " << forced_layers << ", landing layers " << landing_layers
                                   << ", foreign layers " << foreign_layers;
    }
    return out;
}

} // namespace Slic3r
