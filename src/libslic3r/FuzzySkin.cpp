#include <memory>
#include <random>
#include <thread>

#include "libslic3r/Algorithm/LineSegmentation/LineSegmentation.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "FuzzySkin.hpp"
#include "FuzzyTexture.hpp"

namespace Slic3r {

// Produces a random value between 0 and 1. Thread-safe.
static double random_value()
{
    thread_local std::random_device rd;
    thread_local std::mt19937                           gen(rd.entropy() > 0 ? rd() : std::hash<std::thread::id>()(std::this_thread::get_id()));
    thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(gen);
}

// ADNL: all texture evaluation (legacy noise included) lives in FuzzyTexture.
static FuzzyTextureParams texture_params(const PrintRegionConfig &cfg)
{
    FuzzyTextureParams p;
    p.pattern       = FuzzyPattern(int(cfg.fuzzy_skin_noise_type.value));
    p.scale         = cfg.fuzzy_skin_scale.value;
    p.octaves       = cfg.fuzzy_skin_octaves.value;
    p.persistence   = cfg.fuzzy_skin_persistence.value;
    p.projection    = FuzzyProjection(int(cfg.fuzzy_skin_texture_projection.value));
    p.angle_deg     = cfg.fuzzy_skin_texture_angle.value;
    p.stretch       = cfg.fuzzy_skin_texture_stretch.value;
    p.profile       = FuzzyProfile(int(cfg.fuzzy_skin_texture_profile.value));
    p.duty          = cfg.fuzzy_skin_texture_fill.value / 100.;
    p.repeats       = cfg.fuzzy_skin_texture_repeats.value;
    p.azimuth_deg   = cfg.fuzzy_skin_texture_azimuth.value;
    p.elevation_deg = cfg.fuzzy_skin_texture_elevation.value;
    p.jitter        = cfg.fuzzy_skin_texture_jitter.value / 100.;
    p.invert        = cfg.fuzzy_skin_texture_invert.value;
    p.image_path    = cfg.fuzzy_skin_texture_image.value;
    return p;
}

// Signed area of a closed junction loop (> 0 when counter-clockwise).
static double junctions_area(const Arachne::ExtrusionJunctions &js)
{
    double a = 0.;
    for (size_t i = 0, j = js.size() - 1; i < js.size(); j = i++)
        a += double(js[j].p.x()) * double(js[i].p.y()) - double(js[i].p.x()) * double(js[j].p.y());
    return 0.5 * a;
}

void fuzzy_polyline(Points &poly, bool closed, coordf_t slice_z, const PrintRegionConfig &config, double outward_sign)
{
    const FuzzyTexture tex(texture_params(config));
    const double thickness      = scaled<double>(config.fuzzy_skin_thickness.value);
    const double point_distance = scaled<double>(tex.sampling_distance(config.fuzzy_skin_point_distance.value));
    const bool   rnd            = tex.random_spacing();
    const double min_dist_between_points = rnd ? point_distance * 3. / 4. : point_distance;
    const double range_random_point_dist = rnd ? point_distance / 2. : 0.;
    double       dist_left_over          = rnd ? random_value() * (min_dist_between_points / 2.) : 0.;

    Points out;
    out.reserve(poly.size());
    Point *p0 = closed ? &poly.back() : &poly.front();
    for (auto it_pt1 = closed ? poly.begin() : std::next(poly.begin()); it_pt1 != poly.end(); ++it_pt1) {
        Point &p1 = *it_pt1;
        Vec2d  p0p1      = (p1 - *p0).cast<double>();
        double p0p1_size = p0p1.norm();
        if (p0p1_size <= 0.) { p0 = &p1; continue; }
        const Vec2d perp_n = perp(p0p1).normalized();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + (rnd ? random_value() * range_random_point_dist : 0.)) {
            Point pa = *p0 + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            if (tex.legacy()) {
                double r = tex.value_legacy(unscale<double>(pa.x()), unscale<double>(pa.y()), slice_z) * thickness;
                out.emplace_back(pa + (perp_n * r).cast<coord_t>());
            } else {
                const Vec2d n_out = perp_n * outward_sign;
                const double r = tex.value(Vec3d(unscale<double>(pa.x()), unscale<double>(pa.y()), slice_z), n_out) * thickness;
                out.emplace_back(pa + (n_out * r).cast<coord_t>());
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0             = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = poly.size() - 2;
        out.emplace_back(poly[point_idx]);
        if (point_idx == 0) break;
        --point_idx;
    }
    if (out.size() >= 3)
        poly = std::move(out);
}

void fuzzy_polygon(Polygon &polygon, coordf_t slice_z, const PrintRegionConfig &config, double outward_sign)
{
    fuzzy_polyline(polygon.points, true, slice_z, config, outward_sign);
}

void fuzzy_extrusion_line(Arachne::ExtrusionLine &ext_lines, coordf_t slice_z, const PrintRegionConfig &config, double outward_sign)
{
    const FuzzyTexture tex(texture_params(config));
    const double thickness      = scaled<double>(config.fuzzy_skin_thickness.value);
    const double point_distance = scaled<double>(tex.sampling_distance(config.fuzzy_skin_point_distance.value));
    const bool   rnd            = tex.random_spacing();
    const double min_dist_between_points = rnd ? point_distance * 3. / 4. : point_distance;
    const double range_random_point_dist = rnd ? point_distance / 2. : 0.;
    const double min_extrusion_width     = scaled<double>(0.01); // minimum line width (mm) for Extrusion/Combined
    double       dist_left_over          = rnd ? random_value() * (min_dist_between_points / 2.) : 0.;

    const FuzzySkinMode mode = config.fuzzy_skin_mode.value;

    Arachne::ExtrusionJunction *p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());
    for (auto &p1 : ext_lines) {
        if (p0->p == p1.p) {
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }
        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points + (rnd ? random_value() * range_random_point_dist : 0.)) {
            Point  pa = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            Vec2d  perp_n = perp(p0p1).cast<double>().normalized();
            double r;
            if (tex.legacy()) {
                r = tex.value_legacy(unscale<double>(pa.x()), unscale<double>(pa.y()), slice_z) * thickness;
            } else {
                perp_n *= outward_sign;
                r = tex.value(Vec3d(unscale<double>(pa.x()), unscale<double>(pa.y()), slice_z), perp_n) * thickness;
            }
            switch (mode) {
                case FuzzySkinMode::Displacement:
                    out.emplace_back(pa + (perp_n * r).cast<coord_t>(), p1.w, p1.perimeter_index);
                    break;
                case FuzzySkinMode::Extrusion:
                    out.emplace_back(pa, std::max(p1.w + r + min_extrusion_width, min_extrusion_width), p1.perimeter_index);
                    break;
                case FuzzySkinMode::Combined: {
                    double rad = std::max(p1.w + r + min_extrusion_width, min_extrusion_width);
                    out.emplace_back(pa + (perp_n * ((rad - p1.w) / 2.)).cast<coord_t>(), rad, p1.perimeter_index);
                    break;
                }
            }
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0             = &p1;
    }

    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0) {
            break;
        }

        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) {
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3) {
        ext_lines.junctions = std::move(out);
    }
}

