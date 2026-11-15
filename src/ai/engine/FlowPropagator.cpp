#include "FlowPropagator.h"

#include <algorithm>
#include <cmath>

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

    Impl()
    {
        dis = cv::optflow::createOptFlow_DIS(
            cv::optflow::DISOpticalFlow::PRESET_MEDIUM);
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
                                   const cv::Mat &flow)
{
    FlowPropagationResult result;
    cv::Mat mapX(destination.height(), destination.width(), CV_32FC1);
    cv::Mat mapY(destination.height(), destination.width(), CV_32FC1);
    for (int y = 0; y < flow.rows; ++y) {
        const cv::Vec2f *row = flow.ptr<cv::Vec2f>(y);
        float *mapXRow = mapX.ptr<float>(y);
        float *mapYRow = mapY.ptr<float>(y);
        for (int x = 0; x < flow.cols; ++x) {
            mapXRow[x] = float(x) + row[x][0];
            mapYRow[x] = float(y) + row[x][1];
        }
    }

    const cv::Mat source = maskMat(sourceMask);
    cv::Mat warped;
    cv::remap(source, warped, mapX, mapY, cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar(0));

    BinaryMask mask;
    mask.width = destination.width();
    mask.height = destination.height();
    mask.bits.assign(size_t(mask.width) * size_t(mask.height), 0);
    int area = 0;
    for (int y = 0; y < warped.rows; ++y) {
        const uchar *row = warped.ptr<uchar>(y);
        uint8_t *out = mask.bits.data() + size_t(y) * mask.width;
        for (int x = 0; x < warped.cols; ++x) {
            if (row[x] >= 128) {
                out[x] = 255;
                ++area;
            }
        }
    }

    double sumMagnitude = 0.0;
    double maxMagnitude = 0.0;
    const int pixels = flow.rows * flow.cols;
    for (int y = 0; y < flow.rows; ++y) {
        const cv::Vec2f *row = flow.ptr<cv::Vec2f>(y);
        for (int x = 0; x < flow.cols; ++x) {
            const double magnitude =
                std::sqrt(double(row[x][0]) * row[x][0]
                          + double(row[x][1]) * row[x][1]);
            sumMagnitude += magnitude;
            maxMagnitude = std::max(maxMagnitude, magnitude);
        }
    }

    result.ok = true;
    result.mask = std::move(mask);
    result.meanFlowPx =
        pixels > 0 ? float(sumMagnitude / double(pixels)) : 0.0f;
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
        return warpWithFlow(current, previousMask, flow);
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
        return warpWithFlow(previous, currentMask, flow);
    } catch (const cv::Exception &exception) {
        result.error = QStringLiteral("光流传播失败：%1")
                           .arg(QString::fromUtf8(exception.what()));
    }
    return result;
}

} // namespace VideoEditor
