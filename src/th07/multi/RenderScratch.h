#pragma once

struct VertexTex1DiffuseXyzrhw;

namespace th07 { namespace render {
// A stable address across scene changes and device resets.
VertexTex1DiffuseXyzrhw* SpriteVertexScratch();
} }
