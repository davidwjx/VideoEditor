#include "FlowPropagator.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/optflow.hpp>

#include "Confidence.h"

namespace VideoEditor {

namespace {

cv::Mat grayMat(const QImage &image)
{
    const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
    if (gray.isNull())
        return {};
    return cv::Mat(gray.height(), gray.width(), CV_8UC1,
                   const_cast<uchar *>(gray.constBits()), gray.bytesPerLine())
        .clone();
}

cv::Mat maskMat(const BinaryMask &mask)
{
    return cv::Mat(mask.height, mask.width, CV_8UC1,
                   const_cast<uint8_t *>(mask.bits.data()),
                   size_t(mask.width))
        .clone();
}

int maskArea(const BinaryMask &mask)
{
    return int(std::count_if(mask.bits.begin(), mask.bits.end(),
                             [](uint8_t value) { return value != 0; }));
}

} // namespace

struct FlowPropagator::Impl {
    cv::Ptr<cv::optflow::DISOpticalFlow> dis;
    // Cached (x, y) pixel grid reused by every propagation at a given size, so
    // the per-frame map construction is one vectorized add instead of a scalar
    // loop.
    cv::Mat baseGrid;
    cv::Size baseSize;

    Impl()
    {
        // PRESET_FAST keeps the K=5 propagation IoU above the 0.90 gate while
        // reducing the 1080p DIS cost; verified by TrackingPipelineTest.
        dis = cv::optflow::createOptFlow_DIS(
            cv::optflow::DISOpticalFlow::PRESET_FAST);
    }

    const cv::Mat &gridFor(cv::Size size)
    {
        if (baseSize != size || baseGrid.empty()) {
            baseGrid.create(size.height, size.width, CV_32FC2);
            for (int y = 0; y < size.height; ++y) {
                cv::Vec2f *row = baseGrid.ptr<cv::Vec2f>(y);
                for (int x = 0; x < size.width; ++x)
                    row[x] = cv::Vec2f(float(x), float(y));
            }
            baseSize = size;
        }
        return baseGrid;
    }
};

FlowPropagator::FlowPropagator()
    : m_impl(std::make_unique<Impl>())
{
}

FlowPropagator::~FlowPropagator() = default;

namespace {

// Samples `sourceMask` on the destination grid at (x + flow.x, y + flow.y).
// One DIS pass provides both the warp and the flow-magnitude metrics; the
// inverse field is an adequate proxy for confidence gating.
FlowPropagationResult warpWithFlow(const QImage &destination,
                                   const BinaryMask &sourceMask,
                                   const cv::Mat &flow,
                                   const cv::Mat &baseGrid)
{
    FlowPropagationResult result;

    // (x,y) + flow in one vectorized pass, then a single CV_32FC2 remap.
    cv::Mat map;
    cv::add(flow, baseGrid, map);
    const cv::Mat source = maskMat(sourceMask);
    cv::Mat warped;
    cv::remap(source, warped, map, cv::noArray(), cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar(0));

    // Threshold the whole warp at once, then copy the 0/255 bytes directly.
    cv::Mat binary;
    cv::compare(warped, 128, binary, cv::CMP_GE);
    const int area = cv::countNonZero(binary);

    BinaryMask mask;
    mask.width = destination.width();
    mask.height = destination.height();
    mask.bits.assign(size_t(mask.width) * size_t(mask.height), 0);
    for (int y = 0; y < binary.rows; ++y) {
        std::memcpy(mask.bits.data() + size_t(y) * size_t(mask.width),
                    binary.ptr<uchar>(y), size_t(mask.width));
    }

    // Flow magnitude metrics without a per-pixel sqrt loop.
    std::vector<cv::Mat> channels;
    cv::split(flow, channels);
    cv::Mat magnitude;
    cv::magnitude(channels[0], channels[1], magnitude);
    cv::Scalar mean;
    cv::Scalar stddev;
    cv::meanStdDev(magnitude, mean, stddev);
    double maxMagnitude = 0.0;
    cv::minMaxLoc(magnitude, nullptr, &maxMagnitude);
    const int pixels = flow.rows * flow.cols;

    result.ok = true;
    result.mask = std::move(mask);
    result.meanFlowPx = pixels > 0 ? float(mean[0]) : 0.0f;
    result.maxFlowPx = float(maxMagnitude);
    result.areaPx = area;
    result.areaChangePercent = areaChangePercent(area, maskArea(sourceMask));
    return result;
}

} // namespace

FlowPropagationResult FlowPropagator::propagate(const QImage &previous,
                                                const QImage &current,
                                                const BinaryMask &previousMask)
{
    FlowPropagationResult result;
    if (previous.isNull() || current.isNull()) {
        result.error = QStringLiteral("光流传播输入帧为空。");
        return result;
    }
    if (previous.size() != current.size()) {
        result.error = QStringLiteral("光流传播前后帧尺寸不一致。");
        return result;
    }
    if (!previousMask.isValid()
        || previousMask.width != previous.width()
        || previousMask.height != previous.height()) {
        result.error = QStringLiteral("光流传播输入的遮罩尺寸与画面不一致。");
        return result;
    }

    const cv::Mat previousGray = grayMat(previous);
    const cv::Mat currentGray = grayMat(current);
    if (previousGray.empty() || currentGray.empty()) {
        result.error = QStringLiteral("无法转换光流输入灰度图。");
        return result;
    }

    try {
        // current -> previous displacement: sampling the previous mask at
        // (x + flow) warps it forward onto the current frame.
        cv::Mat flow;
        m_impl->dis->calc(currentGray, previousGray, flow);
        return warpWithFlow(current, previousMask, flow,
                            m_impl->gridFor(flow.size()));
    } catch (const cv::Exception &exception) {
        result.error = QStringLiteral("光流传播失败：%1")
                           .arg(QString::fromUtf8(exception.what()));
    }
    return result;
}

FlowPropagationResult FlowPropagator::propagateBackward(
    const QImage &previous, const QImage &current,
    const BinaryMask &currentMask)
{
    FlowPropagationResult result;
    if (previous.isNull() || current.isNull()) {
        result.error = QStringLiteral("光流传播输入帧为空。");
        return result;
    }
    if (previous.size() != current.size()) {
        result.error = QStringLiteral("光流传播前后帧尺寸不一致。");
        return result;
    }
    if (!currentMask.isValid() || currentMask.width != current.width()
        || currentMask.height != current.height()) {
        result.error = QStringLiteral("光流传播输入的遮罩尺寸与画面不一致。");
        return result;
    }

    const cv::Mat previousGray = grayMat(previous);
    const cv::Mat currentGray = grayMat(current);
    if (previousGray.empty() || currentGray.empty()) {
        result.error = QStringLiteral("无法转换光流输入灰度图。");
        return result;
    }

    try {
        // previous -> current displacement: sampling the current mask at
        // (x + flow) warps it backward onto the previous frame.
        cv::Mat flow;
        m_impl->dis->calc(previousGray, currentGray, flow);
        return warpWithFlow(previous, currentMask, flow,
                            m_impl->gridFor(flow.size()));
    } catch (const cv::Exception &exception) {
        result.error = QStringLiteral("光流传播失败：%1")
                           .arg(QString::fromUtf8(exception.what()));
    }
    return result;
}

} // namespace VideoEditor
