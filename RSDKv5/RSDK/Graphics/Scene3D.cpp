#include "RSDK/Core/RetroEngine.hpp"
#include <algorithm>

using namespace RSDK;

enum ModelFlags {
    MODEL_NOFLAGS     = 0,
    MODEL_USENORMALS  = 1 << 0,
    MODEL_USETEXTURES = 1 << 1,
    MODEL_USECOLOURS  = 1 << 2,
};

#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#if defined(RSDK_USE_MISTER)
#include <stdint.h>
#include <string.h>

#define SCENE3D_SOURCE_UFO_DECORATION 1
#define SCENE3D_SOURCE_UFO_PLAYER 2
#define SCENE3D_FACE_INDEXED 0x40000000
#define SCENE3D_FACE_INDEX_MASK 0x3FFFFFFF
#define SCENE3D_OUT_LEFT 0x01
#define SCENE3D_OUT_RIGHT 0x02
#define SCENE3D_OUT_TOP 0x04
#define SCENE3D_OUT_BOTTOM 0x08
#define SCENE3D_OUT_NEAR 0x10
#define SCENE3D_OUT_SCREEN_MASK (SCENE3D_OUT_LEFT | SCENE3D_OUT_RIGHT | SCENE3D_OUT_TOP | SCENE3D_OUT_BOTTOM)
#define SCENE3D_FACE_CACHED_COLOR 0x01
#define SCENE3D_FACE_COLOR_CACHE_MODELS 24
#define SCENE3D_FACE_COLOR_CACHE_FRAMES 64
#define SCENE3D_FACE_COLOR_CACHE_ENTRIES 8192

static uint32_t s_s3d_draw_source;
static Scene3DFace s_s3d_sort_buffer[SCENE3D_VERT_COUNT];
static uint8_t s_s3d_cached_face_color_model[MODEL_COUNT];
static uint8_t s_s3d_skip_model_normals[MODEL_COUNT];
static uint32_t s_s3d_face_color_cache_next;

struct Scene3DFaceColorCache {
    bool valid;
    uint16 modelID;
    uint16 faceCount;
    uint16 frameCount;
    uint32_t diffuseKey;
    uint32_t diffuseIntensityKey;
    uint32_t specularIntensityKey;
    uint64_t frameMask;
    uint32 color[SCENE3D_FACE_COLOR_CACHE_ENTRIES];
    uint16 color16[SCENE3D_FACE_COLOR_CACHE_ENTRIES];
};

static Scene3DFaceColorCache s_s3d_face_color_cache[SCENE3D_FACE_COLOR_CACHE_MODELS];

#define SCENE3D_MODEL_OPT_VERT_LIMIT 4096
static uint16 s_s3d_model_opt_remap[SCENE3D_MODEL_OPT_VERT_LIMIT];
static uint16 s_s3d_model_opt_keep[SCENE3D_MODEL_OPT_VERT_LIMIT];
static ModelVertex s_s3d_model_opt_frame[SCENE3D_MODEL_OPT_VERT_LIMIT];
static Color s_s3d_model_opt_colors[SCENE3D_MODEL_OPT_VERT_LIMIT];

static inline bool Scene3D_ShouldCacheIndexedVertices(const Model *mdl)
{
    if (!mdl->vertices || !mdl->indices || !mdl->vertCount || !mdl->indexCount || !mdl->faceVertCount)
        return false;
    if (mdl->vertCount > SCENE3D_VERT_COUNT)
        return false;

    return (uint32)mdl->vertCount * 4 <= (uint32)mdl->indexCount * 3;
}

static inline bool Scene3D_ShouldStoreIndexedFaces(const Model *mdl, uint8 drawMode)
{
    if (drawMode != S3D_SOLIDCOLOR_SHADED_SCREEN && drawMode != S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN)
        return false;

    return Scene3D_ShouldCacheIndexedVertices(mdl);
}

static inline bool Scene3D_ShouldSkipModelNormals(uint16 modelID)
{
    return modelID < MODEL_COUNT && s_s3d_skip_model_normals[modelID] && s_s3d_cached_face_color_model[modelID];
}

extern "C" {
void Scene3D_SetDrawSource(uint32_t source) { s_s3d_draw_source = source; }

void Scene3D_EnableCachedModelFaceColors(uint16_t modelID) {
    if (modelID < MODEL_COUNT)
        s_s3d_cached_face_color_model[modelID] = 1;
}

void Scene3D_EnableSkipModelNormals(uint16_t modelID) {
    if (modelID < MODEL_COUNT)
        s_s3d_skip_model_normals[modelID] = 1;
}

int32_t Scene3D_GetModelBounds(uint16_t modelID,
                               int32_t *minX, int32_t *minY, int32_t *minZ,
                               int32_t *maxX, int32_t *maxY, int32_t *maxZ) {
    if (modelID >= MODEL_COUNT)
        return 0;

    Model *model = &modelList[modelID];
    if (!model->vertices || !model->vertCount)
        return 0;

    int32_t vertexCount = model->vertCount;
    if (model->frameCount > 1)
        vertexCount *= model->frameCount;

    int32_t loX = 0x7FFFFFFF;
    int32_t loY = 0x7FFFFFFF;
    int32_t loZ = 0x7FFFFFFF;
    int32_t hiX = -0x7FFFFFFF;
    int32_t hiY = -0x7FFFFFFF;
    int32_t hiZ = -0x7FFFFFFF;

    for (int32_t v = 0; v < vertexCount; ++v) {
        ModelVertex *vertex = &model->vertices[v];

        if (vertex->x < loX) loX = vertex->x;
        if (vertex->y < loY) loY = vertex->y;
        if (vertex->z < loZ) loZ = vertex->z;
        if (vertex->x > hiX) hiX = vertex->x;
        if (vertex->y > hiY) hiY = vertex->y;
        if (vertex->z > hiZ) hiZ = vertex->z;
    }

    *minX = loX;
    *minY = loY;
    *minZ = loZ;
    *maxX = hiX;
    *maxY = hiY;
    *maxZ = hiZ;
    return 1;
}

int32_t Scene3D_ModelFitsScene(uint16_t sceneID, uint16_t modelID) {
    if (sceneID >= SCENE3D_COUNT || modelID >= MODEL_COUNT)
        return 0;

    Scene3D *scene = &scene3DList[sceneID];
    Model *model   = &modelList[modelID];
    if (!model->indices || !model->indexCount)
        return 0;

    const int32_t faceCount = model->faceVertCount ? model->indexCount / model->faceVertCount : 0;
    if ((int32_t)scene->vertLimit - (int32_t)scene->faceCount < faceCount)
        return 0;

    if (Scene3D_ShouldCacheIndexedVertices(model)) {
        int32_t vertexAvailable = (int32_t)scene->vertLimit - (int32_t)scene->vertexCount;
        int32_t refAvailable    = (int32_t)scene->vertLimit - (int32_t)scene->faceRefCount;
        return vertexAvailable >= (int32_t)model->vertCount && refAvailable >= (int32_t)model->indexCount;
    }

    int32_t available = (int32_t)scene->vertLimit - (int32_t)scene->vertexCount;
    return available >= (int32_t)model->indexCount;
}

} // extern "C"

static bool Scene3D_ShouldCullBackface(const Scene3DVertex *faceVerts, int32 faceVertCount)
{
    if (faceVertCount < 3)
        return false;

    int64_t crossX = 0;
    int64_t crossY = 0;
    int64_t crossZ = 0;
    const Scene3DVertex *origin = &faceVerts[0];

    for (int32 i = 1; i < faceVertCount - 1; ++i) {
        const Scene3DVertex *a = &faceVerts[i];
        const Scene3DVertex *b = &faceVerts[i + 1];

        int64_t ax = (int64_t)a->x - origin->x;
        int64_t ay = (int64_t)a->y - origin->y;
        int64_t az = (int64_t)a->z - origin->z;
        int64_t bx = (int64_t)b->x - origin->x;
        int64_t by = (int64_t)b->y - origin->y;
        int64_t bz = (int64_t)b->z - origin->z;

        crossX = ay * bz - az * by;
        crossY = az * bx - ax * bz;
        crossZ = ax * by - ay * bx;
        if (crossX || crossY || crossZ)
            break;
    }

    if (!crossX && !crossY && !crossZ)
        return false;

    int64_t normalX = 0;
    int64_t normalY = 0;
    int64_t normalZ = 0;
    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;

    for (int32 i = 0; i < faceVertCount; ++i) {
        normalX += faceVerts[i].nx;
        normalY += faceVerts[i].ny;
        normalZ += faceVerts[i].nz;
        centerX += faceVerts[i].x;
        centerY += faceVerts[i].y;
        centerZ += faceVerts[i].z;
    }

    if (!normalX && !normalY && !normalZ)
        return false;

    int64_t orientDot = crossX * normalX + crossY * normalY + crossZ * normalZ;
    if (orientDot < 0) {
        crossX = -crossX;
        crossY = -crossY;
        crossZ = -crossZ;
    }

    int64_t viewDot = crossX * centerX + crossY * centerY + crossZ * centerZ;
    return viewDot >= 0;
}

static bool Scene3D_ShouldCullBackfacePtrs(Scene3DVertex *const *faceVerts, int32 faceVertCount)
{
    if (faceVertCount < 3)
        return false;

    int64_t crossX = 0;
    int64_t crossY = 0;
    int64_t crossZ = 0;
    Scene3DVertex *origin = faceVerts[0];

    for (int32 i = 1; i < faceVertCount - 1; ++i) {
        Scene3DVertex *a = faceVerts[i];
        Scene3DVertex *b = faceVerts[i + 1];

        int64_t ax = (int64_t)a->x - origin->x;
        int64_t ay = (int64_t)a->y - origin->y;
        int64_t az = (int64_t)a->z - origin->z;
        int64_t bx = (int64_t)b->x - origin->x;
        int64_t by = (int64_t)b->y - origin->y;
        int64_t bz = (int64_t)b->z - origin->z;

        crossX = ay * bz - az * by;
        crossY = az * bx - ax * bz;
        crossZ = ax * by - ay * bx;
        if (crossX || crossY || crossZ)
            break;
    }

    if (!crossX && !crossY && !crossZ)
        return false;

    int64_t normalX = 0;
    int64_t normalY = 0;
    int64_t normalZ = 0;
    int64_t centerX = 0;
    int64_t centerY = 0;
    int64_t centerZ = 0;

    for (int32 i = 0; i < faceVertCount; ++i) {
        Scene3DVertex *v = faceVerts[i];
        normalX += v->nx;
        normalY += v->ny;
        normalZ += v->nz;
        centerX += v->x;
        centerY += v->y;
        centerZ += v->z;
    }

    if (!normalX && !normalY && !normalZ)
        return false;

    int64_t orientDot = crossX * normalX + crossY * normalY + crossZ * normalZ;
    if (orientDot < 0) {
        crossX = -crossX;
        crossY = -crossY;
        crossZ = -crossZ;
    }

    int64_t viewDot = crossX * centerX + crossY * centerY + crossZ * centerZ;
    return viewDot >= 0;
}

static bool Scene3D_UsesScreenCull(uint8 drawMode)
{
    return drawMode == S3D_SOLIDCOLOR_SHADED_SCREEN || drawMode == S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN;
}

static inline int32 Scene3D_FaceDepthPtrs(Scene3DVertex *const *facePtrs, int32 faceVertCount)
{
    switch (faceVertCount) {
        default:
        case 1:
            return facePtrs[0]->z;

        case 2:
            return (facePtrs[0]->z >> 1) + (facePtrs[1]->z >> 1);

        case 3: {
            int32 faceDepth = facePtrs[0]->z >> 1;
            faceDepth = (faceDepth + (facePtrs[1]->z >> 1)) >> 1;
            faceDepth += facePtrs[2]->z >> 1;
            return faceDepth;
        }

        case 4:
            return (facePtrs[0]->z >> 2) + (facePtrs[1]->z >> 2) + (facePtrs[2]->z >> 2) + (facePtrs[3]->z >> 2);
    }
}

#if defined(RSDK_USE_MISTER)
static inline int32 Scene3D_FaceDepthRefs(const Scene3D *scn, const uint16 *refs, int32 faceVertCount)
{
    switch (faceVertCount) {
        default:
        case 1:
            return scn->vertices[refs[0]].z;

        case 2:
            return (scn->vertices[refs[0]].z >> 1) + (scn->vertices[refs[1]].z >> 1);

        case 3: {
            int32 faceDepth = scn->vertices[refs[0]].z >> 1;
            faceDepth = (faceDepth + (scn->vertices[refs[1]].z >> 1)) >> 1;
            faceDepth += scn->vertices[refs[2]].z >> 1;
            return faceDepth;
        }

        case 4:
            return (scn->vertices[refs[0]].z >> 2) + (scn->vertices[refs[1]].z >> 2) + (scn->vertices[refs[2]].z >> 2)
                   + (scn->vertices[refs[3]].z >> 2);
    }
}
#endif

static inline uint32 Scene3D_SortKey(int32 depth)
{
    return ~(((uint32)depth) ^ 0x80000000U);
}

static void Scene3D_SortFaces(Scene3DFace *faces, int32 faceCount)
{
    if (faceCount <= 1)
        return;

    if (faceCount > SCENE3D_VERT_COUNT) {
        std::sort(faces, faces + faceCount, [](const Scene3DFace &a, const Scene3DFace &b) { return a.depth > b.depth; });
        return;
    }

    Scene3DFace *src = faces;
    Scene3DFace *dst = s_s3d_sort_buffer;
    uint32 counts[256];
    uint32 offsets[256];

    memset(counts, 0, sizeof(counts));
    uint32 minKey = 0xFFFFFFFFU;
    uint32 maxKey = 0;
    for (int32 i = 0; i < faceCount; ++i) {
        uint32 key = Scene3D_SortKey(src[i].depth);
        if (key < minKey)
            minKey = key;
        if (key > maxKey)
            maxKey = key;
        counts[key & 0xFF]++;
    }

    uint32 diff = minKey ^ maxKey;
    if (!diff)
        return;

    int32 passCount = 1;
    if (diff >> 8)
        passCount = 2;
    if (diff >> 16)
        passCount = 3;
    if (diff >> 24)
        passCount = 4;

    for (int32 pass = 0; pass < passCount; ++pass) {
        int32 shift = pass << 3;
        if (pass) {
            memset(counts, 0, sizeof(counts));
            for (int32 i = 0; i < faceCount; ++i)
                counts[(Scene3D_SortKey(src[i].depth) >> shift) & 0xFF]++;
        }

        uint32 offset = 0;
        for (int32 i = 0; i < 256; ++i) {
            offsets[i] = offset;
            offset += counts[i];
        }

        for (int32 i = 0; i < faceCount; ++i) {
            uint32 bucket = (Scene3D_SortKey(src[i].depth) >> shift) & 0xFF;
            dst[offsets[bucket]++] = src[i];
        }

        std::swap(src, dst);
    }

    if (src != faces)
        memcpy(faces, src, sizeof(Scene3DFace) * faceCount);
}

