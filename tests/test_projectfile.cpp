#include <QtTest>

#include <sndfile.h>

#include "AudioFileReader.h"
#include "AudioFileWriter.h"
#include "ProjectFile.h"

using namespace zrecord;

namespace {

// Values a real session produces: ordinary audio, full scale, and the
// over-full-scale samples that gain or echo leave in a clip.
const std::vector<float> kLoudSamples = {0.5f, 0.99f, 1.0f, 1.2f, 1.5f, -1.5f, 2.0f, -3.0f};

// Project has a mutex, so it can't be returned by value; build in place.
void fill(Project& project, const std::vector<float>& samples) {
    project.channels = 1;
    project.sampleRate = 44100.0;
    Track track;
    track.name = "T";
    Clip clip;
    clip.channels = 1;
    clip.samples = samples;
    track.clips.push_back(clip);
    project.tracks.push_back(track);
}

bool writeRaw(const QString& path, const std::vector<float>& samples, int format) {
    SF_INFO info{};
    info.samplerate = 44100;
    info.channels = 1;
    info.format = format;
    SNDFILE* file = sf_open(path.toStdString().c_str(), SFM_WRITE, &info);
    if (file == nullptr) {
        return false;
    }
    sf_writef_float(file, samples.data(), static_cast<sf_count_t>(samples.size()));
    sf_close(file);
    return true;
}

} // namespace

class TestProjectFile : public QObject {
    Q_OBJECT

private slots:
    void saveKeepsSamplesBeyondFullScale();
    void savedClipsAreFloatWav();
    void loadsLegacy24BitProjects();
    void integerExportClipsInsteadOfWrapping();
    void failedLoadLeavesProjectUntouched();
    void loadResamplesClipsAtAnotherRate();
};

void TestProjectFile::saveKeepsSamplesBeyondFullScale() {
    // Regression: clips were saved as 24-bit PCM without clipping, so 1.2
    // reopened as -0.8 and 2.0 as 0.0 -- loud takes came back as crackle.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath("loud.zrproj").toStdString();

    Project saved;
    fill(saved, kLoudSamples);
    std::string error;
    QVERIFY2(ProjectFile::save(saved, path, error), error.c_str());

    Project loaded;
    QVERIFY2(ProjectFile::load(loaded, path, error), error.c_str());
    QCOMPARE(loaded.tracks.size(), size_t(1));
    QCOMPARE(loaded.tracks[0].clips.size(), size_t(1));
    // Exact: a project must reopen as it was saved.
    QCOMPARE(loaded.tracks[0].clips[0].samples, kLoudSamples);
}

void TestProjectFile::savedClipsAreFloatWav() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("f.zrproj");
    Project saved;
    fill(saved, kLoudSamples);
    std::string error;
    QVERIFY2(ProjectFile::save(saved, path.toStdString(), error), error.c_str());

    SF_INFO info{};
    SNDFILE* file = sf_open((path + "/audio/track0_clip0.wav").toStdString().c_str(), SFM_READ, &info);
    QVERIFY(file != nullptr);
    sf_close(file);
    QCOMPARE(info.format & SF_FORMAT_TYPEMASK, SF_FORMAT_WAV);
    QCOMPARE(info.format & SF_FORMAT_SUBMASK, SF_FORMAT_FLOAT);
}

void TestProjectFile::loadsLegacy24BitProjects() {
    // Projects saved before clips became float hold 24-bit PCM clips; they
    // must keep opening.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath("old.zrproj/audio");
    const QString root = dir.filePath("old.zrproj");
    const std::vector<float> samples = {0.0f, 0.25f, -0.5f, 0.75f};
    QVERIFY(writeRaw(root + "/audio/track0_clip0.wav", samples, SF_FORMAT_WAV | SF_FORMAT_PCM_24));

    QFile json(root + "/project.json");
    QVERIFY(json.open(QIODevice::WriteOnly));
    json.write(R"({"sampleRate":44100,"channels":1,"labels":[],
        "tracks":[{"name":"Old","muted":false,"soloed":false,"gainDb":0,"envelope":[],"display":"waveform",
                   "clips":[{"file":"audio/track0_clip0.wav","startFrame":"100"}]}]})");
    json.close();

    Project loaded;
    std::string error;
    QVERIFY2(ProjectFile::load(loaded, root.toStdString(), error), error.c_str());
    QCOMPARE(loaded.tracks.size(), size_t(1));
    const Clip& clip = loaded.tracks[0].clips[0];
    QCOMPARE(clip.startFrame, int64_t(100));
    QCOMPARE(clip.samples.size(), samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        QVERIFY2(std::fabs(clip.samples[i] - samples[i]) < 1e-6f, qPrintable(QString::number(i)));
    }
}

