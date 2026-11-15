#include <gtest/gtest.h>

#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "AppController.h"
#include "AudioEngine.h"
#include "AudioSink.h"
#include "MediaListModel.h"
#include "PreviewController.h"
#include "ProjectStore.h"

using namespace VideoEditor;

namespace {

QString fixture(const QString &name)
{
    return QDir(QStringLiteral(VIDEOEDITOR_FIXTURE_DIR)).filePath(name);
}

// Keep the app-level tests off the real WASAPI device.
AudioEngine::SinkFactory nullSinkFactory()
{
    return [](int sampleRate, int channels, QString *) {
        return std::unique_ptr<IAudioSink>(
            new NullAudioSink(sampleRate, channels));
    };
}

void writeRecovery(const QString &path)
{
    Project project;
    Asset asset;
    asset.id = QStringLiteral("asset-1");
    asset.path = QStringLiteral("C:/media/example.mp4");
    asset.info.path = asset.path;
    asset.info.supported = true;
    project.assets.append(asset);

    ProjectStore store(path);
    QString error;
    ASSERT_TRUE(store.saveRecovery(project, &error)) << error.toStdString();
}

} // namespace

TEST(AppController, ImportsSupportedFile)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST(AppController, ImportsAudioOnlyAsset)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString path = fixture(QStringLiteral("bgm.mp3"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_TRUE(controller.lastError().isEmpty());
    const QVariant codec = controller.mediaModel()->data(
        controller.mediaModel()->index(0, 0), MediaListModel::CodecRole);
    EXPECT_TRUE(codec.toString().contains(QStringLiteral("音频")));
}

TEST(AppController, SelftestTimelineBuildsThreeVideoTwoAudioOneSubtitleProject)
{
    constexpr qint64 kPerfSourceDurationUs = 5'000'000;
    constexpr int kVideoTracks = 3;
    constexpr int kAudioTracks = 2;
    constexpr int kClipsPerVideoTrack = 2;
    const QString video = fixture(QStringLiteral("perf1080.mp4"));
    const QString tone = fixture(QStringLiteral("tone.wav"));
    const QString bgm = fixture(QStringLiteral("bgm.mp3"));
    if (!QFileInfo::exists(video) || !QFileInfo::exists(tone)
        || !QFileInfo::exists(bgm)) {
        GTEST_SKIP() << "missing fixtures";
    }

    // No VideoPreviewItem is attached in a unit test, so this builds the
    // fixture project without opening the 1080p preview compositor.
    AppController controller(nullptr, {}, nullSinkFactory());
    controller.selftestTimeline();

    const Project &project = controller.document()->project();
    int videoTracks = 0;
    int audioTracks = 0;
    int subtitleTracks = 0;
    int videoClips = 0;
    int audioClips = 0;
    int64_t videoCoverageEndUs = 0;
    for (const Track &track : project.tracks) {
        switch (track.type) {
        case TrackType::Video:
            ++videoTracks;
            videoClips += int(track.clips.size());
            // Each video track repeats the five-second 1080p fixture so the
            // perf gate never measures empty gaps with no video layers active.
            for (const Clip &clip : track.clips) {
                EXPECT_EQ(clip.sourceOutUs - clip.sourceInUs,
                          kPerfSourceDurationUs);
                videoCoverageEndUs = qMax(videoCoverageEndUs, clip.timelineEndUs());
            }
            break;
        case TrackType::Audio:
            ++audioTracks;
            audioClips += int(track.clips.size());
            break;
        case TrackType::Subtitle:
            ++subtitleTracks;
            break;
        }
    }

    EXPECT_EQ(videoTracks, kVideoTracks);
    EXPECT_EQ(audioTracks, kAudioTracks);
    EXPECT_EQ(subtitleTracks, 1);
    EXPECT_EQ(videoClips, kVideoTracks * kClipsPerVideoTrack);
    EXPECT_EQ(audioClips, kAudioTracks);
    EXPECT_GE(videoCoverageEndUs, kClipsPerVideoTrack * kPerfSourceDurationUs);
    const Track *subtitle =
        controller.document()->trackById(QStringLiteral("track-s1"));
    ASSERT_NE(subtitle, nullptr);
    EXPECT_EQ(subtitle->cues.size(), 3);
}

TEST(AppController, ReportsMissingAudioOutputForSilentPreview)
{
    // Device open failure must surface the spec's silent-preview message
    // instead of leaving the timeline mysteriously quiet.
    const auto failingSink =
        [](int, int, QString *error) -> std::unique_ptr<IAudioSink> {
        if (error)
            *error = QStringLiteral("测试输出设备不可用。");
        return {};
    };
    AppController controller(nullptr, {}, failingSink);

    EXPECT_FALSE(controller.audioEngine()->hasOutputDevice());
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("无音频输出")));
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("静音预览")));
}