static inline void Scene3D_FillSpanColor16(uint16 *fbPtr, int32 count, uint16 color16)
{
#if defined(__ARM_NEON__)
    uint16x8_t vColor = vdupq_n_u16(color16);
    int32 neonEnd     = count & ~7;
    for (int32 x = 0; x < neonEnd; x += 8)
        vst1q_u16(fbPtr + x, vColor);
    for (int32 x = neonEnd; x < count; ++x)
        fbPtr[x] = color16;
#else
    for (int32 x = 0; x < count; ++x)
        fbPtr[x] = color16;
#endif
}

static inline int32 Scene3D_FlatEdgeDelta(int32 x1, int32 y1, int32 x2, int32 y2)
{
#if defined(__ARM_NEON__)
    return (int32)((float)TO_FIXED(x2 - x1) * (1.0f / (float)(y2 - y1)));
#else
    return TO_FIXED(x2 - x1) / (y2 - y1);
#endif
}

static inline void Scene3D_DrawFlatTriColor16(int32 x0, int32 y0, int32 x1, int32 y1, int32 x2, int32 y2,
                                              int32 clipTop, int32 clipBottom, uint16 color16)
{
    if (y1 < y0) {
        std::swap(y0, y1);
        std::swap(x0, x1);
    }
    if (y2 < y1) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }
    if (y1 < y0) {
        std::swap(y0, y1);
        std::swap(x0, x1);
    }

    if (y0 == y2)
        return;

    const int32 clipLeft  = currentScreen->clipBound_X1;
    const int32 clipRight = currentScreen->clipBound_X2;
    const int32 pitch     = currentScreen->pitch;
    int32 longDelta       = Scene3D_FlatEdgeDelta(x0, y0, x2, y2);

    if (y0 != y1) {
        int32 shortDelta = Scene3D_FlatEdgeDelta(x0, y0, x1, y1);
        int32 yStart     = MAX(y0, clipTop);
        int32 yEnd       = MIN(y1 == y2 ? y1 : y1 - 1, clipBottom);

        if (yStart <= yEnd) {
            int32 longX  = TO_FIXED(x0) + (yStart - y0) * longDelta;
            int32 shortX = TO_FIXED(x0) + (yStart - y0) * shortDelta;
            uint16 *fb   = &currentScreen->frameBuffer[yStart * pitch];

            for (int32 y = yStart; y <= yEnd; ++y) {
                int32 start = shortX >> 16;
                int32 end   = longX >> 16;
                if (start > end)
                    std::swap(start, end);
                if (start < clipLeft)
                    start = clipLeft;
                if (end > clipRight)
                    end = clipRight;

                int32 count = end - start;
                if (count > 0)
                    Scene3D_FillSpanColor16(&fb[start], count, color16);

                longX += longDelta;
                shortX += shortDelta;
                fb += pitch;
            }
        }
    }

    if (y1 != y2) {
        int32 shortDelta = Scene3D_FlatEdgeDelta(x1, y1, x2, y2);
        int32 yStart     = MAX(y1, clipTop);
        int32 yEnd       = MIN(y2, clipBottom);

        if (yStart <= yEnd) {
            int32 longX  = TO_FIXED(x0) + (yStart - y0) * longDelta;
            int32 shortX = TO_FIXED(x1) + (yStart - y1) * shortDelta;
            uint16 *fb   = &currentScreen->frameBuffer[yStart * pitch];

            for (int32 y = yStart; y <= yEnd; ++y) {
                int32 start = shortX >> 16;
                int32 end   = longX >> 16;
                if (start > end)
                    std::swap(start, end);
                if (start < clipLeft)
                    start = clipLeft;
                if (end > clipRight)
                    end = clipRight;

                int32 count = end - start;
                if (count > 0)
                    Scene3D_FillSpanColor16(&fb[start], count, color16);

                longX += longDelta;
                shortX += shortDelta;
                fb += pitch;
            }
        }
    }
}

static bool Scene3D_DrawFaceColor16Fast(const Scene3DVertex *vertices, int32 vertCount, int32 topScreen, int32 bottomScreen, uint16 color16)
{
    if (vertCount != 3 && vertCount != 4)
        return false;

    int32 clipBottom = bottomScreen;
    if (clipBottom >= currentScreen->clipBound_Y2)
        clipBottom = currentScreen->clipBound_Y2 - 1;
    if (clipBottom < topScreen)
        return true;

    int32 x0 = FROM_FIXED(vertices[0].tx);
    int32 y0 = FROM_FIXED(vertices[0].ty);
    int32 x1 = FROM_FIXED(vertices[1].tx);
    int32 y1 = FROM_FIXED(vertices[1].ty);
    int32 x2 = FROM_FIXED(vertices[2].tx);
    int32 y2 = FROM_FIXED(vertices[2].ty);

    Scene3D_DrawFlatTriColor16(x0, y0, x1, y1, x2, y2, topScreen, clipBottom, color16);
    if (vertCount == 4) {
        int32 x3 = FROM_FIXED(vertices[3].tx);
        int32 y3 = FROM_FIXED(vertices[3].ty);
        Scene3D_DrawFlatTriColor16(x0, y0, x2, y2, x3, y3, topScreen, clipBottom, color16);
    }

    return true;
}

static void Scene3D_DrawFaceColor16(const Scene3DVertex *vertices, int32 vertCount, int32 topScreen, int32 bottomScreen, uint16 color16)
{
    if (topScreen == bottomScreen)
        return;

    if (Scene3D_DrawFaceColor16Fast(vertices, vertCount, topScreen, bottomScreen, color16))
        return;

    ScanEdge *edge = &scanEdgeBuffer[topScreen];
    for (int32 s = topScreen; s <= bottomScreen; ++s) {
        edge->start = 0x7FFF;
        edge->end   = -1;
        ++edge;
    }

    for (int32 v = 0; v < vertCount - 1; ++v)
        ProcessScanEdge(vertices[v + 0].tx, vertices[v + 0].ty, vertices[v + 1].tx, vertices[v + 1].ty);
    ProcessScanEdge(vertices[0].tx, vertices[0].ty, vertices[vertCount - 1].tx, vertices[vertCount - 1].ty);

    uint16 *frameBuffer = &currentScreen->frameBuffer[topScreen * currentScreen->pitch];
    edge                = &scanEdgeBuffer[topScreen];

    for (int32 s = topScreen; s <= bottomScreen; ++s) {
        int32 start = edge->start;
        int32 end   = edge->end;

        if (start < currentScreen->clipBound_X1)
            start = currentScreen->clipBound_X1;
        if (start > currentScreen->clipBound_X2)
            start = currentScreen->clipBound_X2;

        if (end < currentScreen->clipBound_X1)
            end = currentScreen->clipBound_X1;
        if (end > currentScreen->clipBound_X2)
            end = currentScreen->clipBound_X2;

        int32 count = end - start;
        if (count > 0) {
            uint16 *fbPtr = &frameBuffer[start];
            Scene3D_FillSpanColor16(fbPtr, count, color16);
        }

        ++edge;
        frameBuffer += currentScreen->pitch;
    }
}

static bool Scene3D_DrawFaceColor16IndexedFast(Scene3D *scn, const uint16 *refs, int32 vertCount, int32 topScreen, int32 bottomScreen,
                                               uint16 color16)
{
    if (vertCount != 3 && vertCount != 4)
        return false;

    int32 clipBottom = bottomScreen;
    if (clipBottom >= currentScreen->clipBound_Y2)
        clipBottom = currentScreen->clipBound_Y2 - 1;
    if (clipBottom < topScreen)
        return true;

    const Scene3DVertex *v0 = &scn->vertices[refs[0]];
    const Scene3DVertex *v1 = &scn->vertices[refs[1]];
    const Scene3DVertex *v2 = &scn->vertices[refs[2]];
    int32 x0 = FROM_FIXED(v0->tx);
    int32 y0 = FROM_FIXED(v0->ty);
    int32 x1 = FROM_FIXED(v1->tx);
    int32 y1 = FROM_FIXED(v1->ty);
    int32 x2 = FROM_FIXED(v2->tx);
    int32 y2 = FROM_FIXED(v2->ty);

    Scene3D_DrawFlatTriColor16(x0, y0, x1, y1, x2, y2, topScreen, clipBottom, color16);
    if (vertCount == 4) {
        const Scene3DVertex *v3 = &scn->vertices[refs[3]];
        int32 x3 = FROM_FIXED(v3->tx);
        int32 y3 = FROM_FIXED(v3->ty);
        Scene3D_DrawFlatTriColor16(x0, y0, x2, y2, x3, y3, topScreen, clipBottom, color16);
    }

    return true;
}

static void Scene3D_DrawFaceColor16Indexed(Scene3D *scn, const uint16 *refs, int32 vertCount, int32 topScreen, int32 bottomScreen,
                                           uint16 color16)
{
    if (topScreen == bottomScreen)
        return;

    if (Scene3D_DrawFaceColor16IndexedFast(scn, refs, vertCount, topScreen, bottomScreen, color16))
        return;

    Scene3DVertex scratch[4];
    for (int32 v = 0; v < vertCount; ++v)
        scratch[v] = scn->vertices[refs[v]];

    Scene3D_DrawFaceColor16(scratch, vertCount, topScreen, bottomScreen, color16);
}

static inline uint32 Scene3D_ScreenOutCode(int32 sx, int32 sy, int32 clipLeft, int32 clipRight, int32 clipTop, int32 clipBottom)
{
    uint32 code = 0;
    if (sx < clipLeft)
        code |= SCENE3D_OUT_LEFT;
    if (sx > clipRight)
        code |= SCENE3D_OUT_RIGHT;
    if (sy < clipTop)
        code |= SCENE3D_OUT_TOP;
    if (sy > clipBottom)
        code |= SCENE3D_OUT_BOTTOM;
    return code;
}

static inline void Scene3D_ProjectScreenVertices(Scene3D *scn, int32 centerXFixed, int32 centerYFixed,
                                                 bool storeOutCode, int32 clipLeft, int32 clipRight,
                                                 int32 clipTop, int32 clipBottom)
{
    Scene3DVertex *verts = scn->vertices;
    for (int32 v = 0; v < scn->vertexCount; ++v) {
        Scene3DVertex *vert = &verts[v];
        const int32 z = vert->z;
        if (z < 0x100) {
            if (storeOutCode)
                vert->outCode = SCENE3D_OUT_NEAR;
            continue;
        }

#if defined(__ARM_NEON__)
        float invZ = 1.0f / (float)z;
        vert->tx = centerXFixed + (int32)((float)(vert->x << scn->projectionX) * invZ * 65536.0f);
        vert->ty = centerYFixed - (int32)((float)(vert->y << scn->projectionY) * invZ * 65536.0f);
#else
        vert->tx = centerXFixed + ((vert->x << scn->projectionX) / z << 16);
        vert->ty = centerYFixed - ((vert->y << scn->projectionY) / z << 16);
#endif
        if (storeOutCode)
            vert->outCode = Scene3D_ScreenOutCode(vert->tx, vert->ty, clipLeft, clipRight, clipTop, clipBottom);
    }
}
#endif

#if RETRO_REV0U
#include "Legacy/Scene3DLegacy.cpp"
#endif

Model RSDK::modelList[MODEL_COUNT];
Scene3D RSDK::scene3DList[SCENE3D_COUNT];

ScanEdge RSDK::scanEdgeBuffer[SCREEN_YSIZE * 2];

void RSDK::ProcessScanEdge(int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 ix1 = FROM_FIXED(x1);
    int32 iy1 = FROM_FIXED(y1);
    int32 ix2 = FROM_FIXED(x2);
    int32 iy2 = FROM_FIXED(y2);

    int32 top = FROM_FIXED(y1);
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top = FROM_FIXED(y2);
            ix1 = FROM_FIXED(x2);
            ix2 = FROM_FIXED(x1);
            iy1 = FROM_FIXED(y2);
            iy2 = FROM_FIXED(y1);
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;
            int32 scanPos = TO_FIXED(ix1);
#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
            float inv_size = 1.0f / (float)(iy2 - iy1);
            int32 delta    = (int32)((float)TO_FIXED(ix2 - ix1) * inv_size);
#else
            int32 delta = TO_FIXED(ix2 - ix1) / (iy2 - iy1);
#endif
            if (top < 0) {
                scanPos -= top * delta;
                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                int32 scanX = scanPos >> 16;
                if (scanX < edge->start)
                    edge->start = scanX;
                if (scanX > edge->end)
                    edge->end = scanX;
                scanPos += delta;
                ++edge;
            }
        }
    }
}

void RSDK::ProcessScanEdgeClr(uint32 c1, uint32 c2, int32 x1, int32 y1, int32 x2, int32 y2)
{
    int32 iy1 = FROM_FIXED(y1);
    int32 iy2 = FROM_FIXED(y2);
    int32 ix1 = FROM_FIXED(x1);
    int32 ix2 = FROM_FIXED(x2);

    int32 top     = FROM_FIXED(y1);
    uint32 color1 = c1;
    uint32 color2 = c2;
    if (iy1 != iy2) {
        if (iy1 > iy2) {
            top    = FROM_FIXED(y2);
            ix1    = FROM_FIXED(x2);
            ix2    = FROM_FIXED(x1);
            iy1    = FROM_FIXED(y2);
            iy2    = FROM_FIXED(y1);
            color1 = c2;
            color2 = c1;
        }

        int32 bottom = iy2 + 1;
        if (top < currentScreen->clipBound_Y2 && bottom >= currentScreen->clipBound_Y1) {
            if (bottom > currentScreen->clipBound_Y2)
                bottom = currentScreen->clipBound_Y2;

            int32 size  = iy2 - iy1;
            int32 scanX = TO_FIXED(ix1);

            int32 c1R   = (color1 & 0xFF0000);
            int32 c2R   = (color2 & 0xFF0000);
            int32 scanR = c1R;

            int32 c1G   = (color1 & 0x00FF00) << 8;
            int32 c2G   = (color2 & 0x00FF00) << 8;
            int32 scanG = c1G;

            int32 c1B   = (color1 & 0x0000FF) << 16;
            int32 c2B   = (color2 & 0x0000FF) << 16;
            int32 scanB = c1B;

#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
            // One VFP float div shared across all 4 deltas (no hw int divider on Cortex-A9).
            float inv_size = 1.0f / (float)size;
            int32 deltaX   = (int32)((float)TO_FIXED(ix2 - ix1) * inv_size);
            int32 deltaR   = (int32)((float)(c2R - c1R) * inv_size);
            int32 deltaG   = (int32)((float)(c2G - c1G) * inv_size);
            int32 deltaB   = (int32)((float)(c2B - c1B) * inv_size);
#else
            int32 deltaX = TO_FIXED(ix2 - ix1) / size;

            int32 deltaR = 0;
            if (c1R != c2R)
                deltaR = (c2R - c1R) / size;

            int32 deltaG = 0;
            if (c1G != c2G)
                deltaG = (c2G - c1G) / size;

            int32 deltaB = 0;
            if (c1B != c2B)
                deltaB = (c2B - c1B) / size;
#endif

            if (top < 0) {
                scanX -= top * deltaX;

                scanR -= top * deltaR;
                scanG -= top * deltaG;
                scanB -= top * deltaB;

                top = 0;
            }

            ScanEdge *edge = &scanEdgeBuffer[top];
            for (int32 i = top; i < bottom; ++i) {
                if (FROM_FIXED(scanX) < edge->start) {
                    edge->start = FROM_FIXED(scanX);

                    edge->startR = scanR;
                    edge->startG = scanG;
                    edge->startB = scanB;
                }

                if (FROM_FIXED(scanX) > edge->end) {
                    edge->end = FROM_FIXED(scanX);

                    edge->endR = scanR;
                    edge->endG = scanG;
                    edge->endB = scanB;
                }

                scanX += deltaX;

                scanR += deltaR;
                scanG += deltaG;
                scanB += deltaB;

                ++edge;
            }
        }
    }
}

