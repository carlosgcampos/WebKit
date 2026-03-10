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

#include "config.h"
#include "SkiaCompositingLayer.h"

#if USE(SKIA)
#include "CoordinatedAnimatedBackingStoreClient.h"
#include "CoordinatedBackingStore.h"
#include "CoordinatedImageBackingStore.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedTileBuffer.h"
#include "SkiaCompositingLayer3DRenderingContext.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkRRect.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SkiaCompositingLayer);

Ref<SkiaCompositingLayer> SkiaCompositingLayer::create()
{
    return adoptRef(*new SkiaCompositingLayer());
}

SkiaCompositingLayer::~SkiaCompositingLayer() = default;

void SkiaCompositingLayer::setChildren(Vector<Ref<SkiaCompositingLayer>>&& newChildren)
{
    if (m_children == newChildren)
        return;

    while (!m_children.isEmpty()) {
        auto child = m_children.takeLast();
        child->m_parent = nullptr;
    }

    m_children = WTF::move(newChildren);
    for (auto& child : m_children) {
        child->removeFromParent();
        child->m_parent = this;
    }
}

void SkiaCompositingLayer::removeFromParent()
{
    RefPtr parent = std::exchange(m_parent, nullptr);
    if (!parent)
        return;

    parent->m_children.removeFirstMatching([this](auto& layer) {
        return layer.ptr() == this;
    });
}

void SkiaCompositingLayer::setUseBackingStore(bool useBackingStore, CoordinatedAnimatedBackingStoreClient* animatedBackingStoreClient)
{
    if (!useBackingStore) {
        m_backingStore = nullptr;
        m_animatedBackingStoreClient = nullptr;
        return;
    }

    if (!m_backingStore)
        m_backingStore = CoordinatedBackingStore::create();
    m_animatedBackingStoreClient = animatedBackingStoreClient;
}

void SkiaCompositingLayer::updateBackingStore(CoordinatedBackingStoreProxy::Update&& update, float scale)
{
    ASSERT(m_backingStore);
    m_backingStore->resize(m_size, scale);
    for (auto tileID : update.tilesToCreate())
        m_backingStore->createTile(tileID);
    for (auto tileID : update.tilesToRemove())
        m_backingStore->removeTile(tileID);
    for (const auto& tileUpdate : update.tilesToUpdate())
        m_backingStore->updateTile(tileUpdate.tileID, tileUpdate.dirtyRect, tileUpdate.tileRect, tileUpdate.buffer.copyRef(), { });
    m_backingStore->processPendingUpdates();
}

void SkiaCompositingLayer::setImageBackingStore(CoordinatedImageBackingStore* imageBackingStore)
{
    m_imageBackingStore = imageBackingStore;
}

void SkiaCompositingLayer::setContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&& contentsBuffer)
{
    m_contentsBuffer = WTF::move(contentsBuffer);
}

void SkiaCompositingLayer::setContentsSolidColor(const Color& color)
{
    m_contentsSolidColor = color;
}

void SkiaCompositingLayer::setMask(RefPtr<SkiaCompositingLayer>&& mask)
{
    m_mask = WTF::move(mask);
}

void SkiaCompositingLayer::setReplica(RefPtr<SkiaCompositingLayer>&& replica)
{
    m_replica = WTF::move(replica);
    if (m_replica)
        m_replica->m_replicatedLayer = this;
}

