#include "FlowPropagator.h"

#include <algorithm>
#include <cmath>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/optflow.hpp>

#include "Confidence.h"

namespace VideoEditor {

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

    const QImage previousGrayImage =
        previous.convertToFormat(QImage::Format_Grayscale8);
    const QImage currentGrayImage =
        current.convertToFormat(QImage::Format_Grayscale8);
    if (previousGrayImage.isNull() || currentGrayImage.isNull()) {
        result.error = QStringLiteral("无法转换光流输入灰度图。");
        return result;
    }

    const cv::Mat previousGray(previousGrayImage.height(),
                               previousGrayImage.width(), CV_8UC1,
                               const_cast<uchar *>(previousGrayImage.constBits()),
                               previousGrayImage.bytesPerLine());
    const cv::Mat currentGray(currentGrayImage.height(),
                              currentGrayImage.width(), CV_8UC1,
                              const_cast<uchar *>(currentGrayImage.constBits()),
                              currentGrayImage.bytesPerLine());

    try {
        cv::Mat forwardFlow;
        m_impl->dis->calc(previousGray, currentGray, forwardFlow);
        cv::Mat backwardFlow;
        m_impl->dis->calc(currentGray, previousGray, backwardFlow);

        // back(u,v) points at the previous frame pixel that maps to (u,v) in
        // the current frame, so sampling previousMask at (u + back) warps the
        // mask forward.
        cv::Mat mapX(currentGray.size(), CV_32FC1);
        cv::Mat mapY(currentGray.size(), CV_32FC1);
        for (int y = 0; y < currentGray.rows; ++y) {
            const cv::Vec2f *back = backwardFlow.ptr<cv::Vec2f>(y);
            float *mapXRow = mapX.ptr<float>(y);
            float *mapYRow = mapY.ptr<float>(y);
            for (int x = 0; x < currentGray.cols; ++x) {
                mapXRow[x] = float(x) + back[x][0];
                mapYRow[x] = float(y) + back[x][1];
            }
        }

        cv::Mat previousMaskMat(previousMask.height, previousMask.width,
                                CV_8UC1,
                                const_cast<uint8_t *>(previousMask.bits.data()));
        cv::Mat warped;
        cv::remap(previousMaskMat, warped, mapX, mapY, cv::INTER_LINEAR,
                  cv::BORDER_CONSTANT, cv::Scalar(0));

        BinaryMask newMask;
        newMask.width = current.width();
        newMask.height = current.height();
        newMask.bits.assign(size_t(newMask.width) * size_t(newMask.height), 0);
        int area = 0;
        for (int y = 0; y < warped.rows; ++y) {
            const uchar *row = warped.ptr<uchar>(y);
            uint8_t *out = newMask.bits.data() + size_t(y) * newMask.width;
            for (int x = 0; x < warped.cols; ++x) {
                if (row[x] >= 128) {
                    out[x] = 255;
                    ++area;
                }
            }
        }

        double sumMagnitude = 0.0;
        double maxMagnitude = 0.0;
        const int pixels = forwardFlow.rows * forwardFlow.cols;
        for (int y = 0; y < forwardFlow.rows; ++y) {
            const cv::Vec2f *row = forwardFlow.ptr<cv::Vec2f>(y);
            for (int x = 0; x < forwardFlow.cols; ++x) {
                const double magnitude =
                    std::sqrt(double(row[x][0]) * row[x][0]
                              + double(row[x][1]) * row[x][1]);
                sumMagnitude += magnitude;
                maxMagnitude = std::max(maxMagnitude, magnitude);
            }
        }

        const int previousArea =
            int(std::count_if(previousMask.bits.begin(),
                              previousMask.bits.end(),
                              [](uint8_t value) { return value != 0; }));

        result.ok = true;
        result.mask = std::move(newMask);
        result.meanFlowPx = pixels > 0
            ? float(sumMagnitude / double(pixels))
            : 0.0f;
        result.maxFlowPx = float(maxMagnitude);
        result.areaPx = area;
        result.areaChangePercent = areaChangePercent(area, previousArea);
    } catch (const cv::Exception &exception) {
        result.error = QStringLiteral("光流传播失败：%1")
                           .arg(QString::fromUtf8(exception.what()));
    }
    return result;
}

} // namespace VideoEditor