void RSDK::SetIdentityMatrix(Matrix *matrix)
{
    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixMultiply(Matrix *dest, Matrix *matrixA, Matrix *matrixB)
{
    int32 result[4][4];
    memset(result, 0, 4 * 4 * sizeof(int32));

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA        = i / 4;
        uint32 rowB        = i % 4;
        result[rowB][rowA] = (matrixA->values[3][rowA] * matrixB->values[rowB][3] >> 8) + (matrixA->values[2][rowA] * matrixB->values[rowB][2] >> 8)
                             + (matrixA->values[1][rowA] * matrixB->values[rowB][1] >> 8)
                             + (matrixA->values[0][rowA] * matrixB->values[rowB][0] >> 8);
    }

    for (int32 i = 0; i < 0x10; ++i) {
        uint32 rowA              = i / 4;
        uint32 rowB              = i % 4;
        dest->values[rowB][rowA] = result[rowB][rowA];
    }
}
void RSDK::MatrixTranslateXYZ(Matrix *matrix, int32 x, int32 y, int32 z, bool32 setIdentity)
{
    if (setIdentity) {
        matrix->values[0][0] = 0x100;
        matrix->values[1][0] = 0;
        matrix->values[2][0] = 0;
        matrix->values[0][1] = 0;
        matrix->values[1][1] = 0x100;
        matrix->values[2][1] = 0;
        matrix->values[0][2] = 0;
        matrix->values[1][2] = 0;
        matrix->values[2][2] = 0x100;
        matrix->values[3][0] = 0;
        matrix->values[3][1] = 0;
        matrix->values[3][2] = 0;
        matrix->values[3][3] = 0x100;
    }

    matrix->values[0][3] = x >> 8;
    matrix->values[1][3] = y >> 8;
    matrix->values[2][3] = z >> 8;
}
void RSDK::MatrixScaleXYZ(Matrix *matrix, int32 scaleX, int32 scaleY, int32 scaleZ)
{
    matrix->values[0][0] = scaleX;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = scaleY;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = scaleZ;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateX(Matrix *matrix, int16 rotationX)
{
    int32 sine   = sin1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 cosine = cos1024LookupTable[rotationX & 0x3FF] >> 2;

    matrix->values[0][0] = 0x100;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = sine;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = -sine;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateY(Matrix *matrix, int16 rotationY)
{
    int32 sine           = sin1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 cosine         = cos1024LookupTable[rotationY & 0x3FF] >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = 0;
    matrix->values[2][0] = sine;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = 0;
    matrix->values[1][1] = 0x100;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = -sine;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = cosine;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateZ(Matrix *matrix, int16 rotationZ)
{
    int32 sine           = sin1024LookupTable[rotationZ & 0x3FF] >> 2;
    int32 cosine         = cos1024LookupTable[rotationZ & 0x3FF] >> 2;
    matrix->values[0][0] = cosine;
    matrix->values[1][0] = -sine;
    matrix->values[2][0] = 0;
    matrix->values[3][0] = 0;
    matrix->values[0][1] = sine;
    matrix->values[1][1] = cosine;
    matrix->values[2][1] = 0;
    matrix->values[3][1] = 0;
    matrix->values[0][2] = 0;
    matrix->values[1][2] = 0;
    matrix->values[2][2] = 0x100;
    matrix->values[3][2] = 0;
    matrix->values[0][3] = 0;
    matrix->values[1][3] = 0;
    matrix->values[2][3] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixRotateXYZ(Matrix *matrix, int16 rotationX, int16 rotationY, int16 rotationZ)
{
    int32 sinX = sin1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 cosX = cos1024LookupTable[rotationX & 0x3FF] >> 2;
    int32 sinY = sin1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 cosY = cos1024LookupTable[rotationY & 0x3FF] >> 2;
    int32 sinZ = sin1024LookupTable[rotationZ & 0x3FF] >> 2;
    int32 cosZ = cos1024LookupTable[rotationZ & 0x3FF] >> 2;

    matrix->values[0][0] = (cosZ * cosY >> 8) + (sinZ * (sinY * sinX >> 8) >> 8);
    matrix->values[0][1] = -(sinZ * cosX) >> 8;
    matrix->values[0][2] = (sinZ * (cosY * sinX >> 8) >> 8) - (cosZ * sinY >> 8);
    matrix->values[0][3] = 0;
    matrix->values[1][0] = (sinZ * cosY >> 8) - (cosZ * (sinY * sinX >> 8) >> 8);
    matrix->values[1][1] = cosZ * cosX >> 8;
    matrix->values[1][2] = (-(sinZ * sinY) >> 8) - (cosZ * (cosY * sinX >> 8) >> 8);
    matrix->values[1][3] = 0;
    matrix->values[2][0] = sinY * cosX >> 8;
    matrix->values[2][1] = sinX;
    matrix->values[2][2] = cosY * cosX >> 8;
    matrix->values[2][3] = 0;
    matrix->values[3][0] = 0;
    matrix->values[3][1] = 0;
    matrix->values[3][2] = 0;
    matrix->values[3][3] = 0x100;
}
void RSDK::MatrixInverse(Matrix *dest, Matrix *matrix)
{
    double inv[16], det;
    double m[16];
    for (int32 y = 0; y < 4; ++y) {
        for (int32 x = 0; x < 4; ++x) {
            m[(y << 2) + x] = matrix->values[y][x] / 256.0;
        }
    }

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];

    if (det == 0)
        return;

    det = 1.0 / det;

    for (int32 i = 0; i < 0x10; ++i) inv[i] = (int32)((inv[i] * det) * 256);
    for (int32 i = 0; i < 0x10; ++i) dest->values[i / 4][i % 4] = (int32)inv[i];
}

#if defined(RSDK_USE_MISTER)
static bool Scene3D_ModelVerticesMatch(const Model *model, int32 oldVertCount, int32 a, int32 b)
{
    if ((model->flags & MODEL_USECOLOURS) && model->colors[a].color != model->colors[b].color)
        return false;

    for (int32 f = 0; f < model->frameCount; ++f) {
        const ModelVertex *va = &model->vertices[(f * oldVertCount) + a];
        const ModelVertex *vb = &model->vertices[(f * oldVertCount) + b];

        if (va->x != vb->x || va->y != vb->y || va->z != vb->z
            || va->nx != vb->nx || va->ny != vb->ny || va->nz != vb->nz) {
            return false;
        }
    }

    return true;
}

static void Scene3D_OptimizeModelVertices(Model *model)
{
    if (!model->vertices || !model->indices || model->vertCount < 2 || !model->indexCount || !model->frameCount)
        return;
    if (model->vertCount > SCENE3D_MODEL_OPT_VERT_LIMIT)
        return;
    if (model->flags & MODEL_USETEXTURES)
        return;

    const int32 oldVertCount = model->vertCount;
    int32 newVertCount       = 0;

    for (int32 v = 0; v < oldVertCount; ++v) {
        int32 match = -1;
        for (int32 n = 0; n < newVertCount; ++n) {
            if (Scene3D_ModelVerticesMatch(model, oldVertCount, v, s_s3d_model_opt_keep[n])) {
                match = n;
                break;
            }
        }

        if (match >= 0) {
            s_s3d_model_opt_remap[v] = (uint16)match;
        }
        else {
            s_s3d_model_opt_keep[newVertCount] = (uint16)v;
            s_s3d_model_opt_remap[v] = (uint16)newVertCount;
            ++newVertCount;
        }
    }

    if (newVertCount >= oldVertCount)
        return;

    for (int32 f = 0; f < model->frameCount; ++f) {
        ModelVertex *src = &model->vertices[f * oldVertCount];
        for (int32 v = 0; v < newVertCount; ++v)
            s_s3d_model_opt_frame[v] = src[s_s3d_model_opt_keep[v]];

        memcpy(&model->vertices[f * newVertCount], s_s3d_model_opt_frame, sizeof(ModelVertex) * newVertCount);
    }

    if (model->flags & MODEL_USECOLOURS) {
        for (int32 v = 0; v < newVertCount; ++v)
            s_s3d_model_opt_colors[v] = model->colors[s_s3d_model_opt_keep[v]];

        memcpy(model->colors, s_s3d_model_opt_colors, sizeof(Color) * newVertCount);
    }

    for (int32 i = 0; i < model->indexCount; ++i) {
        if (model->indices[i] < oldVertCount)
            model->indices[i] = s_s3d_model_opt_remap[model->indices[i]];
    }

    model->vertCount = (uint16)newVertCount;
}
#endif

#if defined(RSDK_USE_MISTER)
static void Scene3D_InvalidateFaceColorCache(uint16 modelID);
#endif

uint16 RSDK::LoadMesh(const char *filename, uint8 scope)
{
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    char fullFilePath[0x100];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Meshes/%s", filename);

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(fullFilePath, hash);

    for (int32 i = 0; i < MODEL_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, modelList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < MODEL_COUNT; ++id) {
        if (modelList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= MODEL_COUNT)
        return -1;

#if defined(RSDK_USE_MISTER)
    Scene3D_InvalidateFaceColorCache(id);
#endif

    Model *model = &modelList[id];
    FileInfo info;
    InitFileInfo(&info);
    if (LoadFile(&info, fullFilePath, FMODE_RB)) {
        uint32 sig = ReadInt32(&info, false);

        if (sig != RSDK_SIGNATURE_MDL) {
            CloseFile(&info);
            return -1;
        }

        model->scope = scope;
        HASH_COPY_MD5(model->hash, hash);

        model->flags         = ReadInt8(&info);
        model->faceVertCount = ReadInt8(&info);

        model->vertCount  = ReadInt16(&info);
        model->frameCount = ReadInt16(&info);

        AllocateStorage((void **)&model->vertices, sizeof(ModelVertex) * model->vertCount * model->frameCount, DATASET_STG, true);
        if (model->flags & MODEL_USETEXTURES)
            AllocateStorage((void **)&model->texCoords, sizeof(TexCoord) * model->vertCount, DATASET_STG, true);
        if (model->flags & MODEL_USECOLOURS)
            AllocateStorage((void **)&model->colors, sizeof(Color) * model->vertCount, DATASET_STG, true);

        if (model->flags & MODEL_USETEXTURES) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->texCoords[v].x = ReadSingle(&info);
                model->texCoords[v].y = ReadSingle(&info);
            }
        }

        if (model->flags & MODEL_USECOLOURS) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->colors[v].color = ReadInt32(&info, false);
            }
        }

        model->indexCount = ReadInt16(&info);
        AllocateStorage((void **)&model->indices, sizeof(uint16) * model->indexCount, DATASET_STG, true);
        for (int32 i = 0; i < model->indexCount; ++i) model->indices[i] = ReadInt16(&info);

        for (int32 f = 0; f < model->frameCount; ++f) {
            for (int32 v = 0; v < model->vertCount; ++v) {
                model->vertices[(f * model->vertCount) + v].x = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].y = (int32)(ReadSingle(&info) * 0x100);
                model->vertices[(f * model->vertCount) + v].z = (int32)(ReadSingle(&info) * 0x100);

                model->vertices[(f * model->vertCount) + v].nx = 0;
                model->vertices[(f * model->vertCount) + v].ny = 0;
                model->vertices[(f * model->vertCount) + v].nz = 0;
                if (model->flags & MODEL_USENORMALS) {
                    model->vertices[(f * model->vertCount) + v].nx = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].ny = (int32)(ReadSingle(&info) * 0x10000);
                    model->vertices[(f * model->vertCount) + v].nz = (int32)(ReadSingle(&info) * 0x10000);
                }
            }
        }

#if defined(RSDK_USE_MISTER)
        Scene3D_OptimizeModelVertices(model);
#endif

        CloseFile(&info);
        return id;
    }
    return -1;
}
uint16 RSDK::Create3DScene(const char *name, uint16 vertexLimit, uint8 scope)
{
    if (!scope || scope > SCOPE_STAGE)
        return -1;

    RETRO_HASH_MD5(hash);
    GEN_HASH_MD5(name, hash);

    for (int32 i = 0; i < SCENE3D_COUNT; ++i) {
        if (HASH_MATCH_MD5(hash, scene3DList[i].hash)) {
            return i;
        }
    }

    uint16 id = -1;
    for (id = 0; id < SCENE3D_COUNT; ++id) {
        if (scene3DList[id].scope == SCOPE_NONE)
            break;
    }

    if (id >= SCENE3D_COUNT)
        return -1;

    Scene3D *scene = &scene3DList[id];

    if (vertexLimit > SCENE3D_VERT_COUNT || !vertexLimit)
        vertexLimit = SCENE3D_VERT_COUNT;

    scene->scope = scope;
    HASH_COPY_MD5(scene->hash, hash);
    scene->vertLimit = vertexLimit;
    scene->faceCount = 6;

    scene->projectionX = 8;
    scene->projectionY = 8;
    AllocateStorage((void **)&scene->vertices, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->normals, sizeof(Scene3DVertex) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceVertCounts, sizeof(uint8) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceVertexRefs, sizeof(uint16) * vertexLimit, DATASET_STG, true);
    AllocateStorage((void **)&scene->faceBuffer, sizeof(Scene3DFace) * vertexLimit, DATASET_STG, true);

    return id;
}

#if defined(RSDK_USE_MISTER)
static inline void Scene3D_SetFaceLayout(Scene3D *scn, int32 faceBase, int32 startIndex, int32 faceCount, uint8 faceVertCount, bool indexed,
                                         int32 depth = 0)
{
    memset(&scn->faceVertCounts[faceBase], faceVertCount, faceCount);
    int32 index = indexed ? (startIndex | SCENE3D_FACE_INDEXED) : startIndex;
    for (int32 f = 0; f < faceCount; ++f) {
        Scene3DFace *face = &scn->faceBuffer[faceBase + f];
        face->index = index + f * faceVertCount;
        face->depth = depth;
        face->flags = 0;
    }
}

static inline void Scene3D_WriteCachedPos(Scene3DVertex *vertex, const Matrix *matWorld, int32 x, int32 y, int32 z)
{
    vertex->x = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (matWorld->values[0][0] * x >> 8)
                + (matWorld->values[0][1] * y >> 8);
    vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (z * matWorld->values[1][2] >> 8)
                + (matWorld->values[1][0] * x >> 8);
    vertex->z = matWorld->values[2][3] + (x * matWorld->values[2][0] >> 8) + (matWorld->values[2][2] * z >> 8)
                + (matWorld->values[2][1] * y >> 8);
    vertex->tx = 0;
    vertex->ty = 0;
    vertex->outCode = 0;
}

static inline void Scene3D_WriteCachedNormal(Scene3DVertex *vertex, const Matrix *matNormals, int32 nx, int32 ny, int32 nz)
{
    vertex->nx = (nz * matNormals->values[0][2] >> 8) + (nx * matNormals->values[0][0] >> 8) + (matNormals->values[0][1] * ny >> 8);
    vertex->ny = (ny * matNormals->values[1][1] >> 8) + (nz * matNormals->values[1][2] >> 8) + (nx * matNormals->values[1][0] >> 8);
    vertex->nz = (ny * matNormals->values[2][1] >> 8) + (matNormals->values[2][0] * nx >> 8) + (nz * matNormals->values[2][2] >> 8);
}

