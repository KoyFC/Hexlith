#include "domain/gpu/rasterizer.h"

#include "domain/gpu/float24.h"
#include "domain/gpu/shader_arithmetic.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace hexlith::domain::gpu {
namespace {

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

// A ten-bit field read as signed.
constexpr std::int32_t SignedField10(std::uint32_t word, unsigned low) {
    constexpr unsigned Shift = 22;
    return static_cast<std::int32_t>(Field(word, low, 10) << Shift) >> Shift;
}

float Float24Register(std::uint32_t value) {
    constexpr unsigned Float24Bits = 24;
    return Float24ToFloat32(Field(value, 0, Float24Bits));
}

// Rasterizer registers. Source: GBATEK, Rasterizer registers.
namespace raster {
constexpr std::uint16_t FaceCulling = 0x040;
constexpr std::uint16_t ViewportHalfWidth = 0x041;
constexpr std::uint16_t ViewportHalfHeight = 0x043;
constexpr std::uint16_t ClipEnable = 0x047;
constexpr std::uint16_t ClipCoefficientFirst = 0x048;
constexpr std::uint16_t DepthScale = 0x04D;
constexpr std::uint16_t DepthOffset = 0x04E;
constexpr std::uint16_t ScissorMode = 0x065;
constexpr std::uint16_t ScissorFirst = 0x066;
constexpr std::uint16_t ScissorLast = 0x067;
constexpr std::uint16_t ViewportCorner = 0x068;
constexpr std::uint16_t DepthMap = 0x06D;
}  // namespace raster

// Framebuffer registers. Source: GBATEK, Framebuffer registers.
namespace framebuffer {
constexpr std::uint16_t ColorOperation = 0x100;
constexpr std::uint16_t StencilTest = 0x105;
constexpr std::uint16_t StencilOperation = 0x106;
constexpr std::uint16_t DepthColorMask = 0x107;
constexpr std::uint16_t ColorWrite = 0x113;
constexpr std::uint16_t DepthWrite = 0x115;
constexpr std::uint16_t DepthFormat = 0x116;
constexpr std::uint16_t ColorFormat = 0x117;
constexpr std::uint16_t DepthAddress = 0x11C;
constexpr std::uint16_t ColorAddress = 0x11D;
constexpr std::uint16_t Dimensions = 0x11E;
}  // namespace framebuffer

constexpr std::uint16_t LightingDisable = 0x1C6;

// A vertex on its way through clipping and setup: its attributes, and once
// it has been divided by w, where it lands on the screen.
struct Vertex {
    std::array<float, OutputVertex::Slots> values{};
    std::array<float, 3> screen{};

    [[nodiscard]] Vec4 Position() const {
        return {values[0], values[1], values[2], values[3]};
    }
};

// Products here are the PICA's, zero for zero times infinity, as Azahar's
// float24 type computes them; sums run left to right.
float Dot(const Vec4& a, const Vec4& b) {
    return ShaderMultiply(a[0], b[0]) + ShaderMultiply(a[1], b[1]) + ShaderMultiply(a[2], b[2]) +
           ShaderMultiply(a[3], b[3]);
}

// ---------------------------------------------------------------------------
// Clipping, Sutherland-Hodgman against the planes of the view volume. Source:
// GBATEK, Rasterizer registers, for the custom plane; Azahar's AddTriangle
// for the planes themselves, the small tolerance on each and the plane just
// in front of w = 0 that keeps every w positive.

struct ClipPlane {
    Vec4 coefficients{};
    Vec4 bias{};
};

// Azahar's EPSILON_Z: a vertex this far outside a plane is kept, for games
// that draw 2D elements on the near plane.
constexpr float InsideTolerance = 0.00000001F;
constexpr float NearW = 0.00001F;

constexpr std::array<ClipPlane, 7> ViewPlanes = {{
    {.coefficients = {-1, 0, 0, 1}},
    {.coefficients = {1, 0, 0, 1}},
    {.coefficients = {0, -1, 0, 1}},
    {.coefficients = {0, 1, 0, 1}},
    {.coefficients = {0, 0, -1, 0}},
    {.coefficients = {0, 0, 1, 1}},
    {.coefficients = {0, 0, 0, 1}, .bias = {0, 0, 0, NearW}},
}};

float PlaneDistance(const ClipPlane& plane, const Vertex& vertex) {
    const Vec4 position = vertex.Position();
    const Vec4 biased{position[0] + plane.bias[0],
                      position[1] + plane.bias[1],
                      position[2] + plane.bias[2],
                      position[3] + plane.bias[3]};
    return Dot(biased, plane.coefficients);
}

bool Inside(const ClipPlane& plane, const Vertex& vertex) {
    return PlaneDistance(plane, vertex) >= -InsideTolerance;
}

// The point on the edge from `inside` to `outside` where the plane crosses.
Vertex Intersection(const ClipPlane& plane, const Vertex& first, const Vertex& second) {
    const float distance = PlaneDistance(plane, first);
    const float previous = PlaneDistance(plane, second);
    const float factor = previous / (previous - distance);
    Vertex result = first;
    for (std::size_t slot = 0; slot < OutputVertex::Slots; ++slot) {
        result.values.at(slot) = ShaderMultiply(first.values.at(slot), factor) +
                                 ShaderMultiply(second.values.at(slot), 1.0F - factor);
    }
    return result;
}

// Each plane adds at most one vertex to the three.
constexpr std::size_t MaximumPolygon = 3 + ViewPlanes.size() + 1;

struct Polygon {
    std::array<Vertex, MaximumPolygon> vertices{};
    std::size_t size = 0;

