/*
 * ElevationMap.cpp
 *
 *  Created on: Feb 5, 2014
 *      Author: Péter Fankhauser
 *	 Institute: ETH Zurich, Autonomous Systems Lab
 */
#include "ElevationMap.hpp"

// StarlETH Navigation
#include "ElevationMapFunctors.hpp"
#include <TransformationMath.hpp>
#include <ElevationMessageHelpers.hpp>

// Math
#include <math.h>

// ROS Logging
#include <ros/ros.h>

using namespace std;
using namespace Eigen;

namespace starleth_elevation_mapping {

ElevationMap::ElevationMap()
{
  minVariance_ = 0.0;
  maxVariance_ = 0.0;
  mahalanobisDistanceThreshold_ = 0.0;
  multiHeightNoise_ = 0.0;
  biggerHeightThresholdFactor_ = 0.0;
  biggerHeightNoiseFactor_ = 0.0;
  minHorizontalVariance_ = 0.0;
  maxHorizontalVariance_ = 0.0;
  reset();
}

ElevationMap::~ElevationMap()
{

}

bool ElevationMap::setSize(const Eigen::Array2d& length, const double& resolution)
{
  ROS_ASSERT(length_(0) > 0.0);
  ROS_ASSERT(length_(1) > 0.0);
  ROS_ASSERT(resolution_ > 0.0);

  int nRows = static_cast<int>(round(length(0) / resolution));
  int nCols = static_cast<int>(round(length(1) / resolution));
  elevationData_.resize(nRows, nCols);
  varianceData_.resize(nRows, nCols);
  horizontalVarianceDataX_.resize(nRows, nCols);
  horizontalVarianceDataY_.resize(nRows, nCols);
  colorData_.resize(nRows, nCols);
  labelData_.resize(nRows, nCols);

  reset();

  resolution_ = resolution;
  length_ = (Array2i(nRows, nCols).cast<double>() * resolution_).matrix();

  ROS_DEBUG_STREAM("Elevation map matrix resized to " << elevationData_.rows() << " rows and "  << elevationData_.cols() << " columns.");
  return true;
}

bool ElevationMap::add(const pcl::PointCloud<pcl::PointXYZRGB>::Ptr pointCloud, Eigen::Matrix<float, Eigen::Dynamic, 3>& pointCloudVariances)
{
  for (unsigned int i = 0; i < pointCloud->size(); ++i)
  {
    auto& point = pointCloud->points[i];

    Array2i index;
    if (!starleth_elevation_msg::getIndexFromPosition(
        index, Vector2d(point.x, point.y), length_, resolution_, getBufferSize(), bufferStartIndex_))
      continue; // Skip this point if it does not lie within the elevation map

    auto& elevation = elevationData_(index(0), index(1));
    auto& variance = varianceData_(index(0), index(1));
    auto& horizontalVarianceX = horizontalVarianceDataX_(index(0), index(1));
    auto& horizontalVarianceY = horizontalVarianceDataY_(index(0), index(1));
    auto& color = colorData_(index(0), index(1));
    auto& multiHeightLabel = labelData_(index(0), index(1));
    RowVector3f pointVariance = pointCloudVariances.row(i);

    if (std::isnan(elevation) || std::isinf(variance))
    {
      // No prior information in elevation map, use measurement.
      elevation = point.z;
      variance = pointVariance.z();
      horizontalVarianceX = pointVariance.x();
      horizontalVarianceY = pointVariance.y();
      starleth_elevation_msg::copyColorVectorToValue(point.getRGBVector3i(), color);
      continue;
    }

    double mahalanobisDistance = sqrt(pow(point.z - elevation, 2) / variance);

    if (mahalanobisDistance < mahalanobisDistanceThreshold_)
    {
      // Fuse measurement with elevation map data.
      elevation = (variance * point.z + pointVariance.z() * elevation) / (variance + pointVariance.z());
      variance =  (pointVariance.z() * variance) / (pointVariance.z() + variance);
      // TODO add color fusion
      starleth_elevation_msg::copyColorVectorToValue(point.getRGBVector3i(), color);
      continue;
    }
    if (point.z > elevation && mahalanobisDistance < biggerHeightThresholdFactor_ * mahalanobisDistanceThreshold_)
    {
      // Overwrite elevation map information with measurement data
      elevation = point.z;
      variance = pointVariance.z();
      starleth_elevation_msg::copyColorVectorToValue(point.getRGBVector3i(), color);
      continue;
    }

    if (point.z > elevation && mahalanobisDistance > biggerHeightThresholdFactor_ * mahalanobisDistanceThreshold_)
    {
      variance += biggerHeightNoiseFactor_ * multiHeightNoise_;
      continue;
    }

    horizontalVarianceX = 0.0;
    horizontalVarianceY = 0.0;

    // TODO Add label to cells which are potentially multi-level

    // Add noise to cells which have ignored lower values,
    // such we outliers and moving objects are removed
    variance += multiHeightNoise_;
  }

  clean();

  return true;
}

bool ElevationMap::update(Eigen::MatrixXf varianceUpdate, Eigen::MatrixXf horizontalVarianceUpdateX, Eigen::MatrixXf horizontalVarianceUpdateY)
{
  Array2i bufferSize = getBufferSize();

  if (!(
      (Array2i(varianceUpdate.rows(), varianceUpdate.cols()) == bufferSize).all() &&
      (Array2i(horizontalVarianceUpdateX.rows(), horizontalVarianceUpdateX.cols()) == bufferSize).all() &&
      (Array2i(horizontalVarianceUpdateY.rows(), horizontalVarianceUpdateY.cols()) == bufferSize).all()
      ))
  {
    ROS_ERROR("The size of the update matrices does not match.");
    return false;
  }

  varianceData_ += varianceUpdate;
  horizontalVarianceDataX_ += horizontalVarianceUpdateX;
  horizontalVarianceDataY_ += horizontalVarianceUpdateY;
  clean();

  return true;
}


bool ElevationMap::generateFusedMap()
{
  ROS_DEBUG("Fusing elevation map...");

  // Initializations.
  MatrixXf fusedElevationData(elevationData_.rows(), elevationData_.cols());
  MatrixXf fusedVarianceData(varianceData_.rows(), varianceData_.cols());
  fusedElevationData.setConstant(NAN);
  fusedVarianceData.setConstant(numeric_limits<float>::infinity());

  // For each cell in map.
  for (unsigned int i = 0; i < fusedElevationData.rows(); ++i)
  {
    for (unsigned int j = 0; j < fusedElevationData.cols(); ++j)
    {
      // Center of submap in map.
      Vector2d center;
      starleth_elevation_msg::getPositionFromIndex(center, Array2i(i, j),
                                                   length_, resolution_,
                                                   getBufferSize(), bufferStartIndex_);

      // Size of submap (2 sigma bound).
      Array2d size = 4 * Array2d(horizontalVarianceDataX_(i, j), horizontalVarianceDataY_(i, j)).sqrt();

      // TODO Don't skip holes.
      if (!(std::isfinite(size[0]) && std::isfinite(size[1]))) continue;

      // Get submap data.
      MatrixXf elevationSubmap, variancesSubmap, horizontalVariancesXSubmap, horizontalVariancesYSubmap;
      Array2i centerIndex;
      getSubmap(elevationSubmap, centerIndex, elevationData_, center, size);
      getSubmap(variancesSubmap, centerIndex, varianceData_, center, size);
      getSubmap(horizontalVariancesXSubmap, centerIndex, horizontalVarianceDataX_, center, size);
      getSubmap(horizontalVariancesYSubmap, centerIndex, horizontalVarianceDataY_, center, size);

      // Prepare data fusion.
      ArrayXf means, variances, weights;
      Array2i submapBufferSize(elevationSubmap.rows(), elevationSubmap.cols());
      int maxNumberOfCellsToFuse = submapBufferSize.prod();
      means.resize(maxNumberOfCellsToFuse);
      variances.resize(maxNumberOfCellsToFuse);
      weights.resize(maxNumberOfCellsToFuse);

      // Position of center index in submap.
      Vector2d centerInSubmap;
      starleth_elevation_msg::getPositionFromIndex(centerInSubmap, centerIndex,
                                                   size, resolution_,
                                                   submapBufferSize, Array2i(0, 0));

      unsigned int n = 0;

      for (unsigned int p = 0; p < elevationSubmap.rows(); ++p)
      {
        for (unsigned int q = 0; q < elevationSubmap.cols(); ++q)
        {
          if (!(std::isfinite(elevationSubmap(p, q))
               && std::isfinite(variancesSubmap(p, q))
               && std::isnormal(horizontalVariancesXSubmap(p, q))
               && std::isnormal(horizontalVariancesYSubmap(p, q)))) continue;

          means[n] = elevationSubmap(p, q);
          variances[n] = variancesSubmap(p, q);

          // Compute weight from probability.
          Vector2d positionInSubmap;
          starleth_elevation_msg::getPositionFromIndex(positionInSubmap, Array2i(p, q),
                                                       size, resolution_,
                                                       submapBufferSize, Array2i(0, 0));

          Vector2d distanceToCenter = positionInSubmap - centerInSubmap;

          float probabilityX = fabs(
                cumulativeDistributionFunction(distanceToCenter.x() - resolution_ / 2.0, 0.0, sqrt(horizontalVariancesXSubmap(p, q)))
              - cumulativeDistributionFunction(distanceToCenter.x() + resolution_ / 2.0, 0.0, sqrt(horizontalVariancesXSubmap(p, q))));
          float probabilityY = fabs(
                cumulativeDistributionFunction(distanceToCenter.y() - resolution_ / 2.0, 0.0, sqrt(horizontalVariancesYSubmap(p, q)))
              - cumulativeDistributionFunction(distanceToCenter.y() + resolution_ / 2.0, 0.0, sqrt(horizontalVariancesYSubmap(p, q))));

          weights[n] = probabilityX * probabilityY;

          n++;
        }
      }

      if (n == 0)
      {
        // Nothing to fuse.
        fusedElevationData(i, j) = elevationData_(i, j);
        fusedVarianceData(i, j) = varianceData_(i, j);
        continue;
      }

      // Fuse.
      means.conservativeResize(n);
      variances.conservativeResize(n);
      weights.conservativeResize(n);

      float mean = (weights * means).sum() / weights.sum();
      float variance = (weights * (variances.square() + means.square())).sum() / weights.sum() - pow(mean, 2);
      if(std::isnan(-variance)) variance = 0.0;

      if (!(std::isfinite(variance) && std::isfinite(mean)))
      {
        ROS_ERROR("Something went wrong when fusing the map: Mean = %f, Variance = %f", mean, variance);
        continue;
      }

      // Add to fused map.
      fusedElevationData(i, j) = mean;
      fusedVarianceData(i, j) = variance;
    }
  }

  return true;
}

bool ElevationMap::getSubmap(Eigen::MatrixXf& submap, Eigen::Array2i& centerIndex, const Eigen::MatrixXf& map, const Eigen::Vector2d& center, const Eigen::Array2d& size)
{
  Array2i submapTopLeftIndex, submapSize;
  if (!starleth_elevation_msg::getSubmapIndexAndSize(submapTopLeftIndex, centerIndex,
                                                     submapSize, center, size,
                                                     length_,
                                                     resolution_,
                                                     getBufferSize(),
                                                     bufferStartIndex_))
  {
    ROS_ERROR("Position of requested submap is not part of the map.");
    return false;
  }

  return getSubmap(submap, map, submapTopLeftIndex, submapSize);
}

bool ElevationMap::getSubmap(Eigen::MatrixXf& submap, const Eigen::MatrixXf& map, const Eigen::Array2i& topLeftindex, const Eigen::Array2i& size)
{
  std::vector<Eigen::Array2i> submapIndeces;
  std::vector<Eigen::Array2i> submapSizes;

  if(!starleth_elevation_msg::getBufferRegionsForSubmap(submapIndeces, submapSizes, topLeftindex, size, getBufferSize(), bufferStartIndex_))
  {
     ROS_ERROR("Cannot access submap of this size.");
     return false;
  }

  unsigned int nRows =
      (submapSizes[3](0) + submapSizes[1](0) > submapSizes[2](0) + submapSizes[0](0)) ?
          submapSizes[3](0) + submapSizes[1](0) : submapSizes[2](0) + submapSizes[0](0);
  unsigned int nCols =
      (submapSizes[3](1) + submapSizes[2](1) > submapSizes[1](1) + submapSizes[0](1)) ?
          submapSizes[3](1) + submapSizes[2](1) : submapSizes[1](1) + submapSizes[0](1);

  submap.resize(nRows, nCols);

  submap.topLeftCorner    (submapSizes[0](0), submapSizes[0](1)) = map.block(submapIndeces[0](0), submapIndeces[0](1), submapSizes[0](0), submapSizes[0](1));
  submap.topRightCorner   (submapSizes[1](0), submapSizes[1](1)) = map.block(submapIndeces[1](0), submapIndeces[1](1), submapSizes[1](0), submapSizes[1](1));
  submap.bottomLeftCorner (submapSizes[2](0), submapSizes[2](1)) = map.block(submapIndeces[2](0), submapIndeces[2](1), submapSizes[2](0), submapSizes[2](1));
  submap.bottomRightCorner(submapSizes[3](0), submapSizes[3](1)) = map.block(submapIndeces[3](0), submapIndeces[3](1), submapSizes[3](0), submapSizes[3](1));

  return true;
}

bool ElevationMap::relocate(const Eigen::Vector3d& position)
{
  Vector2d alignedPosition;
  starleth_elevation_msg::getAlignedPosition(position.head(2),
                                        alignedPosition, length_,
                                        resolution_);

  Vector2d positionShift = alignedPosition - toParentTransform_.translation().head(2);

  Array2i indexShift;
  starleth_elevation_msg::getIndexShiftFromPositionShift(indexShift, positionShift, resolution_);

  // Delete fields that fall out of map (and become empty cells).
  for (int i = 0; i < indexShift.size(); i++)
  {
    if (indexShift[i] != 0)
    {
      if (abs(indexShift[i]) >= getBufferSize()(i))
      {
        // Entire map is dropped.
        elevationData_.setConstant(NAN);
      }
      else
      {
        // Drop cells out of map.
        int sign = (indexShift[i] > 0 ? 1 : -1);
        int startIndex = bufferStartIndex_[i] - (sign < 0 ? 1 : 0);
        int endIndex = startIndex - sign + indexShift[i];
        int nCells = abs(indexShift[i]);

        int index = (sign > 0 ? startIndex : endIndex);
        starleth_elevation_msg::mapIndexWithinRange(index, getBufferSize()[i]);

        if (index + nCells <= getBufferSize()[i])
        {
          // One region to drop.
          if (i == 0) resetCols(index, nCells);
          if (i == 1) resetRows(index, nCells);
        }
        else
        {
          // Two regions to drop.
          int firstIndex = index;
          int firstNCells = getBufferSize()[i] - firstIndex;
          if (i == 0) resetCols(firstIndex, firstNCells);
          if (i == 1) resetRows(firstIndex, firstNCells);

          int secondIndex = 0;
          int secondNCells = nCells - firstNCells;
          if (i == 0) resetCols(secondIndex, secondNCells);
          if (i == 1) resetRows(secondIndex, secondNCells);
        }
      }
    }
  }

  // Update information.
  bufferStartIndex_ += indexShift;
  starleth_elevation_msg::mapIndexWithinRange(bufferStartIndex_, getBufferSize());
  toParentTransform_.translation().head(2) = alignedPosition;

  ROS_DEBUG("Elevation has been moved to position (%f, %f).", toParentTransform_.translation().head(2).x(), toParentTransform_.translation().head(2).y());
  return true;
}

bool ElevationMap::reset()
{
  toParentTransform_.setIdentity();
  elevationData_.setConstant(NAN);
  varianceData_.setConstant(numeric_limits<float>::infinity());
  horizontalVarianceDataX_.setConstant(numeric_limits<float>::infinity());
  horizontalVarianceDataY_.setConstant(numeric_limits<float>::infinity());
  colorData_.setConstant(0);
  labelData_.setConstant(0);
  bufferStartIndex_.setZero();
  length_.setZero();
  resolution_ = 0.0;
  return true;
}

Eigen::Array2i ElevationMap::getBufferSize()
{
  return Array2i(elevationData_.rows(), elevationData_.cols());
}

bool ElevationMap::clean()
{
  varianceData_ = varianceData_.unaryExpr(VarianceClampOperator<double>(minVariance_, maxVariance_));
  horizontalVarianceDataX_ = horizontalVarianceDataX_.unaryExpr(VarianceClampOperator<double>(minHorizontalVariance_, maxHorizontalVariance_));
  horizontalVarianceDataY_ = horizontalVarianceDataY_.unaryExpr(VarianceClampOperator<double>(minHorizontalVariance_, maxHorizontalVariance_));
  return true;
}

void ElevationMap::resetCols(unsigned int index, unsigned int nCols)
{
  elevationData_.block(index, 0, nCols, getBufferSize()[1]).setConstant(NAN);
}

void ElevationMap::resetRows(unsigned int index, unsigned int nRows)
{
  elevationData_.block(0, index, getBufferSize()[0], nRows).setConstant(NAN);
}

const double& ElevationMap::getResolution()
{
  return resolution_;
}

const Eigen::Array2d& ElevationMap::getLength()
{
  return length_;
}

const Eigen::Affine3d& ElevationMap::getMapToParentTransform()
{
  return toParentTransform_;
}

const Eigen::Array2i& ElevationMap::getBufferStartIndex()
{
  return bufferStartIndex_;
}

const Eigen::MatrixXf& ElevationMap::getRawElevationData()
{
  return elevationData_;
}

const Eigen::MatrixXf& ElevationMap::getRawVarianceData()
{
  return varianceData_;
}

const Eigen::Matrix<unsigned long, Eigen::Dynamic, Eigen::Dynamic>& ElevationMap::getRawColorData()
{
  return colorData_;
}

float ElevationMap::cumulativeDistributionFunction(float x, float mean, float standardDeviation)
{
  return 0.5 * erfc(-(x-mean)/(standardDeviation*sqrt(2.0)));
}

} /* namespace starleth_elevation_mapping */