static inline void Scene3D_WriteCachedZeroNormal(Scene3DVertex *vertex)
{
    vertex->nx = 0;
    vertex->ny = 0;
    vertex->nz = 0;
}

static inline bool Scene3D_CanStoreIndexedFaces(const Model *mdl, const Scene3D *scn, uint8 drawMode)
{
    if (!Scene3D_ShouldStoreIndexedFaces(mdl, drawMode))
        return false;

    const int32 faceCount = mdl->indexCount / mdl->faceVertCount;
    return (int32)scn->vertLimit - (int32)scn->vertexCount >= (int32)mdl->vertCount
           && (int32)scn->vertLimit - (int32)scn->faceRefCount >= (int32)mdl->indexCount
           && (int32)scn->vertLimit - (int32)scn->faceCount >= faceCount;
}

static inline bool Scene3D_UseCoarseModelDepth(uint8 drawMode)
{
    // Friend's commit enabled coarse per-model depth sort for UFO_Decoration —
    // sorts by matWorld[2][3]>>5 instead of per-face avg depth. He flagged
    // visual artifacts ("subtle issues around z-buffer") that he didn't fix.
    // Held back from this take; revisit once we baseline-measure with the
    // per-face depth path active. Restore by returning the original predicate.
    (void)drawMode;
    return false;
}

static inline int32 Scene3D_CoarseModelDepth(const Matrix *matWorld)
{
    return matWorld->values[2][3] >> 5;
}

static void Scene3D_InvalidateFaceColorCache(uint16 modelID)
{
    if (modelID < MODEL_COUNT) {
        s_s3d_cached_face_color_model[modelID] = 0;
        s_s3d_skip_model_normals[modelID] = 0;
    }

    for (int32 i = 0; i < SCENE3D_FACE_COLOR_CACHE_MODELS; ++i) {
        if (s_s3d_face_color_cache[i].valid && s_s3d_face_color_cache[i].modelID == modelID)
            s_s3d_face_color_cache[i].valid = false;
    }
}

static inline void Scene3D_BuildLitColor16(const Scene3D *scn, uint32 baseColor, int32 normal, uint32 *outColor, uint16 *outColor16)
{
    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

    int32 specular = normalVal >> 6 >> scn->specularIntensityX;
    specular       = CLAMP(specular, 0x00, 0xFF);
    int32 r = specular + ((int32)((baseColor >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

    specular = normalVal >> 6 >> scn->specularIntensityY;
    specular = CLAMP(specular, 0x00, 0xFF);
    int32 g = specular + ((int32)((baseColor >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

    specular = normalVal >> 6 >> scn->specularIntensityZ;
    specular = CLAMP(specular, 0x00, 0xFF);
    int32 b = specular + ((int32)(baseColor & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

    r = CLAMP(r, 0x00, 0xFF);
    g = CLAMP(g, 0x00, 0xFF);
    b = CLAMP(b, 0x00, 0xFF);

    *outColor = (r << 16) | (g << 8) | b;
    *outColor16 = rgb32To16_B[b] | rgb32To16_G[g] | rgb32To16_R[r];
}

static inline uint32 Scene3D_DiffuseKey(const Scene3D *scn)
{
    return ((uint32)scn->diffuseX & 0xFF) | (((uint32)scn->diffuseY & 0xFF) << 8) | (((uint32)scn->diffuseZ & 0xFF) << 16);
}

static inline uint32 Scene3D_DiffuseIntensityKey(const Scene3D *scn)
{
    return ((uint32)scn->diffuseIntensityX & 0xFF) | (((uint32)scn->diffuseIntensityY & 0xFF) << 8)
           | (((uint32)scn->diffuseIntensityZ & 0xFF) << 16);
}

static inline uint32 Scene3D_SpecularIntensityKey(const Scene3D *scn)
{
    return ((uint32)scn->specularIntensityX & 0xFF) | (((uint32)scn->specularIntensityY & 0xFF) << 8)
           | (((uint32)scn->specularIntensityZ & 0xFF) << 16);
}

static Scene3DFaceColorCache *Scene3D_GetFaceColorCache(uint16 modelID, const Model *mdl, const Scene3D *scn)
{
    if (modelID >= MODEL_COUNT || !s_s3d_cached_face_color_model[modelID] || !mdl->faceVertCount || !mdl->frameCount)
        return NULL;
    if (mdl->frameCount > SCENE3D_FACE_COLOR_CACHE_FRAMES)
        return NULL;

    const uint16 faceCount = mdl->indexCount / mdl->faceVertCount;
    if (!faceCount || (uint32)faceCount * (uint32)mdl->frameCount > SCENE3D_FACE_COLOR_CACHE_ENTRIES)
        return NULL;

    const uint32 diffuseKey = Scene3D_DiffuseKey(scn);
    const uint32 diffuseIntensityKey = Scene3D_DiffuseIntensityKey(scn);
    const uint32 specularIntensityKey = Scene3D_SpecularIntensityKey(scn);

    for (int32 i = 0; i < SCENE3D_FACE_COLOR_CACHE_MODELS; ++i) {
        Scene3DFaceColorCache *cache = &s_s3d_face_color_cache[i];
        if (cache->valid && cache->modelID == modelID && cache->faceCount == faceCount && cache->frameCount == mdl->frameCount
            && cache->diffuseKey == diffuseKey && cache->diffuseIntensityKey == diffuseIntensityKey
            && cache->specularIntensityKey == specularIntensityKey)
            return cache;
    }

    Scene3DFaceColorCache *cache = &s_s3d_face_color_cache[s_s3d_face_color_cache_next++ % SCENE3D_FACE_COLOR_CACHE_MODELS];
    for (int32 i = 0; i < SCENE3D_FACE_COLOR_CACHE_MODELS; ++i) {
        if (!s_s3d_face_color_cache[i].valid) {
            cache = &s_s3d_face_color_cache[i];
            break;
        }
    }

    cache->valid = true;
    cache->modelID = modelID;
    cache->faceCount = faceCount;
    cache->frameCount = mdl->frameCount;
    cache->diffuseKey = diffuseKey;
    cache->diffuseIntensityKey = diffuseIntensityKey;
    cache->specularIntensityKey = specularIntensityKey;
    cache->frameMask = 0;
    return cache;
}

static bool Scene3D_ApplyCachedFaceColors(Scene3D *scn, const Model *mdl, uint16 modelID, int32 frameID, int32 faceBase, color color)
{
    Scene3DFaceColorCache *cache = Scene3D_GetFaceColorCache(modelID, mdl, scn);
    if (!cache || frameID < 0 || frameID >= cache->frameCount)
        return false;

    const uint64_t frameBit = 1ULL << frameID;
    const int32 faceCount = cache->faceCount;
    const int32 frameColorBase = faceCount * frameID;

    if (!(cache->frameMask & frameBit)) {
        const int32 frameVertBase = frameID * mdl->vertCount;
        for (int32 f = 0; f < faceCount; ++f) {
            const int32 indexBase = f * mdl->faceVertCount;
            int32 ny = 0;

            for (int32 v = 0; v < mdl->faceVertCount; ++v) {
                const ModelVertex *modelVert = &mdl->vertices[frameVertBase + mdl->indices[indexBase + v]];
                ny += modelVert->ny;
            }

            uint32 baseColor = color;
            if (mdl->flags & MODEL_USECOLOURS)
                baseColor = mdl->colors[mdl->indices[indexBase]].color;

            Scene3D_BuildLitColor16(scn, baseColor, ny / mdl->faceVertCount,
                                    &cache->color[frameColorBase + f], &cache->color16[frameColorBase + f]);
        }
        cache->frameMask |= frameBit;
    }

    for (int32 f = 0; f < faceCount; ++f) {
        Scene3DFace *face = &scn->faceBuffer[faceBase + f];
        face->color = cache->color[frameColorBase + f];
        face->color16 = cache->color16[frameColorBase + f];
        face->flags |= SCENE3D_FACE_CACHED_COLOR;
    }

    return true;
}

static void Scene3D_WriteIndexedRefs(Scene3D *scn, int32 vertBase, int32 refBase, const uint16 *indices, int32 indexCount)
{
    uint16 *refs = &scn->faceVertexRefs[refBase];
    for (int32 i = 0; i < indexCount; ++i)
        refs[i] = (uint16)(vertBase + indices[i]);
}

static bool Scene3D_AddModelIndexed(const Model *mdl, Scene3D *scn, int32 vertID, int32 faceBase, int32 refBase, Matrix *matWorld,
                                    Matrix *matNormals, color color, uint8 drawMode)
{
    if (!Scene3D_CanStoreIndexedFaces(mdl, scn, drawMode))
        return false;

    const uint16 modelID = (uint16)(mdl - modelList);
    const int32 faceCount = mdl->indexCount / mdl->faceVertCount;
    int32 sortDepth = Scene3D_UseCoarseModelDepth(drawMode) ? Scene3D_CoarseModelDepth(matWorld) : 0;
    Scene3D_SetFaceLayout(scn, faceBase, refBase, faceCount, mdl->faceVertCount, true, sortDepth);
    Scene3D_WriteIndexedRefs(scn, vertID, refBase, mdl->indices, mdl->indexCount);
    const bool cachedFaceColors = drawMode == S3D_SOLIDCOLOR_SHADED_SCREEN
                                  && Scene3D_ApplyCachedFaceColors(scn, mdl, modelID, 0, faceBase, color);
    const bool skipNormals = cachedFaceColors && Scene3D_ShouldSkipModelNormals(modelID);

    for (int32 v = 0; v < mdl->vertCount; ++v) {
        const ModelVertex *modelVert = &mdl->vertices[v];
        Scene3DVertex *cached        = &scn->vertices[vertID + v];

        Scene3D_WriteCachedPos(cached, matWorld, modelVert->x, modelVert->y, modelVert->z);

        switch (mdl->flags) {
            case MODEL_USENORMALS:
                if (!skipNormals && matNormals)
                    Scene3D_WriteCachedNormal(cached, matNormals, modelVert->nx, modelVert->ny, modelVert->nz);
                else
                    Scene3D_WriteCachedZeroNormal(cached);
                cached->color = color;
                break;

            case MODEL_USENORMALS | MODEL_USECOLOURS:
                if (!skipNormals && matNormals)
                    Scene3D_WriteCachedNormal(cached, matNormals, modelVert->nx, modelVert->ny, modelVert->nz);
                else
                    Scene3D_WriteCachedZeroNormal(cached);
                cached->color = mdl->colors[v].color;
                break;

            default:
            case MODEL_NOFLAGS:
            case MODEL_USECOLOURS:
                Scene3D_WriteCachedZeroNormal(cached);
                cached->color = color;
                break;
        }
    }

    return true;
}

static bool Scene3D_AddMeshFrameIndexed(const Model *mdl, Scene3D *scn, int32 vertID, int32 faceBase, int32 refBase, Matrix *matWorld,
                                        Matrix *matNormals, int32 frameOffset, int32 nextFrameOffset, int32 interpolate, color color,
                                        uint8 drawMode)
{
    if (!Scene3D_CanStoreIndexedFaces(mdl, scn, drawMode))
        return false;

    const uint16 modelID = (uint16)(mdl - modelList);
    const int32 faceCount = mdl->indexCount / mdl->faceVertCount;
    int32 sortDepth = Scene3D_UseCoarseModelDepth(drawMode) ? Scene3D_CoarseModelDepth(matWorld) : 0;
    Scene3D_SetFaceLayout(scn, faceBase, refBase, faceCount, mdl->faceVertCount, true, sortDepth);
    Scene3D_WriteIndexedRefs(scn, vertID, refBase, mdl->indices, mdl->indexCount);
    const int32 frameID = frameOffset / mdl->vertCount;
    const int32 nextFrameID = nextFrameOffset / mdl->vertCount;
    const bool cachedFaceColors = drawMode == S3D_SOLIDCOLOR_SHADED_SCREEN
                                  && Scene3D_ApplyCachedFaceColors(scn, mdl, modelID, frameID, faceBase, color);
    const bool skipNormals = cachedFaceColors && Scene3D_ShouldSkipModelNormals(modelID);

    for (int32 v = 0; v < mdl->vertCount; ++v) {
        const ModelVertex *frameVert     = &mdl->vertices[frameOffset + v];
        const ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + v];
        Scene3DVertex *cached            = &scn->vertices[vertID + v];

        int32 x = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
        int32 y = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
        int32 z = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);

        Scene3D_WriteCachedPos(cached, matWorld, x, y, z);

        switch (mdl->flags) {
            case MODEL_USENORMALS: {
                if (!skipNormals && matNormals) {
                    int32 nx = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                    int32 ny = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                    int32 nz = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);
                    Scene3D_WriteCachedNormal(cached, matNormals, nx, ny, nz);
                }
                else {
                    Scene3D_WriteCachedZeroNormal(cached);
                }
                cached->color = color;
                break;
            }

            case MODEL_USENORMALS | MODEL_USECOLOURS: {
                if (!skipNormals && matNormals) {
                    int32 nx = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                    int32 ny = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                    int32 nz = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);
                    Scene3D_WriteCachedNormal(cached, matNormals, nx, ny, nz);
                }
                else {
                    Scene3D_WriteCachedZeroNormal(cached);
                }
                cached->color = mdl->colors[v].color;
                break;
            }

            default:
            case MODEL_NOFLAGS:
            case MODEL_USECOLOURS:
                Scene3D_WriteCachedZeroNormal(cached);
                cached->color = color;
                break;
        }
    }

    return true;
}
#endif

void RSDK::AddModelToScene(uint16 modelFrames, uint16 sceneIndex, uint8 drawMode, Matrix *matWorld, Matrix *matNormals, color color)
{
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld) {
            Model *mdl            = &modelList[modelFrames];
            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            int32 faceBase        = scn->faceCount;
            int32 refBase         = scn->faceRefCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[faceBase];
            int32 indCnt          = mdl->indexCount;
            int32 faceCount       = mdl->faceVertCount ? indCnt / mdl->faceVertCount : 0;
#if defined(RSDK_USE_MISTER)
            if (Scene3D_AddModelIndexed(mdl, scn, vertID, faceBase, refBase, matWorld, matNormals, color, drawMode)) {
                scn->vertexCount += mdl->vertCount;
                scn->faceRefCount += indCnt;
                scn->drawMode = drawMode;
                scn->faceCount += faceCount;
                return;
            }
#endif
            if (scn->vertLimit - vertID >= indCnt && scn->vertLimit - faceBase >= faceCount) {
                scn->vertexCount += indCnt;
                scn->drawMode = drawMode;
                scn->faceCount += faceCount;
#if defined(RSDK_USE_MISTER)
                int32 sortDepth = Scene3D_UseCoarseModelDepth(drawMode) ? Scene3D_CoarseModelDepth(matWorld) : 0;
                Scene3D_SetFaceLayout(scn, faceBase, vertID, faceCount, mdl->faceVertCount, false, sortDepth);
#endif

                int32 i = 0;
                int32 f = 0;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
                        {
                            // Pre-fill face vertex counts (all faces same size).
                            int32 nFaces = mdl->indexCount / mdl->faceVertCount;
                            memset(faceVertCounts, mdl->faceVertCount, nFaces);

                            // Cache matrix coefficients — broadcast translation cols to vectors.
                            const int32_t m00 = matWorld->values[0][0], m01 = matWorld->values[0][1],
                                          m02 = matWorld->values[0][2];
                            const int32_t m10 = matWorld->values[1][0], m11 = matWorld->values[1][1],
                                          m12 = matWorld->values[1][2];
                            const int32_t m20 = matWorld->values[2][0], m21 = matWorld->values[2][1],
                                          m22 = matWorld->values[2][2];
                            const int32x4_t vT0 = vdupq_n_s32(matWorld->values[0][3]);
                            const int32x4_t vT1 = vdupq_n_s32(matWorld->values[1][3]);
                            const int32x4_t vT2 = vdupq_n_s32(matWorld->values[2][3]);

                            int32 total    = mdl->indexCount;
                            int32 neon_end = total & ~3;

                            for (i = 0; i < neon_end; i += 4) {
                                const ModelVertex *v0 = &mdl->vertices[indices[i + 0]];
                                const ModelVertex *v1 = &mdl->vertices[indices[i + 1]];
                                const ModelVertex *v2 = &mdl->vertices[indices[i + 2]];
                                const ModelVertex *v3 = &mdl->vertices[indices[i + 3]];

                                int32x4_t vx = vdupq_n_s32(v0->x);
                                vx           = vsetq_lane_s32(v1->x, vx, 1);
                                vx           = vsetq_lane_s32(v2->x, vx, 2);
                                vx           = vsetq_lane_s32(v3->x, vx, 3);
                                int32x4_t vy = vdupq_n_s32(v0->y);
                                vy           = vsetq_lane_s32(v1->y, vy, 1);
                                vy           = vsetq_lane_s32(v2->y, vy, 2);
                                vy           = vsetq_lane_s32(v3->y, vy, 3);
                                int32x4_t vz = vdupq_n_s32(v0->z);
                                vz           = vsetq_lane_s32(v1->z, vz, 1);
                                vz           = vsetq_lane_s32(v2->z, vz, 2);
                                vz           = vsetq_lane_s32(v3->z, vz, 3);

                                int32x4_t ox =
                                    vaddq_s32(vT0, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m00), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m01), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m02), 8))));
                                int32x4_t oy =
                                    vaddq_s32(vT1, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m10), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m11), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m12), 8))));
                                int32x4_t oz =
                                    vaddq_s32(vT2, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m20), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m21), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m22), 8))));

                                Scene3DVertex *o0 = &scn->vertices[vertID + 0];
                                Scene3DVertex *o1 = &scn->vertices[vertID + 1];
                                Scene3DVertex *o2 = &scn->vertices[vertID + 2];
                                Scene3DVertex *o3 = &scn->vertices[vertID + 3];

                                o0->x = vgetq_lane_s32(ox, 0); o0->y = vgetq_lane_s32(oy, 0); o0->z = vgetq_lane_s32(oz, 0);
                                o1->x = vgetq_lane_s32(ox, 1); o1->y = vgetq_lane_s32(oy, 1); o1->z = vgetq_lane_s32(oz, 1);
                                o2->x = vgetq_lane_s32(ox, 2); o2->y = vgetq_lane_s32(oy, 2); o2->z = vgetq_lane_s32(oz, 2);
                                o3->x = vgetq_lane_s32(ox, 3); o3->y = vgetq_lane_s32(oy, 3); o3->z = vgetq_lane_s32(oz, 3);
                                o0->nx = 0; o0->ny = 0; o0->nz = 0; o0->color = color;
                                o1->nx = 0; o1->ny = 0; o1->nz = 0; o1->color = color;
                                o2->nx = 0; o2->ny = 0; o2->nz = 0; o2->color = color;
                                o3->nx = 0; o3->ny = 0; o3->nz = 0; o3->color = color;
                                vertID += 4;
                            }
                            for (; i < total; ++i) {
                                const ModelVertex *mv = &mdl->vertices[indices[i]];
                                Scene3DVertex *sv     = &scn->vertices[vertID++];
                                sv->x  = matWorld->values[0][3] + (mv->z * m02 >> 8) + (m00 * mv->x >> 8) + (m01 * mv->y >> 8);
                                sv->y  = matWorld->values[1][3] + (mv->y * m11 >> 8) + (mv->z * m12 >> 8) + (m10 * mv->x >> 8);
                                sv->z  = matWorld->values[2][3] + (mv->x * m20 >> 8) + (m22 * mv->z >> 8) + (m21 * mv->y >> 8);
                                sv->nx = 0; sv->ny = 0; sv->nz = 0; sv->color = color;
                            }
                        }
