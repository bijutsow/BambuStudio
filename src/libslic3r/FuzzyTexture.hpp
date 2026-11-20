#ifndef libslic3r_FuzzyTexture_hpp_
#define libslic3r_FuzzyTexture_hpp_

// ADNL Texture Studio for fuzzy skin.
//
// Evaluates a displacement field (range [-1, 1]) for a point on a wall. The field is a procedural
// pattern or a grayscale height map, laid onto the surface by a projection (box / cylindrical / planar)
// and transformed by angle, stretch and size. Positive values push the wall outwards (relief),
// negative values carve into it.
//
// This header has no dependency on PrintConfig so the engine can be unit-tested on its own.

#include <memory>
#include <string>

#include <Eigen/Core>

namespace noise { namespace module { class Module; } }

namespace Slic3r {

// Must match the numeric values of Slic3r::NoiseType in PrintConfig.hpp.
enum class FuzzyPattern : int {
    Classic = 0,
    Perlin,
    Billow,
    RidgedMulti,
    Voronoi,
    // ADNL patterns
    Ribs,
    Waves,
    Knurl,
    Hexagon,
    Bricks,
    Dots,
    Woodgrain,
    Cracks,
    Weave,
    Scales,
    Image,
};

// Must match Slic3r::FuzzySkinProjection.
enum class FuzzyProjection : int { Auto = 0, Box, Cylindrical, Planar };
// Must match Slic3r::FuzzySkinProfile.
enum class FuzzyProfile : int { Sine = 0, Triangle, Square, Sawtooth };

struct FuzzyTextureParams
{
    FuzzyPattern    pattern     = FuzzyPattern::Classic;
    double          scale       = 1.0;   // mm: feature size / period / image tile width
    int             octaves     = 4;
    double          persistence = 0.5;
    FuzzyProjection projection  = FuzzyProjection::Auto;
    double          angle_deg   = 0.;    // pattern rotation on the surface (0 = lines parallel to the layers)
    double          stretch     = 1.;    // > 1 elongates features along the pattern direction
    double          azimuth_deg = 0.;    // planar projection: direction the projection plane faces (around Z)
    double          elevation_deg = 0.;  // planar projection: tilt of the projection plane (45 for a part printed at 45°)
    int             repeats     = 0;     // cylindrical: repetitions around the axis (0 = keep `scale` at the local radius)
    FuzzyProfile    profile     = FuzzyProfile::Sine;
    double          duty        = 0.5;   // fraction of the pattern that is in relief (0.05 .. 0.95)
    double          jitter      = 0.;    // 0..1 mix of classic random noise for a hand-made look
    bool            invert      = false;
    double          offset_u    = 0.;    // mm, shift along the pattern
    double          offset_v    = 0.;    // mm
    std::string     image_path;          // grayscale PNG/JPG height map (white = relief)
};

class FuzzyTexture
{
public:
    explicit FuzzyTexture(const FuzzyTextureParams &params);
    ~FuzzyTexture();
    FuzzyTexture(const FuzzyTexture &) = delete;
    FuzzyTexture &operator=(const FuzzyTexture &) = delete;

    // True when the legacy fuzzy skin algorithm must be used unchanged (classic / 3D noise without
    // any of the new transforms). Keeps old projects bit-for-bit compatible in behaviour.
    bool legacy() const { return m_legacy; }

    // Point distance to use for a user point distance (mm). Patterns need several samples per period.
    double sampling_distance(double user_point_distance_mm) const;

    // Whether the sample spacing may be randomized (only for noise-like textures).
    bool random_spacing() const { return m_legacy || m_params.pattern == FuzzyPattern::Classic; }

    // Legacy 3D evaluation (x, y, z in mm). Used when legacy() == true.
    double value_legacy(double x, double y, double z) const;

    // p: point in mm (object coordinates), n: outward unit normal of the wall in XY.
    // Returns a value in [-1, 1].
    double value(const Eigen::Vector3d &p, const Eigen::Vector2d &n) const;

    const FuzzyTextureParams &params() const { return m_params; }

    struct Image; // height map, defined in FuzzyTexture.cpp

    // True if the image could not be loaded (pattern == Image only).
    bool image_missing() const;

private:
    double pattern2d(double a, double b, double w) const; // a: along the pattern, b: across, w: depth coordinate
    double project_and_eval(const Eigen::Vector3d &p, const Eigen::Vector2d &n) const;
    double eval_uv(double u, double v, double w) const;

    FuzzyTextureParams                       m_params;
    bool                                     m_legacy { true };
    double                                   m_cos { 1. }, m_sin { 0. };
    Eigen::Vector3d                          m_plane_u { 1., 0., 0. }, m_plane_v { 0., 0., 1. }, m_plane_n { 0., 1., 0. };
    std::unique_ptr<noise::module::Module>   m_noise;
    std::shared_ptr<const Image>             m_image;
};

} // namespace Slic3r

#endif // libslic3r_FuzzyTexture_hpp_
