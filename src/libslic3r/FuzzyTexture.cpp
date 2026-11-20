// ADNL Texture Studio for fuzzy skin — see FuzzyTexture.hpp.

#include "FuzzyTexture.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include <boost/filesystem.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <Eigen/Geometry>

#include "noise.h"

namespace Slic3r {

namespace {

constexpr double PI    = 3.14159265358979323846;
constexpr double TWOPI = 2. * PI;
constexpr double SQRT3 = 1.7320508075688772;

double random_signed()
{
    thread_local std::mt19937 gen(std::random_device{}() ^ (unsigned) std::hash<std::thread::id>()(std::this_thread::get_id()));
    thread_local std::uniform_real_distribution<double> dist(-1.0, 1.0);
    return dist(gen);
}

inline double fract(double x) { return x - std::floor(x); }
inline double clamp01(double x) { return std::clamp(x, 0., 1.); }
inline double smoothstep(double e0, double e1, double x)
{
    if (e1 <= e0) return x < e0 ? 0. : 1.;
    const double t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3. - 2. * t);
}

// Periodic profile of period 1, range [-1, 1].
double profile_wave(double t, FuzzyProfile profile, double duty)
{
    const double f = fract(t);
    switch (profile) {
    case FuzzyProfile::Triangle: return 1. - 4. * std::abs(fract(t + 0.25) - 0.5);
    case FuzzyProfile::Square: {
        // Anti-aliased square wave: relief for the first `duty` of the period.
        const double e = 0.04;
        const double up = smoothstep(-e, e, f) * (1. - smoothstep(duty - e, duty + e, f)) + smoothstep(1. - e, 1. + e, f);
        return 2. * std::min(up, 1.) - 1.;
    }
    case FuzzyProfile::Sawtooth: return 2. * f - 1.;
    case FuzzyProfile::Sine:
    default: return std::sin(TWOPI * t);
    }
}

// Turn a normalized "distance from the groove" (0 at groove, 1 deep inside the cell) into relief.
inline double cell_relief(double x, double duty, FuzzyProfile profile)
{
    const double e0 = 1. - std::clamp(duty, 0.05, 0.95);
    switch (profile) {
    case FuzzyProfile::Sine:     return 2. * smoothstep(e0 * 0.5, std::min(1., e0 * 1.5 + 0.05), x) - 1.;   // soft pillow
    case FuzzyProfile::Triangle: return 2. * clamp01(x / std::max(e0 * 2., 1e-3)) - 1.;                      // chamfered
    case FuzzyProfile::Sawtooth: return 2. * clamp01(x) - 1.;
    case FuzzyProfile::Square:
    default:                     return 2. * smoothstep(e0 - 0.03, e0 + 0.03, x) - 1.;                       // crisp
    }
}

// 2D integer hash -> [0, 1)
inline double hash2(int x, int y, int seed = 0)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / double(0x1000000);
}

// Distance from (x, y) to the nearest edge of a pointy-top hexagonal grid with circumradius 1.
// Returns 0 on an edge, sqrt(3)/2 at a cell centre.
double hex_edge_distance(double x, double y)
{
    const double q = SQRT3 / 3. * x - 1. / 3. * y;
    const double r = 2. / 3. * y;
    double cx = q, cz = r, cy = -cx - cz;
    double rx = std::round(cx), ry = std::round(cy), rz = std::round(cz);
    const double dx = std::abs(rx - cx), dy = std::abs(ry - cy), dz = std::abs(rz - cz);
    if (dx > dy && dx > dz) rx = -ry - rz;
    else if (dy > dz)       ry = -rx - rz;
    else                    rz = -rx - ry;
    (void) ry;
    const double hx = SQRT3 * (rx + rz / 2.), hy = 1.5 * rz;
    const double px = std::abs(x - hx), py = std::abs(y - hy);
    return SQRT3 / 2. - std::max(px, px * 0.5 + py * SQRT3 / 2.);
}