#else
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                            + (matWorld->values[0][0] * modelVert->x >> 8) + (matWorld->values[0][1] * modelVert->y >> 8);
                                vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                            + (modelVert->z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                vertex->z = matWorld->values[2][3] + ((modelVert->x * matWorld->values[2][0]) >> 8)
                                            + ((matWorld->values[2][2] * modelVert->z >> 8) + (matWorld->values[2][1] * modelVert->y >> 8));
                                vertex->nx    = 0;
                                vertex->ny    = 0;
                                vertex->nz    = 0;
                                vertex->color = color;
                            }
                        }
#endif
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
                            {
                                int32 nFaces = mdl->indexCount / mdl->faceVertCount;
                                memset(faceVertCounts, mdl->faceVertCount, nFaces);

                                const int32_t m00 = matWorld->values[0][0],   m01 = matWorld->values[0][1],   m02 = matWorld->values[0][2];
                                const int32_t m10 = matWorld->values[1][0],   m11 = matWorld->values[1][1],   m12 = matWorld->values[1][2];
                                const int32_t m20 = matWorld->values[2][0],   m21 = matWorld->values[2][1],   m22 = matWorld->values[2][2];
                                const int32_t n00 = matNormals->values[0][0], n01 = matNormals->values[0][1], n02 = matNormals->values[0][2];
                                const int32_t n10 = matNormals->values[1][0], n11 = matNormals->values[1][1], n12 = matNormals->values[1][2];
                                const int32_t n20 = matNormals->values[2][0], n21 = matNormals->values[2][1], n22 = matNormals->values[2][2];
                                const int32x4_t vT0 = vdupq_n_s32(matWorld->values[0][3]);
                                const int32x4_t vT1 = vdupq_n_s32(matWorld->values[1][3]);
                                const int32x4_t vT2 = vdupq_n_s32(matWorld->values[2][3]);

                                int32 total    = mdl->indexCount;
                                int32 neon_end = total & ~3;

                                for (i = 0; i < neon_end; i += 4) {
                                    const ModelVertex *v0 = &mdl->vertices[indices[i + 0]];
                                    const ModelVertex *v1 = &mdl->vertices[indices[i + 1]];
                                    const ModelVertex *v2 = &mdl->vertices[indices[i + 2]];
                                    const ModelVertex *v3 = &mdl->vertices[indices[i + 3]];

                                    int32x4_t vx = vdupq_n_s32(v0->x); vx = vsetq_lane_s32(v1->x, vx, 1); vx = vsetq_lane_s32(v2->x, vx, 2); vx = vsetq_lane_s32(v3->x, vx, 3);
                                    int32x4_t vy = vdupq_n_s32(v0->y); vy = vsetq_lane_s32(v1->y, vy, 1); vy = vsetq_lane_s32(v2->y, vy, 2); vy = vsetq_lane_s32(v3->y, vy, 3);
                                    int32x4_t vz = vdupq_n_s32(v0->z); vz = vsetq_lane_s32(v1->z, vz, 1); vz = vsetq_lane_s32(v2->z, vz, 2); vz = vsetq_lane_s32(v3->z, vz, 3);
                                    int32x4_t vnx = vdupq_n_s32(v0->nx); vnx = vsetq_lane_s32(v1->nx, vnx, 1); vnx = vsetq_lane_s32(v2->nx, vnx, 2); vnx = vsetq_lane_s32(v3->nx, vnx, 3);
                                    int32x4_t vny = vdupq_n_s32(v0->ny); vny = vsetq_lane_s32(v1->ny, vny, 1); vny = vsetq_lane_s32(v2->ny, vny, 2); vny = vsetq_lane_s32(v3->ny, vny, 3);
                                    int32x4_t vnz = vdupq_n_s32(v0->nz); vnz = vsetq_lane_s32(v1->nz, vnz, 1); vnz = vsetq_lane_s32(v2->nz, vnz, 2); vnz = vsetq_lane_s32(v3->nz, vnz, 3);

                                    int32x4_t ox =
                                        vaddq_s32(vT0, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m00), 8),
                                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m01), 8),
                                                                            vshrq_n_s32(vmulq_n_s32(vz, m02), 8))));
                                    int32x4_t oy =
                                        vaddq_s32(vT1, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m10), 8),
                                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m11), 8),
                                                                            vshrq_n_s32(vmulq_n_s32(vz, m12), 8))));
                                    int32x4_t oz =
                                        vaddq_s32(vT2, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m20), 8),
                                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m21), 8),
                                                                            vshrq_n_s32(vmulq_n_s32(vz, m22), 8))));
                                    int32x4_t onx =
                                        vaddq_s32(vshrq_n_s32(vmulq_n_s32(vnx, n00), 8),
                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vny, n01), 8),
                                                            vshrq_n_s32(vmulq_n_s32(vnz, n02), 8)));
                                    int32x4_t ony =
                                        vaddq_s32(vshrq_n_s32(vmulq_n_s32(vnx, n10), 8),
                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vny, n11), 8),
                                                            vshrq_n_s32(vmulq_n_s32(vnz, n12), 8)));
                                    int32x4_t onz =
                                        vaddq_s32(vshrq_n_s32(vmulq_n_s32(vnx, n20), 8),
                                                  vaddq_s32(vshrq_n_s32(vmulq_n_s32(vny, n21), 8),
                                                            vshrq_n_s32(vmulq_n_s32(vnz, n22), 8)));

                                    Scene3DVertex *o0 = &scn->vertices[vertID + 0];
                                    Scene3DVertex *o1 = &scn->vertices[vertID + 1];
                                    Scene3DVertex *o2 = &scn->vertices[vertID + 2];
                                    Scene3DVertex *o3 = &scn->vertices[vertID + 3];

                                    o0->x = vgetq_lane_s32(ox, 0);  o0->y = vgetq_lane_s32(oy, 0);  o0->z = vgetq_lane_s32(oz, 0);
                                    o1->x = vgetq_lane_s32(ox, 1);  o1->y = vgetq_lane_s32(oy, 1);  o1->z = vgetq_lane_s32(oz, 1);
                                    o2->x = vgetq_lane_s32(ox, 2);  o2->y = vgetq_lane_s32(oy, 2);  o2->z = vgetq_lane_s32(oz, 2);
                                    o3->x = vgetq_lane_s32(ox, 3);  o3->y = vgetq_lane_s32(oy, 3);  o3->z = vgetq_lane_s32(oz, 3);
                                    o0->nx = vgetq_lane_s32(onx, 0); o0->ny = vgetq_lane_s32(ony, 0); o0->nz = vgetq_lane_s32(onz, 0); o0->color = color;
                                    o1->nx = vgetq_lane_s32(onx, 1); o1->ny = vgetq_lane_s32(ony, 1); o1->nz = vgetq_lane_s32(onz, 1); o1->color = color;
                                    o2->nx = vgetq_lane_s32(onx, 2); o2->ny = vgetq_lane_s32(ony, 2); o2->nz = vgetq_lane_s32(onz, 2); o2->color = color;
                                    o3->nx = vgetq_lane_s32(onx, 3); o3->ny = vgetq_lane_s32(ony, 3); o3->nz = vgetq_lane_s32(onz, 3); o3->color = color;
                                    vertID += 4;
                                }
                                for (; i < total; ++i) {
                                    const ModelVertex *mv = &mdl->vertices[indices[i]];
                                    Scene3DVertex *sv     = &scn->vertices[vertID++];
                                    sv->x  = matWorld->values[0][3]  + (mv->z * m02 >> 8) + (mv->x * m00 >> 8) + (mv->y * m01 >> 8);
                                    sv->y  = matWorld->values[1][3]  + (mv->y * m11 >> 8) + (m10 * mv->x >> 8) + (mv->z * m12 >> 8);
                                    sv->z  = matWorld->values[2][3]  + (mv->x * m20 >> 8) + (m22 * mv->z >> 8) + (m21 * mv->y >> 8);
                                    sv->nx = (mv->nz * n02 >> 8) + (mv->nx * n00 >> 8) + (n01 * mv->ny >> 8);
                                    sv->ny = (mv->ny * n11 >> 8) + (mv->nz * n12 >> 8) + (mv->nx * n10 >> 8);
                                    sv->nz = (mv->ny * n21 >> 8) + (n20 * mv->nx >> 8)  + (mv->nz * n22 >> 8);
                                    sv->color = color;
                                }
                            }
#else
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                                + (modelVert->x * matWorld->values[0][0] >> 8) + (modelVert->y * matWorld->values[0][1] >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                                + (matWorld->values[1][0] * modelVert->x >> 8) + (modelVert->z * matWorld->values[1][2] >> 8);
                                    vertex->z = matWorld->values[2][3] + (modelVert->x * matWorld->values[2][0] >> 8)
                                                + (matWorld->values[2][2] * modelVert->z >> 8) + (matWorld->values[2][1] * modelVert->y >> 8);

                                    vertex->nx = (modelVert->nz * matNormals->values[0][2] >> 8) + (modelVert->nx * matNormals->values[0][0] >> 8)
                                                 + (matNormals->values[0][1] * modelVert->ny >> 8);
                                    vertex->ny = (modelVert->ny * matNormals->values[1][1] >> 8) + (modelVert->nz * matNormals->values[1][2] >> 8)
                                                 + (modelVert->nx * matNormals->values[1][0] >> 8);
                                    vertex->nz =
                                        ((modelVert->ny * matNormals->values[2][1]) >> 8)
                                        + ((matNormals->values[2][0] * modelVert->nx >> 8) + (modelVert->nz * matNormals->values[2][2] >> 8));

                                    vertex->color = color;
                                }
                            }
