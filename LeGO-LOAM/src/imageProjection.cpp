// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <boost/circular_buffer.hpp>
#include "imageProjection.h"

ImageProjection::ImageProjection(ros::NodeHandle& nh,
                                 size_t N_scan,
                                 size_t horizontal_scan,
                                 Channel<ProjectionOut>& output_channel)
    : _nh(nh), _N_scan(N_scan), _horizon_scan(horizontal_scan),
      _output_channel(output_channel)
{
  _sub_laser_cloud = nh.subscribe<sensor_msgs::PointCloud2>(
      pointCloudTopic, 1, &ImageProjection::cloudHandler, this);

  _pub_full_cloud =
      nh.advertise<sensor_msgs::PointCloud2>("/full_cloud_projected", 1);
  _pub_full_info_cloud =
      nh.advertise<sensor_msgs::PointCloud2>("/full_cloud_info", 1);

  _pub_ground_cloud = nh.advertise<sensor_msgs::PointCloud2>("/ground_cloud", 1);
  _pub_segmented_cloud =
      nh.advertise<sensor_msgs::PointCloud2>("/segmented_cloud", 1);
  _pub_segmented_cloud_pure =
      nh.advertise<sensor_msgs::PointCloud2>("/segmented_cloud_pure", 1);
  _pub_segmented_cloud_info =
      nh.advertise<cloud_msgs::cloud_info>("/segmented_cloud_info", 1);
  _pub_outlier_cloud = nh.advertise<sensor_msgs::PointCloud2>("/outlier_cloud", 1);

  const size_t cloud_size = _N_scan * _horizon_scan;

  _laser_cloud_in.reset(new pcl::PointCloud<PointType>());
  _full_cloud.reset(new pcl::PointCloud<PointType>());
  _full_info_cloud.reset(new pcl::PointCloud<PointType>());

  _ground_cloud.reset(new pcl::PointCloud<PointType>());
  _segmented_cloud.reset(new pcl::PointCloud<PointType>());
  _segmented_cloud_pure.reset(new pcl::PointCloud<PointType>());
  _outlier_cloud.reset(new pcl::PointCloud<PointType>());

  _full_cloud->points.resize(cloud_size);
  _full_info_cloud->points.resize(cloud_size);

}

void ImageProjection::resetParameters() {
  const size_t cloud_size = _N_scan * _horizon_scan;
  PointType nanPoint;
  nanPoint.x = std::numeric_limits<float>::quiet_NaN();
  nanPoint.y = std::numeric_limits<float>::quiet_NaN();
  nanPoint.z = std::numeric_limits<float>::quiet_NaN();

  _laser_cloud_in->clear();
  _ground_cloud->clear();
  _segmented_cloud->clear();
  _segmented_cloud_pure->clear();
  _outlier_cloud->clear();

  _range_mat.resize(_N_scan, _horizon_scan);
  _ground_mat.resize(_N_scan, _horizon_scan);
  _label_mat.resize(_N_scan, _horizon_scan);

  _range_mat.fill(FLT_MAX);
  _ground_mat.setZero();
  _label_mat.setZero();

  _label_count = 1;

  std::fill(_full_cloud->points.begin(), _full_cloud->points.end(), nanPoint);
  std::fill(_full_info_cloud->points.begin(), _full_info_cloud->points.end(),
            nanPoint);

  _seg_msg.startRingIndex.assign(_N_scan, 0);
  _seg_msg.endRingIndex.assign(_N_scan, 0);

  _seg_msg.segmentedCloudGroundFlag.assign(cloud_size, false);
  _seg_msg.segmentedCloudColInd.assign(cloud_size, 0);
  _seg_msg.segmentedCloudRange.assign(cloud_size, 0);
}

