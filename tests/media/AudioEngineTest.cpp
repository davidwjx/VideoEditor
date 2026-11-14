#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

#include "AudioClock.h"
#include "AudioEngine.h"
#include "AudioSink.h"
#include "WasapiOutput.h"

using namespace VideoEditor;

namespace {

// Shared counters so tests can observe a sink after ownership moved into the
// engine (the factory must return a unique_ptr).
struct SinkStats {
    std::atomic<int64_t> written{0};
    std::atomic<int64_t> played{0};
    std::atomic<int64_t> pending{0};
    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    std::atomic<int> flushes{0};
    std::atomic<int> writes{0};
    std::atomic<int> maxOverRequest{0};
    std::atomic_bool acceptStart{true};
    std::atomic_bool lost{false};
    std::atomic<int> lostAfterWrites{0};
};

// Device with no buffering: every accepted frame is immediately "played".
class CountingSink : public IAudioSink {
public:
    CountingSink(int sampleRate, int channels, std::shared_ptr<SinkStats> stats)
        : m_sampleRate(sampleRate)
        , m_channels(channels)
        , m_stats(std::move(stats))
    {
    }

    bool start(QString *error) override
    {
        m_stats->starts.fetch_add(1);
        if (!m_stats->acceptStart.load()) {
            if (error)
                *error = QStringLiteral("没有可用的音频输出设备。");
            return false;
        }
        return true;
    }
    void stop() override { m_stats->stops.fetch_add(1); }
    void flush() override { m_stats->flushes.fetch_add(1); }
    int write(const float *, int frames) override
    {
        m_stats->written += frames;
        m_stats->played += frames;
        const int writes = m_stats->writes.fetch_add(1) + 1;
        const int limit = m_stats->lostAfterWrites.load();
        if (limit > 0 && writes >= limit)
            m_stats->lost.store(true);
        return frames;
    }
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_stats->played.load(); }
    bool deviceLost() const override { return m_stats->lost.load(); }

private:
    int m_sampleRate;
    int m_channels;
    std::shared_ptr<SinkStats> m_stats;
};

// Models a real device buffer of `capacityFrames`: write() fills it, the test
// drains it explicitly with drainPending(). Nothing is played otherwise.
class BufferedSink : public IAudioSink {
public:
    BufferedSink(int sampleRate, int channels, std::shared_ptr<SinkStats> stats,
                 int capacityFrames)
        : m_sampleRate(sampleRate)
        , m_channels(channels)
        , m_stats(std::move(stats))
        , m_capacity(capacityFrames)
    {
    }

    bool start(QString *) override
    {
        m_stats->starts.fetch_add(1);
        return m_stats->acceptStart.load();
    }
    void stop() override { m_stats->stops.fetch_add(1); }
    void flush() override
    {
        // IAudioClient::Reset() semantics: queued frames are discarded and the
        // device padding drops to zero, so playedFrames() reports them as
        // consumed.
        m_stats->played += m_stats->pending.exchange(0);
        m_stats->flushes.fetch_add(1);
    }
    int availableFrames() const override
    {
        return std::max(0, m_capacity - int(m_stats->pending.load()));
    }
    int write(const float *, int frames) override
    {
        const int room = availableFrames();
        if (frames > room)
            m_stats->maxOverRequest.store(
                std::max(m_stats->maxOverRequest.load(), frames - room));
        const int accepted = std::min(frames, room);
        m_stats->pending += accepted;
        m_stats->written += accepted;
        m_stats->writes.fetch_add(1);
        return accepted;
    }
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_stats->played.load(); }

private:
    int m_sampleRate;
    int m_channels;
    std::shared_ptr<SinkStats> m_stats;
    int m_capacity;
};

// Device that can only accept `limit` frames per write. A regression to
// render-then-drop shows up as a request larger than availableFrames().
class ThrottledSink : public IAudioSink {
public:
    ThrottledSink(int sampleRate, int channels, std::shared_ptr<SinkStats> stats,
                  int limit)
        : m_sampleRate(sampleRate)
        , m_channels(channels)
        , m_stats(std::move(stats))
        , m_limit(limit)
    {
    }

