#include "gfx/image.hpp"

namespace finux::gfx {

Image::Image(int width, int height) {
  if (width > 0 && height > 0) impl_.create(width, height, BL_FORMAT_PRGB32);
}

uint32_t* Image::pixels() {
  BLImageData data{};
  if (impl_.get_data(&data) != BL_SUCCESS) return nullptr;
  return static_cast<uint32_t*>(data.pixel_data);
}

const uint32_t* Image::pixels() const {
  BLImageData data{};
  if (impl_.get_data(&data) != BL_SUCCESS) return nullptr;
  return static_cast<const uint32_t*>(data.pixel_data);
}

int Image::strideWords() const {
  BLImageData data{};
  if (impl_.get_data(&data) != BL_SUCCESS) return 0;
  return static_cast<int>(data.stride / sizeof(uint32_t));
}

void Image::fill(Color c) {
  if (!valid()) return;
  const uint32_t v = premultiply(c);
  const int stride = strideWords();
  uint32_t* p = pixels();
  if (!p) return;
  for (int y = 0; y < height(); ++y) {
    uint32_t* row = p + static_cast<ptrdiff_t>(y) * stride;
    for (int x = 0; x < width(); ++x) row[x] = v;
  }
}

bool Image::savePng(const std::string& path) const {
  if (!valid()) return false;
  return impl_.write_to_file(path.c_str()) == BL_SUCCESS;
}

Image Image::loadPng(const std::string& path) {
  Image out;
  BLImage tmp;
  if (tmp.read_from_file(path.c_str()) != BL_SUCCESS) return out;
  // Reference captures arrive as opaque RGB; normalise to the surface format
  // so comparisons never straddle two pixel layouts.
  if (tmp.format() != BL_FORMAT_PRGB32) {
    BLImage converted(tmp.width(), tmp.height(), BL_FORMAT_PRGB32);
    BLContext ctx(converted);
    ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
    ctx.blit_image(BLPointI(0, 0), tmp);
    ctx.end();
    out.impl_ = converted;
  } else {
    out.impl_ = tmp;
  }
  return out;
}

}  // namespace finux::gfx