void ImageProjection::cloudHandler(
    const sensor_msgs::PointCloud2ConstPtr& laserCloudMsg) {
  // Reset parameters
  resetParameters();

  // Copy and remove NAN points
  pcl::fromROSMsg(*laserCloudMsg, *_laser_cloud_in);
  std::vector<int> indices;
  pcl::removeNaNFromPointCloud(*_laser_cloud_in, *_laser_cloud_in, indices);
  _seg_msg.header = laserCloudMsg->header;

  findStartEndAngle();
  // Range image projection
  projectPointCloud();
  // Mark ground points
  groundRemoval();
  // Point cloud segmentation
  cloudSegmentation();
  //publish (optionally)
  publishClouds();
}


void ImageProjection::projectPointCloud() {
  // range image projection
  const size_t cloudSize = _laser_cloud_in->points.size();

  for (size_t i = 0; i < cloudSize; ++i) {
    PointType thisPoint = _laser_cloud_in->points[i];

    float range = sqrt(thisPoint.x * thisPoint.x +
                       thisPoint.y * thisPoint.y +
                       thisPoint.z * thisPoint.z);

    // find the row and column index in the image for this point
    float verticalAngle = std::asin(thisPoint.z / range);
        //std::atan2(thisPoint.z, sqrt(thisPoint.x * thisPoint.x + thisPoint.y * thisPoint.y));

    int rowIdn = (verticalAngle + ang_bottom) / ang_res_y;
    if (rowIdn < 0 || rowIdn >= _N_scan) {
      continue;
    }

    float horizonAngle = std::atan2(thisPoint.x, thisPoint.y);

    int columnIdn = -round((horizonAngle - M_PI_2) / ang_res_x) + _horizon_scan * 0.5;

    if (columnIdn >= _horizon_scan){
      columnIdn -= _horizon_scan;
    }

    if (columnIdn < 0 || columnIdn >= _horizon_scan){
      continue;
    }

    if (range < 0.1){
      continue;
    }

    _range_mat(rowIdn, columnIdn) = range;

    thisPoint.intensity = (float)rowIdn + (float)columnIdn / 10000.0;

    size_t index = columnIdn + rowIdn * _horizon_scan;
    _full_cloud->points[index] = thisPoint;
    // the corresponding range of a point is saved as "intensity"
    _full_info_cloud->points[index] = thisPoint;
    _full_info_cloud->points[index].intensity = range;
  }
}

void ImageProjection::findStartEndAngle() {
  // start and end orientation of this cloud
  auto point = _laser_cloud_in->points.front();
  _seg_msg.startOrientation = -std::atan2(point.y, point.x);

  point = _laser_cloud_in->points.back();
  _seg_msg.endOrientation = -std::atan2(point.y, point.x) + 2 * M_PI;

  if (_seg_msg.endOrientation - _seg_msg.startOrientation > 3 * M_PI) {
    _seg_msg.endOrientation -= 2 * M_PI;
  } else if (_seg_msg.endOrientation - _seg_msg.startOrientation < M_PI) {
    _seg_msg.endOrientation += 2 * M_PI;
  }
  _seg_msg.orientationDiff =
      _seg_msg.endOrientation - _seg_msg.startOrientation;
}

void ImageProjection::groundRemoval() {
  // _ground_mat
  // -1, no valid info to check if ground of not
  //  0, initial value, after validation, means not ground
  //  1, ground
  for (size_t j = 0; j < _horizon_scan; ++j) {
    for (size_t i = 0; i < groundScanInd; ++i) {
      size_t lowerInd = j + (i)*_horizon_scan;
      size_t upperInd = j + (i + 1) * _horizon_scan;

      if (_full_cloud->points[lowerInd].intensity == -1 ||
          _full_cloud->points[upperInd].intensity == -1) {
        // no info to check, invalid points
        _ground_mat(i, j) = -1;
        continue;
      }

      float dX =
          _full_cloud->points[upperInd].x - _full_cloud->points[lowerInd].x;
      float dY =
          _full_cloud->points[upperInd].y - _full_cloud->points[lowerInd].y;
      float dZ =
          _full_cloud->points[upperInd].z - _full_cloud->points[lowerInd].z;

      float vertical_angle = std::atan2(dZ , sqrt(dX * dX + dY * dY + dZ * dZ));

      // TODO: review this change

      if ( (vertical_angle - sensorMountAngle) <= 10 * DEG_TO_RAD) {
        _ground_mat(i, j) = 1;
        _ground_mat(i + 1, j) = 1;
      }
    }
  }
  // extract ground cloud (_ground_mat == 1)
  // mark entry that doesn't need to label (ground and invalid point) for
  // segmentation note that ground remove is from 0~_N_scan-1, need _range_mat
  // for mark label matrix for the 16th scan
  for (size_t i = 0; i < _N_scan; ++i) {
    for (size_t j = 0; j < _horizon_scan; ++j) {
      if (_ground_mat(i, j) == 1 ||
          _range_mat(i, j) == FLT_MAX) {
        _label_mat(i, j) = -1;
      }
    }
  }

  for (size_t i = 0; i <= groundScanInd; ++i) {
    for (size_t j = 0; j < _horizon_scan; ++j) {
      if (_ground_mat(i, j) == 1)
        _ground_cloud->push_back(_full_cloud->points[j + i * _horizon_scan]);
    }
  }
}

