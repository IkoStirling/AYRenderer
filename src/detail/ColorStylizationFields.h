#pragma once

namespace ayt::render::detail {

// Persistence has a single field inventory. Keep runtime implementation and UI
// independent of JSON, and add every new look field here in the same change.
template<class Settings,class Visitor>
void visitColorStylizationFieldsV1(Settings& s, Visitor&& v)
{
#define AY_STYLE_FIELD(name) v(#name,s.name)
    AY_STYLE_FIELD(enabled); AY_STYLE_FIELD(pixelationEnabled); AY_STYLE_FIELD(pixelSize);
    AY_STYLE_FIELD(posterizationEnabled); AY_STYLE_FIELD(colorLevels); AY_STYLE_FIELD(ditherStrength);
    AY_STYLE_FIELD(colorMapping); AY_STYLE_FIELD(shadowColor); AY_STYLE_FIELD(highlightColor);
    AY_STYLE_FIELD(paletteSize); AY_STYLE_FIELD(palette); AY_STYLE_FIELD(printPattern);
    AY_STYLE_FIELD(printSpacing); AY_STYLE_FIELD(printAngle); AY_STYLE_FIELD(printStrength);
    AY_STYLE_FIELD(printGamma);
    AY_STYLE_FIELD(inkColor); AY_STYLE_FIELD(paperColor); AY_STYLE_FIELD(outlineMode);
    AY_STYLE_FIELD(outlineWidth); AY_STYLE_FIELD(outlineDepthThreshold);
    AY_STYLE_FIELD(outlineNormalThreshold); AY_STYLE_FIELD(outlineLuminanceThreshold);
    AY_STYLE_FIELD(outlineStrength); AY_STYLE_FIELD(outlineColor);
#undef AY_STYLE_FIELD
}

template<class Settings,class Visitor>
void visitAnalogDisplayFields(Settings& s,Visitor&& v) {
    v("enabled",s.enabled);v("scanlineStrength",s.scanlineStrength);v("maskStrength",s.maskStrength);
    v("chromaticOffset",s.chromaticOffset);v("curvature",s.curvature);
    v("vignetteStrength",s.vignetteStrength);v("grainStrength",s.grainStrength);v("noiseSeed",s.noiseSeed);
}
template<class Settings,class Visitor>
void visitColorStylizationFieldsV2(Settings& s,Visitor&& v) {
    visitColorStylizationFieldsV1(s,v);v("display",s.display);
}
template<class Settings,class Visitor>
void visitTemporalFeedbackFields(Settings& s,Visitor&& v) {
    v("enabled",s.enabled);v("mode",s.mode);v("historyWeight",s.historyWeight);
    v("holdFrames",s.holdFrames);v("tileSize",s.tileSize);
    v("historyOffsetX",s.historyOffsetX);v("historyOffsetY",s.historyOffsetY);v("noiseSeed",s.noiseSeed);
}
template<class Settings,class Visitor>
void visitColorStylizationFields(Settings& s,Visitor&& v) {
    visitColorStylizationFieldsV2(s,v);v("temporal",s.temporal);
}

} // namespace ayt::render::detail