TEST(AppController, ReportsAudioDeviceLostDuringSession)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    EXPECT_TRUE(controller.audioEngine()->hasOutputDevice());
    EXPECT_TRUE(controller.lastError().isEmpty());

    controller.audioEngine()->setSinkFactoryForTest(
        [](int, int, QString *error) -> std::unique_ptr<IAudioSink> {
            if (error)
                *error = QStringLiteral("设备已断开。");
            return {};
        });
    QCoreApplication::processEvents();

    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("无音频输出")));
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("静音预览")));
}

TEST(AppController, ReportsUnsupportedOrMissingFile)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("corrupt.mp4")))});
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("无法打开")));

    controller.clearError();
    controller.importUrls({QUrl::fromLocalFile(fixture(QStringLiteral("missing.mp4")))});
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("不存在")));
}

TEST(AppController, UndoRemovesImportedAsset)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    controller.undo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 0);
    controller.redo();
    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
}

TEST(AppController, SavesAndReopensProject)
{
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString projectPath = dir.filePath(QStringLiteral("project"));

    {
        AppController controller(nullptr, {}, nullSinkFactory());
        controller.importUrls({QUrl::fromLocalFile(path)});
        ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

        // No suffix: saveProject must append .veproj.
        controller.saveProject(QUrl::fromLocalFile(projectPath));
        EXPECT_TRUE(controller.lastError().isEmpty());
        EXPECT_TRUE(QFileInfo::exists(projectPath + QStringLiteral(".veproj")));
    }

    AppController reopened(nullptr, {}, nullSinkFactory());
    reopened.openProject(
        QUrl::fromLocalFile(projectPath + QStringLiteral(".veproj")));
    EXPECT_TRUE(reopened.lastError().isEmpty());
    ASSERT_EQ(reopened.mediaModel()->rowCount(), 1);

    const QModelIndex index = reopened.mediaModel()->index(0, 0);
    ASSERT_TRUE(index.isValid());
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::NameRole).toString(),
              QStringLiteral("h264.mp4"));
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::CodecRole).toString(),
              QStringLiteral("H.264"));
    EXPECT_EQ(reopened.mediaModel()->data(index, MediaListModel::PathRole).toString(),
              path);
}

TEST(AppController, OpeningProjectStopsCurrentPreview)
{
    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    AppController controller(nullptr,
                             dir.filePath(QStringLiteral("recovery.json")),
                             nullSinkFactory());
    controller.importUrls({QUrl::fromLocalFile(media)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

    controller.openAsset(controller.document()->project().assets.first().id);
    ASSERT_EQ(controller.preview()->mediaName(), QStringLiteral("h264.mp4"));

    const QString projectPath = dir.filePath(QStringLiteral("project"));
    controller.saveProject(QUrl::fromLocalFile(projectPath));
    ASSERT_TRUE(controller.lastError().isEmpty());

    // Loading another project replaces the document and must clear the old
    // preview instead of leaving its last frame or playback behind.
    controller.openProject(
        QUrl::fromLocalFile(projectPath + QStringLiteral(".veproj")));
    EXPECT_TRUE(controller.lastError().isEmpty());
    EXPECT_TRUE(controller.preview()->mediaName().isEmpty());
    EXPECT_TRUE(controller.preview()->decodeMode().isEmpty());
    EXPECT_EQ(controller.preview()->positionMs(), 0);
    EXPECT_EQ(controller.preview()->durationMs(), 0);
    EXPECT_FALSE(controller.preview()->playing());
}

TEST(AppController, RestoresRecoveryAtStartup)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery, nullSinkFactory());
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.restoreRecovery();

    EXPECT_EQ(controller.mediaModel()->rowCount(), 1);
    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
    EXPECT_TRUE(controller.lastError().isEmpty());
}

