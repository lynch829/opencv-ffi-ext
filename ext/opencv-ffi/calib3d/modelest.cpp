/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                        Intel License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of Intel Corporation may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "cvffi_modelest.h"
#include <algorithm>
#include <iterator>
#include <limits>

#include <opencv2/core/operations.hpp>
#include <opencv2/core/internal.hpp>
#include <opencv2/core/core_c.h>

#include <stdio.h>
#include <sys/time.h>

using namespace std;


CvffiModelEstimator2::CvffiModelEstimator2(int _modelPoints, CvSize _modelSize, int _maxBasicSolutions, int _maxIters)
  : maxIters( _maxIters )
{
    modelPoints = _modelPoints;
    modelSize = _modelSize;
    maxBasicSolutions = _maxBasicSolutions;
    checkPartialSubsets = true;

    // was rng = cvRNG(time(null))
    // This would result in rng having the same seed if
    // several CvffiModelEstimator2 were created in the span
    // of one second --- for example if the C API were called
    // repeatedly.
    //
    // n.b. cvRNG can take an int64
    struct timeval now;
    gettimeofday( &now, NULL );
    rng = cvRNG(now.tv_usec );
}

CvffiModelEstimator2::~CvffiModelEstimator2()
{
}

void CvffiModelEstimator2::setSeed( int64 seed )
{
    rng = cvRNG(seed);
}


int CvffiModelEstimator2::findInliers( const CvMat* m1, const CvMat* m2,
                                    const CvMat* model, CvMat* _err,
                                    CvMat* _mask, double threshold )
{
    int i, count = _err->rows*_err->cols, goodCount = 0;
    const float* err = _err->data.fl;
    uchar* mask = _mask->data.ptr;

    computeReprojError( m1, m2, model, _err );
    threshold *= threshold;
    for( i = 0; i < count; i++ )
        goodCount += mask[i] = err[i] <= threshold;
    return goodCount;
}


CV_IMPL int
cvRANSACUpdateNumIters( double p, double ep,
                        int model_points, int max_iters )
{
    if( model_points <= 0 )
        CV_Error( CV_StsOutOfRange, "the number of model points should be positive" );

    p = MAX(p, 0.);
    p = MIN(p, 1.);
    ep = MAX(ep, 0.);
    ep = MIN(ep, 1.);

    // avoid inf's & nan's
    double num = MAX(1. - p, DBL_MIN);
    double denom = 1. - pow(1. - ep,model_points);
    if( denom < DBL_MIN )
        return 0;

    num = log(num);
    denom = log(denom);
    
    return denom >= 0 || -num >= max_iters*(-denom) ?
        max_iters : cvRound(num/denom);
}

bool CvffiModelEstimator2::runRANSAC( const CvMat* m1, const CvMat* m2, CvMat* model,
                                    CvMat* mask0, int &totalIters,
                                    double reprojThreshold, 
                                    double confidence  )
{

    bool result = false;
    cv::Ptr<CvMat> mask = cvCloneMat(mask0);
    cv::Ptr<CvMat> models, err, tmask;
    cv::Ptr<CvMat> ms1, ms2;

    int count = m1->rows*m1->cols, maxGoodCount = 0;
    CV_Assert( CV_ARE_SIZES_EQ(m1, m2) && CV_ARE_SIZES_EQ(m1, mask) );

    int iter, niters = maxIters;

    if( count < modelPoints )
      return false;

    models = cvCreateMat( modelSize.height*maxBasicSolutions, modelSize.width, CV_64FC1 );
    err = cvCreateMat( 1, count, CV_32FC1 );
    tmask = cvCreateMat( 1, count, CV_8UC1 );
    
    if( count > modelPoints )
    {
        ms1 = cvCreateMat( 1, modelPoints, m1->type );
        ms2 = cvCreateMat( 1, modelPoints, m2->type );
    }
    else
    {
        niters = 1;
        ms1 = cvCloneMat(m1);
        ms2 = cvCloneMat(m2);
    }

    for( iter = 0; iter < niters; iter++ )
    {
        int i, goodCount, nmodels;
        if( count > modelPoints )
        {
            bool found = getSubset( m1, m2, ms1, ms2, 300 );
            if( !found )
            {
                if( iter == 0 )
                    return false;
                break;
            }
        }

        nmodels = runKernel( ms1, ms2, models );
        if( nmodels <= 0 )
            continue;
        for( i = 0; i < nmodels; i++ )
        {
            CvMat model_i;
            cvGetRows( models, &model_i, i*modelSize.height, (i+1)*modelSize.height );
            goodCount = findInliers( m1, m2, &model_i, err, tmask, reprojThreshold );

            if( goodCount > MAX(maxGoodCount, modelPoints-1) )
            {
                std::swap(tmask, mask);
                cvCopy( &model_i, model );
                maxGoodCount = goodCount;
                niters = cvRANSACUpdateNumIters( confidence,
                    (double)(count - goodCount)/count, modelPoints, niters );
            }
        }
    }

    if( iter == maxIters )
      printf( "Ran to max iters %d\n", iter );

    if( maxGoodCount > 0 )
    {
        if( mask != mask0 )
            cvCopy( mask, mask0 );
        result = true;
    }

    totalIters = iter;

    return result;
}


static CV_IMPLEMENT_QSORT( icvSortDistances, int, CV_LT )