bool should_fuzzify(const PrintRegionConfig &config, const size_t layer_idx, const size_t perimeter_idx, const bool is_contour)
{
    const FuzzySkinType fuzzy_skin_type = config.fuzzy_skin.value;

    if (fuzzy_skin_type == FuzzySkinType::None || fuzzy_skin_type == FuzzySkinType::Disabled_fuzzy)
        return false;
    if (!config.fuzzy_skin_first_layer && layer_idx <= 0)
        return false;

    const bool fuzzify_contours = perimeter_idx == 0 || fuzzy_skin_type == FuzzySkinType::AllWalls;
    const bool fuzzify_holes    = fuzzify_contours && (fuzzy_skin_type == FuzzySkinType::All || fuzzy_skin_type == FuzzySkinType::AllWalls);

    return is_contour ? fuzzify_contours : fuzzify_holes;
}

Polygon apply_fuzzy_skin(const Polygon &polygon, const PrintRegionConfig &base_config, const PerimeterRegions &perimeter_regions, size_t layer_idx, size_t perimeter_idx, bool is_contour, coordf_t slice_z)
{
    using namespace Slic3r::Algorithm::LineSegmentation;

    // ADNL: +1 when perp() (left normal) points out of the material, -1 otherwise.
    const double outward_sign = (polygon.is_counter_clockwise() == is_contour) ? -1. : 1.;

    auto apply_fuzzy_skin_on_polygon = [&](const Polygon &polygon, const PrintRegionConfig &config) -> Polygon {
        if (should_fuzzify(config, layer_idx, perimeter_idx, is_contour)) {
            Polygon fuzzified_polygon = polygon;
            fuzzy_polygon(fuzzified_polygon, slice_z, config, outward_sign);
            return fuzzified_polygon;
        }
        return polygon;
    };

    if (perimeter_regions.empty())
        return apply_fuzzy_skin_on_polygon(polygon, base_config);

    PolylineRegionSegments segments = polygon_segmentation(polygon, base_config, perimeter_regions);
    if (segments.size() == 1)
        return apply_fuzzy_skin_on_polygon(polygon, segments.front().config);

    Polygon fuzzified_polygon;
    for (PolylineRegionSegment &segment : segments) {
        const PrintRegionConfig &config = segment.config;
        if (should_fuzzify(config, layer_idx, perimeter_idx, is_contour))
            fuzzy_polyline(segment.polyline.points, false, slice_z, config, outward_sign);

        assert(!segment.polyline.empty());
        if (segment.polyline.empty()) {
            continue;
        } else if (!fuzzified_polygon.empty() && fuzzified_polygon.back() == segment.polyline.front()) {
            // Remove the last point to avoid duplicate points.
            fuzzified_polygon.points.pop_back();
        }

        Slic3r::append(fuzzified_polygon.points, std::move(segment.polyline.points));
    }

    assert(!fuzzified_polygon.empty());
    if (fuzzified_polygon.front() == fuzzified_polygon.back()) {
        // Remove the last point to avoid duplicity between the first and the last point.
        fuzzified_polygon.points.pop_back();
    }

    return fuzzified_polygon;
}