TEST(AppController, DiscardRecoveryRemovesFileAndClearsPrompt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery, nullSinkFactory());
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.discardRecovery();

    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, SuccessfulSaveDiscardsExistingRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    AppController controller(nullptr, recovery, nullSinkFactory());
    ASSERT_TRUE(controller.recoveryAvailable());

    controller.importUrls({QUrl::fromLocalFile(media)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);
    ASSERT_TRUE(controller.document()->isDirty());

    controller.saveProject(QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved"))));

    EXPECT_TRUE(controller.lastError().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, CleanExitDiscardsStaleRecoveryWithoutPendingPrompt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));

    // No recovery file at startup, so no prompt is pending.
    AppController controller(nullptr, recovery, nullSinkFactory());
    ASSERT_FALSE(controller.recoveryAvailable());
    ASSERT_FALSE(controller.document()->isDirty());

    // Simulate an autosave written later in the session, after the startup
    // prompt check, that the user has since made obsolete.
    writeRecovery(recovery);
    ASSERT_TRUE(QFileInfo::exists(recovery));

    controller.discardRecoveryIfClean();

    EXPECT_FALSE(QFileInfo::exists(recovery));
    EXPECT_FALSE(controller.recoveryAvailable());
}

TEST(AppController, CleanExitKeepsPendingRecovery)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    writeRecovery(recovery);

    AppController controller(nullptr, recovery, nullSinkFactory());
    ASSERT_TRUE(controller.recoveryAvailable());
    ASSERT_FALSE(controller.document()->isDirty());

    // Ignoring the modal must not destroy the snapshot the user was offered.
    controller.discardRecoveryIfClean();

    EXPECT_TRUE(QFileInfo::exists(recovery));
    EXPECT_TRUE(controller.recoveryAvailable());
}

TEST(PreviewController, CloseMediaClearsPreviewState)
{
    const QString media = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(media))
        GTEST_SKIP() << "missing fixture";

    PreviewController controller;
    controller.openMedia(media);
    EXPECT_EQ(controller.mediaName(), QStringLiteral("h264.mp4"));

    controller.closeMedia();

    EXPECT_TRUE(controller.mediaName().isEmpty());
    EXPECT_TRUE(controller.decodeMode().isEmpty());
    EXPECT_EQ(controller.positionMs(), 0);
    EXPECT_EQ(controller.durationMs(), 0);
    EXPECT_FALSE(controller.playing());
}

TEST(AppController, CorruptRecoveryKeepsPromptAndReportsError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString recovery = dir.filePath(QStringLiteral("project.autosave.json"));
    QFile file(recovery);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{ this is not valid json");
    file.close();

    AppController controller(nullptr, recovery, nullSinkFactory());
    EXPECT_TRUE(controller.recoveryAvailable());

    controller.restoreRecovery();

    EXPECT_FALSE(controller.lastError().isEmpty());
    EXPECT_TRUE(controller.lastError().contains(QStringLiteral("工程文件")));
    EXPECT_TRUE(QFileInfo::exists(recovery));
    EXPECT_TRUE(controller.recoveryAvailable());
}

TEST(MediaListModel, ExposesRolesForImportedFixture)
{
    AppController controller(nullptr, {}, nullSinkFactory());
    const QString path = fixture(QStringLiteral("h264.mp4"));
    if (!QFile::exists(path))
        GTEST_SKIP() << "missing fixture";

    controller.importUrls({QUrl::fromLocalFile(path)});
    ASSERT_EQ(controller.mediaModel()->rowCount(), 1);

    const QModelIndex index = controller.mediaModel()->index(0, 0);
    ASSERT_TRUE(index.isValid());
    const auto role = [&controller, index](int role) {
        return controller.mediaModel()->data(index, role);
    };

    EXPECT_EQ(role(MediaListModel::NameRole).toString(), QStringLiteral("h264.mp4"));
    EXPECT_EQ(role(MediaListModel::PathRole).toString(), path);
    EXPECT_EQ(role(MediaListModel::CodecRole).toString(), QStringLiteral("H.264"));
    EXPECT_EQ(role(MediaListModel::ResolutionRole).toString(), QStringLiteral("640×360"));
    EXPECT_EQ(role(MediaListModel::DurationRole).toString(), QStringLiteral("00:00:02.000"));
    EXPECT_TRUE(role(MediaListModel::ProxyPathRole).toString().isEmpty());
}
