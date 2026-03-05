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
#include "CoordinatedBackingStoreProxy.h"
#include "FloatPoint.h"
#include "FloatPoint3D.h"
#include "FloatRect.h"
#include "TransformationMatrix.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkCanvas.h>
#include <skia/core/SkM44.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#include <wtf/RefCountedAndCanMakeWeakPtr.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {
class CoordinatedAnimatedBackingStoreClient;
class CoordinatedBackingStore;
class CoordinatedImageBackingStore;

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
    void setMasksToBounds(bool masksToBounds) { m_masksToBounds = masksToBounds; }
    void setContentsRect(const FloatRect& rect) { m_contentsRect = rect; }

    void setChildren(Vector<Ref<SkiaCompositingLayer>>&&);

    void setUseBackingStore(bool, CoordinatedAnimatedBackingStoreClient* = nullptr);
    void updateBackingStore(CoordinatedBackingStoreProxy::Update&&, float);
    void setImageBackingStore(CoordinatedImageBackingStore*);
    void setContentsBuffer(std::unique_ptr<CoordinatedPlatformLayerBuffer>&&);

    void computeTransforms();
    void paint(SkCanvas&);

private:
    SkiaCompositingLayer() = default;

    void removeFromParent();

    void recursivePaint(SkCanvas&);
    void paintLayer(SkCanvas&);

    Vector<Ref<SkiaCompositingLayer>> m_children;
    WeakPtr<SkiaCompositingLayer> m_parent;
    FloatSize m_size;
    FloatPoint m_position;
    FloatPoint3D m_anchorPoint { 0.5f, 0.5f, 0 };
    FloatPoint m_boundsOrigin;
    FloatRect m_contentsRect;
    TransformationMatrix m_transform;
    TransformationMatrix m_childrenTransform;
    bool m_masksToBounds { false };
    RefPtr<CoordinatedBackingStore> m_backingStore;
    RefPtr<CoordinatedAnimatedBackingStoreClient> m_animatedBackingStoreClient;
    RefPtr<CoordinatedImageBackingStore> m_imageBackingStore;
    std::unique_ptr<CoordinatedPlatformLayerBuffer> m_contentsBuffer;
    struct {
        TransformationMatrix local;
        TransformationMatrix combined;
        TransformationMatrix combinedForChildren;
    } m_transforms;
};

} // namespace WebCore

#endif // USE(SKIA)