void TestProjectFile::integerExportClipsInsteadOfWrapping() {
    // Integer-PCM exports can't hold >1.0, but they must clip, never wrap.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = dir.filePath("x.wav").toStdString();
    std::string error;
    QVERIFY2(AudioFileWriter::write(path, kLoudSamples, 44100, 1, AudioFormat::Wav, error), error.c_str());

    std::vector<float> back;
    int rate = 0;
    int channels = 0;
    QVERIFY2(AudioFileReader::read(path, back, rate, channels, error), error.c_str());
    QCOMPARE(back.size(), kLoudSamples.size());
    for (size_t i = 0; i < back.size(); ++i) {
        const float expected = std::clamp(kLoudSamples[i], -1.0f, 1.0f);
        QVERIFY2(std::fabs(back[i] - expected) < 1e-4f,
                 qPrintable(QString("%1 -> %2").arg(kLoudSamples[i]).arg(back[i])));
    }
}

void TestProjectFile::failedLoadLeavesProjectUntouched() {
    // Regression: a clip that failed to read aborted load() after it had
    // already cleared the target, so a failed Open wiped the open project.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("broken.zrproj");
    {
        Project other;
        fill(other, {0.1f, 0.2f});
        other.channels = 1;
        other.sampleRate = 22050.0;
        std::string error;
        QVERIFY2(ProjectFile::save(other, path.toStdString(), error), error.c_str());
    }
    QVERIFY(QFile::remove(path + "/audio/track0_clip0.wav"));

    Project project;
    fill(project, {0.5f, -0.5f, 0.25f});
    project.channels = 2;
    project.sampleRate = 48000.0;
    project.tracks.push_back(Track{});
    project.labels.push_back(Label{10, 20, "keep"});
    project.selection.trackIndex = 0;
    project.selection.startFrame = 0;
    project.selection.endFrame = 1;
    project.playheadFrame = 7;
    project.clipboard = {0.3f, 0.3f};

    std::string error;
    QVERIFY(!ProjectFile::load(project, path.toStdString(), error));
    QVERIFY(!error.empty());

    QCOMPARE(project.channels, 2);
    QCOMPARE(project.sampleRate, 48000.0);
    QCOMPARE(project.tracks.size(), size_t(2));
    QCOMPARE(project.tracks[0].clips.size(), size_t(1));
    QCOMPARE(project.tracks[0].clips[0].samples, (std::vector<float>{0.5f, -0.5f, 0.25f}));
    QCOMPARE(project.labels.size(), size_t(1));
    QCOMPARE(project.labels[0].text, std::string("keep"));
    QCOMPARE(project.selection.trackIndex, 0);
    QCOMPARE(project.selection.endFrame, int64_t(1));
    QCOMPARE(project.playheadFrame, int64_t(7));
    QCOMPARE(project.clipboard.size(), size_t(2));
}

void TestProjectFile::loadResamplesClipsAtAnotherRate() {
    // A clip whose file rate differs from the project's used to be loaded
    // as-is and played at the wrong speed.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir(dir.path()).mkpath("mixed.zrproj/audio");
    const QString root = dir.filePath("mixed.zrproj");
    QVERIFY(writeRaw(root + "/audio/a.wav", std::vector<float>(22050, 0.25f), SF_FORMAT_WAV | SF_FORMAT_FLOAT));
    // writeRaw writes 44.1 kHz; claim the project is 88.2 kHz.
    QFile json(root + "/project.json");
    QVERIFY(json.open(QIODevice::WriteOnly));
    json.write(R"({"sampleRate":88200,"channels":1,"labels":[],
        "tracks":[{"name":"T","clips":[{"file":"audio/a.wav","startFrame":"0"}]}]})");
    json.close();

    Project loaded;
    std::string error;
    QVERIFY2(ProjectFile::load(loaded, root.toStdString(), error), error.c_str());
    const int64_t frames = loaded.tracks[0].clips[0].frameCount();
    QVERIFY2(std::llabs(frames - 44100) <= 2, qPrintable(QString::number(frames))); // still 0.5 s
}

QTEST_GUILESS_MAIN(TestProjectFile)
#include "test_projectfile.moc"