// Jittered-grid Voronoi: returns F2 - F1 (0 on a cell border).
double voronoi_border(double x, double y)
{
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    double f1 = 1e9, f2 = 1e9;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            const int    cx = ix + i, cy = iy + j;
            const double px = cx + 0.1 + 0.8 * hash2(cx, cy, 1);
            const double py = cy + 0.1 + 0.8 * hash2(cx, cy, 2);
            const double d  = std::hypot(px - x, py - y);
            if (d < f1) { f2 = f1; f1 = d; }
            else if (d < f2) f2 = d;
        }
    return f2 - f1;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Height-map cache (shared between threads / regions)

struct FuzzyTexture::Image
{
    cv::Mat     gray;  // CV_32F, 0..1
    std::string path;
    std::time_t mtime { 0 };
    bool        valid() const { return !gray.empty(); }
    double      sample(double x, double y) const // x, y in pixels, wrapping
    {
        const int    w = gray.cols, h = gray.rows;
        const double fx = x - std::floor(x / w) * w, fy = y - std::floor(y / h) * h;
        const int    x0 = int(fx) % w, y0 = int(fy) % h, x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
        const double tx = fx - std::floor(fx), ty = fy - std::floor(fy);
        const float *r0 = gray.ptr<float>(y0), *r1 = gray.ptr<float>(y1);
        return (r0[x0] * (1. - tx) + r0[x1] * tx) * (1. - ty) + (r1[x0] * (1. - tx) + r1[x1] * tx) * ty;
    }
};

static std::shared_ptr<const FuzzyTexture::Image> load_height_map(const std::string &path)
{
    static std::mutex                                                     mtx;
    static std::map<std::string, std::shared_ptr<const FuzzyTexture::Image>> cache;
    if (path.empty())
        return nullptr;

    std::time_t mtime = 0;
    try {
        const boost::filesystem::path p(path);
        if (!boost::filesystem::exists(p))
            return nullptr;
        mtime = boost::filesystem::last_write_time(p);
    } catch (...) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mtx);
    if (auto it = cache.find(path); it != cache.end() && it->second && it->second->mtime == mtime)
        return it->second;

    auto img = std::make_shared<FuzzyTexture::Image>();
    img->path  = path;
    img->mtime = mtime;
    cv::Mat src = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (!src.empty()) {
        cv::Mat g;
        if (src.channels() == 4) {
            cv::cvtColor(src, g, cv::COLOR_BGRA2GRAY);
        } else if (src.channels() == 3) {
            cv::cvtColor(src, g, cv::COLOR_BGR2GRAY);
        } else
            g = src;
        const double maxv = g.depth() == CV_16U ? 65535. : g.depth() == CV_8U ? 255. : 1.;
        g.convertTo(img->gray, CV_32F, 1. / maxv);
        // Light blur so a few noisy pixels do not turn into nozzle chatter.
        cv::GaussianBlur(img->gray, img->gray, cv::Size(3, 3), 0.6);
    }
    cache[path] = img;
    return img;
}

// ---------------------------------------------------------------------------------------------------------------------

