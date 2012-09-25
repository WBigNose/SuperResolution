// Copyright (c) 2012, Vladislav Vinogradov (jet47)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "nlm_based.hpp"
#include <opencv2/core/internal.hpp>
#include "extract_patch.hpp"

using namespace std;
using namespace cv;
using namespace cv::superres;

CV_INIT_ALGORITHM(NlmBased, "VideoSuperResolution.NlmBased",
                  obj.info()->addParam(obj, "scale", obj.scale, false, 0, 0,
                                       "Scale factor.");
                  obj.info()->addParam(obj, "searchAreaRadius", obj.searchAreaRadius, false, 0, 0,
                                       "Radius of the patch search area in low resolution image.");
                  obj.info()->addParam(obj, "timeRadius", obj.timeRadius, false, 0, 0,
                                       "Radius of the time search area.");
                  obj.info()->addParam(obj, "patchSize", obj.patchSize, false, 0, 0,
                                       "Size of tha patch at in high resolution image.");
                  obj.info()->addParam(obj, "sigma", obj.sigma, false, 0, 0,
                                       "Weight of the patch difference"));

bool NlmBased::init()
{
    return !NlmBased_info_auto.name().empty();
}

Ptr<VideoSuperResolution> NlmBased::create()
{
    return Ptr<VideoSuperResolution>(new NlmBased);
}

NlmBased::NlmBased()
{
    scale = 2;
    searchAreaRadius = 10;
    timeRadius = 2;
    patchSize = 13;
    sigma = 7.5;
}

namespace
{
#ifdef HAVE_TBB
    typedef tbb::blocked_range<int> Range1D;
    typedef tbb::blocked_range2d<int> Range2D;

    template <class Body>
    void loop2D(const Range2D& range, const Body& body)
    {
        tbb::parallel_for(range, body);
    }
#else
    class Range1D
    {
    public:
        Range1D(int begin, int end) : begin_(begin), end_(end) {}

        int begin() const { return begin_; }
        int end() const { return end_; }

    private:
        int begin_, end_;
    };

    class Range2D
    {
    public:
        Range2D(int row_begin, int row_end, int col_begin, int col_end) : rows_(row_begin, row_end), cols_(col_begin, col_end) {}

        const Range1D& rows() const { return rows_; }
        const Range1D& cols() const { return cols_; }

    private:
        Range1D rows_, cols_;
    };

    template <class Body>
    void loop2D(const Range2D& range, const Body& body)
    {
        body(range);
    }
#endif

    double calcNlmWeight(const Mat_<Vec3b>& patch1, const Mat_<Vec3b>& patch2, double patchDiffWeight)
    {
        CV_DbgAssert(patch1.size() == patch2.size());

        double patchDiff = 0.0;
        for (int y = 0; y < patch1.rows; ++y)
        {
            for (int x = 0; x < patch1.cols; ++x)
            {
                Vec3b val1 = patch1(y, x);
                Vec3b val2 = patch2(y, x);

                Point3d val1d(val1[0], val1[1], val1[2]);
                Point3d val2d(val2[0], val2[1], val2[2]);

                Point3d diff = val1d - val2d;

                patchDiff += diff.ddot(diff);
            }
        }

        return exp(-patchDiff * patchDiffWeight);
    }

    struct LoopBody
    {
        void operator ()(const Range2D& range) const;

        int scale;
        int searchAreaRadius;
        int timeRadius;
        int patchSize;
        double sigma;

        int procPos;

        vector<Mat>* y;
        vector<Mat>* Y;

        mutable Mat_<Point3d> V;
        mutable Mat_<Point3d> W;
    };