void SkiaCompositingLayer::computeTransforms(RefPtr<SkiaCompositingLayer> parent)
{
    m_transforms.local = m_transform;

    if (!m_size.isEmpty() || !m_masksToBounds) {
        TransformationMatrix parentTransform;
        if (parent)
            parentTransform = parent == m_parent ? parent->m_transforms.combinedForChildren : parent->m_transforms.combined;

        FloatPoint origin(m_anchorPoint.x(), m_anchorPoint.y());
        origin.scale(m_size.width(), m_size.height());
        m_transforms.combined = parentTransform;
        m_transforms.combined
            .translate3d(origin.x() + (m_position.x() - m_boundsOrigin.x()), origin.y() + (m_position.y() - m_boundsOrigin.y()), m_anchorPoint.z())
            .multiply(m_transforms.local);

        m_transforms.combinedForChildren = m_transforms.combined;
        m_transforms.combined.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

        if (isReplica())
            m_transforms.combined.translate(-m_position.x(), -m_position.y());

        if (!m_preserves3D)
            m_transforms.combinedForChildren.flatten();
        m_transforms.combinedForChildren.multiply(m_childrenTransform);
        m_transforms.combinedForChildren.translate3d(-origin.x(), -origin.y(), -m_anchorPoint.z());

        m_visible = m_backfaceVisibility || !m_transforms.combined.isBackFaceVisible();

        if (m_animatedBackingStoreClient) {
            // FIXME: use future combined.
            m_animatedBackingStoreClient->requestBackingStoreUpdateIfNeeded(m_transforms.combined);
        }
    }

    if (m_mask)
        m_mask->computeTransforms(m_replicatedLayer ? m_replicatedLayer.get() : this);
    if (m_replica)
        m_replica->computeTransforms(m_replica->m_replicatedLayer);

    for (auto& child : m_children)
        child->computeTransforms(this);
}

void SkiaCompositingLayer::paint(SkCanvas& canvas)
{
    computeTransforms();
    PaintContext context;
    recursivePaint(canvas, context);
}

void SkiaCompositingLayer::paintSelf(SkCanvas& canvas, PaintContext& context)
{
    if (m_size.isEmpty() || !m_visible || !m_contentsVisible || !hasVisualContent())
        return;

    canvas.save();
    canvas.concat(SkM44(m_transforms.combined));

    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAlphaf(context.opacity);
    if (context.isMask)
        paint.setBlendMode(SkBlendMode::kDstIn);

    if (m_backingStore)
        m_backingStore->paintToCanvas(canvas, paint);

    if (m_contentsSolidColor.isValid() && m_contentsSolidColor.isVisible()) {
        paint.setColor(SkColor(m_contentsSolidColor.colorWithAlphaMultipliedBy(context.opacity)));
        canvas.drawRect(m_contentsRect, paint);
    } else if (m_contentsBuffer || m_imageBackingStore) {
        bool shouldClipContents = m_contentsClippingRect.isRounded() || !m_contentsClippingRect.rect().contains(m_contentsRect);
        if (shouldClipContents) {
            canvas.save();
            if (m_contentsClippingRect.isRounded())
                canvas.clipRRect(SkRRect(m_contentsClippingRect), true);
            else
                canvas.clipRect(SkRect(m_contentsClippingRect.rect()));
        }

        if (m_contentsBuffer)
            m_contentsBuffer->paintToCanvas(canvas, m_contentsRect, paint);
        else if (auto* buffer = m_imageBackingStore->buffer())
            buffer->paintToCanvas(canvas, m_contentsRect, paint);

        if (shouldClipContents)
            canvas.restore();
    }

    canvas.restore();
}

void SkiaCompositingLayer::paintSelfAndChildren(SkCanvas& canvas, PaintContext& context)
{
    paintSelf(canvas, context);

    if (m_children.isEmpty())
        return;

    bool shouldClip = (m_masksToBounds || m_contentsRectClipsDescendants) && !m_preserves3D;
    if (shouldClip) {
        canvas.save();
        if (m_contentsRectClipsDescendants) {
            SkPathBuilder builder;
            if (m_contentsClippingRect.isRounded())
                builder.addRRect(SkRRect(m_contentsClippingRect));
            else
                builder.addRect(SkRect(m_contentsClippingRect.rect()));
            canvas.clipPath(builder.detach().makeTransform(SkM44(m_transforms.combined).asM33()), true);
        } else {
            auto clipTransform = m_transforms.combined;
            clipTransform.translate(m_boundsOrigin.x(), m_boundsOrigin.y());
            SkPathBuilder builder;
            builder.addRect(SkRect(effectiveLayerRect()));
            canvas.clipPath(builder.detach().makeTransform(SkM44(clipTransform).asM33()));
        }
    }

    for (auto& child : m_children)
        child->recursivePaint(canvas, context);

    if (shouldClip)
        canvas.restore();
}