#endif
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (modelVert->z * matWorld->values[0][2] >> 8)
                                                + (matWorld->values[0][0] * modelVert->x >> 8) + (matWorld->values[0][1] * modelVert->y >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->y * matWorld->values[1][1] >> 8)
                                                + (modelVert->z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                    vertex->z = matWorld->values[2][3] + ((matWorld->values[2][2] * modelVert->z) >> 8)
                                                + ((matWorld->values[2][0] * modelVert->x >> 8) + (matWorld->values[2][1] * modelVert->y >> 8));

                                    // MiSTer T2-E: matNormals==NULL branch — zero normals inline
                                    // (replaces Prepare3DScene memset). See Scene3D.hpp comment.
                                    vertex->nx    = 0;
                                    vertex->ny    = 0;
                                    vertex->nz    = 0;
                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[0][1] >> 8) + (matWorld->values[0][0] * modelVert->x >> 8);
                                    vertex->y = matWorld->values[1][3] + (matWorld->values[1][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[1][1] >> 8) + (matWorld->values[1][0] * modelVert->x >> 8);
                                    vertex->z = matWorld->values[2][3] + (modelVert->x * matWorld->values[2][0] >> 8)
                                                + (modelVert->y * matWorld->values[2][1] >> 8) + (matWorld->values[2][2] * modelVert->z >> 8);

                                    vertex->nx = (matNormals->values[0][0] * modelVert->nx >> 8) + (modelVert->ny * matNormals->values[0][1] >> 8)
                                                 + (matNormals->values[0][2] * modelVert->nz >> 8);
                                    vertex->ny = (matNormals->values[1][0] * modelVert->nx >> 8) + (modelVert->ny * matNormals->values[1][1] >> 8)
                                                 + (matNormals->values[1][2] * modelVert->nz >> 8);
                                    vertex->nz =
                                        ((matNormals->values[2][2] * modelVert->nz) >> 8)
                                        + ((modelVert->ny * matNormals->values[2][1] >> 8) + (matNormals->values[2][0] * modelVert->nx >> 8));

                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *modelVert = &mdl->vertices[indices[i]];
                                    Color *modelColor      = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex  = &scn->vertices[vertID++];

                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][0] * modelVert->x >> 8)
                                                + (modelVert->y * matWorld->values[0][1] >> 8) + (modelVert->z * matWorld->values[0][2] >> 8);
                                    vertex->y = matWorld->values[1][3] + (modelVert->z * matWorld->values[1][2] >> 8)
                                                + (matWorld->values[1][0] * modelVert->x >> 8) + (modelVert->y * matWorld->values[1][1] >> 8);
                                    vertex->z = matWorld->values[2][3] + (matWorld->values[2][2] * modelVert->z >> 8)
                                                + (modelVert->y * matWorld->values[2][1] >> 8) + (modelVert->x * matWorld->values[2][0] >> 8);

                                    // MiSTer T2-E: matNormals==NULL branch — zero normals inline
                                    // (replaces Prepare3DScene memset). See Scene3D.hpp comment.
                                    vertex->nx    = 0;
                                    vertex->ny    = 0;
                                    vertex->nz    = 0;
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
}
void RSDK::AddMeshFrameToScene(uint16 modelFrames, uint16 sceneIndex, Animator *animator, uint8 drawMode, Matrix *matWorld, Matrix *matNormals,
                               color color)
{
    if (modelFrames < MODEL_COUNT && sceneIndex < SCENE3D_COUNT) {
        if (matWorld && animator) {
            Model *mdl            = &modelList[modelFrames];
            Scene3D *scn          = &scene3DList[sceneIndex];
            uint16 *indices       = mdl->indices;
            int32 vertID          = scn->vertexCount;
            int32 faceBase        = scn->faceCount;
            int32 refBase         = scn->faceRefCount;
            uint8 *faceVertCounts = &scn->faceVertCounts[faceBase];
            int32 indCnt          = mdl->indexCount;
            int32 faceCount       = mdl->faceVertCount ? indCnt / mdl->faceVertCount : 0;

            int32 nextFrame = animator->frameID + 1;
            if (nextFrame >= animator->frameCount)
                nextFrame = animator->loopIndex;
            int32 frameOffset     = animator->frameID * mdl->vertCount;
            int32 nextFrameOffset = nextFrame * mdl->vertCount;

#if defined(RSDK_USE_MISTER)
            if (Scene3D_AddMeshFrameIndexed(mdl, scn, vertID, faceBase, refBase, matWorld, matNormals, frameOffset, nextFrameOffset,
                                            animator->timer, color, drawMode)) {
                scn->vertexCount += mdl->vertCount;
                scn->faceRefCount += indCnt;
                scn->drawMode = drawMode;
                scn->faceCount += faceCount;
                return;
            }
#endif
            if (scn->vertLimit - vertID >= indCnt && scn->vertLimit - faceBase >= faceCount) {
                scn->vertexCount += indCnt;
                scn->drawMode = drawMode;
                scn->faceCount += faceCount;
#if defined(RSDK_USE_MISTER)
                int32 sortDepth = Scene3D_UseCoarseModelDepth(drawMode) ? Scene3D_CoarseModelDepth(matWorld) : 0;
                Scene3D_SetFaceLayout(scn, faceBase, vertID, faceCount, mdl->faceVertCount, false, sortDepth);
#endif
                int32 i           = 0;
                int32 f           = 0;
                int32 interpolate = animator->timer;
                switch (mdl->flags) {
                    default:
                    case MODEL_NOFLAGS:
                    case MODEL_USECOLOURS:
#if defined(RSDK_USE_MISTER) && defined(__ARM_NEON__)
                        {
                            int32 nFaces = mdl->indexCount / mdl->faceVertCount;
                            memset(faceVertCounts, mdl->faceVertCount, nFaces);

                            const int32_t m00 = matWorld->values[0][0], m01 = matWorld->values[0][1], m02 = matWorld->values[0][2];
                            const int32_t m10 = matWorld->values[1][0], m11 = matWorld->values[1][1], m12 = matWorld->values[1][2];
                            const int32_t m20 = matWorld->values[2][0], m21 = matWorld->values[2][1], m22 = matWorld->values[2][2];
                            const int32x4_t vT0   = vdupq_n_s32(matWorld->values[0][3]);
                            const int32x4_t vT1   = vdupq_n_s32(matWorld->values[1][3]);
                            const int32x4_t vT2   = vdupq_n_s32(matWorld->values[2][3]);
                            const int32x4_t vInterp = vdupq_n_s32(interpolate);

                            int32 total    = mdl->indexCount;
                            int32 neon_end = total & ~3;

                            for (i = 0; i < neon_end; i += 4) {
                                const ModelVertex *f0 = &mdl->vertices[frameOffset + indices[i + 0]];
                                const ModelVertex *f1 = &mdl->vertices[frameOffset + indices[i + 1]];
                                const ModelVertex *f2 = &mdl->vertices[frameOffset + indices[i + 2]];
                                const ModelVertex *f3 = &mdl->vertices[frameOffset + indices[i + 3]];
                                const ModelVertex *n0 = &mdl->vertices[nextFrameOffset + indices[i + 0]];
                                const ModelVertex *n1 = &mdl->vertices[nextFrameOffset + indices[i + 1]];
                                const ModelVertex *n2 = &mdl->vertices[nextFrameOffset + indices[i + 2]];
                                const ModelVertex *n3 = &mdl->vertices[nextFrameOffset + indices[i + 3]];

                                // Gather current frame x,y,z
                                int32x4_t vfx = vdupq_n_s32(f0->x); vfx = vsetq_lane_s32(f1->x, vfx, 1); vfx = vsetq_lane_s32(f2->x, vfx, 2); vfx = vsetq_lane_s32(f3->x, vfx, 3);
                                int32x4_t vfy = vdupq_n_s32(f0->y); vfy = vsetq_lane_s32(f1->y, vfy, 1); vfy = vsetq_lane_s32(f2->y, vfy, 2); vfy = vsetq_lane_s32(f3->y, vfy, 3);
                                int32x4_t vfz = vdupq_n_s32(f0->z); vfz = vsetq_lane_s32(f1->z, vfz, 1); vfz = vsetq_lane_s32(f2->z, vfz, 2); vfz = vsetq_lane_s32(f3->z, vfz, 3);
                                // Gather next frame x,y,z
                                int32x4_t vnx = vdupq_n_s32(n0->x); vnx = vsetq_lane_s32(n1->x, vnx, 1); vnx = vsetq_lane_s32(n2->x, vnx, 2); vnx = vsetq_lane_s32(n3->x, vnx, 3);
                                int32x4_t vny = vdupq_n_s32(n0->y); vny = vsetq_lane_s32(n1->y, vny, 1); vny = vsetq_lane_s32(n2->y, vny, 2); vny = vsetq_lane_s32(n3->y, vny, 3);
                                int32x4_t vnz = vdupq_n_s32(n0->z); vnz = vsetq_lane_s32(n1->z, vnz, 1); vnz = vsetq_lane_s32(n2->z, vnz, 2); vnz = vsetq_lane_s32(n3->z, vnz, 3);

                                // Interpolate: x = fx + ((interp * (nx - fx)) >> 8)
                                int32x4_t vx = vaddq_s32(vfx, vshrq_n_s32(vmulq_s32(vsubq_s32(vnx, vfx), vInterp), 8));
                                int32x4_t vy = vaddq_s32(vfy, vshrq_n_s32(vmulq_s32(vsubq_s32(vny, vfy), vInterp), 8));
                                int32x4_t vz = vaddq_s32(vfz, vshrq_n_s32(vmulq_s32(vsubq_s32(vnz, vfz), vInterp), 8));

                                int32x4_t ox =
                                    vaddq_s32(vT0, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m00), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m01), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m02), 8))));
                                int32x4_t oy =
                                    vaddq_s32(vT1, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m10), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m11), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m12), 8))));
                                int32x4_t oz =
                                    vaddq_s32(vT2, vaddq_s32(vshrq_n_s32(vmulq_n_s32(vx, m20), 8),
                                                              vaddq_s32(vshrq_n_s32(vmulq_n_s32(vy, m21), 8),
                                                                        vshrq_n_s32(vmulq_n_s32(vz, m22), 8))));

                                Scene3DVertex *o0 = &scn->vertices[vertID + 0];
                                Scene3DVertex *o1 = &scn->vertices[vertID + 1];
                                Scene3DVertex *o2 = &scn->vertices[vertID + 2];
                                Scene3DVertex *o3 = &scn->vertices[vertID + 3];

                                o0->x = vgetq_lane_s32(ox, 0); o0->y = vgetq_lane_s32(oy, 0); o0->z = vgetq_lane_s32(oz, 0);
                                o1->x = vgetq_lane_s32(ox, 1); o1->y = vgetq_lane_s32(oy, 1); o1->z = vgetq_lane_s32(oz, 1);
                                o2->x = vgetq_lane_s32(ox, 2); o2->y = vgetq_lane_s32(oy, 2); o2->z = vgetq_lane_s32(oz, 2);
                                o3->x = vgetq_lane_s32(ox, 3); o3->y = vgetq_lane_s32(oy, 3); o3->z = vgetq_lane_s32(oz, 3);
                                o0->nx = 0; o0->ny = 0; o0->nz = 0; o0->color = color;
                                o1->nx = 0; o1->ny = 0; o1->nz = 0; o1->color = color;
                                o2->nx = 0; o2->ny = 0; o2->nz = 0; o2->color = color;
                                o3->nx = 0; o3->ny = 0; o3->nz = 0; o3->color = color;
                                vertID += 4;
                            }
                            for (; i < total; ++i) {
                                const ModelVertex *fv = &mdl->vertices[frameOffset + indices[i]];
                                const ModelVertex *nv = &mdl->vertices[nextFrameOffset + indices[i]];
                                int32 x = fv->x + ((interpolate * (nv->x - fv->x)) >> 8);
                                int32 y = fv->y + ((interpolate * (nv->y - fv->y)) >> 8);
                                int32 z = fv->z + ((interpolate * (nv->z - fv->z)) >> 8);
                                Scene3DVertex *sv = &scn->vertices[vertID++];
                                sv->x = matWorld->values[0][3] + (z * m02 >> 8) + (m00 * x >> 8) + (m01 * y >> 8);
                                sv->y = matWorld->values[1][3] + (y * m11 >> 8) + (z * m12 >> 8) + (m10 * x >> 8);
                                sv->z = matWorld->values[2][3] + (x * m20 >> 8) + (m22 * z >> 8) + (m21 * y >> 8);
                                sv->nx = 0; sv->ny = 0; sv->nz = 0; sv->color = color;
                            }
                        }
#else
                        for (; i < mdl->indexCount;) {
                            faceVertCounts[f++] = mdl->faceVertCount;

                            for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                i++;
                                Scene3DVertex *vertex = &scn->vertices[vertID++];
                                vertex->x             = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (matWorld->values[0][0] * x >> 8)
                                            + (matWorld->values[0][1] * y >> 8);
                                vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (z * matWorld->values[1][2] >> 8)
                                            + (matWorld->values[1][0] * x >> 8);
                                vertex->z = matWorld->values[2][3] + ((x * matWorld->values[2][0]) >> 8)
                                            + ((matWorld->values[2][2] * z >> 8) + (matWorld->values[2][1] * y >> 8));
                                vertex->nx    = 0;
                                vertex->ny    = 0;
                                vertex->nz    = 0;
                                vertex->color = color;
                            }
                        }
