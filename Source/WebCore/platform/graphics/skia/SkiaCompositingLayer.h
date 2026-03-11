/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(SKIA)
#include "Color.h"
#include "CoordinatedBackingStoreProxy.h"
#include "FloatPoint.h"
#include "FloatPoint3D.h"
#include "FloatRect.h"
#include "FloatRoundedRect.h"
#include "SkiaCompositingLayerOverlapRegions.h"
#include "TextureMapperAnimation.h"
#include "TransformationMatrix.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkM44.h>
#include <skia/effects/SkImageFilters.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/MonotonicTime.h>
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {
class CoordinatedAnimatedBackingStoreClient;
class CoordinatedBackingStore;
class CoordinatedImageBackingStore;
class CoordinatedPlatformLayerBuffer;
class FilterOperations;

class SkiaCompositingLayer final : public RefCountedAndCanMakeWeakPtr<SkiaCompositingLayer> {
    WTF_MAKE_TZONE_ALLOCATED(SkiaCompositingLayer);
public:
    static Ref<SkiaCompositingLayer> create();
    ~SkiaCompositingLayer();

    void setSize(const FloatSize& size) { m_size = size; }
    void setPosition(const FloatPoint& point) { m_position = point; }
    void setAnchorPoint(const FloatPoint3D& point) { m_anchorPoint = point; }
    void setBoundsOrigin(const FloatPoint& point) { m_boundsOrigin = point; }
    void setTransform(const TransformationMatrix& matrix) { m_transform = matrix; }
    void setChildrenTransform(const TransformationMatrix& matrix) { m_childrenTransform = matrix; }
    void setPreserves3D(bool preserves3D) { m_preserves3D = preserves3D; }
    void setBackfaceVisibility(bool visible) { m_backfaceVisibility = visible; }
    void setContentsVisible(bool visible) { m_contentsVisible = visible; }
    void setMasksToBounds(bool masksToBounds) { m_masksToBounds = masksToBounds; }
    void setContentsClippingRect(const FloatRoundedRect& rect) { m_contentsClippingRect = rect; }
    void setContentsRectClipsDescendants(bool clips) { m_contentsRectClipsDescendants = clips; }
    void setOpacity(float opacity) { m_opacity = opacity; }
    void setContentsRect(const FloatRect& rect) { m_contentsRect = rect; }
    void setAnimations(const TextureMapperAnimations& animations) { m_animations = animations; }
    void setContentsTiling(const FloatSize& size, const FloatSize& phase) { m_contentsTiling = { size, phase }; }
    void setMask(RefPtr<SkiaCompositingLayer>&&);
    void setReplica(RefPtr<SkiaCompositingLayer>&&);
    void setFilters(const FilterOperations&);
    void setChildren(Vector<Ref<SkiaCompositingLayer>>&&);

    void setUseBackingStore(bool, CoordinatedAnimatedBackingStoreClient* = nullptr);
    void updateBackingStore(CoordinatedBackingStoreProxy::Update&&, float);
    void setImageBackingStore(CoordinatedImageBackingStore*);
    void setContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&&);
    void setContentsSolidColor(const Color&);

    void setShowDebugBorder(bool showDebugBorder) { m_showDebugBorder = showDebugBorder; }
    void setDebugBorderColor(Color debugBorderColor) { m_debugBorderColor = debugBorderColor; }
    void setDebugBorderWidth(float debugBorderWidth) { m_debugBorderWidth = debugBorderWidth; }
    void setShowRepaintCounter(bool showRepaintCounter) { m_showRepaintCounter = showRepaintCounter; }
    void setRepaintCount(int repaintCount) { m_repaintCount = repaintCount; }

    const TransformationMatrix& toSurfaceTransform() const { return m_transforms.combined; }
    FloatRect effectiveLayerRect() const { return FloatRect({ }, m_size); }

    bool paint(SkCanvas&);

private:
    SkiaCompositingLayer() = default;

    void removeFromParent();
    bool isVisible() const;
    bool isLeafOf3DRenderingContext() const { return !m_preserves3D && (m_parent && m_parent->m_preserves3D); }
    bool isReplica() const { return !!m_replicatedLayer; }
    bool hasVisualContent() const;

    bool computeTransformsAndAnimations(RefPtr<SkiaCompositingLayer>, MonotonicTime);

    struct PaintContext {
        float opacity { 1 };
        bool isMask { false };
        TransformationMatrix accumulatedReplicaTransform;
    };

    void recursivePaint(SkCanvas&, PaintContext&);
    void paintSelf(SkCanvas&, PaintContext&);
    void paintSelfAndChildren(SkCanvas&, PaintContext&);
    void paintSelfAndChildrenWithReplicaFilterAndMask(SkCanvas&, PaintContext&);
    void paintUsingOverlapRegions(SkCanvas&, PaintContext&);
    void paintUsing3DRenderingContext(SkCanvas&, PaintContext&);
    void paintWithOptionalFilterAndMask(SkCanvas&, PaintContext&, const RefPtr<SkiaCompositingLayer>& mask, const sk_sp<SkImageFilter>&, Function<void()>&&);
    TransformationMatrix replicaTransform() const;
    void collect3DRenderingContextLayers(Vector<Ref<SkiaCompositingLayer>>&);

    enum class IncludesReplica : bool { No, Yes };
    void computeOverlapRegions(ComputeOverlapRegionData&, const TransformationMatrix& accumulatedReplicaTransform, IncludesReplica = IncludesReplica::Yes);

    struct AnimationsState {
        std::optional<TransformationMatrix> transform;
        std::optional<float> opacity;
        sk_sp<SkImageFilter> filter;
        bool isRunning { false };
    };
    std::optional<AnimationsState> syncAnimations(MonotonicTime);

    const TransformationMatrix& localTransform() const;
    float opacity() const;
    sk_sp<SkImageFilter> filter() const;

    Vector<Ref<SkiaCompositingLayer>> m_children;
    WeakPtr<SkiaCompositingLayer> m_parent;
    FloatSize m_size;
    FloatPoint m_position;
    FloatPoint3D m_anchorPoint { 0.5f, 0.5f, 0 };
    FloatPoint m_boundsOrigin;
    FloatRect m_contentsRect;
    struct {
        FloatSize size;
        FloatSize phase;
    } m_contentsTiling;
    TransformationMatrix m_transform;
    TransformationMatrix m_childrenTransform;
    bool m_preserves3D { false };
    bool m_backfaceVisibility { true };
    bool m_contentsVisible { true };
    bool m_visible { true };
    bool m_masksToBounds { false };
    bool m_contentsRectClipsDescendants { false };
    FloatRoundedRect m_contentsClippingRect;
    float m_opacity { 1 };
    RefPtr<SkiaCompositingLayer> m_mask;
    RefPtr<SkiaCompositingLayer> m_replica;
    WeakPtr<SkiaCompositingLayer> m_replicatedLayer;
    RefPtr<CoordinatedBackingStore> m_backingStore;
    RefPtr<CoordinatedAnimatedBackingStoreClient> m_animatedBackingStoreClient;
    RefPtr<CoordinatedImageBackingStore> m_imageBackingStore;
    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_contentsBuffer;
    Color m_contentsSolidColor;
    Color m_debugBorderColor;
    float m_debugBorderWidth { 0 };
    int m_repaintCount { 0 };
    bool m_showDebugBorder : 1 { false };
    bool m_showRepaintCounter : 1 { false };
    sk_sp<SkImageFilter> m_filter;
    TextureMapperAnimations m_animations;
    std::optional<AnimationsState> m_animationsState;
    struct {
        TransformationMatrix combined;
        TransformationMatrix combinedForChildren;
    } m_transforms;
};

} // namespace WebCore

#endif // USE(SKIA)