    bool start(QString *) override
    {
        m_stats->starts.fetch_add(1);
        return true;
    }
    void stop() override { m_stats->stops.fetch_add(1); }
    void flush() override { m_stats->flushes.fetch_add(1); }
    int availableFrames() const override { return m_limit; }
    int write(const float *, int frames) override
    {
        if (frames > m_limit)
            m_stats->maxOverRequest.store(
                std::max(m_stats->maxOverRequest.load(), frames - m_limit));
        const int accepted = std::min(frames, m_limit);
        m_stats->written += accepted;
        m_stats->played += accepted;
        m_stats->writes.fetch_add(1);
        return accepted;
    }
    int sampleRate() const override { return m_sampleRate; }
    int channels() const override { return m_channels; }
    int64_t playedFrames() const override { return m_stats->played.load(); }

private:
    int m_sampleRate;
    int m_channels;
    std::shared_ptr<SinkStats> m_stats;
    int m_limit;
};

void installCounting(AudioEngine &engine, const std::shared_ptr<SinkStats> &stats)
{
    engine.setSinkFactoryForTest([stats](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(new CountingSink(sampleRate, channels, stats));
    });
}

void installBuffered(AudioEngine &engine, const std::shared_ptr<SinkStats> &stats,
                     int capacityFrames)
{
    engine.setSinkFactoryForTest(
        [stats, capacityFrames](int sampleRate, int channels, QString *) {
            return std::unique_ptr<IAudioSink>(
                new BufferedSink(sampleRate, channels, stats, capacityFrames));
        });
}

void installThrottled(AudioEngine &engine, const std::shared_ptr<SinkStats> &stats,
                      int limit)
{
    engine.setSinkFactoryForTest(
        [stats, limit](int sampleRate, int channels, QString *) {
            return std::unique_ptr<IAudioSink>(
                new ThrottledSink(sampleRate, channels, stats, limit));
        });
}

void drainPending(const std::shared_ptr<SinkStats> &stats)
{
    stats->played += stats->pending.exchange(0);
}

} // namespace

TEST(AudioEngine, StartsPauseAndSeekWithInjectedSink)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installCounting(engine, stats);
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

TEST(AudioEngine, SeekWhilePlayingFlushesQueuedAudioAndRebasesClock)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installBuffered(engine, stats, 24000);
    engine.setProject(Project{});
    engine.play(0);
    QTest::qWait(120);

    ASSERT_GT(stats->pending.load(), 0);          // device buffered, unplayed
    const int64_t writtenBeforeSeek = stats->written.load();
    EXPECT_EQ(engine.positionUs(), 0);            // clock follows played frames

    engine.seek(2'000'000);
    QTest::qWait(40);

    // Flush must discard the queued pre-seek audio and the baseline must be
    // captured afterwards, so nothing stale is replayed or counted.
    EXPECT_GE(stats->flushes.load(), 1);
    EXPECT_GE(stats->played.load(), writtenBeforeSeek);
    EXPECT_EQ(engine.positionUs(), 2'000'000);

    // Let the fake device consume the post-seek audio: the clock may only
    // advance by content produced after the seek.
    drainPending(stats);
    QTest::qWait(20);
    engine.pause();
    const int64_t delta = engine.positionUs() - 2'000'000;
    EXPECT_GT(delta, 0);
    EXPECT_LE(delta, 100'000);                    // <=100 ms post-seek audio
    engine.shutdown();
}

TEST(AudioEngine, PauseFlushesPendingAndCapturesFinalPosition)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installBuffered(engine, stats, 24000);
    engine.setProject(Project{});
    engine.play(1'000'000);
    QTest::qWait(120);
    ASSERT_GT(stats->pending.load(), 0);

    engine.pause();                               // blocking

    EXPECT_GE(stats->flushes.load(), 1);
    EXPECT_EQ(stats->pending.load(), 0);          // nothing left to replay
    EXPECT_EQ(stats->played.load(), stats->written.load());
    EXPECT_EQ(engine.positionUs(), 1'000'000);

    const int64_t pausedUs = engine.positionUs();
    QTest::qWait(50);
    EXPECT_EQ(engine.positionUs(), pausedUs);     // no tick lags behind pause
    engine.shutdown();
}

TEST(AudioEngine, StartFailureClearsDeviceFlagAndFallsBack)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    stats->acceptStart.store(false);
    installCounting(engine, stats);
    EXPECT_TRUE(engine.hasOutputDevice());

    QSignalSpy failures(&engine, &AudioEngine::deviceFailed);
    engine.setProject(Project{});
    engine.play(0);
    QTest::qWait(40);

    EXPECT_FALSE(engine.hasOutputDevice());
    EXPECT_TRUE(engine.lastDeviceError().contains(QStringLiteral("音频")));
    EXPECT_GE(failures.count(), 1);
    EXPECT_EQ(engine.positionUs(), 0);
    engine.pause();
    engine.shutdown();
}

