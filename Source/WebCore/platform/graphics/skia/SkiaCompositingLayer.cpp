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
#include "ColorMatrix.h"
#include "CoordinatedAnimatedBackingStoreClient.h"
#include "CoordinatedBackingStore.h"
#include "CoordinatedImageBackingStore.h"
#include "CoordinatedPlatformLayerBuffer.h"
#include "CoordinatedTileBuffer.h"
#include "FilterOperations.h"
#include "FontCache.h"
#include "Region.h"
#include "SkiaCompositingLayer3DRenderingContext.h"
#include "SkiaCompositingLayerOverlapRegions.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkColorFilter.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkPathBuilder.h>
#include <skia/core/SkRRect.h>
#include <skia/effects/SkImageFilters.h>
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

static sk_sp<SkImageFilter> createFilter(const FilterOperation& filterOperation, sk_sp<SkImageFilter> input)
{
    switch (filterOperation.type()) {
    case FilterOperation::Type::Grayscale: {
        ColorMatrix<5, 4> matrix(grayscaleColorMatrix(downcast<BasicColorMatrixFilterOperation>(filterOperation).amount()));
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Sepia: {
        ColorMatrix<5, 4> matrix(sepiaColorMatrix(downcast<BasicColorMatrixFilterOperation>(filterOperation).amount()));
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Saturate: {
        ColorMatrix<5, 4> matrix(saturationColorMatrix(downcast<BasicColorMatrixFilterOperation>(filterOperation).amount()));
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::HueRotate: {
        ColorMatrix<5, 4> matrix(hueRotateColorMatrix(downcast<BasicColorMatrixFilterOperation>(filterOperation).amount()));
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Invert: {
        const auto matrix = invertColorMatrix(downcast<BasicComponentTransferFilterOperation>(filterOperation).amount());
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Opacity: {
        const auto matrix = opacityColorMatrix(downcast<BasicComponentTransferFilterOperation>(filterOperation).amount());
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Brightness: {
        ColorMatrix<5, 4> matrix(brightnessColorMatrix(downcast<BasicComponentTransferFilterOperation>(filterOperation).amount()));
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Contrast: {
        const auto matrix = contrastColorMatrix(downcast<BasicComponentTransferFilterOperation>(filterOperation).amount());
        return SkImageFilters::ColorFilter(SkColorFilters::Matrix(matrix.data().data()), input);
    }
    case FilterOperation::Type::Blur: {
        auto sigma = downcast<BlurFilterOperation>(filterOperation).stdDeviation();
        // FIXME: do we need to add crop rect?
        return SkImageFilters::Blur(sigma, sigma, SkTileMode::kDecal, input);
    }
    case FilterOperation::Type::DropShadow: {
        auto& dropShadow = downcast<DropShadowFilterOperation>(filterOperation);
        return SkImageFilters::DropShadow(dropShadow.x(), dropShadow.y(), dropShadow.stdDeviation(), dropShadow.stdDeviation(), dropShadow.color(), input);
    }
    case FilterOperation::Type::Passthrough:
    case FilterOperation::Type::Default:
    case FilterOperation::Type::None:
        break;
    }

    return nullptr;
}

static sk_sp<SkImageFilter> createFilters(const FilterOperations& filterOperations)
{
    sk_sp<SkImageFilter> filter;
    for (const auto& filterOperation : filterOperations)
        filter = createFilter(filterOperation, filter);
    return filter;
}

void SkiaCompositingLayer::setFilters(const FilterOperations& filterOperations)
{
    m_filter = createFilters(filterOperations);
}

const TransformationMatrix& SkiaCompositingLayer::localTransform() const
{
    if (!m_animationsState || !m_animationsState->isRunning)
        return m_transform;

    return m_animationsState->transform ? m_animationsState->transform.value() : m_transform;
}

float SkiaCompositingLayer::opacity() const
{
    if (!m_animationsState || !m_animationsState->isRunning)
        return m_opacity;

    return m_animationsState->opacity.value_or(m_opacity);
}

sk_sp<SkImageFilter> SkiaCompositingLayer::filter() const
{
    if (!m_animationsState || !m_animationsState->isRunning)
        return m_filter;

    return m_animationsState->filter ? m_animationsState->filter : m_filter;
}

std::optional<SkiaCompositingLayer::AnimationsState> SkiaCompositingLayer::syncAnimations(MonotonicTime time)
{
    if (m_animations.isEmpty())
        return std::nullopt;

    TextureMapperAnimation::ApplicationResult applicationResults;
    m_animations.apply(applicationResults, time);

    AnimationsState state;
    state.transform = applicationResults.transform;
    state.opacity = applicationResults.opacity;
    if (applicationResults.filters)
        state.filter = createFilters(*applicationResults.filters);
    state.isRunning = applicationResults.hasRunningAnimations;
    return state;
}

bool SkiaCompositingLayer::computeTransformsAndAnimations(RefPtr<SkiaCompositingLayer> parent, MonotonicTime time)
{
    m_animationsState = syncAnimations(time);
    bool hasRunningAnimations = m_animationsState ? m_animationsState->isRunning : false;

    if (!m_size.isEmpty() || !m_masksToBounds) {
        TransformationMatrix parentTransform;
        if (parent)
            parentTransform = parent == m_parent ? parent->m_transforms.combinedForChildren : parent->m_transforms.combined;

        FloatPoint origin(m_anchorPoint.x(), m_anchorPoint.y());
        origin.scale(m_size.width(), m_size.height());
        m_transforms.combined = parentTransform;
        m_transforms.combined
            .translate3d(origin.x() + (m_position.x() - m_boundsOrigin.x()), origin.y() + (m_position.y() - m_boundsOrigin.y()), m_anchorPoint.z())
            .multiply(localTransform());

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
        hasRunningAnimations |= m_mask->computeTransformsAndAnimations(m_replicatedLayer ? m_replicatedLayer.get() : this, time);
    if (m_replica)
        hasRunningAnimations |= m_replica->computeTransformsAndAnimations(m_replica->m_replicatedLayer, time);

    for (auto& child : m_children)
        hasRunningAnimations |= child->computeTransformsAndAnimations(this, time);

    // If the layer is invisible because of opacity and there's no opacity animation, the content won't
    // be visible ever, so triggering repaints doesn't make sense.
    if (!m_opacity && !(m_animationsState && m_animationsState->opacity))
        return false;

    return hasRunningAnimations;
}

bool SkiaCompositingLayer::paint(SkCanvas& canvas)
{
    bool hasRunningAnimations = computeTransformsAndAnimations(nullptr, MonotonicTime::now());
    PaintContext context;
    recursivePaint(canvas, context);
    return hasRunningAnimations;
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
        else if (auto* buffer = m_imageBackingStore->buffer()) {
            if (m_contentsTiling.size.isEmpty())
                buffer->paintToCanvas(canvas, m_contentsRect, paint);
            else {
                canvas.save();
                canvas.clipRect(SkRect(m_contentsRect));

                float startX = m_contentsRect.x() + m_contentsTiling.phase.width();
                float startY = m_contentsRect.y() + m_contentsTiling.phase.height();

                // Adjust start position to cover the contentsRect from the beginning.
                while (startX > m_contentsRect.x())
                    startX -= m_contentsTiling.size.width();
                while (startY > m_contentsRect.y())
                    startY -= m_contentsTiling.size.height();

                for (float y = startY; y < m_contentsRect.maxY(); y += m_contentsTiling.size.height()) {
                    for (float x = startX; x < m_contentsRect.maxX(); x += m_contentsTiling.size.width()) {
                        FloatRect tileRect(x, y, m_contentsTiling.size.width(), m_contentsTiling.size.height());
                        buffer->paintToCanvas(canvas, tileRect, paint);
                    }
                }

                canvas.restore();
            }
        }

        if (shouldClipContents)
            canvas.restore();
    }

    if (m_showDebugBorder) {
        SkPaint borderPaint;
        borderPaint.setStyle(SkPaint::kStroke_Style);
        borderPaint.setColor(SkColor(m_debugBorderColor));
        borderPaint.setStrokeWidth(m_debugBorderWidth);
        borderPaint.setAntiAlias(true);

        if (m_backingStore)
            canvas.drawRect(SkRect(effectiveLayerRect()), borderPaint);
        if (m_contentsBuffer || m_imageBackingStore || (m_contentsSolidColor.isValid() && m_contentsSolidColor.isVisible()))
            canvas.drawRect(SkRect(m_contentsRect), borderPaint);
    }

    // Capture the full canvas-to-device position while the layer transform is still active.
    SkPoint deviceOrigin { 0, 0 };
    if (m_showRepaintCounter) {
        auto mapped = canvas.getLocalToDevice().map(0, 0, 0, 1);
        if (std::abs(mapped.w) > std::numeric_limits<float>::epsilon())
            deviceOrigin = { mapped.x / mapped.w, mapped.y / mapped.w };
        else
            deviceOrigin = { mapped.x, mapped.y };
    }

    canvas.restore();

    if (m_showRepaintCounter) {
        constexpr float pointSize = 14;
        constexpr float padding = 3;
        auto counterString = String::number(m_repaintCount).ascii();

        static SkFont font = [] {
            auto typeface = FontCache::forCurrentThread().fontManager().matchFamilyStyle("monospace", SkFontStyle::Bold());
            SkFont f(typeface, pointSize);
            f.setEdging(SkFont::Edging::kAntiAlias);
            f.setSubpixel(true);
            return f;
        }();

        SkRect textBounds;
        font.measureText(counterString.data(), counterString.length(), SkTextEncoding::kUTF8, &textBounds);
        float textWidth = textBounds.width() + padding * 2;
        float textHeight = textBounds.height() + padding * 2;

        canvas.save();
        canvas.resetMatrix();

        SkPaint backgroundPaint;
        backgroundPaint.setColor(SkColor(m_debugBorderColor));
        backgroundPaint.setStyle(SkPaint::kFill_Style);
        canvas.drawRect(SkRect::MakeXYWH(deviceOrigin.x(), deviceOrigin.y(), textWidth, textHeight), backgroundPaint);

        SkPaint textPaint;
        textPaint.setColor(SK_ColorWHITE);
        textPaint.setAntiAlias(true);
        canvas.drawString(counterString.data(), deviceOrigin.x() + padding, deviceOrigin.y() - textBounds.fTop + padding, font, textPaint);

        canvas.restore();
    }
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
    if (opacity() < cOpacityVisibilityThreshold)
        return false;
    return true;
}

TransformationMatrix SkiaCompositingLayer::replicaTransform() const
{
    return TransformationMatrix(m_replica->m_transforms.combined)
        .multiply(m_transforms.combined.inverse().value_or(TransformationMatrix()));
}

void SkiaCompositingLayer::paintWithOptionalFilterAndMask(SkCanvas& canvas, PaintContext& context, const RefPtr<SkiaCompositingLayer>& mask, const sk_sp<SkImageFilter>& filter, Function<void()>&& paintContents)
{
    // Clip to mask bounds to limit saveLayer buffer sizes.
    if (mask) {
        canvas.save();
        canvas.concat(SkM44(mask->m_transforms.combined));
        canvas.clipRect(SkRect::MakeWH(mask->m_size.width(), mask->m_size.height()));
        canvas.concat(SkM44(mask->m_transforms.combined.inverse().value_or(TransformationMatrix())));
    }

    // Mask and filter: two saveLayer calls -- isolation for the mask,
    // then SrcIn to clip filtered output by the mask alpha.
    if (mask && filter) {
        canvas.saveLayer(nullptr, nullptr);
        mask->paintSelf(canvas, context);

        SkPaint paint;
        paint.setBlendMode(SkBlendMode::kSrcIn);
        paint.setImageFilter(filter);
        canvas.saveLayer(nullptr, &paint);

        paintContents();

        canvas.restore();
        canvas.restore();
        canvas.restore();
        return;
    }

    // Mask only: single saveLayer with DstIn. The clip above ensures
    // content outside the mask bounds is discarded (DstIn alone would
    // leave those pixels untouched).
    if (mask) {
        canvas.saveLayer(nullptr, nullptr);

        paintContents();

        SetForScope scopedMask(context.isMask, true);
        mask->paintSelf(canvas, context);

        canvas.restore();
        canvas.restore();
        return;
    }

    // Filter only.
    if (filter) {
        SkPaint paint;
        paint.setImageFilter(filter);
        canvas.saveLayer(nullptr, &paint);

        paintContents();

        canvas.restore();
        return;
    }

    paintContents();
}

void SkiaCompositingLayer::paintSelfAndChildrenWithReplicaFilterAndMask(SkCanvas& canvas, PaintContext& context)
{
    auto filter = this->filter();

    if (m_replica) {
        auto newAccumulatedReplicaTransform = TransformationMatrix(context.accumulatedReplicaTransform).multiply(replicaTransform());
        SetForScope scopedReplicaTransform(context.accumulatedReplicaTransform, newAccumulatedReplicaTransform);

        paintWithOptionalFilterAndMask(canvas, context, m_mask, {}, [&] {
            paintWithOptionalFilterAndMask(canvas, context, m_replica->m_mask, filter, [&] {
                canvas.save();
                canvas.concat(SkM44(replicaTransform()));
                paintSelfAndChildren(canvas, context);
                canvas.restore();
            });
        });
    }

    paintWithOptionalFilterAndMask(canvas, context, m_mask, filter, [&] {
        paintSelfAndChildren(canvas, context);
    });
}

void SkiaCompositingLayer::recursivePaint(SkCanvas& canvas, PaintContext& context)
{
    if (!isVisible())
        return;

    SetForScope scopedOpacity(context.opacity, context.opacity * opacity());

    if (m_preserves3D) {
        paintUsing3DRenderingContext(canvas, context);
        return;
    }

    if (opacity() < 1)
        paintUsingOverlapRegions(canvas, context);
    else
        paintSelfAndChildrenWithReplicaFilterAndMask(canvas, context);
}

void SkiaCompositingLayer::computeOverlapRegions(ComputeOverlapRegionData& data, const TransformationMatrix& accumulatedReplicaTransform, IncludesReplica includesReplica)
{
    if (!m_visible || !m_contentsVisible)
        return;

    FloatRect localBoundingRect;
    if (m_backingStore || m_masksToBounds || m_mask)
        localBoundingRect = effectiveLayerRect();
    else if (m_contentsBuffer || m_imageBackingStore || (m_contentsSolidColor.isValid() && m_contentsSolidColor.isVisible()))
        localBoundingRect = m_contentsRect;

    TransformationMatrix transform(accumulatedReplicaTransform);
    transform.multiply(m_transforms.combined);

    auto viewportBoundingRect = data.transformedBoundingBox(transform, localBoundingRect);

    switch (data.mode) {
    case ComputeOverlapRegionMode::Intersection:
        data.resolveOverlaps(viewportBoundingRect);
        break;
    case ComputeOverlapRegionMode::Union:
    case ComputeOverlapRegionMode::Mask:
        data.overlapRegion.unite(viewportBoundingRect);
        break;
    }

    if (m_replica && includesReplica == IncludesReplica::Yes) {
        TransformationMatrix newReplicaTransform(accumulatedReplicaTransform);
        newReplicaTransform.multiply(replicaTransform());
        computeOverlapRegions(data, newReplicaTransform, IncludesReplica::No);
    }

    if (!m_masksToBounds && data.mode != ComputeOverlapRegionMode::Mask) {
        for (auto& child : m_children)
            child->computeOverlapRegions(data, accumulatedReplicaTransform);
    }
}

void SkiaCompositingLayer::paintUsingOverlapRegions(SkCanvas& canvas, PaintContext& context)
{
    ComputeOverlapRegionData data {
        .mode = ComputeOverlapRegionMode::Intersection,
        .clipBounds = canvas.getDeviceClipBounds(),
        .overlapRegion = { },
        .nonOverlapRegion = { }
    };
    computeOverlapRegions(data, context.accumulatedReplicaTransform);

    SkiaCompositingLayerOverlapRegions::paint(canvas, context.opacity, data,
        [&](float effectiveOpacity) {
            SetForScope scopedOpacity(context.opacity, effectiveOpacity);
            paintSelfAndChildrenWithReplicaFilterAndMask(canvas, context);
        });
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
        if (isVisible() && (hasVisualContent() || filter() || (isLeafOf3DRenderingContext() && !m_children.isEmpty())))
            layers.append(Ref { *this });

        // Stop recursion on 3d rendering context leaf
        if (isLeafOf3DRenderingContext())
            return;
    }

    for (auto& child : m_children)
        child->collect3DRenderingContextLayers(layers);
}

void SkiaCompositingLayer::paintUsing3DRenderingContext(SkCanvas& canvas, PaintContext& context)
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