FuzzyTexture::FuzzyTexture(const FuzzyTextureParams &params) : m_params(params)
{
    FuzzyTextureParams &p = m_params;
    p.scale   = std::max(0.05, p.scale);
    p.stretch = std::clamp(p.stretch, 0.05, 50.);
    p.duty    = std::clamp(p.duty, 0.05, 0.95);
    p.jitter  = std::clamp(p.jitter, 0., 1.);

    const bool is_noise      = p.pattern <= FuzzyPattern::Voronoi;
    const bool default_xform = std::abs(p.angle_deg) < 1e-9 && std::abs(p.stretch - 1.) < 1e-9 && std::abs(p.offset_u) < 1e-9 &&
                               std::abs(p.offset_v) < 1e-9 && p.jitter == 0. && !p.invert;
    m_legacy = is_noise && (p.pattern == FuzzyPattern::Classic || (p.projection == FuzzyProjection::Auto && default_xform));
    if (!m_legacy && p.projection == FuzzyProjection::Auto)
        p.projection = FuzzyProjection::Box;

    const double a = p.angle_deg * PI / 180.;
    m_cos = std::cos(a);
    m_sin = std::sin(a);

    // Planar projection basis: the plane faces direction N (azimuth around Z, elevation above the XY plane).
    const double az = p.azimuth_deg * PI / 180., el = p.elevation_deg * PI / 180.;
    m_plane_n = Eigen::Vector3d(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el));
    m_plane_u = Eigen::Vector3d(-std::sin(az), std::cos(az), 0.);     // horizontal in-plane axis
    m_plane_v = m_plane_n.cross(m_plane_u).normalized();               // "up" in-plane axis
    if (m_plane_v.z() < 0) m_plane_v = -m_plane_v;

    const double freq = 1. / p.scale;
    switch (p.pattern) {
    case FuzzyPattern::Perlin: {
        auto m = std::make_unique<noise::module::Perlin>();
        m->SetFrequency(freq); m->SetOctaveCount(std::clamp(p.octaves, 1, 30)); m->SetPersistence(p.persistence);
        m_noise = std::move(m);
        break;
    }
    case FuzzyPattern::Billow: {
        auto m = std::make_unique<noise::module::Billow>();
        m->SetFrequency(freq); m->SetOctaveCount(std::clamp(p.octaves, 1, 30)); m->SetPersistence(p.persistence);
        m_noise = std::move(m);
        break;
    }
    case FuzzyPattern::RidgedMulti: {
        auto m = std::make_unique<noise::module::RidgedMulti>();
        m->SetFrequency(freq); m->SetOctaveCount(std::clamp(p.octaves, 1, 30));
        m_noise = std::move(m);
        break;
    }
    case FuzzyPattern::Voronoi: {
        auto m = std::make_unique<noise::module::Voronoi>();
        m->SetFrequency(freq); m->SetDisplacement(1.0);
        m_noise = std::move(m);
        break;
    }
    case FuzzyPattern::Woodgrain: {
        auto m = std::make_unique<noise::module::Perlin>();
        m->SetFrequency(1.); m->SetOctaveCount(3); m->SetPersistence(0.5);
        m_noise = std::move(m);
        break;
    }
    case FuzzyPattern::Image: m_image = load_height_map(p.image_path); break;
    default: break;
    }
}

FuzzyTexture::~FuzzyTexture() = default;

bool FuzzyTexture::image_missing() const
{
    return m_params.pattern == FuzzyPattern::Image && !(m_image && m_image->valid());
}

double FuzzyTexture::sampling_distance(double user_pd) const
{
    if (m_legacy || m_params.pattern == FuzzyPattern::Classic)
        return user_pd;
    // Several samples per feature, but never below 0.08 mm (G-code size / firmware queue).
    double feature = m_params.scale * std::min(1., m_params.stretch);
    if (m_params.pattern == FuzzyPattern::Image && m_image && m_image->valid())
        feature = std::max(m_params.scale / m_image->gray.cols * 2., 0.08) * 8.;
    return std::clamp(std::min(user_pd, feature / 12.), 0.08, std::max(user_pd, 0.08));
}

double FuzzyTexture::value_legacy(double x, double y, double z) const
{
    if (!m_noise || m_params.pattern == FuzzyPattern::Classic)
        return random_signed();
    return std::clamp(m_noise->GetValue(x, y, z), -1., 1.);
}

double FuzzyTexture::value(const Eigen::Vector3d &p, const Eigen::Vector2d &n) const
{
    if (m_legacy)
        return value_legacy(p.x(), p.y(), p.z());
    double v = project_and_eval(p, n);
    if (m_params.invert)
        v = -v;
    if (m_params.jitter > 0.)
        v = (1. - m_params.jitter) * v + m_params.jitter * random_signed();
    return std::clamp(v, -1., 1.);
}