Arachne::ExtrusionLine apply_fuzzy_skin(const Arachne::ExtrusionLine &extrusion, const PrintRegionConfig &base_config, const PerimeterRegions &perimeter_regions, size_t layer_idx, size_t perimeter_idx, bool is_contour, coordf_t slice_z)
{
    using namespace Slic3r::Algorithm::LineSegmentation;
    using namespace Slic3r::Arachne;

    // ADNL: orientation of the loop decides which side is "outside".
    double outward_sign = -1.;
    if (extrusion.is_closed && extrusion.junctions.size() >= 3)
        outward_sign = ((junctions_area(extrusion.junctions) > 0.) == is_contour) ? -1. : 1.;

    if (perimeter_regions.empty()) {
        if (should_fuzzify(base_config, layer_idx, perimeter_idx, is_contour)) {
            ExtrusionLine fuzzified_extrusion = extrusion;
            fuzzy_extrusion_line(fuzzified_extrusion, slice_z, base_config, outward_sign);
            return fuzzified_extrusion;
        }
        return extrusion;
    }

    ExtrusionRegionSegments segments = extrusion_segmentation(extrusion, base_config, perimeter_regions);
    ExtrusionLine           fuzzified_extrusion(extrusion.inset_idx, extrusion.is_odd, extrusion.is_closed);

    for (ExtrusionRegionSegment &segment : segments) {
        const PrintRegionConfig &config = segment.config;
        if (should_fuzzify(config, layer_idx, perimeter_idx, is_contour))
            fuzzy_extrusion_line(segment.extrusion, slice_z, config, outward_sign);

        assert(!segment.extrusion.empty());
        if (segment.extrusion.empty()) {
            continue;
        } else if (!fuzzified_extrusion.empty() && fuzzified_extrusion.back().p == segment.extrusion.front().p) {
            // Remove the last point to avoid duplicate points (We don't care if the width of both points is different.).
            fuzzified_extrusion.junctions.pop_back();
        }

        Slic3r::append(fuzzified_extrusion.junctions, std::move(segment.extrusion.junctions));
    }

    assert(!fuzzified_extrusion.empty());

    return fuzzified_extrusion;
}

} // namespace Slic3r::Feature::FuzzySkin::Feature::FuzzySkin