bool SkiaCompositingLayer::isVisible() const
{
    constexpr float cOpacityVisibilityThreshold = 0.01;
    if (m_size.isEmpty() && (m_masksToBounds || m_children.isEmpty()))
        return false;
    if (!m_visible && m_children.isEmpty())
        return false;
    if (!m_contentsVisible && m_children.isEmpty())
        return false;
    if (!hasVisualContent() && m_children.isEmpty())
        return false;
    if (m_opacity < cOpacityVisibilityThreshold)
        return false;
    return true;
}

TransformationMatrix SkiaCompositingLayer::replicaTransform() const
{
    return TransformationMatrix(m_replica->m_transforms.combined)
        .multiply(m_transforms.combined.inverse().value_or(TransformationMatrix()));
}

void SkiaCompositingLayer::paintSelfAndChildrenWithReplica(SkCanvas& canvas, PaintContext& context)
{
    if (m_replica) {
        canvas.save();
        canvas.concat(SkM44(replicaTransform()));
        paintSelfAndChildren(canvas, context);
        canvas.restore();
    }

    paintSelfAndChildren(canvas, context);
}

void SkiaCompositingLayer::recursivePaint(SkCanvas& canvas, PaintContext& context)
{
    if (!isVisible())
        return;

    SetForScope scopedOpacity(context.opacity, context.opacity * m_opacity);

    if (m_preserves3D) {
        paintWith3DRenderingContext(canvas, context);
        return;
    }

    bool hasMask = !!m_mask;
    bool hasReplicaMask = m_replica && m_replica->m_mask;

    if (hasMask || hasReplicaMask) {
        // Paint replica with its own mask (if any).
        if (m_replica) {
            if (hasReplicaMask)
                canvas.saveLayer(nullptr, nullptr);

            canvas.save();
            canvas.concat(SkM44(replicaTransform()));
            paintSelfAndChildren(canvas, context);
            canvas.restore();

            if (hasReplicaMask) {
                SetForScope scopedMask(context.isMask, true);
                m_replica->m_mask->paintSelf(canvas, context);
                canvas.restore();
            }
        }

        // Paint original with its own mask.
        if (hasMask)
            canvas.saveLayer(nullptr, nullptr);

        paintSelfAndChildren(canvas, context);

        if (hasMask) {
            SetForScope scopedMask(context.isMask, true);
            m_mask->paintSelf(canvas, context);
            canvas.restore();
        }
    } else
        paintSelfAndChildrenWithReplica(canvas, context);
}

bool SkiaCompositingLayer::hasVisualContent() const
{
    // FIXME: hasFilters() / hasBackdrop() conditions
    // FIXME: Consider background color (compare with `bool hasVisualContent`)
    return m_backingStore || m_imageBackingStore || m_contentsBuffer
        || (m_contentsSolidColor.isValid() && m_contentsSolidColor.isVisible());
}

void SkiaCompositingLayer::collect3DRenderingContextLayers(Vector<Ref<SkiaCompositingLayer>>& layers)
{
    if (m_preserves3D || isLeafOf3DRenderingContext()) {
        // Add layers to 3d rendering context only if they get actually painted.
        if (isVisible() && (hasVisualContent() || (isLeafOf3DRenderingContext() && !m_children.isEmpty())))
            layers.append(Ref { *this });

        // Stop recursion on 3d rendering context leaf
        if (isLeafOf3DRenderingContext())
            return;
    }

    for (auto& child : m_children)
        child->collect3DRenderingContextLayers(layers);
}

void SkiaCompositingLayer::paintWith3DRenderingContext(SkCanvas& canvas, PaintContext& context)
{
    Vector<Ref<SkiaCompositingLayer>> layers;
    collect3DRenderingContextLayers(layers);

    SkiaCompositingLayer3DRenderingContext::paint(layers, [&](SkiaCompositingLayer& layer, std::optional<SkPath> clipPath) {
        if (clipPath) {
            canvas.save();
            canvas.clipPath(*clipPath);
        }

        if (layer.m_preserves3D)
            layer.paintSelf(canvas, context);
        else
            layer.recursivePaint(canvas, context);

        if (clipPath)
            canvas.restore();
    });
}

} // namespace WebCore

#endif // USE(SKIA)
