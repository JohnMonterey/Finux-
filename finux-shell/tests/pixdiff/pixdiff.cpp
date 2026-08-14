#include "pixdiff/pixdiff.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace finux::pixdiff {
namespace {

struct Lab {
  double l, a, b;
};

double srgbToLinear(double channel) {
  return channel <= 0.04045 ? channel / 12.92
                            : std::pow((channel + 0.055) / 1.055, 2.4);
}

double labCurve(double t) {
  constexpr double kEpsilon = 216.0 / 24389.0;
  constexpr double kKappa = 24389.0 / 27.0;
  return t > kEpsilon ? std::cbrt(t) : (kKappa * t + 16.0) / 116.0;
}

// The surface is premultiplied; un-premultiply before interpreting the
// channels as colour, or every translucent hover layer compares as too dark.
Lab toLab(uint32_t premultiplied) {
  const double alpha = ((premultiplied >> 24) & 0xFF) / 255.0;
  double r = ((premultiplied >> 16) & 0xFF) / 255.0;
  double g = ((premultiplied >> 8) & 0xFF) / 255.0;
  double b = (premultiplied & 0xFF) / 255.0;
  if (alpha > 0.0) {
    r = std::min(1.0, r / alpha);
    g = std::min(1.0, g / alpha);
    b = std::min(1.0, b / alpha);
  }

  const double lr = srgbToLinear(r);
  const double lg = srgbToLinear(g);
  const double lb = srgbToLinear(b);

  // sRGB -> XYZ (D65), then XYZ -> Lab against the D65 white point.
  const double x = (0.4124564 * lr + 0.3575761 * lg + 0.1804375 * lb) / 0.95047;
  const double y = (0.2126729 * lr + 0.7151522 * lg + 0.0721750 * lb) / 1.00000;
  const double z = (0.0193339 * lr + 0.1191920 * lg + 0.9503041 * lb) / 1.08883;

  const double fx = labCurve(x);
  const double fy = labCurve(y);
  const double fz = labCurve(z);
  return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double deltaE76(Lab p, Lab q) {
  const double dl = p.l - q.l;
  const double da = p.a - q.a;
  const double db = p.b - q.b;
  return std::sqrt(dl * dl + da * da + db * db);
}

}  // namespace

Result compare(const gfx::Image& actual, const gfx::Image& expected,
               const Options& options) {
  Result result;

  if (!actual.valid() || !expected.valid()) {
    result.message = "one or both images are invalid";
    return result;
  }
  if (actual.size() != expected.size()) {
    std::ostringstream out;
    out << "size mismatch: actual " << actual.width() << "x" << actual.height()
        << ", expected " << expected.width() << "x" << expected.height();
    result.message = out.str();
    return result;
  }
  result.sizesMatch = true;

  const int width = actual.width();
  const int height = actual.height();
  double total = 0.0;

  for (int y = 0; y < height; ++y) {
    const uint32_t* actualRow = actual.rowAt(y);
    const uint32_t* expectedRow = expected.rowAt(y);
    for (int x = 0; x < width; ++x) {
      const double delta = deltaE76(toLab(actualRow[x]), toLab(expectedRow[x]));
      total += delta;
      if (delta > result.maxDelta) {
        result.maxDelta = delta;
        result.worstPixel = {x, y};
      }
      if (delta > options.perPixelTolerance) ++result.failingPixels;
    }
  }

  result.comparedPixels = width * height;
  result.meanDelta = total / result.comparedPixels;
  result.failingFraction =
      static_cast<double>(result.failingPixels) / result.comparedPixels;
  result.passed = result.failingFraction <= options.maxFailingFraction &&
                  result.maxDelta <= options.maxSingleDelta;

  std::ostringstream out;
  out << result.failingPixels << "/" << result.comparedPixels << " pixels over ΔE "
      << options.perPixelTolerance << " (" << result.failingFraction * 100.0
      << "%), mean ΔE " << result.meanDelta << ", max ΔE " << result.maxDelta
      << " at (" << result.worstPixel.x << "," << result.worstPixel.y << ")";
  result.message = out.str();
  return result;
}

gfx::Image visualize(const gfx::Image& actual, const gfx::Image& expected,
                     const Options& options) {
  gfx::Image out;
  if (!actual.valid() || !expected.valid() || actual.size() != expected.size()) {
    return out;
  }

  out = gfx::Image(actual.width(), actual.height());
  for (int y = 0; y < actual.height(); ++y) {
    const uint32_t* actualRow = actual.rowAt(y);
    const uint32_t* expectedRow = expected.rowAt(y);
    uint32_t* outRow = out.rowAt(y);
    for (int x = 0; x < actual.width(); ++x) {
      const double delta = deltaE76(toLab(actualRow[x]), toLab(expectedRow[x]));
      if (delta > options.perPixelTolerance) {
        // Saturate magenta with the magnitude so a glance separates "a shade
        // off" from "completely wrong".
        const double t = std::min(1.0, delta / options.maxSingleDelta);
        const auto v = static_cast<uint32_t>(80 + 175 * t);
        outRow[x] = 0xFF000000u | (v << 16) | v;
      } else {
        // Dim grayscale backdrop so the magenta reads against the layout.
        const uint32_t px = actualRow[x];
        const uint32_t luma =
            (((px >> 16) & 0xFF) * 54 + ((px >> 8) & 0xFF) * 183 +
             (px & 0xFF) * 19) >>
            10;
        outRow[x] = 0xFF000000u | (luma << 16) | (luma << 8) | luma;
      }
    }
  }
  return out;
}

FileResult compareToReference(const gfx::Image& actual,
                              const std::string& referenceDir,
                              const std::string& name,
                              const Options& options) {
  namespace fs = std::filesystem;

  FileResult out;
  const fs::path dir(referenceDir);
  const fs::path reference = dir / (name + ".png");
  const fs::path outputDir = dir.parent_path() / "output";

  std::error_code ec;
  fs::create_directories(outputDir, ec);

  out.actualPath = (outputDir / (name + ".actual.png")).string();
  actual.savePng(out.actualPath);

  if (!fs::exists(reference)) {
    out.referenceExisted = false;
    out.result.message =
        "no reference at " + reference.string() +
        "; wrote actual output to " + out.actualPath +
        ". Capture the corresponding Windows 10 region and save it there.";
    return out;
  }

  out.referenceExisted = true;
  const gfx::Image expected = gfx::Image::loadPng(reference.string());
  if (!expected.valid()) {
    out.result.message = "could not decode reference " + reference.string();
    return out;
  }

  out.result = compare(actual, expected, options);
  if (!out.result.passed && out.result.sizesMatch) {
    const gfx::Image diff = visualize(actual, expected, options);
    out.diffPath = (outputDir / (name + ".diff.png")).string();
    diff.savePng(out.diffPath);
  }
  return out;
}

}  // namespace finux::pixdiff