TEST(AudioEngine, ThrottledDeviceRendersOnlyWritableFrames)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installThrottled(engine, stats, 100);
    engine.setProject(Project{});
    engine.play(0);
    QTest::qWait(150);
    engine.pause();

    EXPECT_GT(stats->writes.load(), 0);
    // The engine must never ask for more frames than the device can take,
    // which is exactly the old render-480-then-drop bug.
    EXPECT_EQ(stats->maxOverRequest.load(), 0);
    EXPECT_EQ(engine.positionUs(), stats->played.load() * 1'000'000 / 48000);
    engine.shutdown();
}

TEST(AudioEngine, DeviceLossStopsPlaybackAndFallsBack)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    stats->lostAfterWrites.store(3);
    installCounting(engine, stats);

    QSignalSpy failures(&engine, &AudioEngine::deviceFailed);
    engine.setProject(Project{});
    engine.play(0);
    QTest::qWait(100);

    EXPECT_FALSE(engine.hasOutputDevice());
    EXPECT_TRUE(engine.lastDeviceError().contains(QStringLiteral("音频")));
    EXPECT_GE(failures.count(), 1);

    const int64_t stoppedUs = engine.positionUs();
    QTest::qWait(40);
    EXPECT_EQ(engine.positionUs(), stoppedUs);    // loop stopped, wall clock next
    engine.shutdown();
}

TEST(AudioEngine, PauseAppliesPendingSeek)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installBuffered(engine, stats, 24000);
    engine.setProject(Project{});
    engine.play(0);
    QTest::qWait(60);

    engine.seek(2'000'000); // deferred to the next block while playing
    engine.pause();         // blocking: must apply the pending seek first
    EXPECT_EQ(engine.positionUs(), 2'000'000);
    EXPECT_GE(stats->flushes.load(), 1);
    engine.shutdown();
}

TEST(AudioEngine, ShutdownIsIdempotentAndDestructorDoesNotDeadlock)
{
    auto engine = std::make_unique<AudioEngine>();
    auto stats = std::make_shared<SinkStats>();
    installCounting(*engine, stats);
    engine->setProject(Project{});
    engine->play(0);
    QTest::qWait(20);

    engine->shutdown();
    engine->shutdown(); // must return immediately, not re-post to a dead thread
    engine.reset();     // destructor must join without deadlocking
    SUCCEED();
}

TEST(AudioEngine, SeekWhilePausedUpdatesPositionImmediately)
{
    AudioEngine engine;
    auto stats = std::make_shared<SinkStats>();
    installCounting(engine, stats);
    engine.setProject(Project{});

    engine.seek(1'500'000);
    EXPECT_EQ(engine.positionUs(), 1'500'000);

    engine.play(1'500'000);
    QTest::qWait(30);
    EXPECT_GE(engine.positionUs(), 1'500'000);
    engine.pause();
    engine.shutdown();
}

TEST(AudioSink, NullSinkCountsFramesAndCanPace)
{
    NullAudioSink sink;
    EXPECT_EQ(sink.sampleRate(), 48000);
    EXPECT_EQ(sink.channels(), 2);
    EXPECT_FALSE(sink.deviceLost());
    ASSERT_TRUE(sink.start(nullptr));

    const float block[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    EXPECT_EQ(sink.write(block, 2), 2);
    EXPECT_EQ(sink.playedFrames(), 2);
    sink.flush(); // no internal buffer: flush keeps the played count
    EXPECT_EQ(sink.playedFrames(), 2);

    sink.setPaceRealTime(true);
    QElapsedTimer timer;
    timer.start();
    EXPECT_EQ(sink.write(block, 480), 480); // 10 ms at 48 kHz
    EXPECT_GE(timer.elapsed(), 5);

    sink.stop();
    EXPECT_FALSE(sink.running());
}

TEST(WasapiOutput, FloatToPcm16ClampsAndRounds)
{
    const float input[] = {0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f};
    std::int16_t out[6] = {};
    floatToPcm16(input, out, 6);
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 32767);
    EXPECT_EQ(out[2], -32767);
    EXPECT_EQ(out[3], 32767);   // clamped
    EXPECT_EQ(out[4], -32767);  // clamped
    EXPECT_EQ(out[5], 16384);   // rounded from 16383.5
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
