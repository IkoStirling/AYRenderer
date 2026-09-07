#include "detail/SMAAPass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayt::render::detail
{

namespace {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }

Vec2 lerp(Vec2 a, Vec2 b, float t)
{
    return a + (b - a) * t;
}

Vec2 lineArea(Vec2 p1, Vec2 p2, int pixel)
{
    const Vec2 d = p2 - p1;
    const float x1 = static_cast<float>(pixel);
    const float x2 = x1 + 1.0f;
    const float y1 = p1.y + d.y * (x1 - p1.x) / d.x;
    const float y2 = p1.y + d.y * (x2 - p1.x) / d.x;
    const bool inside = (x1 >= p1.x && x1 < p2.x)
        || (x2 > p1.x && x2 <= p2.x);
    if (!inside) {
        return {};
    }

    const bool trapezoid = std::signbit(y1) == std::signbit(y2)
        || std::abs(y1) < 0.0001f || std::abs(y2) < 0.0001f;
    if (trapezoid) {
        const float area = 0.5f * (y1 + y2);
        return area < 0.0f ? Vec2{std::abs(area), 0.0f}
                           : Vec2{0.0f, std::abs(area)};
    }

    const float crossing = -p1.y * d.x / d.y + p1.x;
    float integral = 0.0f;
    const float fraction = std::modf(crossing, &integral);
    const float a1 = crossing > p1.x ? y1 * fraction * 0.5f : 0.0f;
    const float a2 = crossing < p2.x
        ? y2 * (1.0f - fraction) * 0.5f : 0.0f;
    const float dominant = std::abs(a1) > std::abs(a2) ? a1 : -a2;
    return dominant < 0.0f
        ? Vec2{std::abs(a1), std::abs(a2)}
        : Vec2{std::abs(a2), std::abs(a1)};
}

std::pair<Vec2, Vec2> smoothArea(float distance, Vec2 a1, Vec2 a2)
{
    const Vec2 b1{0.5f * std::sqrt(2.0f * a1.x),
                  0.5f * std::sqrt(2.0f * a1.y)};
    const Vec2 b2{0.5f * std::sqrt(2.0f * a2.x),
                  0.5f * std::sqrt(2.0f * a2.y)};
    const float t = std::clamp(distance / 32.0f, 0.0f, 1.0f);
    return {lerp(b1, a1, t), lerp(b2, a2, t)};
}

Vec2 areaOrtho(uint8_t pattern, int left, int right, float offset)
{
    const float distance = static_cast<float>(left + right + 1);
    const float upper = 0.5f + offset;
    const float lower = upper - 1.0f;
    const Vec2 leftLower{0.0f, lower};
    const Vec2 leftUpper{0.0f, upper};
    const Vec2 middle{distance * 0.5f, 0.0f};
    const Vec2 rightLower{distance, lower};
    const Vec2 rightUpper{distance, upper};

    switch (pattern) {
    case 0: return {};
    case 1: return left <= right ? lineArea(leftLower, middle, left) : Vec2{};
    case 2: return left >= right ? lineArea(middle, rightLower, left) : Vec2{};
    case 3: {
        const auto smoothed = smoothArea(
            distance, lineArea(leftLower, middle, left),
            lineArea(middle, rightLower, left));
        return smoothed.first + smoothed.second;
    }
    case 4: return left <= right ? lineArea(leftUpper, middle, left) : Vec2{};
    case 5: return {};
    case 6: {
        const Vec2 full = lineArea(leftUpper, rightLower, left);
        if (std::abs(offset) < 0.0001f) {
            return full;
        }
        const Vec2 split = lineArea(leftUpper, middle, left)
            + lineArea(middle, rightLower, left);
        return (full + split) * 0.5f;
    }
    case 7: return lineArea(leftUpper, rightLower, left);
    case 8: return left >= right ? lineArea(middle, rightUpper, left) : Vec2{};
    case 9: {
        const Vec2 full = lineArea(leftLower, rightUpper, left);
        if (std::abs(offset) < 0.0001f) {
            return full;
        }
        const Vec2 split = lineArea(leftLower, middle, left)
            + lineArea(middle, rightUpper, left);
        return (full + split) * 0.5f;
    }
    case 10: return {};
    case 11: return lineArea(leftLower, rightUpper, left);
    case 12: {
        const auto smoothed = smoothArea(
            distance, lineArea(leftUpper, middle, left),
            lineArea(middle, rightUpper, left));
        return smoothed.first + smoothed.second;
    }
    case 13: return lineArea(leftLower, rightUpper, left);
    case 14: return lineArea(leftUpper, rightLower, left);
    case 15: return {};
    default: return {};
    }
}

float lineSide(Vec2 point, Vec2 p1, Vec2 p2)
{
    const Vec2 middle = (p1 + p2) * 0.5f;
    const float a = p2.y - p1.y;
    const float b = p1.x - p2.x;
    return a * (point.x - middle.x) + b * (point.y - middle.y);
}

float halfPlaneSquareCoverage(Vec2 p1, Vec2 p2, Vec2 pixel)
{
    std::array<Vec2, 8> input{};
    std::array<Vec2, 8> output{};
    input[0] = pixel;
    input[1] = pixel + Vec2{1.0f, 0.0f};
    input[2] = pixel + Vec2{1.0f, 1.0f};
    input[3] = pixel + Vec2{0.0f, 1.0f};
    size_t inputCount = 4;
    size_t outputCount = 0;

    for (size_t i = 0; i < inputCount; ++i) {
        const Vec2 current = input[i];
        const Vec2 next = input[(i + 1u) % inputCount];
        const float currentSide = lineSide(current, p1, p2);
        const float nextSide = lineSide(next, p1, p2);
        const bool currentInside = currentSide >= 0.0f;
        const bool nextInside = nextSide >= 0.0f;
        if (currentInside) {
            output[outputCount++] = current;
        }
        if (currentInside != nextInside) {
            const float denominator = currentSide - nextSide;
            const float t = std::abs(denominator) > 0.000001f
                ? currentSide / denominator : 0.0f;
            output[outputCount++] = current + (next - current) * t;
        }
    }
    if (outputCount < 3u) {
        return 0.0f;
    }

    float twiceArea = 0.0f;
    for (size_t i = 0; i < outputCount; ++i) {
        const Vec2 a = output[i];
        const Vec2 b = output[(i + 1u) % outputCount];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    return std::clamp(std::abs(twiceArea) * 0.5f, 0.0f, 1.0f);
}

constexpr std::array<std::array<uint8_t, 2>, 16> kDiagEdgeLayout = {{
    {{0, 0}}, {{1, 0}}, {{0, 2}}, {{1, 2}},
    {{2, 0}}, {{3, 0}}, {{2, 2}}, {{3, 2}},
    {{0, 1}}, {{1, 1}}, {{0, 3}}, {{1, 3}},
    {{2, 1}}, {{3, 1}}, {{2, 3}}, {{3, 3}},
}};

Vec2 diagonalLineArea(uint8_t pattern, Vec2 p1, Vec2 p2,
                      int left, Vec2 offset)
{
    if (kDiagEdgeLayout[pattern][0] > 0u) {
        p1 = p1 + offset;
    }
    if (kDiagEdgeLayout[pattern][1] > 0u) {
        p2 = p2 + offset;
    }
    const Vec2 base = Vec2{1.0f, 0.0f}
        + Vec2{static_cast<float>(left), static_cast<float>(left)};
    const float a1 = halfPlaneSquareCoverage(p1, p2, base);
    const float a2 = halfPlaneSquareCoverage(
        p1, p2, base + Vec2{0.0f, 1.0f});
    return {1.0f - a1, a2};
}

Vec2 areaDiag(uint8_t pattern, int left, int right, Vec2 offset)
{
    const float distance = static_cast<float>(left + right + 1);
    const Vec2 diagonal{distance, distance};
    const Vec2 p00{0.0f, 0.0f};
    const Vec2 p10{1.0f, 0.0f};
    const Vec2 p11{1.0f, 1.0f};
    const auto area = [&](Vec2 p1, Vec2 p2) {
        return diagonalLineArea(pattern, p1, p2, left, offset);
    };
    const auto average = [](Vec2 a, Vec2 b) { return (a + b) * 0.5f; };

    switch (pattern) {
    case 0:  return average(area(p11, p11 + diagonal),
                            area(p10, p10 + diagonal));
    case 1:  return average(area(p10, p00 + diagonal),
                            area(p10, p10 + diagonal));
    case 2:  return average(area(p00, p10 + diagonal),
                            area(p10, p10 + diagonal));
    case 3:  return area(p10, p10 + diagonal);
    case 4:  return average(area(p11, p00 + diagonal),
                            area(p11, p10 + diagonal));
    case 5:  return average(area(p11, p00 + diagonal),
                            area(p10, p10 + diagonal));
    case 6:  return area(p11, p10 + diagonal);
    case 7:  return average(area(p11, p10 + diagonal),
                            area(p10, p10 + diagonal));
    case 8:  return average(area(p00, p11 + diagonal),
                            area(p10, p11 + diagonal));
    case 9:  return area(p10, p11 + diagonal);
    case 10: return average(area(p00, p11 + diagonal),
                            area(p10, p10 + diagonal));
    case 11: return average(area(p10, p11 + diagonal),
                            area(p10, p10 + diagonal));
    case 12: return area(p11, p11 + diagonal);
    case 13: return average(area(p11, p11 + diagonal),
                            area(p10, p11 + diagonal));
    case 14: return average(area(p11, p11 + diagonal),
                            area(p11, p10 + diagonal));
    case 15: return average(area(p11, p11 + diagonal),
                            area(p10, p10 + diagonal));
    default: return {};
    }
}

uint8_t toAreaByte(float value)
{
    return static_cast<uint8_t>(
        std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

void writeArea(std::vector<uint8_t>& pixels, uint16_t x, uint16_t y,
               Vec2 area)
{
    const size_t index =
        (static_cast<size_t>(y) * SMAAPass::kAreaTextureWidth + x) * 2u;
    pixels[index] = toAreaByte(area.x);
    pixels[index + 1u] = toAreaByte(area.y);
}

std::array<int8_t, 33> makeSearchReverseLookup()
{
    std::array<int8_t, 33> reverse{};
    reverse.fill(-1);
    for (uint8_t pattern = 0; pattern < 16u; ++pattern) {
        const int value = ((pattern >> 0u) & 1u)
            + 3 * ((pattern >> 1u) & 1u)
            + 7 * ((pattern >> 2u) & 1u)
            + 21 * ((pattern >> 3u) & 1u);
        reverse[static_cast<size_t>(value)] = static_cast<int8_t>(pattern);
    }
    return reverse;
}

bool searchBit(int8_t pattern, uint8_t bit)
{
    return pattern >= 0
        && ((static_cast<uint8_t>(pattern) >> bit) & 1u) != 0u;
}

uint8_t searchDeltaLeft(int8_t left, int8_t top)
{
    uint8_t distance = searchBit(top, 3u) ? 1u : 0u;
    if (distance == 1u && searchBit(top, 2u)
        && !searchBit(left, 1u) && !searchBit(left, 3u)) {
        ++distance;
    }
    return distance;
}

uint8_t searchDeltaRight(int8_t left, int8_t top)
{
    uint8_t distance = searchBit(top, 3u)
        && !searchBit(left, 1u) && !searchBit(left, 3u) ? 1u : 0u;
    if (distance == 1u && searchBit(top, 2u)
        && !searchBit(left, 0u) && !searchBit(left, 2u)) {
        ++distance;
    }
    return distance;
}

} // namespace

std::vector<uint8_t> generateSmaaAreaTextureRg8()
{
    constexpr std::array<std::array<uint8_t, 2>, 16> orthoLayout = {{
        {{0, 0}}, {{3, 0}}, {{0, 3}}, {{3, 3}},
        {{1, 0}}, {{4, 0}}, {{1, 3}}, {{4, 3}},
        {{0, 1}}, {{3, 1}}, {{0, 4}}, {{3, 4}},
        {{1, 1}}, {{4, 1}}, {{1, 4}}, {{4, 4}},
    }};
    constexpr std::array<float, 7> orthoOffsets = {
        0.0f, -0.25f, 0.25f, -0.125f, 0.125f, -0.375f, 0.375f,
    };
    constexpr std::array<Vec2, 5> diagOffsets = {{
        {0.0f, 0.0f}, {0.25f, -0.25f}, {-0.25f, 0.25f},
        {0.125f, -0.125f}, {-0.125f, 0.125f},
    }};
    std::vector<uint8_t> pixels(
        static_cast<size_t>(SMAAPass::kAreaTextureWidth)
            * SMAAPass::kAreaTextureHeight * 2u,
        0u);

    constexpr uint16_t orthoSize = 16;
    for (uint16_t slice = 0; slice < orthoOffsets.size(); ++slice) {
        for (uint8_t pattern = 0; pattern < orthoLayout.size(); ++pattern) {
            for (uint16_t y = 0; y < orthoSize; ++y) {
                for (uint16_t x = 0; x < orthoSize; ++x) {
                    const Vec2 area = areaOrtho(
                        pattern, static_cast<int>(x * x),
                        static_cast<int>(y * y), orthoOffsets[slice]);
                    const uint16_t dstX =
                        orthoLayout[pattern][0] * orthoSize + x;
                    const uint16_t dstY = slice * 5u * orthoSize
                        + orthoLayout[pattern][1] * orthoSize + y;
                    writeArea(pixels, dstX, dstY, area);
                }
            }
        }
    }

    constexpr uint16_t diagSize = 20;
    constexpr uint16_t diagBaseX = 5u * orthoSize;
    for (uint16_t slice = 0; slice < diagOffsets.size(); ++slice) {
        for (uint8_t pattern = 0; pattern < kDiagEdgeLayout.size(); ++pattern) {
            for (uint16_t y = 0; y < diagSize; ++y) {
                for (uint16_t x = 0; x < diagSize; ++x) {
                    const uint16_t dstX = diagBaseX
                        + kDiagEdgeLayout[pattern][0] * diagSize + x;
                    const uint16_t dstY = slice * 4u * diagSize
                        + kDiagEdgeLayout[pattern][1] * diagSize + y;
                    writeArea(pixels, dstX, dstY,
                              areaDiag(pattern, x, y, diagOffsets[slice]));
                }
            }
        }
    }
    return pixels;
}

std::vector<uint8_t> generateSmaaSearchTextureR8()
{
    const std::array<int8_t, 33> reverse = makeSearchReverseLookup();
    std::vector<uint8_t> pixels(
        static_cast<size_t>(SMAAPass::kSearchTextureWidth)
            * SMAAPass::kSearchTextureHeight,
        0u);
    for (uint16_t y = 0; y < SMAAPass::kSearchTextureHeight; ++y) {
        const uint16_t sourceY = 32u - y;
        const int8_t top = reverse[sourceY];
        for (uint16_t x = 0; x < SMAAPass::kSearchTextureWidth; ++x) {
            const bool rightHalf = x >= 33u;
            const uint16_t sourceX = rightHalf ? x - 33u : x;
            const int8_t left = reverse[sourceX];
            if (left < 0 || top < 0) {
                continue;
            }
            const uint8_t distance = rightHalf
                ? searchDeltaRight(left, top)
                : searchDeltaLeft(left, top);
            pixels[static_cast<size_t>(y) * SMAAPass::kSearchTextureWidth + x]
                = static_cast<uint8_t>(127u * distance);
        }
    }
    return pixels;
}

} // namespace ayt::render::detail
