/*
 * Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2025 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "CoordinatedBackingStoreTile.h"

#if USE(COORDINATED_GRAPHICS)
#include "BitmapTexturePool.h"
#include "CoordinatedTileBuffer.h"
#include "GraphicsLayer.h"
#include <wtf/SystemTracing.h>

#if USE(SKIA)
#include "BitmapTexture.h"
#include "PlatformDisplay.h"
#include "SkiaPaintingEngine.h"
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/gpu/ganesh/GrBackendSurface.h>
#include <skia/gpu/ganesh/SkImageGanesh.h>
#include <skia/gpu/ganesh/gl/GrGLBackendSurface.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebCore {

CoordinatedBackingStoreTile::CoordinatedBackingStoreTile(float scale)
    : m_scale(scale)
{
}

CoordinatedBackingStoreTile::~CoordinatedBackingStoreTile() = default;

void CoordinatedBackingStoreTile::addUpdate(Update&& update)
{
    m_updates.append(WTF::move(update));
}

void CoordinatedBackingStoreTile::processPendingUpdates()
{
    auto updates = WTF::move(m_updates);
    auto updatesCount = updates.size();
    if (!updatesCount)
        return;

#if USE(SKIA)
    // The underlying GL texture is about to be updated; the cached SkImage wrapping it is now stale.
    m_cachedSkImage = nullptr;
#endif

    WTFBeginSignpost(this, CoordinatedSwapBuffers, "%zu updates", updatesCount);
    for (unsigned updateIndex = 0; updateIndex < updatesCount; ++updateIndex) {
        auto& update = updates[updateIndex];

        WTFBeginSignpost(this, CoordinatedSwapBuffer, "%u/%zu, rect %ix%i+%i+%i", updateIndex + 1, updatesCount, update.tileRect.x(), update.tileRect.y(), update.tileRect.width(), update.tileRect.height());

        FloatRect unscaledTileRect(update.tileRect);
        unscaledTileRect.scale(1. / m_scale);

        OptionSet<BitmapTexture::Flags> flags { };
        if (update.buffer->supportsAlpha())
            flags.add(BitmapTexture::Flags::SupportsAlpha);

#if USE(SKIA) && USE(GBM)
        if (SkiaPaintingEngine::shouldUseLinearTileTextures()) {
            flags.add(BitmapTexture::Flags::BackedByDMABuf);
            flags.add(BitmapTexture::Flags::ForceLinearBuffer);
        } else if (SkiaPaintingEngine::shouldUseVivanteSuperTiledTileTextures()) {
            flags.add(BitmapTexture::Flags::BackedByDMABuf);
            flags.add(BitmapTexture::Flags::ForceVivanteSuperTiledBuffer);
        }
#endif

        WTFBeginSignpost(this, AcquireTexture);
        if (!m_texture || unscaledTileRect != m_rect) {
            m_rect = unscaledTileRect;
            m_texture = BitmapTexturePool::singleton().acquireTexture(update.tileRect.size(), flags);
        } else if (update.buffer->supportsAlpha() == m_texture->isOpaque())
            m_texture->reset(update.tileRect.size(), flags);
        WTFEndSignpost(this, AcquireTexture);

        update.buffer->waitUntilPaintingComplete();

#if USE(SKIA)
        if (update.buffer->isBackedByOpenGL()) {
            WTFBeginSignpost(this, CopyTextureGPUToGPU);
            auto& buffer = static_cast<CoordinatedAcceleratedTileBuffer&>(update.buffer.get());
            buffer.serverWait();

            // Fast path: whole tile content changed -- take ownership of the incoming texture, replacing the existing tile buffer (avoiding texture copies).
            if (update.sourceRect.size() == update.tileRect.size()) {
                ASSERT(update.sourceRect.location().isZero());
                m_texture->swapTexture(buffer.texture());
            } else
                m_texture->copyFromExternalTexture(buffer.texture().id(), update.sourceRect, toIntSize(update.bufferOffset));

            WTFEndSignpost(this, CopyTextureGPUToGPU);
            WTFEndSignpost(this, CoordinatedSwapBuffer);
            continue;
        }
#endif

        WTFBeginSignpost(this, CopyTextureCPUToGPU);
        ASSERT(!update.buffer->isBackedByOpenGL());
        auto& buffer = static_cast<CoordinatedUnacceleratedTileBuffer&>(update.buffer.get());
        m_texture->updateContents(buffer.data(), update.sourceRect, update.bufferOffset, buffer.stride(), buffer.pixelFormat());
        WTFEndSignpost(this, CopyTextureCPUToGPU);

        WTFEndSignpost(this, CoordinatedSwapBuffer);
    }
    WTFEndSignpost(this, CoordinatedSwapBuffers);
}

#if USE(SKIA)
const sk_sp<SkImage>& CoordinatedBackingStoreTile::ensureSkImage()
{
    if (!m_cachedSkImage && m_texture) {
        auto* grContext = PlatformDisplay::sharedDisplay().skiaGrContext();
        GrGLTextureInfo externalTexture;
        externalTexture.fTarget = GL_TEXTURE_2D;
        externalTexture.fID = m_texture->id();
        externalTexture.fFormat = GL_RGBA8;
        const auto& size = m_texture->size();
        auto backendTexture = GrBackendTextures::MakeGL(size.width(), size.height(), skgpu::Mipmapped::kNo, externalTexture);
        m_cachedSkImage = SkImages::BorrowTextureFrom(grContext, backendTexture, kTopLeft_GrSurfaceOrigin, kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    }
    return m_cachedSkImage;
}
#endif

} // namespace WebCore

#endif // USE(COORDINATED_GRAPHICS)