bool CvffiModelEstimator2::runLMeDS( const CvMat* m1, const CvMat* m2, CvMat* model,
                                  CvMat* mask, double confidence, int _maxIters )
{
    const double outlierRatio = 0.45;
    bool result = false;
    cv::Ptr<CvMat> models;
    cv::Ptr<CvMat> ms1, ms2;
    cv::Ptr<CvMat> err;

    int count = m1->rows*m1->cols;
    double minMedian = DBL_MAX, sigma;

    CV_Assert( CV_ARE_SIZES_EQ(m1, m2) && CV_ARE_SIZES_EQ(m1, mask) );

    int maxIters;
    if( _maxIters > 0 )
      maxIters = _maxIters;
    else if( _maxIters < 0 )
      maxIters = -_maxIters * count;
    else
      maxIters = count;
    int iter, niters = maxIters;

    if( count < modelPoints )
        return false;

    models = cvCreateMat( modelSize.height*maxBasicSolutions, modelSize.width, CV_64FC1 );
    err = cvCreateMat( 1, count, CV_32FC1 );
    
    if( count > modelPoints )
    {
        ms1 = cvCreateMat( 1, modelPoints, m1->type );
        ms2 = cvCreateMat( 1, modelPoints, m2->type );
    }
    else
    {
        niters = 1;
        ms1 = cvCloneMat(m1);
        ms2 = cvCloneMat(m2);
    }

    niters = cvRound(log(1-confidence)/log(1-pow(1-outlierRatio,(double)modelPoints)));
    niters = MIN( MAX(niters, 3), maxIters );

    for( iter = 0; iter < niters; iter++ )
    {
        int i, nmodels;
        if( count > modelPoints )
        {
            bool found = getSubset( m1, m2, ms1, ms2, 300 );
            if( !found )
            {
                if( iter == 0 )
                    return false;
                break;
            }
        }

        nmodels = runKernel( ms1, ms2, models );
        if( nmodels <= 0 )
            continue;
        for( i = 0; i < nmodels; i++ )
        {
            CvMat model_i;
            cvGetRows( models, &model_i, i*modelSize.height, (i+1)*modelSize.height );
            computeReprojError( m1, m2, &model_i, err );
            icvSortDistances( err->data.i, count, 0 );

            double median = count % 2 != 0 ?
                err->data.fl[count/2] : (err->data.fl[count/2-1] + err->data.fl[count/2])*0.5;

            if( median < minMedian )
            {
                minMedian = median;
                cvCopy( &model_i, model );
            }
        }
    }

    if( minMedian < DBL_MAX )
    {
        sigma = 2.5*1.4826*(1 + 5./(count - modelPoints))*sqrt(minMedian);
        sigma = MAX( sigma, 0.001 );

        count = findInliers( m1, m2, model, err, mask, sigma );
        result = count >= modelPoints;
    }

    return result;
}


bool CvffiModelEstimator2::getSubset( const CvMat* m1, const CvMat* m2,
                                   CvMat* ms1, CvMat* ms2, int maxAttempts )
{
    cv::AutoBuffer<int> _idx(modelPoints);
    int* idx = _idx;
    int i = 0, j, k, idx_i, iters = 0;
    int type = CV_MAT_TYPE(m1->type), elemSize = CV_ELEM_SIZE(type);
    const int *m1ptr = m1->data.i, *m2ptr = m2->data.i;
    int *ms1ptr = ms1->data.i, *ms2ptr = ms2->data.i;
    int count = m1->cols*m1->rows;

    assert( CV_IS_MAT_CONT(m1->type & m2->type) && (elemSize % sizeof(int) == 0) );
    elemSize /= sizeof(int);

    for(; iters < maxAttempts; iters++)
    {
        for( i = 0; i < modelPoints && iters < maxAttempts; )
        {
            idx[i] = idx_i = cvRandInt(&rng) % count;
            for( j = 0; j < i; j++ )
                if( idx_i == idx[j] )
                    break;
            if( j < i )
                continue;
            for( k = 0; k < elemSize; k++ )
            {
                ms1ptr[i*elemSize + k] = m1ptr[idx_i*elemSize + k];
                ms2ptr[i*elemSize + k] = m2ptr[idx_i*elemSize + k];
            }
            if( checkPartialSubsets && (!checkSubset( ms1, i+1 ) || !checkSubset( ms2, i+1 )))
            {
                iters++;
                continue;
            }
            i++;
        }
        if( !checkPartialSubsets && i == modelPoints &&
            (!checkSubset( ms1, i ) || !checkSubset( ms2, i )))
            continue;
        break;
    }

    return i == modelPoints && iters < maxAttempts;
}


bool CvffiModelEstimator2::checkSubset( const CvMat* m, int count )
{
    int j, k, i, i0, i1;
    CvPoint2D64f* ptr = (CvPoint2D64f*)m->data.ptr;

    assert( CV_MAT_TYPE(m->type) == CV_64FC2 );
    
    if( checkPartialSubsets )
        i0 = i1 = count - 1;
    else
        i0 = 0, i1 = count - 1;
    
    for( i = i0; i <= i1; i++ )
    {
        // check that the i-th selected point does not belong
        // to a line connecting some previously selected points
        for( j = 0; j < i; j++ )
        {
            double dx1 = ptr[j].x - ptr[i].x;
            double dy1 = ptr[j].y - ptr[i].y;
            for( k = 0; k < j; k++ )
            {
                double dx2 = ptr[k].x - ptr[i].x;
                double dy2 = ptr[k].y - ptr[i].y;
                if( fabs(dx2*dy1 - dy2*dx1) <= FLT_EPSILON*(fabs(dx1) + fabs(dy1) + fabs(dx2) + fabs(dy2)))
                    break;
            }
            if( k < j )
                break;
        }
        if( j < i )
            break;
    }

    return i >= i1;
}


