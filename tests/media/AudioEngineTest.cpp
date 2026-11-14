#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include <QTest>

#include "AudioClock.h"
#include "AudioEngine.h"
#include "AudioSink.h"

using namespace VideoEditor;

namespace {

class CountingSink : public IAudioSink {
public:
    explicit CountingSink(int sampleRate, int channels)
        : m_sampleRate(sampleRate), m_channels(channels) {}

    bool start(QString *) override { m_running = true; return true; }
    void stop() override { m_running = false; }
    int write(const float *, int frames) override
    {
        m_written.fetch_add(frames);
        return frames;
    }
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_written.load(); }
    bool running() const { return m_running; }

private:
    int m_sampleRate;
    int m_channels;
    std::atomic<int64_t> m_written{0};
    std::atomic_bool m_running{false};
};

} // namespace

TEST(AudioEngine, StartsPauseAndSeekWithInjectedSink)
{
    AudioEngine engine;
    engine.setSinkFactoryForTest([](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(new CountingSink(sampleRate, channels));
    });
    ASSERT_TRUE(engine.hasOutputDevice());

    Project project;
    project.timeline.sampleRate = 48000;
    engine.setProject(project);
    engine.play(0);
    QTest::qWait(60);
    const int64_t first = engine.positionUs();
    engine.pause();
    EXPECT_GE(first, 0);
    engine.play(2'000'000);
    QTest::qWait(40);
    EXPECT_GE(engine.positionUs(), 2'000'000);
    engine.shutdown();
}

TEST(AudioEngine, FallsBackWhenDeviceOpenFails)
{
    AudioEngine engine;
    engine.setSinkFactoryForTest([](int, int, QString *error) {
        if (error)
            *error = QStringLiteral("没有可用的音频输出设备。");
        return std::unique_ptr<IAudioSink>();
    });
    EXPECT_FALSE(engine.hasOutputDevice());
    EXPECT_TRUE(engine.lastDeviceError().contains(QStringLiteral("音频")));
    engine.setProject(Project{});
    engine.play(0);          // must not crash or block
    engine.pause();
    engine.shutdown();
}

TEST(AudioClock, ResetAndSetAreMonotonicPerPlayback)
{
    AudioClock clock;
    clock.reset(1'000'000);
    EXPECT_EQ(clock.positionUs(), 1'000'000);
    clock.setPosition(1'500'000);
    EXPECT_EQ(clock.positionUs(), 1'500'000);
    clock.reset(0);
    EXPECT_EQ(clock.positionUs(), 0);
}

TEST(AudioClock, FrameIndexRoundsToNearestFrame)
{
    const Rational fps{30000, 1001};
    EXPECT_EQ(frameIndexForClock(0, fps), 0);
    EXPECT_EQ(frameIndexForClock(33'367, fps), 1);
    EXPECT_EQ(frameIndexForClock(1'001'000, fps), 30);
}