double FuzzyTexture::project_and_eval(const Eigen::Vector3d &p, const Eigen::Vector2d &n) const
{
    switch (m_params.projection) {
    case FuzzyProjection::Cylindrical: {
        const double r     = std::hypot(p.x(), p.y());
        const double theta = std::atan2(p.y(), p.x());
        const double R     = m_params.repeats > 0 ? m_params.repeats * m_params.scale / TWOPI : r;
        return eval_uv(theta * R, p.z(), r);
    }
    case FuzzyProjection::Planar:
        return eval_uv(p.dot(m_plane_u), p.dot(m_plane_v), p.dot(m_plane_n));
    case FuzzyProjection::Box:
    case FuzzyProjection::Auto:
    default: {
        // Bi-planar (walls only have an XY normal): blend the X-facing and Y-facing maps.
        double wx = std::pow(std::abs(n.x()), 4.), wy = std::pow(std::abs(n.y()), 4.);
        const double sum = wx + wy;
        if (sum < 1e-12) { wx = 1.; wy = 0.; } else { wx /= sum; wy /= sum; }
        double v = 0.;
        // Mirror u on the opposite side so the pattern reads the same way on every face.
        if (wx > 1e-4) v += wx * eval_uv(n.x() >= 0 ? p.y() : -p.y(), p.z(), p.x());
        if (wy > 1e-4) v += wy * eval_uv(n.y() >= 0 ? -p.x() : p.x(), p.z(), p.y());
        return v;
    }
    }
}

double FuzzyTexture::eval_uv(double u, double v, double w) const
{
    u += m_params.offset_u;
    v += m_params.offset_v;
    // Rotate: angle 0 -> pattern lines run along u (parallel to the layers), 90 -> vertical.
    double a = u * m_cos + v * m_sin;
    double b = -u * m_sin + v * m_cos;
    a /= m_params.stretch;
    return pattern2d(a, b, w);
}