void ImageProjection::cloudSegmentation() {
  // segmentation process
  for (size_t i = 0; i < _N_scan; ++i)
    for (size_t j = 0; j < _horizon_scan; ++j)
      if (_label_mat(i, j) == 0) labelComponents(i, j);

  int sizeOfSegCloud = 0;
  // extract segmented cloud for lidar odometry
  for (size_t i = 0; i < _N_scan; ++i) {
    _seg_msg.startRingIndex[i] = sizeOfSegCloud - 1 + 5;

    for (size_t j = 0; j < _horizon_scan; ++j) {
      if (_label_mat(i, j) > 0 || _ground_mat(i, j) == 1) {
        // outliers that will not be used for optimization (always continue)
        if (_label_mat(i, j) == 999999) {
          if (i > groundScanInd && j % 5 == 0) {
            _outlier_cloud->push_back(
                _full_cloud->points[j + i * _horizon_scan]);
            continue;
          } else {
            continue;
          }
        }
        // majority of ground points are skipped
        if (_ground_mat(i, j) == 1) {
          if (j % 5 != 0 && j > 5 && j < _horizon_scan - 5) continue;
        }
        // mark ground points so they will not be considered as edge features
        // later
        _seg_msg.segmentedCloudGroundFlag[sizeOfSegCloud] =
            (_ground_mat(i, j) == 1);
        // mark the points' column index for marking occlusion later
        _seg_msg.segmentedCloudColInd[sizeOfSegCloud] = j;
        // save range info
        _seg_msg.segmentedCloudRange[sizeOfSegCloud] =
            _range_mat(i, j);
        // save seg cloud
        _segmented_cloud->push_back(_full_cloud->points[j + i * _horizon_scan]);
        // size of seg cloud
        ++sizeOfSegCloud;
      }
    }

    _seg_msg.endRingIndex[i] = sizeOfSegCloud - 1 - 5;
  }

  // extract segmented cloud for visualization
  for (size_t i = 0; i < _N_scan; ++i) {
    for (size_t j = 0; j < _horizon_scan; ++j) {
      if (_label_mat(i, j) > 0 && _label_mat(i, j) != 999999) {
        _segmented_cloud_pure->push_back(
            _full_cloud->points[j + i * _horizon_scan]);
        _segmented_cloud_pure->points.back().intensity =
            _label_mat(i, j);
      }
    }
  }
}