    void LoopBody::operator ()(const Range2D& range) const
    {
        const double patchDiffWeight = 1.0 / (2.0 * sigma * sigma);

        const Mat& Z = at(procPos, *Y);

        Point Z_loc;
        for (Z_loc.y = range.rows().begin(); Z_loc.y < range.rows().end(); ++Z_loc.y)
        {
            for (Z_loc.x = range.cols().begin(); Z_loc.x < range.cols().end(); ++Z_loc.x)
            {
                const Mat_<Vec3b> Z_patch = extractPatch<Vec3b>(Z, Z_loc, patchSize);

                for (int t = -timeRadius; t <= timeRadius; ++t)
                {
                    const Mat& Yt = at(procPos + t, *Y);
                    const Mat& yt = at(procPos + t, *y);

                    for (int i = -searchAreaRadius; i <= searchAreaRadius; ++i)
                    {
                        for (int j = -searchAreaRadius; j <= searchAreaRadius; ++j)
                        {
                            Point yt_loc(Z_loc.x / scale + j, Z_loc.y / scale + i);
                            Point Yt_loc(yt_loc.x * scale, yt_loc.y * scale);

                            const Mat_<Vec3b> Yt_patch = extractPatch<Vec3b>(Yt, Yt_loc, patchSize);

                            const double w = calcNlmWeight(Z_patch, Yt_patch, patchDiffWeight);

                            if (w >= 0.1)
                            {
                                Vec3b val = yt.at<Vec3b>(yt_loc);
                                Point3d vald(val[0], val[1], val[2]);

                                V(Z_loc) += w * vald;
                                W(Z_loc) += Point3d(w, w, w);
                            }
                        }
                    }
                }
            }
        }
    }
}

void NlmBased::initImpl(cv::Ptr<IFrameSource>& frameSource)
{
    y.resize(3 * timeRadius + 2);
    Y.resize(3 * timeRadius + 2);

    storePos = -1;
    procPos = storePos - 2 * timeRadius;
    outPos = procPos - timeRadius - 1;

    for (int t = -timeRadius; t <= 2 * timeRadius; ++t)
    {
        Mat frame = frameSource->nextFrame();

        CV_Assert(!frame.empty());

        addNewFrame(frame);
    }

    processFrame(procPos);
    for (int t = 1; t <= timeRadius; ++t)
        processFrame(procPos + t);
    processFrame(procPos);
}

Mat NlmBased::processImpl(const Mat& frame)
{
    addNewFrame(frame);

    processFrame(procPos + timeRadius);
    processFrame(procPos);

    return at(outPos, Y);
}

void NlmBased::processFrame(int idx)
{
    Mat& procY = at(idx, Y);

    procY.convertTo(V, CV_64F);
    W.create(V.size());
    W.setTo(Scalar::all(1));

    LoopBody body;

    body.scale = scale;
    body.searchAreaRadius = searchAreaRadius;
    body.timeRadius = timeRadius;
    body.patchSize = patchSize;
    body.sigma = sigma;

    body.procPos = idx;

    body.y = &y;
    body.Y = &Y;

    body.V = V;
    body.W = W;

    loop2D(Range2D(0, V.rows, 0, V.cols), body);

    divide(V, W, Z);

    Z.convertTo(procY, CV_8U);
}

namespace
{
    Mat addBorder(const Mat& src, int brd)
    {
        Mat buf;
        copyMakeBorder(src, buf, brd, brd, brd, brd, BORDER_CONSTANT);
        return buf(Range(brd, buf.rows - brd), Range(brd, buf.cols - brd));
    }
}

void NlmBased::addNewFrame(const cv::Mat& frame)
{
    CV_DbgAssert(frame.type() == CV_8UC3);
#ifdef _DEBUG
    if (storePos >= 0)
        CV_DbgAssert(frame.size() == at(storePos, y).size());
#endif

    Mat highRes;
    resize(frame, highRes, Size(), scale, scale, INTER_CUBIC);

    ++storePos;
    ++procPos;
    ++outPos;

    at(storePos, y) = addBorder(frame, searchAreaRadius + 1);
    at(storePos, Y) = addBorder(highRes, max(scale * (searchAreaRadius + 1), patchSize));
}