    void Add(const Vertex& vertex) {
        vertices.at(size++) = vertex;
    }
};

Polygon Clip(const Polygon& input, const ClipPlane& plane) {
    Polygon output;
    const Vertex* previous = &input.vertices.at(input.size - 1);
    for (std::size_t index = 0; index < input.size; ++index) {
        const Vertex& vertex = input.vertices.at(index);
        if (Inside(plane, vertex)) {
            if (!Inside(plane, *previous)) {
                output.Add(Intersection(plane, vertex, *previous));
            }
            output.Add(vertex);
        } else if (Inside(plane, *previous)) {
            output.Add(Intersection(plane, vertex, *previous));
        }
        previous = &vertex;
    }
    return output;
}

// A quaternion the opposite way round from the first vertex's is negated, so
// that interpolation takes the short way between them.
void FlipQuaternionIfOpposite(Vertex& vertex, const Vertex& reference) {
    constexpr std::size_t Q = OutputVertex::Quaternion;
    const Vec4 a{
        vertex.values[Q], vertex.values[Q + 1], vertex.values[Q + 2], vertex.values[Q + 3]};
    const Vec4 b{reference.values[Q],
                 reference.values[Q + 1],
                 reference.values[Q + 2],
                 reference.values[Q + 3]};
    if (Dot(a, b) < 0.0F) {
        for (std::size_t component = 0; component < 4; ++component) {
            vertex.values.at(Q + component) =
                ShaderMultiply(vertex.values.at(Q + component), -1.0F);
        }
    }
}

// Divides by w: the attributes become attribute over w, which is linear on
// the screen, and w itself its inverse. The position goes through the
// viewport. Source: GBATEK, GPUREG_VIEWPORT_*; Azahar's MakeScreenCoords.
void MakeScreenCoordinates(const RegisterFile& registers, Vertex& vertex) {
    const float half_width = Float24Register(registers.at(raster::ViewportHalfWidth));
    const float half_height = Float24Register(registers.at(raster::ViewportHalfHeight));
    const std::uint32_t corner = registers.at(raster::ViewportCorner);
    const auto offset_x = static_cast<float>(SignedField10(corner, 0));
    const auto offset_y = static_cast<float>(SignedField10(corner, 16));

    const float inverse_w = 1.0F / vertex.values[3];
    vertex.values[3] = inverse_w;
    for (std::size_t slot = OutputVertex::Quaternion; slot < OutputVertex::Slots; ++slot) {
        vertex.values.at(slot) = ShaderMultiply(vertex.values.at(slot), inverse_w);
    }
    vertex.screen[0] =
        ShaderMultiply(ShaderMultiply(vertex.values[0], inverse_w) + 1.0F, half_width) + offset_x;
    vertex.screen[1] =
        ShaderMultiply(ShaderMultiply(vertex.values[1], inverse_w) + 1.0F, half_height) + offset_y;
    vertex.screen[2] = ShaderMultiply(vertex.values[2], inverse_w);
}

// ---------------------------------------------------------------------------
// Setup and the fragment loop. Source: Azahar's ProcessTriangle; GBATEK for
// the culling, scissor and depth registers.

// 12.4 fixed point, rounded to nearest; a negative value wraps as Azahar's
// conversion to an unsigned 16 bits does on x86.
std::uint16_t ToFixed(float value) {
    constexpr float SubpixelScale = 16.0F;
    return static_cast<std::uint16_t>(ShaderTruncate(std::round(value * SubpixelScale)));
}

struct Point {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

Point ToPoint(const Vertex& vertex) {
    return Point{.x = ToFixed(vertex.screen[0]), .y = ToFixed(vertex.screen[1])};
}

std::int32_t SignedArea(const Point& a, const Point& b, const Point& c) {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

// The fill rule: a pixel exactly on a right-hand edge or on a flat bottom
// edge belongs to the neighbour, which a bias of -1 on that edge's weight
// does. Azahar takes this from the PSP; whether the PICA is the same is not
// known.
bool IsRightSideOrFlatBottomEdge(const Point& vertex, const Point& line1, const Point& line2) {
    if (line1.y == line2.y) {
        return vertex.y < line1.y;
    }
    return vertex.x < line1.x + ((line2.x - line1.x) * (vertex.y - line1.y) / (line2.y - line1.y));
}

constexpr std::uint32_t Subpixels = 16;
constexpr std::uint32_t PixelCentre = 8;
constexpr std::uint32_t FractionMask = Subpixels - 1;

struct Buffers {
    std::uint32_t color_address = 0;
    std::uint32_t depth_address = 0;
    PixelFormat color_format = PixelFormat::Rgba8;
    DepthFormat depth_format = DepthFormat::D16;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

std::optional<Buffers> ReadBuffers(const RegisterFile& registers) {
    constexpr unsigned AddressShift = 3;
    constexpr std::uint32_t AddressMask = 0x0FFFFFFF;
    const std::optional<PixelFormat> color =
        ColorBufferFormat(Field(registers.at(framebuffer::ColorFormat), 16, 3));
    const std::optional<DepthFormat> depth =
        DepthBufferFormat(Field(registers.at(framebuffer::DepthFormat), 0, 2));
    if (!color.has_value() || !depth.has_value()) {
        return std::nullopt;
    }
    const std::uint32_t dimensions = registers.at(framebuffer::Dimensions);
    return Buffers{
        .color_address = (registers.at(framebuffer::ColorAddress) & AddressMask) << AddressShift,
        .depth_address = (registers.at(framebuffer::DepthAddress) & AddressMask) << AddressShift,
        .color_format = *color,
        .depth_format = *depth,
        .width = Field(dimensions, 0, 11),
        .height = Field(dimensions, 12, 10) + 1};
}

// A triangle once set up, and what each of its pixels needs.
class TriangleFiller {
public:
    TriangleFiller(const PicaState& state,
                   GpuMemory& memory,
                   Sampling sampling,
                   const Buffers& buffers,
                   RasterizerStatistics& statistics)
        : state_(&state),
          registers_(&state.Registers()),
          memory_(&memory),
          sampling_(sampling),
          buffers_(&buffers),
          statistics_(&statistics) {}

    // Culls a triangle or puts its vertices in the order it is filled in.
    void Fill(const Vertex& v0, const Vertex& v1, const Vertex& v2);

private:
    struct Weights {
        std::array<float, 3> barycentric{};
        float inverse_w = 0.0F;
    };

    [[nodiscard]] Weights WeightsOf(std::int32_t w0, std::int32_t w1, std::int32_t w2) const;
    [[nodiscard]] Weights WeightsAt(const Point& point) const;
    [[nodiscard]] float Interpolate(const Weights& weights, std::size_t slot) const;
    [[nodiscard]] std::array<TextureCoordinate, 3> TextureCoordinates(const Point& pixel,
                                                                      const Weights& weights) const;
    void Shade(const Point& pixel, std::int32_t w0, std::int32_t w1, std::int32_t w2);
    [[nodiscard]] float
    Depth(const Weights& weights, std::int32_t w0, std::int32_t w1, std::int32_t w2) const;
    [[nodiscard]] bool DepthStencilTest(std::size_t offset, float depth) const;
    [[nodiscard]] std::optional<std::size_t> PixelIndex(const Point& pixel) const;
    void FillInOrder(const Vertex& v0, const Vertex& v1, const Vertex& v2);

    [[nodiscard]] std::uint32_t Register(std::uint16_t id) const {
        return registers_->at(id);
    }

    const PicaState* state_;
    const RegisterFile* registers_;
    GpuMemory* memory_;
    Sampling sampling_;
    const Buffers* buffers_;
    RasterizerStatistics* statistics_;
    std::array<const Vertex*, 3> vertices_{};
    std::array<Point, 3> positions_{};
};

void TriangleFiller::Fill(const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    // GPUREG_FACECULLING_CONFIG: 1 keeps clockwise triangles, 2 counter-
    // clockwise ones, 0 and 3 both. A triangle is filled counter-clockwise,
    // with two of its vertices swapped if it has to be.
    const Point p0 = ToPoint(v0);
    const Point p1 = ToPoint(v1);
    const Point p2 = ToPoint(v2);
    const std::int32_t area = SignedArea(p0, p1, p2);
    switch (Field(Register(raster::FaceCulling), 0, 2)) {
    case 1:
        if (area < 0) {
            FillInOrder(v0, v2, v1);
        }
        return;
    case 2:
        if (area > 0) {
            FillInOrder(v0, v1, v2);
        }
        return;
    default:
        if (area > 0) {
            FillInOrder(v0, v1, v2);
        } else {
            FillInOrder(v0, v2, v1);
        }
        return;
    }
}

void TriangleFiller::FillInOrder(const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    vertices_ = {&v0, &v1, &v2};
    positions_ = {ToPoint(v0), ToPoint(v1), ToPoint(v2)};
    ++statistics_->triangles;

    // The positions are already 16-bit values.
    std::uint32_t min_x =
        static_cast<std::uint32_t>(std::min({positions_[0].x, positions_[1].x, positions_[2].x}));
    std::uint32_t min_y =
        static_cast<std::uint32_t>(std::min({positions_[0].y, positions_[1].y, positions_[2].y}));
    std::uint32_t max_x =
        static_cast<std::uint32_t>(std::max({positions_[0].x, positions_[1].x, positions_[2].x}));
    std::uint32_t max_y =
        static_cast<std::uint32_t>(std::max({positions_[0].y, positions_[1].y, positions_[2].y}));

    // GPUREG_SCISSORTEST_MODE: 1 leaves out the box, 3 leaves out all but
    // it; the second corner is inclusive.
    constexpr std::uint32_t ScissorExclude = 1;
    constexpr std::uint32_t ScissorInclude = 3;
    const std::uint32_t scissor = Field(Register(raster::ScissorMode), 0, 2);
    const std::uint32_t first = Register(raster::ScissorFirst);
    const std::uint32_t last = Register(raster::ScissorLast);
    const std::uint32_t scissor_x1 = (Field(first, 0, 10) * Subpixels) & 0xFFFFU;
    const std::uint32_t scissor_y1 = (Field(first, 16, 10) * Subpixels) & 0xFFFFU;
    const std::uint32_t scissor_x2 = ((Field(last, 0, 10) + 1) * Subpixels) & 0xFFFFU;
    const std::uint32_t scissor_y2 = ((Field(last, 16, 10) + 1) * Subpixels) & 0xFFFFU;
    if (scissor == ScissorInclude) {
        min_x = std::max(min_x, scissor_x1);
        min_y = std::max(min_y, scissor_y1);
        max_x = std::min(max_x, scissor_x2);
        max_y = std::min(max_y, scissor_y2);
    }
    min_x &= ~FractionMask;
    min_y &= ~FractionMask;
    max_x = (max_x + FractionMask) & ~FractionMask & 0xFFFFU;
    max_y = (max_y + FractionMask) & ~FractionMask & 0xFFFFU;

    const std::int32_t bias0 =
        IsRightSideOrFlatBottomEdge(positions_[0], positions_[1], positions_[2]) ? -1 : 0;
    const std::int32_t bias1 =
        IsRightSideOrFlatBottomEdge(positions_[1], positions_[2], positions_[0]) ? -1 : 0;
    const std::int32_t bias2 =
        IsRightSideOrFlatBottomEdge(positions_[2], positions_[0], positions_[1]) ? -1 : 0;

    for (std::uint32_t y = min_y + PixelCentre; y < max_y; y += Subpixels) {
        for (std::uint32_t x = min_x + PixelCentre; x < max_x; x += Subpixels) {
            if (scissor == ScissorExclude && x >= scissor_x1 && x < scissor_x2 && y >= scissor_y1 &&
                y < scissor_y2) {
                continue;
            }
            const Point pixel{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)};
            const std::int32_t w0 = bias0 + SignedArea(positions_[1], positions_[2], pixel);
            const std::int32_t w1 = bias1 + SignedArea(positions_[2], positions_[0], pixel);
            const std::int32_t w2 = bias2 + SignedArea(positions_[0], positions_[1], pixel);
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            Shade(pixel, w0, w1, w2);
        }
    }
}

TriangleFiller::Weights
TriangleFiller::WeightsOf(std::int32_t w0, std::int32_t w1, std::int32_t w2) const {
    Weights weights;
    weights.barycentric = {static_cast<float>(w0), static_cast<float>(w1), static_cast<float>(w2)};
    weights.inverse_w = 1.0F / (ShaderMultiply(vertices_[0]->values[3], weights.barycentric[0]) +
                                ShaderMultiply(vertices_[1]->values[3], weights.barycentric[1]) +
                                ShaderMultiply(vertices_[2]->values[3], weights.barycentric[2]));
    return weights;
}

// The weights at a point without the fill rule's bias, for the rates of
// change.
TriangleFiller::Weights TriangleFiller::WeightsAt(const Point& point) const {
    return WeightsOf(SignedArea(positions_[1], positions_[2], point),
                     SignedArea(positions_[2], positions_[0], point),
                     SignedArea(positions_[0], positions_[1], point));
}

// Perspective-correct: attribute over w interpolated linearly, divided by the
// interpolated one over w.
float TriangleFiller::Interpolate(const Weights& weights, std::size_t slot) const {
    const float over_w = ShaderMultiply(vertices_[0]->values.at(slot), weights.barycentric[0]) +
                         ShaderMultiply(vertices_[1]->values.at(slot), weights.barycentric[1]) +
                         ShaderMultiply(vertices_[2]->values.at(slot), weights.barycentric[2]);
    return ShaderMultiply(over_w, weights.inverse_w);
}

std::array<TextureCoordinate, 3> TriangleFiller::TextureCoordinates(const Point& pixel,
                                                                    const Weights& weights) const {
    constexpr std::array<std::size_t, 3> Slots = {
        OutputVertex::Texcoord0, OutputVertex::Texcoord1, OutputVertex::Texcoord2};
    std::array<TextureCoordinate, 3> coordinates{};
    for (std::size_t unit = 0; unit < Slots.size(); ++unit) {
        coordinates.at(unit).u = Interpolate(weights, Slots.at(unit));
        coordinates.at(unit).v = Interpolate(weights, Slots.at(unit) + 1);
    }
    if (sampling_ != Sampling::Hardware) {
        return coordinates;
    }
    // The rate of change, from the coordinates one pixel to the right and
    // one up.
    const auto s = static_cast<std::int32_t>(Subpixels);
    const Weights right = WeightsAt({.x = pixel.x + s, .y = pixel.y});
    const Weights up = WeightsAt({.x = pixel.x, .y = pixel.y + s});
    for (std::size_t unit = 0; unit < Slots.size(); ++unit) {
        TextureCoordinate& coordinate = coordinates.at(unit);
        coordinate.du_dx = Interpolate(right, Slots.at(unit)) - coordinate.u;
        coordinate.dv_dx = Interpolate(right, Slots.at(unit) + 1) - coordinate.v;
        coordinate.du_dy = Interpolate(up, Slots.at(unit)) - coordinate.u;
        coordinate.dv_dy = Interpolate(up, Slots.at(unit) + 1) - coordinate.v;
    }
    return coordinates;
}

// z over w interpolated linearly, through GPUREG_DEPTHMAP_SCALE and OFFSET;
// with GPUREG_DEPTHMAP_ENABLE clear, multiplied by w for a W-buffer.
float TriangleFiller::Depth(const Weights& weights,
                            std::int32_t w0,
                            std::int32_t w1,
                            std::int32_t w2) const {
    const std::int32_t sum = w0 + w1 + w2;
    const float z_over_w = ((vertices_[0]->screen[2] * static_cast<float>(w0)) +
                            (vertices_[1]->screen[2] * static_cast<float>(w1)) +
                            (vertices_[2]->screen[2] * static_cast<float>(w2))) /
                           static_cast<float>(sum);
    const float scale = Float24Register(Register(raster::DepthScale));
    const float offset = Float24Register(Register(raster::DepthOffset));
    float depth = (z_over_w * scale) + offset;
    if (Field(Register(raster::DepthMap), 0, 1) == 0) {
        depth *= weights.inverse_w * static_cast<float>(sum);
    }
    return std::clamp(depth, 0.0F, 1.0F);
}

// Where a pixel of the buffers is, counted in pixels; the buffers are stored
// bottom row first. Nothing outside them is drawn.
std::optional<std::size_t> TriangleFiller::PixelIndex(const Point& pixel) const {
    const auto x = static_cast<std::uint32_t>(pixel.x) / Subpixels;
    const auto y = static_cast<std::uint32_t>(pixel.y) / Subpixels;
    if (x >= buffers_->width || y >= buffers_->height) {
        return std::nullopt;
    }
    return TiledOffset(x, buffers_->height - 1 - y, buffers_->width, 1);
}

void TriangleFiller::Shade(const Point& pixel, std::int32_t w0, std::int32_t w1, std::int32_t w2) {
    ++statistics_->fragments;
    const std::optional<std::size_t> index = PixelIndex(pixel);
    if (!index.has_value()) {
        return;
    }
    const Weights weights = WeightsOf(w0, w1, w2);
    const float depth = Depth(weights, w0, w1, w2);

    constexpr float ColorScale = 255.0F;
    std::array<std::uint8_t, 4> primary{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        primary.at(channel) = static_cast<std::uint8_t>(ShaderTruncate(
            std::round(Interpolate(weights, OutputVertex::Color + channel) * ColorScale)));
    }
    const std::array<TextureCoordinate, 3> coordinates = TextureCoordinates(pixel, weights);
    const std::array<Color, 4> textures =
        SampleTextures(*registers_,
                       *memory_,
                       coordinates,
                       Interpolate(weights, OutputVertex::Texcoord0W),
                       sampling_,
                       statistics_->unsupported);
    std::array<Color, 2> lighting{};
    if (Field(Register(LightingDisable), 0, 1) == 0) {
        const Vec4 quaternion{Interpolate(weights, OutputVertex::Quaternion),
                              Interpolate(weights, OutputVertex::Quaternion + 1),
                              Interpolate(weights, OutputVertex::Quaternion + 2),
                              Interpolate(weights, OutputVertex::Quaternion + 3)};
        const std::array<float, 3> view{Interpolate(weights, OutputVertex::View),
                                        Interpolate(weights, OutputVertex::View + 1),
                                        Interpolate(weights, OutputVertex::View + 2)};
        lighting = LightFragment(*registers_, *state_, quaternion, view, textures);
    }
    Color color = CombineTextures(
        *registers_,
        textures,
        Color{.red = primary[0], .green = primary[1], .blue = primary[2], .alpha = primary[3]},
        lighting);

    if (!PassesAlphaTest(*registers_, color.alpha)) {
        return;
    }
    ApplyFog(*registers_, *state_, depth, color);
    if (!DepthStencilTest(*index, depth)) {
        return;
    }
    if (Field(Register(framebuffer::ColorWrite), 0, 4) == 0) {
        return;
    }
    const auto bytes_per_pixel = static_cast<std::uint32_t>(BytesPerPixel(buffers_->color_format));
    const std::span<std::uint8_t> bytes = memory_->Bytes(
        buffers_->color_address + static_cast<std::uint32_t>(*index * bytes_per_pixel),
        bytes_per_pixel);
    if (bytes.empty()) {
        return;
    }
    EncodePixel(buffers_->color_format,
                MergeColor(*registers_, color, DecodePixel(buffers_->color_format, bytes)),
                bytes);
}

// The stencil test, then the depth test, each updating the stencil buffer
// by its action. Source: GBATEK, GPUREG_STENCIL_TEST and _OP, GPUREG_DEPTH_
// COLOR_MASK; Azahar's DoDepthStencilTest for the stencil being tested only
// with a D24S8 buffer, for depth being written while its test is off, and
// for the action on passing being taken while it is off too.
bool TriangleFiller::DepthStencilTest(std::size_t offset, float depth) const {
    const auto bytes_per_pixel = static_cast<std::uint32_t>(BytesPerPixel(buffers_->depth_format));
    std::span<std::uint8_t> bytes = memory_->Bytes(
        buffers_->depth_address + static_cast<std::uint32_t>(offset * bytes_per_pixel),
        bytes_per_pixel);
    // A depth buffer outside GPU memory reads as zero and keeps nothing.
    std::array<std::uint8_t, 4> nowhere{};
    if (bytes.empty()) {
        bytes = std::span<std::uint8_t>(nowhere).first(bytes_per_pixel);
    }
    const std::uint32_t stencil_test = Register(framebuffer::StencilTest);
    const std::uint32_t stencil_operation = Register(framebuffer::StencilOperation);
    const std::uint32_t mask = Register(framebuffer::DepthColorMask);
    const bool writes = Field(Register(framebuffer::DepthWrite), 0, 2) != 0;
    const bool stencil_enabled =
        Field(stencil_test, 0, 1) != 0 && buffers_->depth_format == DepthFormat::D24S8;
    const auto reference = static_cast<std::uint8_t>(Field(stencil_test, 16, 8));
    const auto write_mask = static_cast<std::uint8_t>(Field(stencil_test, 8, 8));
    const std::uint8_t old_stencil = stencil_enabled ? DecodeStencil(bytes) : std::uint8_t{0};

    const auto update_stencil = [&](unsigned action_shift) {
        const std::uint8_t result =
            StencilAction(Field(stencil_operation, action_shift, 3), old_stencil, reference);
        if (writes) {
            EncodeStencil(
                static_cast<std::uint8_t>((result & write_mask) | (old_stencil & ~write_mask)),
                bytes);
        }
    };

    if (stencil_enabled) {
        const auto input_mask = static_cast<std::uint8_t>(Field(stencil_test, 24, 8));
        if (!Compare(Field(stencil_test, 4, 3), reference & input_mask, old_stencil & input_mask)) {
            update_stencil(0);
            return false;
        }
    }

    const std::uint32_t z =
        TruncateToUnsigned(depth * static_cast<float>(MaximumDepth(buffers_->depth_format)));
    if (Field(mask, 0, 1) != 0 &&
        !Compare(Field(mask, 4, 3), z, DecodeDepth(buffers_->depth_format, bytes))) {
        if (stencil_enabled) {
            update_stencil(4);
        }
        return false;
    }
    if (writes && Field(mask, 12, 1) != 0) {
        EncodeDepth(buffers_->depth_format, z, bytes);
    }
    if (stencil_enabled) {
        update_stencil(8);
    }
    return true;
}

}  // namespace

void Rasterizer::DrawTriangle(const PicaState& state,
                              const PrimitiveAssembler::Triangle& triangle) {
    const RegisterFile& registers = state.Registers();
    // GPUREG_COLOR_OPERATION's fragment operation mode: 0 is the usual; gas
    // and shadow rendering are not implemented.
    if (Field(registers.at(framebuffer::ColorOperation), 0, 2) != 0) {
        statistics_.unsupported = true;
        return;
    }
    const std::optional<Buffers> buffers = ReadBuffers(registers);
    if (!buffers.has_value()) {
        statistics_.unsupported = true;
        return;
    }

    Polygon polygon;
    for (const OutputVertex& vertex : triangle) {
        polygon.Add(Vertex{.values = vertex.values});
    }
    FlipQuaternionIfOpposite(polygon.vertices[1], polygon.vertices[0]);
    FlipQuaternionIfOpposite(polygon.vertices[2], polygon.vertices[0]);

    for (const ClipPlane& plane : ViewPlanes) {
        polygon = Clip(polygon, plane);
        if (polygon.size < 3) {
            return;
        }
    }
    // GPUREG_FRAGOP_CLIPPING and its plane's four float24 coefficients.
    if (Field(registers.at(raster::ClipEnable), 0, 1) != 0) {
        ClipPlane custom{};
        for (std::size_t component = 0; component < 4; ++component) {
            custom.coefficients.at(component) = Float24Register(
                registers.at(raster::ClipCoefficientFirst + static_cast<std::uint16_t>(component)));
        }
        polygon = Clip(polygon, custom);
        if (polygon.size < 3) {
            return;
        }
    }

    // A fan from the first vertex.
    for (std::size_t index = 0; index < polygon.size; ++index) {
        MakeScreenCoordinates(registers, polygon.vertices.at(index));
    }
    TriangleFiller filler(state, *memory_, sampling_, *buffers, statistics_);
    for (std::size_t index = 0; index + 2 < polygon.size; ++index) {
        filler.Fill(
            polygon.vertices[0], polygon.vertices.at(index + 1), polygon.vertices.at(index + 2));
    }
}

}  // namespace hexlith::domain::gpu