#endif
                        break;

                    case MODEL_USENORMALS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);
                                    i++;

                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (x * matWorld->values[0][0] >> 8)
                                                + (y * matWorld->values[0][1] >> 8);
                                    vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (matWorld->values[1][0] * x >> 8)
                                                + (z * matWorld->values[1][2] >> 8);
                                    vertex->z = matWorld->values[2][3] + (x * matWorld->values[2][0] >> 8) + (matWorld->values[2][2] * z >> 8)
                                                + (matWorld->values[2][1] * y >> 8);
                                    vertex->nx = (nz * matNormals->values[0][2] >> 8) + (nx * matNormals->values[0][0] >> 8)
                                                 + (matNormals->values[0][1] * ny >> 8);
                                    vertex->ny = (ny * matNormals->values[1][1] >> 8) + (nz * matNormals->values[1][2] >> 8)
                                                 + (nx * matNormals->values[1][0] >> 8);
                                    vertex->nz = ((ny * matNormals->values[2][1]) >> 8)
                                                 + ((matNormals->values[2][0] * nx >> 8) + (nz * matNormals->values[2][2] >> 8));
                                    vertex->color = color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    i++;
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (z * matWorld->values[0][2] >> 8) + (matWorld->values[0][0] * x >> 8)
                                                + (matWorld->values[0][1] * y >> 8);
                                    vertex->y = matWorld->values[1][3] + (y * matWorld->values[1][1] >> 8) + (z * matWorld->values[1][2] >> 8)
                                                + (matWorld->values[1][0] * x >> 8);
                                    vertex->z = matWorld->values[2][3] + ((matWorld->values[2][2] * z) >> 8)
                                                + ((matWorld->values[2][0] * x >> 8) + (matWorld->values[2][1] * y >> 8));
                                    // MiSTer T2-E: matNormals==NULL — zero normals inline.
                                    vertex->nx    = 0;
                                    vertex->ny    = 0;
                                    vertex->nz    = 0;
                                    vertex->color = color;
                                }
                            }
                        }
                        break;

                    case MODEL_USENORMALS | MODEL_USECOLOURS:
                        if (matNormals) {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    int32 nx                   = frameVert->nx + ((interpolate * (nextFrameVert->nx - frameVert->nx)) >> 8);
                                    int32 ny                   = frameVert->ny + ((interpolate * (nextFrameVert->ny - frameVert->ny)) >> 8);
                                    int32 nz                   = frameVert->nz + ((interpolate * (nextFrameVert->nz - frameVert->nz)) >> 8);

                                    Color *modelColor     = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][2] * z >> 8) + (y * matWorld->values[0][1] >> 8)
                                                + (matWorld->values[0][0] * x >> 8);
                                    vertex->y = matWorld->values[1][3] + (matWorld->values[1][2] * z >> 8) + (y * matWorld->values[1][1] >> 8)
                                                + (matWorld->values[1][0] * x >> 8);
                                    vertex->z = matWorld->values[2][3] + (x * matWorld->values[2][0] >> 8) + (y * matWorld->values[2][1] >> 8)
                                                + (matWorld->values[2][2] * z >> 8);
                                    vertex->nx = (matNormals->values[0][0] * nx >> 8) + (ny * matNormals->values[0][1] >> 8)
                                                 + (matNormals->values[0][2] * nz >> 8);
                                    vertex->ny = (matNormals->values[1][0] * nx >> 8) + (ny * matNormals->values[1][1] >> 8)
                                                 + (matNormals->values[1][2] * nz >> 8);
                                    vertex->nz = ((matNormals->values[2][2] * nz) >> 8)
                                                 + ((ny * matNormals->values[2][1] >> 8) + (matNormals->values[2][0] * nx >> 8));
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        else {
                            for (; i < mdl->indexCount;) {
                                faceVertCounts[f++] = mdl->faceVertCount;

                                for (int32 c = 0; c < mdl->faceVertCount; ++c) {
                                    ModelVertex *frameVert     = &mdl->vertices[frameOffset + indices[i]];
                                    ModelVertex *nextFrameVert = &mdl->vertices[nextFrameOffset + indices[i]];
                                    int32 x                    = frameVert->x + ((interpolate * (nextFrameVert->x - frameVert->x)) >> 8);
                                    int32 y                    = frameVert->y + ((interpolate * (nextFrameVert->y - frameVert->y)) >> 8);
                                    int32 z                    = frameVert->z + ((interpolate * (nextFrameVert->z - frameVert->z)) >> 8);
                                    Color *modelColor          = &mdl->colors[indices[i++]];
                                    Scene3DVertex *vertex      = &scn->vertices[vertID++];
                                    vertex->x = matWorld->values[0][3] + (matWorld->values[0][0] * x >> 8) + (y * matWorld->values[0][1] >> 8)
                                                + (z * matWorld->values[0][2] >> 8);
                                    vertex->y = matWorld->values[1][3] + (z * matWorld->values[1][2] >> 8) + (matWorld->values[1][0] * x >> 8)
                                                + (y * matWorld->values[1][1] >> 8);
                                    vertex->z = matWorld->values[2][3] + (matWorld->values[2][2] * z >> 8) + (y * matWorld->values[2][1] >> 8)
                                                + (x * matWorld->values[2][0] >> 8);
                                    // MiSTer T2-E: matNormals==NULL — zero normals inline.
                                    vertex->nx    = 0;
                                    vertex->ny    = 0;
                                    vertex->nz    = 0;
                                    vertex->color = modelColor->color;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
}

void RSDK::Draw3DScene(uint16 sceneID)
{
    if (sceneID < SCENE3D_COUNT) {
        Entity *entity = sceneInfo.entity;
        Scene3D *scn   = &scene3DList[sceneID];
#if defined(RSDK_USE_MISTER)
        uint32_t _d3d_source = s_s3d_draw_source;
        s_s3d_draw_source = 0;
#endif
        if (!scn->faceCount)
            return;

        // Setup face buffer.
        // Each face's depth is an average of the depth of its vertices.
        Scene3DVertex *vertices = scn->vertices;
        Scene3DFace *faceBuffer = scn->faceBuffer;
        uint8 *faceVertCounts = scn->faceVertCounts;

#if defined(RSDK_USE_MISTER)
        uint8_t _d3d_draw_mode = scn->drawMode;
        const bool _d3d_screen_cull = Scene3D_UsesScreenCull(_d3d_draw_mode);
        const bool _d3d_flat_screen = _d3d_draw_mode == S3D_SOLIDCOLOR_SHADED_SCREEN;
        const bool _d3d_coarse_depth = _d3d_flat_screen && _d3d_source == SCENE3D_SOURCE_UFO_DECORATION;
        const int32 _d3d_clip_left   = currentScreen->clipBound_X1;
        const int32 _d3d_clip_right  = currentScreen->clipBound_X2;
        const int32 _d3d_clip_top    = currentScreen->clipBound_Y1;
        const int32 _d3d_clip_bottom = currentScreen->clipBound_Y2;
        const int32 _d3d_center_x    = currentScreen->center.x;
        const int32 _d3d_center_y    = currentScreen->center.y;
        const int32 _d3d_clip_left_fixed   = _d3d_clip_left << 16;
        const int32 _d3d_clip_right_fixed  = _d3d_clip_right << 16;
        const int32 _d3d_clip_top_fixed    = _d3d_clip_top << 16;
        const int32 _d3d_clip_bottom_fixed = _d3d_clip_bottom << 16;
        const int32 _d3d_center_x_fixed    = _d3d_center_x << 16;
        const int32 _d3d_center_y_fixed    = _d3d_center_y << 16;
        const bool _d3d_preproject_screen  = _d3d_flat_screen && scn->faceRefCount;
        const bool _d3d_fast_flat_setup    = _d3d_preproject_screen;
        int32 drawFaceCount = scn->faceCount;
        if (_d3d_preproject_screen)
            Scene3D_ProjectScreenVertices(scn, _d3d_center_x_fixed, _d3d_center_y_fixed, _d3d_fast_flat_setup,
                                          _d3d_clip_left_fixed, _d3d_clip_right_fixed,
                                          _d3d_clip_top_fixed, _d3d_clip_bottom_fixed);
#endif

        {
#if defined(RSDK_USE_MISTER)
        int32 outFaceCount = 0;
#else
        int32 vertIndex = 0;
#endif
        for (int32 i = 0; i < scn->faceCount; ++i) {
            uint8 faceVertCount = *faceVertCounts;
            int32 faceDepth = 0;

#if defined(RSDK_USE_MISTER)
            Scene3DFace *srcFace = &faceBuffer[i];
            if (_d3d_fast_flat_setup && (srcFace->index & SCENE3D_FACE_INDEXED)
                && (_d3d_source == SCENE3D_SOURCE_UFO_DECORATION || (srcFace->flags & SCENE3D_FACE_CACHED_COLOR))) {
                const uint16 *refs = &scn->faceVertexRefs[srcFace->index & SCENE3D_FACE_INDEX_MASK];
                int32 minX = 0x7FFFFFFF;
                int32 minY = 0x7FFFFFFF;
                int32 maxX = -0x7FFFFFFF;
                int32 maxY = -0x7FFFFFFF;
                uint32 outOr = 0;
                uint32 outAnd = 0xFFFFFFFFU;

                for (int32 v = 0; v < faceVertCount; ++v) {
                    Scene3DVertex *faceVert = &scn->vertices[refs[v]];
                    uint32 outCode = faceVert->outCode;
                    int32 sx = faceVert->tx;
                    int32 sy = faceVert->ty;

                    outOr |= outCode;
                    outAnd &= outCode;
                    if (sx < minX) minX = sx;
                    if (sy < minY) minY = sy;
                    if (sx > maxX) maxX = sx;
                    if (sy > maxY) maxY = sy;
                }

                bool cullFace = (outOr & SCENE3D_OUT_NEAR) || (outAnd & SCENE3D_OUT_SCREEN_MASK)
                                || maxX < _d3d_clip_left_fixed || minX > _d3d_clip_right_fixed
                                || maxY < _d3d_clip_top_fixed || minY > _d3d_clip_bottom_fixed
                                || (maxX - minX) < 0x10000 || (maxY - minY) < 0x10000;

                if (!cullFace && _d3d_source != SCENE3D_SOURCE_UFO_DECORATION && _d3d_source != SCENE3D_SOURCE_UFO_PLAYER) {
                    Scene3DVertex *facePtrs[4];
                    for (int32 v = 0; v < faceVertCount; ++v)
                        facePtrs[v] = &scn->vertices[refs[v]];

                    if (Scene3D_ShouldCullBackfacePtrs(facePtrs, faceVertCount)) {
                        cullFace = true;
                    }
                }

                if (!cullFace) {
                    uint32 faceColor = srcFace->color;
                    uint16 faceColor16 = srcFace->color16;
                    if (!(srcFace->flags & SCENE3D_FACE_CACHED_COLOR)) {
                        faceColor = scn->vertices[refs[0]].color;
                        int32 r = (faceColor >> 16) & 0xFF;
                        int32 g = (faceColor >> 8) & 0xFF;
                        int32 b = faceColor & 0xFF;
                        faceColor16 = rgb32To16_B[b] | rgb32To16_G[g] | rgb32To16_R[r];
                    }

                    int32 topScreen = FROM_FIXED(minY);
                    int32 bottomScreen = FROM_FIXED(maxY);
                    if (topScreen < _d3d_clip_top)
                        topScreen = _d3d_clip_top;
                    if (topScreen > _d3d_clip_bottom)
                        topScreen = _d3d_clip_bottom;
                    if (bottomScreen < _d3d_clip_top)
                        bottomScreen = _d3d_clip_top;
                    if (bottomScreen > _d3d_clip_bottom)
                        bottomScreen = _d3d_clip_bottom;

                    Scene3DFace *outFace = &scn->faceBuffer[outFaceCount++];
                    outFace->depth = _d3d_coarse_depth ? srcFace->depth : Scene3D_FaceDepthRefs(scn, refs, faceVertCount);
                    outFace->index = srcFace->index;
                    outFace->color = faceColor;
                    outFace->color16 = faceColor16;
                    outFace->top = (int16)topScreen;
                    outFace->bottom = (int16)bottomScreen;
                    outFace->count = faceVertCount;
                    outFace->flags = srcFace->flags;
                }

                ++faceVertCounts;
                continue;
            }

            Scene3DVertex *facePtrs[4];
            if (srcFace->index & SCENE3D_FACE_INDEXED) {
                const uint16 *refs = &scn->faceVertexRefs[srcFace->index & SCENE3D_FACE_INDEX_MASK];
                for (int32 v = 0; v < faceVertCount; ++v)
                    facePtrs[v] = &scn->vertices[refs[v]];
            }
            else {
                Scene3DVertex *faceVerts = &scn->vertices[srcFace->index];
                for (int32 v = 0; v < faceVertCount; ++v)
                    facePtrs[v] = &faceVerts[v];
            }

            if (!_d3d_screen_cull)
                faceDepth = Scene3D_FaceDepthPtrs(facePtrs, faceVertCount);
#else
            Scene3DVertex *faceVerts = vertices;
            switch (faceVertCount) {
                default:
                case 1:
                    faceDepth = vertices[0].z;
                    vertices += faceVertCount;
                    break;

                case 2:
                    faceDepth = vertices[0].z >> 1;
                    faceDepth += vertices[1].z >> 1;
                    vertices += 2;
                    break;

                case 3:
                    faceDepth = vertices[0].z >> 1;
                    faceDepth = (faceDepth + (vertices[1].z >> 1)) >> 1;
                    faceDepth += vertices[2].z >> 1;
                    vertices += 3;
                    break;

                case 4:
                    faceDepth = vertices[0].z >> 2;
                    faceDepth += vertices[1].z >> 2;
                    faceDepth += vertices[2].z >> 2;
                    faceDepth += vertices[3].z >> 2;
                    vertices += 4;
                    break;
            }
#endif

#if defined(RSDK_USE_MISTER)
            bool cullFace = false;
            uint32 faceColor = 0;
            uint16 faceColor16 = 0;
            int16 faceTop = 0;
            int16 faceBottom = 0;
            if (_d3d_screen_cull) {
                if (_d3d_flat_screen) {
                    int32 minX = 0x7FFFFFFF;
                    int32 minY = 0x7FFFFFFF;
                    int32 maxX = -0x7FFFFFFF;
                    int32 maxY = -0x7FFFFFFF;
                    int32 ny = 0;

                    for (int32 v = 0; v < faceVertCount; ++v) {
                        Scene3DVertex *faceVert = facePtrs[v];
                        const int32 z = faceVert->z;
                        if (z < 0x100) {
                            cullFace = true;
                            break;
                        }

                        int32 sx;
                        int32 sy;
                        if (_d3d_preproject_screen) {
                            sx = faceVert->tx;
                            sy = faceVert->ty;
                        }
                        else {
#if defined(__ARM_NEON__)
                            float invZ = 1.0f / (float)z;
                            sx = _d3d_center_x_fixed + (int32)((float)(faceVert->x << scn->projectionX) * invZ * 65536.0f);
                            sy = _d3d_center_y_fixed - (int32)((float)(faceVert->y << scn->projectionY) * invZ * 65536.0f);
#else
                            sx = _d3d_center_x_fixed + ((faceVert->x << scn->projectionX) / z << 16);
                            sy = _d3d_center_y_fixed - ((faceVert->y << scn->projectionY) / z << 16);
#endif
                            faceVert->tx = sx;
                            faceVert->ty = sy;
                        }

                        if (sx < minX) minX = sx;
                        if (sy < minY) minY = sy;
                        if (sx > maxX) maxX = sx;
                        if (sy > maxY) maxY = sy;
                        ny += faceVert->ny;
                    }

                    cullFace = cullFace || maxX < _d3d_clip_left_fixed || minX > _d3d_clip_right_fixed
                               || maxY < _d3d_clip_top_fixed || minY > _d3d_clip_bottom_fixed
                               || (maxX - minX) < 0x10000 || (maxY - minY) < 0x10000;

                    if (!cullFace) {
                        int32 normal    = ny / faceVertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular
                                  + ((int32)((facePtrs[0]->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g = specular
                                  + ((int32)((facePtrs[0]->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b = specular
                                  + ((int32)((facePtrs[0]->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);
                        faceColor = (r << 16) | (g << 8) | (b << 0);
                        faceColor16 = rgb32To16_B[b] | rgb32To16_G[g] | rgb32To16_R[r];

                        int32 topScreen = FROM_FIXED(minY);
                        int32 bottomScreen = FROM_FIXED(maxY);
                        if (topScreen < _d3d_clip_top)
                            topScreen = _d3d_clip_top;
                        if (topScreen > _d3d_clip_bottom)
                            topScreen = _d3d_clip_bottom;
                        if (bottomScreen < _d3d_clip_top)
                            bottomScreen = _d3d_clip_top;
                        if (bottomScreen > _d3d_clip_bottom)
                            bottomScreen = _d3d_clip_bottom;
                        faceTop = (int16)topScreen;
                        faceBottom = (int16)bottomScreen;
                    }
                }
                else {
                    bool allLeft  = true;
                    bool allRight = true;
                    bool allTop   = true;
                    bool allBot   = true;

                    for (int32 v = 0; v < faceVertCount; ++v) {
                        Scene3DVertex *faceVert = facePtrs[v];
                        const int32 z = faceVert->z;
                        if (z < 0x100) {
                            cullFace = true;
                            break;
                        }

                        const int64_t projX = (int64_t)faceVert->x << scn->projectionX;
                        const int64_t projY = (int64_t)faceVert->y << scn->projectionY;
                        if (projX >= (int64_t)(_d3d_clip_left - _d3d_center_x) * z)
                            allLeft = false;
                        if (projX <= (int64_t)(_d3d_clip_right - _d3d_center_x) * z)
                            allRight = false;
                        if (projY <= (int64_t)(_d3d_center_y - _d3d_clip_top) * z)
                            allTop = false;
                        if (projY >= (int64_t)(_d3d_center_y - _d3d_clip_bottom) * z)
                            allBot = false;
                    }

                    cullFace = cullFace || allLeft || allRight || allTop || allBot;
                }

                if (!cullFace && _d3d_source != SCENE3D_SOURCE_UFO_DECORATION && _d3d_source != SCENE3D_SOURCE_UFO_PLAYER
                    && Scene3D_ShouldCullBackfacePtrs(facePtrs, faceVertCount)) {
                    cullFace = true;
                }
            }

            if (!cullFace) {
                if (_d3d_screen_cull)
                    faceDepth = _d3d_coarse_depth ? srcFace->depth : Scene3D_FaceDepthPtrs(facePtrs, faceVertCount);

                Scene3DFace *outFace = &scn->faceBuffer[outFaceCount++];
                outFace->depth = faceDepth;
                outFace->index = srcFace->index;
                outFace->color = faceColor;
                outFace->color16 = faceColor16;
                outFace->top = faceTop;
                outFace->bottom = faceBottom;
                outFace->count = faceVertCount;
                outFace->flags = srcFace->flags;
            }
#else
            faceBuffer->depth = faceDepth;
            faceBuffer->index = vertIndex;
            ++faceBuffer;
#endif
#if !defined(RSDK_USE_MISTER)
            vertIndex += faceVertCount;
#endif
            ++faceVertCounts;
        }

#if defined(RSDK_USE_MISTER)
        if (_d3d_screen_cull)
            drawFaceCount = outFaceCount;
#endif
        }
        // Sort the face buffer. This is needed so that the faces don't overlap each other incorrectly when they're rendered.
        // This is an insertion sort, taken from here:
        // https://web.archive.org/web/20110108233032/http://rosettacode.org/wiki/Sorting_algorithms/Insertion_sort#C

#if defined(RSDK_USE_MISTER)
        Scene3D_SortFaces(scn->faceBuffer, drawFaceCount);
#else
        std::sort(scn->faceBuffer, scn->faceBuffer + scn->faceCount,
            [](const Scene3DFace &a, const Scene3DFace &b){ return a.depth > b.depth; });
#endif

        // Finally, display the faces.

        uint8 *vertCnt = scn->faceVertCounts;
        Vector2 vertPos[4];
        uint32 vertClrs[4];

        switch (scn->drawMode) {
            default: break;

            case S3D_WIREFRAME:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, drawVert[0].color,
                                 entity->alpha, entity->inkEffect, false);
                    }
                    DrawLine(drawVert[0].x << 8, drawVert[0].y << 8, drawVert[*vertCnt - 1].x << 8, drawVert[*vertCnt - 1].y << 8, drawVert[0].color,
                             entity->alpha, entity->inkEffect, false);
                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    for (int32 v = 0; v < *vertCnt; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);
                    }
                    DrawFace(vertPos, *vertCnt, (drawVert->color >> 16) & 0xFF, (drawVert->color >> 8) & 0xFF, (drawVert->color >> 0) & 0xFF,
                             entity->alpha, entity->inkEffect);
                    vertCnt++;
                }
                break;

            // Might have been reserved for textures?
            // not sure about this, just a guess based on tex coords existing in the model format spec
            case S3D_UNUSED_1: break;
            case S3D_UNUSED_2: break;

            case S3D_WIREFRAME_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny1 = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny1 += drawVert[v].ny;
                    }

                    int32 normal    = ny1 / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                    specular = normalVal >> 6 >> scn->specularIntensityY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                    specular = normalVal >> 6 >> scn->specularIntensityZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    for (int32 v = 0; v < vertCount - 1; ++v) {
                        DrawLine(drawVert[v + 0].x << 8, drawVert[v + 0].y << 8, drawVert[v + 1].x << 8, drawVert[v + 1].y << 8, color, entity->alpha,
                                 entity->inkEffect, false);
                    }
                    DrawLine(drawVert[vertCount - 1].x << 8, drawVert[vertCount - 1].y << 8, drawVert[0].x << 8, drawVert[0].y << 8, color,
                             entity->alpha, entity->inkEffect, false);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 ny = 0;
                    for (int32 v = 0; v < vertCount; ++v) {
                        ny += drawVert[v].ny;
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);
                    }

                    int32 normal    = ny / vertCount;
                    int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                    int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                    specular       = CLAMP(specular, 0x00, 0xFF);
                    int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                    specular = normalVal >> 6 >> scn->specularIntensityY;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                    specular = normalVal >> 6 >> scn->specularIntensityZ;
                    specular = CLAMP(specular, 0x00, 0xFF);
                    int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                    r = CLAMP(r, 0x00, 0xFF);
                    g = CLAMP(g, 0x00, 0xFF);
                    b = CLAMP(b, 0x00, 0xFF);

                    uint32 color = (r << 16) | (g << 8) | (b << 0);

                    drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    DrawFace(vertPos, *vertCnt, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    for (int32 v = 0; v < vertCount; ++v) {
                        vertPos[v].x = (drawVert[v].x << 8) - (currentScreen->position.x << 16);
                        vertPos[v].y = (drawVert[v].y << 8) - (currentScreen->position.y << 16);

                        int32 normal    = drawVert[v].ny;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = (normalVal >> 6) >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = (normalVal >> 6) >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = (normalVal >> 6) >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        vertClrs[v] = (r << 16) | (g << 8) | (b << 0);
                    }

                    DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);

                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];

                    int32 v = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = currentScreen->center.x + (drawVert[v].x << scn->projectionX) / vertZ;
                            vertPos[v].y = currentScreen->center.y - (drawVert[v].y << scn->projectionY) / vertZ;
                        }
                    }

                    if (v < 0xFF) {
                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, drawVert[0].color, entity->alpha,
                                     entity->inkEffect, true);
                        }
                        DrawLine(vertPos[0].x, vertPos[0].y, vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, drawVert[0].color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                        }
                    }

                    if (v < 0xFF) {
                        DrawFace(vertPos, *vertCnt, (drawVert[0].color >> 16) & 0xFF, (drawVert[0].color >> 8) & 0xFF,
                                 (drawVert[0].color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }
                    vertCnt++;
                }
                break;

            case S3D_WIREFRAME_SHADED_SCREEN:
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v   = 0;
                    int32 ny1 = 0;
                    for (; v < *vertCnt && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = currentScreen->center.x + (drawVert[v].x << scn->projectionX) / vertZ;
                            vertPos[v].y = currentScreen->center.y - (drawVert[v].y << scn->projectionY) / vertZ;
                            ny1 += drawVert[v].ny;
                        }
                    }

                    if (v < 0xFF) {
                        int32 normal    = ny1 / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        for (int32 v = 0; v < *vertCnt - 1; ++v) {
                            DrawLine(vertPos[v + 0].x, vertPos[v + 0].y, vertPos[v + 1].x, vertPos[v + 1].y, color, entity->alpha, entity->inkEffect,
                                     true);
                        }
                        DrawLine(vertPos[*vertCnt - 1].x, vertPos[*vertCnt - 1].y, vertPos[0].x, vertPos[0].y, color, entity->alpha,
                                 entity->inkEffect, true);
                    }

                    vertCnt++;
                }
                break;

            case S3D_SOLIDCOLOR_SHADED_SCREEN:
#if defined(RSDK_USE_MISTER)
                {
                const bool fastFlatFill = entity->inkEffect == INK_NONE || (entity->inkEffect == INK_ALPHA && entity->alpha > 0xFF);
                for (int32 f = 0; f < drawFaceCount; ++f) {
                    Scene3DFace *face       = &scn->faceBuffer[f];
                    int32 vertCount         = face->count;
                    uint32 color            = face->color;
                    const bool indexed      = (face->index & SCENE3D_FACE_INDEXED) != 0;
                    const uint16 *refs      = indexed ? &scn->faceVertexRefs[face->index & SCENE3D_FACE_INDEX_MASK] : NULL;
                    Scene3DVertex *drawVert = indexed ? NULL : &scn->vertices[face->index];

                    if (fastFlatFill) {
                        if (indexed)
                            Scene3D_DrawFaceColor16Indexed(scn, refs, vertCount, face->top, face->bottom, face->color16);
                        else
                            Scene3D_DrawFaceColor16(drawVert, vertCount, face->top, face->bottom, face->color16);
                    }
                    else {
                        if (indexed) {
                            for (int32 v = 0; v < vertCount; ++v) {
                                Scene3DVertex *vert = &scn->vertices[refs[v]];
                                vertPos[v].x = vert->tx;
                                vertPos[v].y = vert->ty;
                            }
                        }
                        else {
                            for (int32 v = 0; v < vertCount; ++v) {
                                vertPos[v].x = drawVert[v].tx;
                                vertPos[v].y = drawVert[v].ty;
                            }
                        }

                        DrawFace(vertPos, vertCount, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }
                }
                }
#else
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;

                    int32 v  = 0;
                    int32 ny = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                            ny += drawVert[v].ny;
                        }
                    }

                    if (v < 0xFF) {
                        int32 normal    = ny / vertCount;
                        int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                        int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                        specular       = CLAMP(specular, 0x00, 0xFF);
                        int32 r = specular + ((int32)((drawVert[0].color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                        specular = normalVal >> 6 >> scn->specularIntensityY;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 g  = specular + ((int32)((drawVert[0].color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                        specular = normalVal >> 6 >> scn->specularIntensityZ;
                        specular = CLAMP(specular, 0x00, 0xFF);
                        int32 b  = specular + ((int32)((drawVert[0].color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                        r = CLAMP(r, 0x00, 0xFF);
                        g = CLAMP(g, 0x00, 0xFF);
                        b = CLAMP(b, 0x00, 0xFF);

                        uint32 color = (r << 16) | (g << 8) | (b << 0);

                        drawVert = &scn->vertices[scn->faceBuffer[f].index];
                        DrawFace(vertPos, vertCount, (color >> 16) & 0xFF, (color >> 8) & 0xFF, (color >> 0) & 0xFF, entity->alpha, entity->inkEffect);
                    }

                    vertCnt++;
                }
#endif
                break;

            case S3D_SOLIDCOLOR_SHADED_BLENDED_SCREEN:
#if defined(RSDK_USE_MISTER)
                for (int32 f = 0; f < drawFaceCount; ++f) {
                    Scene3DFace *face = &scn->faceBuffer[f];
                    int32 vertCount = face->count;
                    const bool indexed = (face->index & SCENE3D_FACE_INDEXED) != 0;
                    const uint16 *refs = indexed ? &scn->faceVertexRefs[face->index & SCENE3D_FACE_INDEX_MASK] : NULL;
                    Scene3DVertex *drawVert = indexed ? NULL : &scn->vertices[face->index];
#else
                for (int32 f = 0; f < scn->faceCount; ++f) {
                    Scene3DVertex *drawVert = &scn->vertices[scn->faceBuffer[f].index];
                    int32 vertCount         = *vertCnt;
                    // Friend's diff added an indexed-vs-flat shared loop tail
                    // that references `indexed` / `refs` regardless of the
                    // RSDK_USE_MISTER guard. Provide stubs for non-MiSTer
                    // builds so the tail compiles; engine still walks
                    // `drawVert[c]` when indexed=false.
                    const bool indexed     = false;
                    const uint16 *refs     = NULL;
#endif

#if defined(RSDK_USE_MISTER)
                    int32 v = 0;
                    bool cullFace = false;
                    int32 minX = 0x7FFFFFFF;
                    int32 minY = 0x7FFFFFFF;
                    int32 maxX = -0x7FFFFFFF;
                    int32 maxY = -0x7FFFFFFF;

                    for (; v < vertCount && v < 0xFF; ++v) {
                        Scene3DVertex *vert = indexed ? &scn->vertices[refs[v]] : &drawVert[v];
                        int32 vertZ = vert->z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                            break;
                        }

#if defined(__ARM_NEON__)
                        // VFP reciprocal: one float div shared for both x and y projections
                        float inv_z = 1.0f / (float)vertZ;
                        vertPos[v].x = _d3d_center_x_fixed
                                       + (int32)((float)(vert->x << scn->projectionX) * inv_z * 65536.0f);
                        vertPos[v].y = _d3d_center_y_fixed
                                       - (int32)((float)(vert->y << scn->projectionY) * inv_z * 65536.0f);
#else
                        vertPos[v].x = _d3d_center_x_fixed + ((vert->x << scn->projectionX) / vertZ << 16);
                        vertPos[v].y = _d3d_center_y_fixed - ((vert->y << scn->projectionY) / vertZ << 16);
#endif

                        if (vertPos[v].x < minX) minX = vertPos[v].x;
                        if (vertPos[v].y < minY) minY = vertPos[v].y;
                        if (vertPos[v].x > maxX) maxX = vertPos[v].x;
                        if (vertPos[v].y > maxY) maxY = vertPos[v].y;
                    }

                    cullFace = v >= 0xFF || maxX < _d3d_clip_left_fixed || minX > _d3d_clip_right_fixed
                               || maxY < _d3d_clip_top_fixed || minY > _d3d_clip_bottom_fixed
                               || (maxX - minX) < 0x10000 || (maxY - minY) < 0x10000;

                    if (cullFace) {
                        continue;
                    }
#else
                    int32 v = 0;
                    for (; v < vertCount && v < 0xFF; ++v) {
                        int32 vertZ = drawVert[v].z;
                        if (vertZ < 0x100) {
                            v = 0xFF;
                        }
                        else {
                            vertPos[v].x = (currentScreen->center.x << 16) + ((drawVert[v].x << scn->projectionX) / vertZ << 16);
                            vertPos[v].y = (currentScreen->center.y << 16) - ((drawVert[v].y << scn->projectionY) / vertZ << 16);
                        }
                    }
#endif

                    if (v < 0xFF) {
                        for (int32 c = 0; c < vertCount; ++c) {
                            Scene3DVertex *vert = indexed ? &scn->vertices[refs[c]] : &drawVert[c];
                            int32 normal    = vert->ny;
                            int32 normalVal = (normal >> 2) * (abs(normal) >> 2);

                            int32 specular = normalVal >> 6 >> scn->specularIntensityX;
                            specular       = CLAMP(specular, 0x00, 0xFF);
                            int32 r =
                                specular + ((int32)((vert->color >> 16) & 0xFF) * ((normal >> 10) + scn->diffuseX) >> scn->diffuseIntensityX);

                            specular = normalVal >> 6 >> scn->specularIntensityY;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 g =
                                specular + ((int32)((vert->color >> 8) & 0xFF) * ((normal >> 10) + scn->diffuseY) >> scn->diffuseIntensityY);

                            specular = normalVal >> 6 >> scn->specularIntensityZ;
                            specular = CLAMP(specular, 0x00, 0xFF);
                            int32 b =
                                specular + ((int32)((vert->color >> 0) & 0xFF) * ((normal >> 10) + scn->diffuseZ) >> scn->diffuseIntensityZ);

                            r = CLAMP(r, 0x00, 0xFF);
                            g = CLAMP(g, 0x00, 0xFF);
                            b = CLAMP(b, 0x00, 0xFF);

                            vertClrs[c] = (r << 16) | (g << 8) | (b << 0);
                        }
#if defined(RSDK_USE_MISTER)
                        DrawBlendedFace(vertPos, vertClrs, vertCount, entity->alpha, entity->inkEffect);
#else
                        DrawBlendedFace(vertPos, vertClrs, *vertCnt, entity->alpha, entity->inkEffect);
#endif
                    }

#if !defined(RSDK_USE_MISTER)
                    vertCnt++;
#endif
                }
                break;
        }
    }
}
