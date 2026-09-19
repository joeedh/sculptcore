#include "brush/brush.h"

namespace sculptcore::brush {

float Brush::sampleTexBilinear(litestl::math::float2 uv) const
  {
    if (tex_width <= 0 || tex_height <= 0 || tex_pixels.size() == 0) {
      return 1.0f;
    }

    // Texel-space coords with half-texel offset; clamp to edge.
    float fx = uv[0] * (float)tex_width - 0.5f;
    float fy = uv[1] * (float)tex_height - 0.5f;

    int x0 = (int)std::floor(fx);
    int y0 = (int)std::floor(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    int x0c = clampi(x0, 0, tex_width - 1);
    int y0c = clampi(y0, 0, tex_height - 1);
    int x1c = clampi(x0 + 1, 0, tex_width - 1);
    int y1c = clampi(y0 + 1, 0, tex_height - 1);

    float p00 = tex_pixels[y0c * tex_width + x0c];
    float p10 = tex_pixels[y0c * tex_width + x1c];
    float p01 = tex_pixels[y1c * tex_width + x0c];
    float p11 = tex_pixels[y1c * tex_width + x1c];

    float a = p00 * (1.0f - tx) + p10 * tx;
    float b = p01 * (1.0f - tx) + p11 * tx;
    return a * (1.0f - ty) + b * ty;
  }

void Brush::setTexture(int width, int height, litestl::util::Vector<float> &pixels)
  {
    if (width <= 0 || height <= 0 || pixels.size() != size_t(width) * size_t(height)) {
      clearTexture();
      return;
    }
    tex_width = width;
    tex_height = height;
    tex_pixels.clear();
    for (float p : pixels) {
      tex_pixels.append(p);
    }
  }

void Brush::clearTexture()
  {
    tex_width = 0;
    tex_height = 0;
    tex_pixels.clear();
  }

bool Brush::setTextureScriptSource(litestl::util::stringref source,
                                    litestl::util::stringref filename)
  {
    clearTextureScript();
    TextureProgram *p = compileTextureScript(source, filename, texture_script_error);
    if (!p) {
      return false;
    }
    texture_program = p;
    for (float f : p->defaults) {
      texture_params.append(f);
    }
    return true;
  }

bool Brush::setTextureScript(litestl::util::Vector<char> &source)
  {
    litestl::util::Vector<char> buf;
    for (char c : source) {
      buf.append(c);
    }
    buf.append('\0');
    return setTextureScriptSource(buf.data(), "<script>");
  }

void Brush::clearTextureScript()
  {
    if (texture_program) {
      freeTextureProgram(texture_program);
      texture_program = nullptr;
    }
    texture_params.clear();
    texture_script_error = litestl::util::string("");
  }

int Brush::textureParamCount()
  {
    return texture_program ? (int)texture_program->params.size() : 0;
  }

TextureProgramParam *Brush::queriedTextureParamEntry(int i)
  {
    if (!texture_program || i < 0 || i >= (int)texture_program->params.size()) {
      return nullptr;
    }
    return &texture_program->params[i];
  }

bool Brush::setTextureParamAt(int i, float value)
  {
    TextureProgramParam *p = queriedTextureParamEntry(i);
    if (!p || p->isRamp || p->isConst || p->offset < 0) {
      return false;
    }
    if (p->hasRange) {
      value =
          value < p->rangeMin ? p->rangeMin : (value > p->rangeMax ? p->rangeMax : value);
    }
    texture_params[p->offset] = value;
    return true;
  }

bool Brush::setTextureRampAt(int i, litestl::util::Vector<float> &lut)
  {
    TextureProgramParam *p = queriedTextureParamEntry(i);
    if (!p || !p->isRamp || p->offset < 0 || (int)lut.size() != kTexRampSize) {
      return false;
    }
    for (int k = 0; k < kTexRampSize; k++) {
      texture_params[p->offset + k] = lut[k];
    }
    return true;
  }

float Brush::evalTextureAt(float px, float py, float pz, float nx, float ny, float nz)
  {
    if (!texture_program || !texture_program->eval) {
      return 0.0f;
    }
    const float P[3] = {px, py, pz};
    const float N[3] = {nx, ny, nz};
    const float *params = texture_params.size() > 0 ? texture_params.data() : nullptr;
    return texture_program->eval(P, N, params, nullptr);
  }

bool Brush::textureUsesMap()
  {
    return texture_program ? texture_program->usesMap : false;
  }

int Brush::textureParamIndex(const char *name)
  {
    for (int i = 0; i < textureParamCount(); i++) {
      if (texture_program->params[i].name == litestl::util::string(name)) {
        return i;
      }
    }
    return -1;
  }

void Brush::resetStrokePath()
  {
    strokePathCount = 0;
  }

void Brush::pushStrokeSample(float3 pos, float3 normal)
  {
    float arclen = 0.0f;
    if (strokePathCount > 0) {
      const StrokeSample &prev = strokePath[strokePathCount - 1];
      arclen = prev.arclen + (pos - prev.pos).length();
    }
    if (strokePathCount < kStrokePathMax) {
      strokePath[strokePathCount++] = StrokeSample{pos, normal, arclen};
    } else {
      for (int i = 1; i < kStrokePathMax; i++) {
        strokePath[i - 1] = strokePath[i];
      }
      strokePath[kStrokePathMax - 1] = StrokeSample{pos, normal, arclen};
    }
  }

litestl::math::float2 Brush::sampleStrokeUV(float3 co) const
  {
    if (strokePathCount == 0) {
      return litestl::math::float2{0.0f, 0.0f};
    }
    if (strokePathCount == 1) {
      return litestl::math::float2{strokePath[0].arclen,
                                   (co - strokePath[0].pos).length()};
    }

    float bestDist = std::numeric_limits<float>::max();
    float bestArc = 0.0f;
    float bestLat = 0.0f;
    for (int i = 0; i + 1 < strokePathCount; i++) {
      float3 a = strokePath[i].pos;
      float3 ab = strokePath[i + 1].pos - a;
      float len2 = ab.dot(ab);
      float t = len2 > 0.0f ? (co - a).dot(ab) / len2 : 0.0f;
      t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
      float3 d = co - (a + ab * t);
      float dist = d.length();
      if (dist < bestDist) {
        bestDist = dist;
        bestArc =
            strokePath[i].arclen + (strokePath[i + 1].arclen - strokePath[i].arclen) * t;
        bestLat = dist;
      }
    }
    return litestl::math::float2{bestArc, bestLat};
  }

} // namespace sculptcore::brush