void ImageProjection::labelComponents(int row, int col) {

  const float segmentThetaThreshold = tan(segmentTheta);

  std::vector<bool> lineCountFlag(_N_scan, false);
  const size_t cloud_size = _N_scan * _horizon_scan;
  using Coord2D = Eigen::Vector2i;
  boost::circular_buffer<Coord2D> queue(cloud_size);
  boost::circular_buffer<Coord2D> all_pushed(cloud_size);

  queue.push_back({ row,col } );
  all_pushed.push_back({ row,col } );

  const Coord2D neighborIterator[4] = {
      {0, -1}, {-1, 0}, {1, 0}, {0, 1}};

  while (queue.size() > 0) {
    // Pop point
    Coord2D fromInd = queue.front();
    queue.pop_front();

    // Mark popped point
    _label_mat(fromInd.x(), fromInd.y()) = _label_count;
    // Loop through all the neighboring grids of popped grid

    for (const auto& iter : neighborIterator) {
      // new index
      int thisIndX = fromInd.x() + iter.x();
      int thisIndY = fromInd.y() + iter.y();
      // index should be within the boundary
      if (thisIndX < 0 || thisIndX >= _N_scan){
        continue;
      }
      // at range image margin (left or right side)
      if (thisIndY < 0){
        thisIndY = _horizon_scan - 1;
      }
      if (thisIndY >= _horizon_scan){
        thisIndY = 0;
      }
      // prevent infinite loop (caused by put already examined point back)
      if (_label_mat(thisIndX, thisIndY) != 0){
        continue;
      }

      float d1 = std::max(_range_mat(fromInd.x(), fromInd.y()),
                    _range_mat(thisIndX, thisIndY));
      float d2 = std::min(_range_mat(fromInd.x(), fromInd.y()),
                    _range_mat(thisIndX, thisIndY));

      float alpha = (iter.x() == 0) ? segmentAlphaX : segmentAlphaY;
      float tang = (d2 * sin(alpha) / (d1 - d2 * cos(alpha)));

      if (tang > segmentThetaThreshold) {
        queue.push_back( {thisIndX, thisIndY } );

        _label_mat(thisIndX, thisIndY) = _label_count;
        lineCountFlag[thisIndX] = true;

        all_pushed.push_back(  {thisIndX, thisIndY } );
      }
    }
  }

  // check if this segment is valid
  bool feasibleSegment = false;
  if (all_pushed.size() >= 30){
    feasibleSegment = true;
  }
  else if (all_pushed.size() >= segmentValidPointNum) {
    int lineCount = 0;
    for (size_t i = 0; i < _N_scan; ++i) {
      if (lineCountFlag[i] == true) ++lineCount;
    }
    if (lineCount >= segmentValidLineNum) feasibleSegment = true;
  }
  // segment is valid, mark these points
  if (feasibleSegment == true) {
    ++_label_count;
  } else {  // segment is invalid, mark these points
    for (size_t i = 0; i < all_pushed.size(); ++i) {
      _label_mat(all_pushed[i].x(), all_pushed[i].y()) = 999999;
    }
  }
}

void ImageProjection::publishClouds() {
  const auto& cloudHeader = _seg_msg.header;

  sensor_msgs::PointCloud2 laserCloudTemp;

  auto PublishCloud = [&](ros::Publisher& pub,
                          const pcl::PointCloud<PointType>::Ptr& cloud) {
    if (pub.getNumSubscribers() != 0) {
      pcl::toROSMsg(*cloud, laserCloudTemp);
      laserCloudTemp.header.stamp = cloudHeader.stamp;
      laserCloudTemp.header.frame_id = "base_link";
      pub.publish(laserCloudTemp);
    }
  };

  PublishCloud(_pub_outlier_cloud, _outlier_cloud);
  PublishCloud(_pub_segmented_cloud, _segmented_cloud);
  PublishCloud(_pub_full_cloud, _full_cloud);
  PublishCloud(_pub_ground_cloud, _ground_cloud);
  PublishCloud(_pub_segmented_cloud_pure, _segmented_cloud_pure);
  PublishCloud(_pub_full_info_cloud, _full_info_cloud);

  if (_pub_segmented_cloud_info.getNumSubscribers() != 0) {
    _pub_segmented_cloud_info.publish(_seg_msg);
  }

  //--------------------
  ProjectionOut out;
  out.outlier_cloud.reset(new pcl::PointCloud<PointType>());
  out.segmented_cloud.reset(new pcl::PointCloud<PointType>());

  std::swap( out.seg_msg, _seg_msg);
  std::swap(out.outlier_cloud, _outlier_cloud);
  std::swap(out.segmented_cloud, _segmented_cloud);

  _output_channel.send( std::move(out) );

}