double FuzzyTexture::pattern2d(double a, double b, double w) const
{
    const FuzzyTextureParams &P  = m_params;
    const double              s  = P.scale;
    const double              du = P.duty;

    switch (P.pattern) {
    case FuzzyPattern::Classic: return random_signed();
    case FuzzyPattern::Perlin:
    case FuzzyPattern::Billow:
    case FuzzyPattern::RidgedMulti:
    case FuzzyPattern::Voronoi: return m_noise ? std::clamp(m_noise->GetValue(a, b, w), -1., 1.) : 0.;

    case FuzzyPattern::Ribs: return profile_wave(b / s, P.profile, du);

    case FuzzyPattern::Waves: {
        const double amp = (0.15 + 0.85 * du) * s; // duty controls wave height
        return profile_wave((b + amp * std::sin(TWOPI * a / (4. * s))) / s, P.profile, 0.5);
    }

    case FuzzyPattern::Knurl: {
        const double k = 1. / (s * std::sqrt(2.));
        const double w1 = profile_wave((a + b) * k, P.profile == FuzzyProfile::Sine ? FuzzyProfile::Triangle : P.profile, du);
        const double w2 = profile_wave((a - b) * k, P.profile == FuzzyProfile::Sine ? FuzzyProfile::Triangle : P.profile, du);
        return std::clamp(std::min(w1, w2) * 1.5 + 0.5, -1., 1.); // pyramids
    }

    case FuzzyPattern::Hexagon: {
        const double d = hex_edge_distance(a / (s * 0.5), b / (s * 0.5)) / (SQRT3 / 2.); // 0 edge .. 1 centre
        return cell_relief(d, du, P.profile);
    }

    case FuzzyPattern::Bricks: {
        const double h   = s * 0.5; // brick height = half the length (classic 2:1 bond)
        const double row = std::floor(b / h);
        const double aa  = a + ((int64_t(row) & 1) ? s * 0.5 : 0.);
        const double fx = fract(aa / s), fy = fract(b / h);
        const double dmm = std::min(std::min(fx, 1. - fx) * s, std::min(fy, 1. - fy) * h);
        return cell_relief(clamp01(dmm / (h * 0.5)), du, P.profile);
    }

    case FuzzyPattern::Dots: {
        const double row_h = s * SQRT3 / 2.;
        const int    j0    = int(std::floor(b / row_h));
        double       best  = 1e9;
        for (int j = j0 - 1; j <= j0 + 1; ++j) {
            const double off = (j & 1) ? s * 0.5 : 0.;
            const int    i0  = int(std::floor((a - off) / s));
            for (int i = i0 - 1; i <= i0 + 1; ++i)
                best = std::min(best, std::hypot(a - (i * s + off + s * 0.5), b - (j * row_h + row_h * 0.5)));
        }
        const double R = du * 0.5 * s;
        if (best >= R) return -1.;
        const double t = best / R;
        switch (P.profile) {
        case FuzzyProfile::Square:   return 2. * (1. - smoothstep(0.85, 1., t)) - 1.;
        case FuzzyProfile::Triangle: return 1. - 2. * t;
        default:                     return 2. * std::sqrt(std::max(0., 1. - t * t)) - 1.; // dome
        }
    }

    case FuzzyPattern::Woodgrain: {
        const double warp  = m_noise ? m_noise->GetValue(a / (6. * s), b / (2. * s), w / (6. * s)) : 0.;
        const double fine  = m_noise ? m_noise->GetValue(a / (0.7 * s), b / (0.15 * s), 7.3) : 0.;
        const double t     = b / s + 0.9 * warp;
        const double rings = std::pow(0.5 + 0.5 * std::sin(TWOPI * t), 1. + 6. * (1. - du));
        return std::clamp(2. * rings - 1. + 0.25 * fine, -1., 1.);
    }

    case FuzzyPattern::Cracks: {
        const double d = voronoi_border(a / s, b / s);
        return cell_relief(clamp01(d * 2.5), du, P.profile);
    }

    case FuzzyPattern::Weave: {
        const double ca = std::floor(a / s), cb = std::floor(b / s);
        const bool   horizontal = (int64_t(ca + cb) & 1) == 0;
        const double along  = horizontal ? a : b;
        const double across = horizontal ? b : a;
        const double strands = std::abs(std::sin(PI * 3. * across / s));          // 3 strands per tile
        const double over    = std::sin(PI * fract(along / s));                   // bulge over/under
        return std::clamp(2. * (0.55 * std::pow(strands, 0.6 + 1.4 * (1. - du)) + 0.45 * over) - 1., -1., 1.);
    }

    case FuzzyPattern::Scales: {
        const double h = s * 0.5, R = s * 0.5;
        const int    j = int(std::floor(b / h));
        double       val = -1.;
        // The scale of the upper row overlaps the lower one.
        for (int jj : { j + 1, j }) {
            const double off = (jj & 1) ? s * 0.5 : 0.;
            const double ci  = std::round((a - off - s * 0.5) / s);
            const double cx  = ci * s + off + s * 0.5, cy = jj * h;
            const double d   = std::hypot(a - cx, b - cy);
            if (d <= R && b <= cy) {
                const double t = d / R;
                val = P.profile == FuzzyProfile::Square ? (t < du ? 1. : -0.2) : 2. * std::pow(t, 1. + 2. * du) - 1.;
                break;
            }
        }
        return val;
    }

    case FuzzyPattern::Image: {
        if (!m_image || !m_image->valid())
            return 0.;
        const double W = m_image->gray.cols, H = m_image->gray.rows;
        const double tile_w = s, tile_h = s * H / W;
        // Image Y goes down, surface v goes up.
        return 2. * m_image->sample(a / tile_w * W, (1. - b / tile_h) * H) - 1.;
    }
    }
    return 0.;
}

} // namespace Slic3r
