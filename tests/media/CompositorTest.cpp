#include <gtest/gtest.h>

#include <array>
#include <vector>

#include <QImage>

#include "D3D11Context.h"
#include "FrameReadback.h"
#include "LayerCompositor.h"
#include "SharedTextureRing.h"

using namespace VideoEditor;

TEST(Compositor, BlendsTwoImagesAndReadsBack)
{
    QString error;
    auto context = D3D11Context::create(false, &error);
    if (!context)
        GTEST_SKIP() << "D3D11 unavailable: " << error.toStdString();

    SharedTextureRing ring(*context);
    ASSERT_TRUE(ring.create(64, 64, 3, &error)) << error.toStdString();
    LayerCompositor compositor;
    ASSERT_TRUE(compositor.initialize(*context, 64, 64, &error)) << error.toStdString();

    QImage red(64, 64, QImage::Format_ARGB32_Premultiplied);
    red.fill(Qt::red);
    QImage blue(64, 64, QImage::Format_ARGB32_Premultiplied);
    blue.fill(QColor(0, 0, 255, 128));

    // Keyed mutex initial state is key 0 (producer's turn). The producer must
    // release with key 1 before a consumer (readback) can acquire the slot,
    // exactly like TimelineRenderWorker does around the compositor calls.
    IDXGIKeyedMutex *mutex = ring.mutex(0);
    ASSERT_NE(mutex, nullptr);
    ASSERT_EQ(mutex->AcquireSync(0, 5000), S_OK);

    compositor.begin(ring.rtv(0), true);
    ASSERT_TRUE(compositor.drawImage(red, LayerRect{0, 0, 1, 1}, 1.0f, &error))
        << error.toStdString();
    ASSERT_TRUE(compositor.drawImage(blue, LayerRect{0, 0, 0.5f, 1}, 0.5f, &error))
        << error.toStdString();
    compositor.end();
    mutex->ReleaseSync(1);

    const std::vector<uint8_t> pixels =
        readbackSharedTexture(*context, ring.texture(0), &error);
    ASSERT_EQ(pixels.size(), size_t(64 * 64 * 4)) << error.toStdString();
    const auto at = [&pixels](int x, int y) {
        const size_t index = (size_t(y) * 64 + x) * 4;
        return std::array<int, 3>{pixels[index], pixels[index + 1], pixels[index + 2]};
    };
    const auto left = at(8, 32);
    const auto right = at(56, 32);
    EXPECT_GT(left[0], 100);       // red + blue blend
    EXPECT_GT(left[2], 40);
    EXPECT_GT(right[0], 200);      // untouched red
    EXPECT_LT(right[2], 60);
}

TEST(LayerGeometry, FitsAndOffsetsTransform)
{
    const LayerRect fit = layerRectFor(Transform{}, 1920, 1080, 1920, 1080);
    EXPECT_FLOAT_EQ(fit.x, 0.0f);
    EXPECT_FLOAT_EQ(fit.w, 1.0f);

    Transform zoom;
    zoom.scaleMilli = 2000;
    zoom.offsetXPermille = 250;
    const LayerRect zoomed = layerRectFor(zoom, 1920, 1080, 1920, 1080);
    EXPECT_NEAR(zoomed.w, 2.0f, 1e-4f);
    // Centered 2x layer starts at -0.5; a +0.25 frame-width offset moves it to -0.25.
    EXPECT_NEAR(zoomed.x, -0.25f, 1e-4f);
}
