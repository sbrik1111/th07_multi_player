#include "AnmManager.hpp"
#include "multi/RenderScratch.h"
#include "multi/RuntimeData.h"

namespace th07 { namespace render {
namespace {
// Rollback checkpoints are only taken with an empty batch.
VertexTex1DiffuseXyzrhw g_spriteVertices[49152];
}

VertexTex1DiffuseXyzrhw* SpriteVertexScratch() {
    return g_spriteVertices;
}
} }
