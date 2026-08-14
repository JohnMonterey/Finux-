#include "gfx/canvas.hpp"

namespace finux::gfx {
namespace {
BLRgba32 toBl(Color c) { return BLRgba32(c.argb()); }
BLRectI toBl(Rect r) { return BLRectI(r.x, r.y, r.w, r.h); }
}  // namespace

Canvas::Canvas(Image& target) : target_(target), clip_(target.bounds()) {
  ensureContext();
}

Canvas::~Canvas() { syncPixels(); }

void Canvas::ensureContext() {
  if (active_) return;
  ctx_.begin(target_.bl());
  ctx_.set_comp_op(BL_COMP_OP_SRC_OVER);
  active_ = true;
  // begin() starts from a clean state, so the clip stack has to be reapplied
  // rather than assumed to have survived.
  applyClip();
}

void Canvas::syncPixels() {
  if (!active_) return;
  ctx_.end();
  active_ = false;
}

void Canvas::clear(Color c) {
  ensureContext();
  ctx_.save();
  ctx_.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx_.set_fill_style(toBl(c));
  ctx_.fill_rect(toBl(clip_));
  ctx_.restore();
}

void Canvas::fillRect(Rect r, Color c) {
  ensureContext();
  const Rect clipped = r.intersected(clip_);
  if (clipped.empty() || c.transparent()) return;
  ctx_.set_fill_style(toBl(c));
  ctx_.fill_rect(toBl(clipped));
}

void Canvas::strokeRectInside(Rect r, Color c, int thickness) {
  if (r.empty() || thickness <= 0 || c.transparent()) return;
  const int t = std::min(thickness, std::min(r.w, r.h));
  fillRect({r.x, r.y, r.w, t}, c);                    // top
  fillRect({r.x, r.bottom() - t, r.w, t}, c);         // bottom
  fillRect({r.x, r.y + t, t, r.h - 2 * t}, c);        // left
  fillRect({r.right() - t, r.y + t, t, r.h - 2 * t}, c);  // right
}

void Canvas::hLine(int x, int y, int length, Color c) {
  fillRect({x, y, length, 1}, c);
}

void Canvas::vLine(int x, int y, int length, Color c) {
  fillRect({x, y, 1, length}, c);
}

void Canvas::fillRoundedRect(Rect r, double radius, Color c) {
  ensureContext();
  const Rect clipped = r.intersected(clip_);
  if (clipped.empty() || c.transparent()) return;
  ctx_.set_fill_style(toBl(c));
  ctx_.fill_round_rect(BLRoundRect(r.x, r.y, r.w, r.h, radius));
}

void Canvas::blit(Point at, const Image& src) {
  ensureContext();
  if (!src.valid()) return;
  ctx_.blit_image(BLPointI(at.x, at.y), src.bl());
}

void Canvas::blit(Point at, const Image& src, Rect srcRect) {
  ensureContext();
  if (!src.valid() || srcRect.empty()) return;
  ctx_.blit_image(BLPointI(at.x, at.y), src.bl(), toBl(srcRect));
}

void Canvas::pushClip(Rect r) {
  clipStack_.push_back(clip_);
  clip_ = clip_.intersected(r);
  applyClip();
}

void Canvas::popClip() {
  if (clipStack_.empty()) return;
  clip_ = clipStack_.back();
  clipStack_.pop_back();
  applyClip();
}

void Canvas::applyClip() {
  if (!active_) return;  // reapplied by ensureContext() on the next draw
  ctx_.restore_clipping();
  if (!clip_.empty()) ctx_.clip_to_rect(toBl(clip_));
}

}  // namespace finux::gfx
