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
//
// This is an implementation of the algorithm described in the following papers:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.
//   T. Shan and B. Englot. LeGO-LOAM: Lightweight and Ground-Optimized Lidar
//   Odometry and Mapping on Variable Terrain
//      IEEE/RSJ International Conference on Intelligent Robots and Systems
//      (IROS). October 2018.

#include "featureAssociation.h"

const float RAD2DEG = 180.0 / M_PI;

FeatureAssociation::FeatureAssociation(ros::NodeHandle &node, size_t N_scan,
                                       size_t horizontal_scan,
                                       Channel<ProjectionOut> &input_channel,
                                       Channel<AssociationOut> &output_channel)
    : nh(node),
      _N_scan(N_scan),
      _horizontal_scan(horizontal_scan),
      _input_channel(input_channel),
      _output_channel(output_channel) {
  subImu = nh.subscribe<sensor_msgs::Imu>(
      imuTopic, 50, &FeatureAssociation::imuHandler, this);

  pubCornerPointsSharp =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_sharp", 1);
  pubCornerPointsLessSharp =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_less_sharp", 1);
  pubSurfPointsFlat =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_flat", 1);
  pubSurfPointsLessFlat =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_less_flat", 1);

  _pub_cloud_corner_last =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_corner_last", 2);
  _pub_cloud_surf_last =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_surf_last", 2);
  _pub_outlier_cloudLast =
      nh.advertise<sensor_msgs::PointCloud2>("/outlier_cloud_last", 2);
  pubLaserOdometry = nh.advertise<nav_msgs::Odometry>("/laser_odom_to_init", 5);

  cycle_count = 0;
  initializationValue();

  _run_thread = std::thread (&FeatureAssociation::runFeatureAssociation, this);
}

FeatureAssociation::~FeatureAssociation()
{
  _input_channel.send({});
  _run_thread.join();
}

void FeatureAssociation::initializationValue() {
  const size_t cloud_size = _N_scan * _horizontal_scan;
  cloudSmoothness.resize(cloud_size);

  downSizeFilter.setLeafSize(0.2, 0.2, 0.2);

  segmentedCloud.reset(new pcl::PointCloud<PointType>());
  outlierCloud.reset(new pcl::PointCloud<PointType>());

  cornerPointsSharp.reset(new pcl::PointCloud<PointType>());
  cornerPointsLessSharp.reset(new pcl::PointCloud<PointType>());
  surfPointsFlat.reset(new pcl::PointCloud<PointType>());
  surfPointsLessFlat.reset(new pcl::PointCloud<PointType>());

  surfPointsLessFlatScan.reset(new pcl::PointCloud<PointType>());
  surfPointsLessFlatScanDS.reset(new pcl::PointCloud<PointType>());

  cloudCurvature.resize(cloud_size);
  cloudNeighborPicked.resize(cloud_size);
  cloudLabel.resize(cloud_size);

  pointSelCornerInd.resize(cloud_size);
  pointSearchCornerInd1.resize(cloud_size);
  pointSearchCornerInd2.resize(cloud_size);

  pointSelSurfInd.resize(cloud_size);
  pointSearchSurfInd1.resize(cloud_size);
  pointSearchSurfInd2.resize(cloud_size);
  pointSearchSurfInd3.resize(cloud_size);

  systemInitCount = 0;
  systemInited = false;

  imuPointerFront = 0;
  imuPointerLast = -1;
  imuPointerLastIteration = 0;

  imuRollStart = 0;
  imuPitchStart = 0;
  imuYawStart = 0;
  cosImuRollStart = 0;
  cosImuPitchStart = 0;
  cosImuYawStart = 0;
  sinImuRollStart = 0;
  sinImuPitchStart = 0;
  sinImuYawStart = 0;
  imuRollCur = 0;
  imuPitchCur = 0;
  imuYawCur = 0;

  imuVeloStart.setZero();
  imuShiftStart.setZero();

  imuVeloCur.setZero();
  imuShiftCur.setZero();

  imuShiftFromStartCur.setZero();
  imuVeloFromStartCur.setZero();

  imuAngularRotationCur.setZero();
  imuAngularRotationLast.setZero();
  imuAngularFromStart.setZero();

  for (int i = 0; i < imuQueLength; ++i) {
    imuTime[i] = 0;
    imuRoll[i] = 0;
    imuPitch[i] = 0;
    imuYaw[i] = 0;
    imuAcc[i].setZero();
    imuVelo[i].setZero();
    imuShift[i].setZero();
    imuAngularVelo[i].setZero();
    imuAngularRotation[i].setZero();
  }

  skipFrameNum = 1;

  for (int i = 0; i < 6; ++i) {
    transformCur[i] = 0;
    transformSum[i] = 0;
  }

  systemInitedLM = false;

  imuRollLast = 0;
  imuPitchLast = 0;
  imuYawLast = 0;
  imuShiftFromStart.setZero();
  imuVeloFromStart.setZero();

  laserCloudCornerLast.reset(new pcl::PointCloud<PointType>());
  laserCloudSurfLast.reset(new pcl::PointCloud<PointType>());
  laserCloudOri.reset(new pcl::PointCloud<PointType>());
  coeffSel.reset(new pcl::PointCloud<PointType>());

  laserOdometry.header.frame_id = "/camera_init";
  laserOdometry.child_frame_id = "/laser_odom";

  laserOdometryTrans.frame_id_ = "/camera_init";
  laserOdometryTrans.child_frame_id_ = "/laser_odom";

  isDegenerate = false;

  frameCount = skipFrameNum;
}

void FeatureAssociation::updateImuRollPitchYawStartSinCos() {
  cosImuRollStart = cos(imuRollStart);
  cosImuPitchStart = cos(imuPitchStart);
  cosImuYawStart = cos(imuYawStart);
  sinImuRollStart = sin(imuRollStart);
  sinImuPitchStart = sin(imuPitchStart);
  sinImuYawStart = sin(imuYawStart);
}

void FeatureAssociation::ShiftToStartIMU(float pointTime) {
  imuShiftFromStartCur = imuShiftCur - imuShiftStart - imuVeloStart * pointTime;

  float x1 = cosImuYawStart * imuShiftFromStartCur.x() -
             sinImuYawStart * imuShiftFromStartCur.z();
  float y1 = imuShiftFromStartCur.y();
  float z1 = sinImuYawStart * imuShiftFromStartCur.x() +
             cosImuYawStart * imuShiftFromStartCur.z();

  float x2 = x1;
  float y2 = cosImuPitchStart * y1 + sinImuPitchStart * z1;
  float z2 = -sinImuPitchStart * y1 + cosImuPitchStart * z1;

  imuShiftFromStartCur.x() = cosImuRollStart * x2 + sinImuRollStart * y2;
  imuShiftFromStartCur.y() = -sinImuRollStart * x2 + cosImuRollStart * y2;
  imuShiftFromStartCur.z() = z2;
}

void FeatureAssociation::VeloToStartIMU() {
  imuVeloFromStartCur = imuVeloCur - imuVeloStart;

  float x1 = cosImuYawStart * imuVeloFromStartCur.x() -
             sinImuYawStart * imuVeloFromStartCur.z();
  float y1 = imuVeloFromStartCur.y();
  float z1 = sinImuYawStart * imuVeloFromStartCur.x() +
             cosImuYawStart * imuVeloFromStartCur.z();

  float x2 = x1;
  float y2 = cosImuPitchStart * y1 + sinImuPitchStart * z1;
  float z2 = -sinImuPitchStart * y1 + cosImuPitchStart * z1;

  imuVeloFromStartCur.x() = cosImuRollStart * x2 + sinImuRollStart * y2;
  imuVeloFromStartCur.y() = -sinImuRollStart * x2 + cosImuRollStart * y2;
  imuVeloFromStartCur.z() = z2;
}

void FeatureAssociation::TransformToStartIMU(PointType *p) {
  float x1 = cos(imuRollCur) * p->x - sin(imuRollCur) * p->y;
  float y1 = sin(imuRollCur) * p->x + cos(imuRollCur) * p->y;
  float z1 = p->z;

  float x2 = x1;
  float y2 = cos(imuPitchCur) * y1 - sin(imuPitchCur) * z1;
  float z2 = sin(imuPitchCur) * y1 + cos(imuPitchCur) * z1;

  float x3 = cos(imuYawCur) * x2 + sin(imuYawCur) * z2;
  float y3 = y2;
  float z3 = -sin(imuYawCur) * x2 + cos(imuYawCur) * z2;

  float x4 = cosImuYawStart * x3 - sinImuYawStart * z3;
  float y4 = y3;
  float z4 = sinImuYawStart * x3 + cosImuYawStart * z3;

  float x5 = x4;
  float y5 = cosImuPitchStart * y4 + sinImuPitchStart * z4;
  float z5 = -sinImuPitchStart * y4 + cosImuPitchStart * z4;

  p->x = cosImuRollStart * x5 + sinImuRollStart * y5 + imuShiftFromStartCur.x();
  p->y =
      -sinImuRollStart * x5 + cosImuRollStart * y5 + imuShiftFromStartCur.y();
  p->z = z5 + imuShiftFromStartCur.z();
}

void FeatureAssociation::AccumulateIMUShiftAndRotation() {
  float roll = imuRoll[imuPointerLast];
  float pitch = imuPitch[imuPointerLast];
  float yaw = imuYaw[imuPointerLast];
  Vector3 acc = imuAcc[imuPointerLast];

  float x1 = cos(roll) * acc.x() - sin(roll) * acc.y();
  float y1 = sin(roll) * acc.x() + cos(roll) * acc.y();
  float z1 = acc.z();

  float x2 = x1;
  float y2 = cos(pitch) * y1 - sin(pitch) * z1;
  float z2 = sin(pitch) * y1 + cos(pitch) * z1;

  acc.x() = cos(yaw) * x2 + sin(yaw) * z2;
  acc.y() = y2;
  acc.z() = -sin(yaw) * x2 + cos(yaw) * z2;

  int imuPointerBack = (imuPointerLast + imuQueLength - 1) % imuQueLength;
  double timeDiff = imuTime[imuPointerLast] - imuTime[imuPointerBack];
  if (timeDiff < scanPeriod) {
    imuShift[imuPointerLast] = imuShift[imuPointerBack] +
                               imuVelo[imuPointerBack] * timeDiff +
                               acc * timeDiff * timeDiff / 2;

    imuVelo[imuPointerLast] = imuVelo[imuPointerBack] + acc * timeDiff;

    imuAngularRotation[imuPointerLast] =
        imuAngularRotation[imuPointerBack] +
        imuAngularVelo[imuPointerBack] * timeDiff;
  }
}

void FeatureAssociation::imuHandler(const sensor_msgs::Imu::ConstPtr &imuIn) {
  std::lock_guard<std::mutex> lock(_imu_mutex);
  double roll, pitch, yaw;
  tf::Quaternion orientation;
  tf::quaternionMsgToTF(imuIn->orientation, orientation);
  tf::Matrix3x3(orientation).getRPY(roll, pitch, yaw);

  float accX = imuIn->linear_acceleration.y - sin(roll) * cos(pitch) * 9.81;
  float accY = imuIn->linear_acceleration.z - cos(roll) * cos(pitch) * 9.81;
  float accZ = imuIn->linear_acceleration.x + sin(pitch) * 9.81;

  imuPointerLast = (imuPointerLast + 1) % imuQueLength;

  imuTime[imuPointerLast] = imuIn->header.stamp.toSec();

  imuRoll[imuPointerLast] = roll;
  imuPitch[imuPointerLast] = pitch;
  imuYaw[imuPointerLast] = yaw;

  imuAcc[imuPointerLast] = {accX, accY, accZ};

  imuAngularVelo[imuPointerLast] = {imuIn->angular_velocity.x,
                                    imuIn->angular_velocity.y,
                                    imuIn->angular_velocity.z};
  AccumulateIMUShiftAndRotation();
}

void FeatureAssociation::adjustDistortion() {
  bool halfPassed = false;
  int cloudSize = segmentedCloud->points.size();

  PointType point;

  for (int i = 0; i < cloudSize; i++) {
    point.x = segmentedCloud->points[i].y;
    point.y = segmentedCloud->points[i].z;
    point.z = segmentedCloud->points[i].x;

    float ori = -atan2(point.x, point.z);
    if (!halfPassed) {
      if (ori < segInfo.startOrientation - M_PI / 2)
        ori += 2 * M_PI;
      else if (ori > segInfo.startOrientation + M_PI * 3 / 2)
        ori -= 2 * M_PI;

      if (ori - segInfo.startOrientation > M_PI) halfPassed = true;
    } else {
      ori += 2 * M_PI;

      if (ori < segInfo.endOrientation - M_PI * 3 / 2)
        ori += 2 * M_PI;
      else if (ori > segInfo.endOrientation + M_PI / 2)
        ori -= 2 * M_PI;
    }

    float relTime = (ori - segInfo.startOrientation) / segInfo.orientationDiff;
    point.intensity =
        int(segmentedCloud->points[i].intensity) + scanPeriod * relTime;

    if (imuPointerLast >= 0) {
      float pointTime = relTime * scanPeriod;
      imuPointerFront = imuPointerLastIteration;
      while (imuPointerFront != imuPointerLast) {
        if (timeScanCur + pointTime < imuTime[imuPointerFront]) {
          break;
        }
        imuPointerFront = (imuPointerFront + 1) % imuQueLength;
      }

      if (timeScanCur + pointTime > imuTime[imuPointerFront]) {
        imuRollCur = imuRoll[imuPointerFront];
        imuPitchCur = imuPitch[imuPointerFront];
        imuYawCur = imuYaw[imuPointerFront];

        imuVeloCur = imuVelo[imuPointerFront];
        imuShiftCur = imuShift[imuPointerFront];

      } else {
        int imuPointerBack =
            (imuPointerFront + imuQueLength - 1) % imuQueLength;
        float ratioFront = (timeScanCur + pointTime - imuTime[imuPointerBack]) /
                           (imuTime[imuPointerFront] - imuTime[imuPointerBack]);
        float ratioBack = (imuTime[imuPointerFront] - timeScanCur - pointTime) /
                          (imuTime[imuPointerFront] - imuTime[imuPointerBack]);

        imuRollCur = imuRoll[imuPointerFront] * ratioFront +
                     imuRoll[imuPointerBack] * ratioBack;
        imuPitchCur = imuPitch[imuPointerFront] * ratioFront +
                      imuPitch[imuPointerBack] * ratioBack;
        if (imuYaw[imuPointerFront] - imuYaw[imuPointerBack] > M_PI) {
          imuYawCur = imuYaw[imuPointerFront] * ratioFront +
                      (imuYaw[imuPointerBack] + 2 * M_PI) * ratioBack;
        } else if (imuYaw[imuPointerFront] - imuYaw[imuPointerBack] < -M_PI) {
          imuYawCur = imuYaw[imuPointerFront] * ratioFront +
                      (imuYaw[imuPointerBack] - 2 * M_PI) * ratioBack;
        } else {
          imuYawCur = imuYaw[imuPointerFront] * ratioFront +
                      imuYaw[imuPointerBack] * ratioBack;
        }

        imuVeloCur = imuVelo[imuPointerFront] * ratioFront +
                     imuVelo[imuPointerBack] * ratioBack;

        imuShiftCur = imuShift[imuPointerFront] * ratioFront +
                      imuShift[imuPointerBack] * ratioBack;
      }

      if (i == 0) {
        imuRollStart = imuRollCur;
        imuPitchStart = imuPitchCur;
        imuYawStart = imuYawCur;

        imuVeloStart = imuVeloCur;
        imuShiftStart = imuShiftCur;

        if (timeScanCur + pointTime > imuTime[imuPointerFront]) {
          imuAngularRotationCur = imuAngularRotation[imuPointerFront];
        } else {
          int imuPointerBack =
              (imuPointerFront + imuQueLength - 1) % imuQueLength;
          float ratioFront =
              (timeScanCur + pointTime - imuTime[imuPointerBack]) /
              (imuTime[imuPointerFront] - imuTime[imuPointerBack]);
          float ratioBack =
              (imuTime[imuPointerFront] - timeScanCur - pointTime) /
              (imuTime[imuPointerFront] - imuTime[imuPointerBack]);
          imuAngularRotationCur =
              imuAngularRotation[imuPointerFront] * ratioFront +
              imuAngularRotation[imuPointerBack] * ratioBack;
        }

        imuAngularFromStart = imuAngularRotationCur - imuAngularRotationLast;

        imuAngularRotationLast = imuAngularRotationCur;

        updateImuRollPitchYawStartSinCos();
      } else {
        VeloToStartIMU();
        TransformToStartIMU(&point);
      }
    }
    segmentedCloud->points[i] = point;
  }

  imuPointerLastIteration = imuPointerLast;
}

void FeatureAssociation::calculateSmoothness() {
  int cloudSize = segmentedCloud->points.size();
  for (int i = 5; i < cloudSize - 5; i++) {
    float diffRange = segInfo.segmentedCloudRange[i - 5] +
                      segInfo.segmentedCloudRange[i - 4] +
                      segInfo.segmentedCloudRange[i - 3] +
                      segInfo.segmentedCloudRange[i - 2] +
                      segInfo.segmentedCloudRange[i - 1] -
                      segInfo.segmentedCloudRange[i] * 10 +
                      segInfo.segmentedCloudRange[i + 1] +
                      segInfo.segmentedCloudRange[i + 2] +
                      segInfo.segmentedCloudRange[i + 3] +
                      segInfo.segmentedCloudRange[i + 4] +
                      segInfo.segmentedCloudRange[i + 5];

    cloudCurvature[i] = diffRange * diffRange;

    cloudNeighborPicked[i] = 0;
    cloudLabel[i] = 0;

    cloudSmoothness[i].value = cloudCurvature[i];
    cloudSmoothness[i].ind = i;
  }
}

void FeatureAssociation::markOccludedPoints() {
  int cloudSize = segmentedCloud->points.size();

  for (int i = 5; i < cloudSize - 6; ++i) {
    float depth1 = segInfo.segmentedCloudRange[i];
    float depth2 = segInfo.segmentedCloudRange[i + 1];
    int columnDiff = std::abs(int(segInfo.segmentedCloudColInd[i + 1] -
                                  segInfo.segmentedCloudColInd[i]));

    if (columnDiff < 10) {
      if (depth1 - depth2 > 0.3) {
        cloudNeighborPicked[i - 5] = 1;
        cloudNeighborPicked[i - 4] = 1;
        cloudNeighborPicked[i - 3] = 1;
        cloudNeighborPicked[i - 2] = 1;
        cloudNeighborPicked[i - 1] = 1;
        cloudNeighborPicked[i] = 1;
      } else if (depth2 - depth1 > 0.3) {
        cloudNeighborPicked[i + 1] = 1;
        cloudNeighborPicked[i + 2] = 1;
        cloudNeighborPicked[i + 3] = 1;
        cloudNeighborPicked[i + 4] = 1;
        cloudNeighborPicked[i + 5] = 1;
        cloudNeighborPicked[i + 6] = 1;
      }
    }

    float diff1 = std::abs(segInfo.segmentedCloudRange[i - 1] -
                           segInfo.segmentedCloudRange[i]);
    float diff2 = std::abs(segInfo.segmentedCloudRange[i + 1] -
                           segInfo.segmentedCloudRange[i]);

    if (diff1 > 0.02 * segInfo.segmentedCloudRange[i] &&
        diff2 > 0.02 * segInfo.segmentedCloudRange[i])
      cloudNeighborPicked[i] = 1;
  }
}

void FeatureAssociation::extractFeatures() {
  cornerPointsSharp->clear();
  cornerPointsLessSharp->clear();
  surfPointsFlat->clear();
  surfPointsLessFlat->clear();

  for (int i = 0; i < _N_scan; i++) {
    surfPointsLessFlatScan->clear();

    for (int j = 0; j < 6; j++) {
      int sp =
          (segInfo.startRingIndex[i] * (6 - j) + segInfo.endRingIndex[i] * j) /
          6;
      int ep = (segInfo.startRingIndex[i] * (5 - j) +
                segInfo.endRingIndex[i] * (j + 1)) /
                   6 -
               1;

      if (sp >= ep) continue;

      std::sort(cloudSmoothness.begin() + sp, cloudSmoothness.begin() + ep,
                by_value());

      int largestPickedNum = 0;
      for (int k = ep; k >= sp; k--) {
        int ind = cloudSmoothness[k].ind;
        if (cloudNeighborPicked[ind] == 0 &&
            cloudCurvature[ind] > edgeThreshold &&
            segInfo.segmentedCloudGroundFlag[ind] == false) {
          largestPickedNum++;
          if (largestPickedNum <= 2) {
            cloudLabel[ind] = 2;
            cornerPointsSharp->push_back(segmentedCloud->points[ind]);
            cornerPointsLessSharp->push_back(segmentedCloud->points[ind]);
          } else if (largestPickedNum <= 20) {
            cloudLabel[ind] = 1;
            cornerPointsLessSharp->push_back(segmentedCloud->points[ind]);
          } else {
            break;
          }

          cloudNeighborPicked[ind] = 1;
          for (int l = 1; l <= 5; l++) {
            if( ind + l >= segInfo.segmentedCloudColInd.size() ) {
              continue;
            }
            int columnDiff =
                std::abs(int(segInfo.segmentedCloudColInd[ind + l] -
                             segInfo.segmentedCloudColInd[ind + l - 1]));
            if (columnDiff > 10) break;
            cloudNeighborPicked[ind + l] = 1;
          }
          for (int l = -1; l >= -5; l--) {
            if( ind + l < 0 ) {
              continue;
            }
            int columnDiff =
                std::abs(int(segInfo.segmentedCloudColInd[ind + l] -
                             segInfo.segmentedCloudColInd[ind + l + 1]));
            if (columnDiff > 10) break;
            cloudNeighborPicked[ind + l] = 1;
          }
        }
      }

      int smallestPickedNum = 0;
      for (int k = sp; k <= ep; k++) {
        int ind = cloudSmoothness[k].ind;
        if (cloudNeighborPicked[ind] == 0 &&
            cloudCurvature[ind] < surfThreshold &&
            segInfo.segmentedCloudGroundFlag[ind] == true) {
          cloudLabel[ind] = -1;
          surfPointsFlat->push_back(segmentedCloud->points[ind]);

          smallestPickedNum++;
          if (smallestPickedNum >= 4) {
            break;
          }

          cloudNeighborPicked[ind] = 1;
          for (int l = 1; l <= 5; l++) {
            if( ind + l >= segInfo.segmentedCloudColInd.size() ) {
              continue;
            }
            int columnDiff =
                std::abs(int(segInfo.segmentedCloudColInd.at(ind + l) -
                             segInfo.segmentedCloudColInd.at(ind + l - 1)));
            if (columnDiff > 10) break;

            cloudNeighborPicked[ind + l] = 1;
          }
          for (int l = -1; l >= -5; l--) {
            if (ind + l < 0) {
              continue;
            }
            int columnDiff =
                std::abs(int(segInfo.segmentedCloudColInd.at(ind + l) -
                             segInfo.segmentedCloudColInd.at(ind + l + 1)));
            if (columnDiff > 10) break;

            cloudNeighborPicked[ind + l] = 1;
          }
        }
      }

      for (int k = sp; k <= ep; k++) {
        if (cloudLabel[k] <= 0) {
          surfPointsLessFlatScan->push_back(segmentedCloud->points[k]);
        }
      }
    }

    surfPointsLessFlatScanDS->clear();
    downSizeFilter.setInputCloud(surfPointsLessFlatScan);
    downSizeFilter.filter(*surfPointsLessFlatScanDS);

    *surfPointsLessFlat += *surfPointsLessFlatScanDS;
  }
}

void FeatureAssociation::TransformToStart(PointType const *const pi,
                                          PointType *const po) {
  float s = 10 * (pi->intensity - int(pi->intensity));

  float rx = s * transformCur[0];
  float ry = s * transformCur[1];
  float rz = s * transformCur[2];
  float tx = s * transformCur[3];
  float ty = s * transformCur[4];
  float tz = s * transformCur[5];

  float x1 = cos(rz) * (pi->x - tx) + sin(rz) * (pi->y - ty);
  float y1 = -sin(rz) * (pi->x - tx) + cos(rz) * (pi->y - ty);
  float z1 = (pi->z - tz);

  float x2 = x1;
  float y2 = cos(rx) * y1 + sin(rx) * z1;
  float z2 = -sin(rx) * y1 + cos(rx) * z1;

  po->x = cos(ry) * x2 - sin(ry) * z2;
  po->y = y2;
  po->z = sin(ry) * x2 + cos(ry) * z2;
  po->intensity = pi->intensity;
}

void FeatureAssociation::TransformToEnd(PointType const *const pi,
                                        PointType *const po) {
  float s = 10 * (pi->intensity - int(pi->intensity));

  float rx = s * transformCur[0];
  float ry = s * transformCur[1];
  float rz = s * transformCur[2];
  float tx = s * transformCur[3];
  float ty = s * transformCur[4];
  float tz = s * transformCur[5];

  float x1 = cos(rz) * (pi->x - tx) + sin(rz) * (pi->y - ty);
  float y1 = -sin(rz) * (pi->x - tx) + cos(rz) * (pi->y - ty);
  float z1 = (pi->z - tz);

  float x2 = x1;
  float y2 = cos(rx) * y1 + sin(rx) * z1;
  float z2 = -sin(rx) * y1 + cos(rx) * z1;

  float x3 = cos(ry) * x2 - sin(ry) * z2;
  float y3 = y2;
  float z3 = sin(ry) * x2 + cos(ry) * z2;

  rx = transformCur[0];
  ry = transformCur[1];
  rz = transformCur[2];
  tx = transformCur[3];
  ty = transformCur[4];
  tz = transformCur[5];

  float x4 = cos(ry) * x3 + sin(ry) * z3;
  float y4 = y3;
  float z4 = -sin(ry) * x3 + cos(ry) * z3;

  float x5 = x4;
  float y5 = cos(rx) * y4 - sin(rx) * z4;
  float z5 = sin(rx) * y4 + cos(rx) * z4;

  float x6 = cos(rz) * x5 - sin(rz) * y5 + tx;
  float y6 = sin(rz) * x5 + cos(rz) * y5 + ty;
  float z6 = z5 + tz;

  float x7 = cosImuRollStart * (x6 - imuShiftFromStart.x()) -
             sinImuRollStart * (y6 - imuShiftFromStart.y());
  float y7 = sinImuRollStart * (x6 - imuShiftFromStart.x()) +
             cosImuRollStart * (y6 - imuShiftFromStart.y());
  float z7 = z6 - imuShiftFromStart.z();

  float x8 = x7;
  float y8 = cosImuPitchStart * y7 - sinImuPitchStart * z7;
  float z8 = sinImuPitchStart * y7 + cosImuPitchStart * z7;

  float x9 = cosImuYawStart * x8 + sinImuYawStart * z8;
  float y9 = y8;
  float z9 = -sinImuYawStart * x8 + cosImuYawStart * z8;

  float x10 = cos(imuYawLast) * x9 - sin(imuYawLast) * z9;
  float y10 = y9;
  float z10 = sin(imuYawLast) * x9 + cos(imuYawLast) * z9;

  float x11 = x10;
  float y11 = cos(imuPitchLast) * y10 + sin(imuPitchLast) * z10;
  float z11 = -sin(imuPitchLast) * y10 + cos(imuPitchLast) * z10;

  po->x = cos(imuRollLast) * x11 + sin(imuRollLast) * y11;
  po->y = -sin(imuRollLast) * x11 + cos(imuRollLast) * y11;
  po->z = z11;
  po->intensity = int(pi->intensity);
}

void FeatureAssociation::PluginIMURotation(float bcx, float bcy, float bcz,
                                           float blx, float bly, float blz,
                                           float alx, float aly, float alz,
                                           float &acx, float &acy, float &acz) {
  float sbcx = sin(bcx);
  float cbcx = cos(bcx);
  float sbcy = sin(bcy);
  float cbcy = cos(bcy);
  float sbcz = sin(bcz);
  float cbcz = cos(bcz);

  float sblx = sin(blx);
  float cblx = cos(blx);
  float sbly = sin(bly);
  float cbly = cos(bly);
  float sblz = sin(blz);
  float cblz = cos(blz);

  float salx = sin(alx);
  float calx = cos(alx);
  float saly = sin(aly);
  float caly = cos(aly);
  float salz = sin(alz);
  float calz = cos(alz);

  float srx = -sbcx * (salx * sblx + calx * caly * cblx * cbly +
                       calx * cblx * saly * sbly) -
              cbcx * cbcz *
                  (calx * saly * (cbly * sblz - cblz * sblx * sbly) -
                   calx * caly * (sbly * sblz + cbly * cblz * sblx) +
                   cblx * cblz * salx) -
              cbcx * sbcz *
                  (calx * caly * (cblz * sbly - cbly * sblx * sblz) -
                   calx * saly * (cbly * cblz + sblx * sbly * sblz) +
                   cblx * salx * sblz);
  acx = -asin(srx);

  float srycrx =
      (cbcy * sbcz - cbcz * sbcx * sbcy) *
          (calx * saly * (cbly * sblz - cblz * sblx * sbly) -
           calx * caly * (sbly * sblz + cbly * cblz * sblx) +
           cblx * cblz * salx) -
      (cbcy * cbcz + sbcx * sbcy * sbcz) *
          (calx * caly * (cblz * sbly - cbly * sblx * sblz) -
           calx * saly * (cbly * cblz + sblx * sbly * sblz) +
           cblx * salx * sblz) +
      cbcx * sbcy *
          (salx * sblx + calx * caly * cblx * cbly + calx * cblx * saly * sbly);
  float crycrx =
      (cbcz * sbcy - cbcy * sbcx * sbcz) *
          (calx * caly * (cblz * sbly - cbly * sblx * sblz) -
           calx * saly * (cbly * cblz + sblx * sbly * sblz) +
           cblx * salx * sblz) -
      (sbcy * sbcz + cbcy * cbcz * sbcx) *
          (calx * saly * (cbly * sblz - cblz * sblx * sbly) -
           calx * caly * (sbly * sblz + cbly * cblz * sblx) +
           cblx * cblz * salx) +
      cbcx * cbcy *
          (salx * sblx + calx * caly * cblx * cbly + calx * cblx * saly * sbly);
  acy = atan2(srycrx / cos(acx), crycrx / cos(acx));

  float srzcrx = sbcx * (cblx * cbly * (calz * saly - caly * salx * salz) -
                         cblx * sbly * (caly * calz + salx * saly * salz) +
                         calx * salz * sblx) -
                 cbcx * cbcz *
                     ((caly * calz + salx * saly * salz) *
                          (cbly * sblz - cblz * sblx * sbly) +
                      (calz * saly - caly * salx * salz) *
                          (sbly * sblz + cbly * cblz * sblx) -
                      calx * cblx * cblz * salz) +
                 cbcx * sbcz *
                     ((caly * calz + salx * saly * salz) *
                          (cbly * cblz + sblx * sbly * sblz) +
                      (calz * saly - caly * salx * salz) *
                          (cblz * sbly - cbly * sblx * sblz) +
                      calx * cblx * salz * sblz);
  float crzcrx = sbcx * (cblx * sbly * (caly * salz - calz * salx * saly) -
                         cblx * cbly * (saly * salz + caly * calz * salx) +
                         calx * calz * sblx) +
                 cbcx * cbcz *
                     ((saly * salz + caly * calz * salx) *
                          (sbly * sblz + cbly * cblz * sblx) +
                      (caly * salz - calz * salx * saly) *
                          (cbly * sblz - cblz * sblx * sbly) +
                      calx * calz * cblx * cblz) -
                 cbcx * sbcz *
                     ((saly * salz + caly * calz * salx) *
                          (cblz * sbly - cbly * sblx * sblz) +
                      (caly * salz - calz * salx * saly) *
                          (cbly * cblz + sblx * sbly * sblz) -
                      calx * calz * cblx * sblz);
  acz = atan2(srzcrx / cos(acx), crzcrx / cos(acx));
}

void FeatureAssociation::AccumulateRotation(float cx, float cy, float cz,
                                            float lx, float ly, float lz,
                                            float &ox, float &oy, float &oz) {
  float srx = cos(lx) * cos(cx) * sin(ly) * sin(cz) -
              cos(cx) * cos(cz) * sin(lx) - cos(lx) * cos(ly) * sin(cx);
  ox = -asin(srx);

  float srycrx =
      sin(lx) * (cos(cy) * sin(cz) - cos(cz) * sin(cx) * sin(cy)) +
      cos(lx) * sin(ly) * (cos(cy) * cos(cz) + sin(cx) * sin(cy) * sin(cz)) +
      cos(lx) * cos(ly) * cos(cx) * sin(cy);
  float crycrx =
      cos(lx) * cos(ly) * cos(cx) * cos(cy) -
      cos(lx) * sin(ly) * (cos(cz) * sin(cy) - cos(cy) * sin(cx) * sin(cz)) -
      sin(lx) * (sin(cy) * sin(cz) + cos(cy) * cos(cz) * sin(cx));
  oy = atan2(srycrx / cos(ox), crycrx / cos(ox));

  float srzcrx =
      sin(cx) * (cos(lz) * sin(ly) - cos(ly) * sin(lx) * sin(lz)) +
      cos(cx) * sin(cz) * (cos(ly) * cos(lz) + sin(lx) * sin(ly) * sin(lz)) +
      cos(lx) * cos(cx) * cos(cz) * sin(lz);
  float crzcrx =
      cos(lx) * cos(lz) * cos(cx) * cos(cz) -
      cos(cx) * sin(cz) * (cos(ly) * sin(lz) - cos(lz) * sin(lx) * sin(ly)) -
      sin(cx) * (sin(ly) * sin(lz) + cos(ly) * cos(lz) * sin(lx));
  oz = atan2(srzcrx / cos(ox), crzcrx / cos(ox));
}

void FeatureAssociation::findCorrespondingCornerFeatures(int iterCount) {
  int cornerPointsSharpNum = cornerPointsSharp->points.size();

  for (int i = 0; i < cornerPointsSharpNum; i++) {
    PointType pointSel;
    TransformToStart(&cornerPointsSharp->points[i], &pointSel);

    if (iterCount % 5 == 0) {
      kdtreeCornerLast.nearestKSearch(pointSel, 1, pointSearchInd,
                                       pointSearchSqDis);
      int closestPointInd = -1, minPointInd2 = -1;

      if (pointSearchSqDis[0] < nearestFeatureSearchSqDist) {
        closestPointInd = pointSearchInd[0];
        int closestPointScan =
            int(laserCloudCornerLast->points[closestPointInd].intensity);

        float pointSqDis, minPointSqDis2 = nearestFeatureSearchSqDist;
        for (int j = closestPointInd + 1; j < cornerPointsSharpNum; j++) {
          if (int(laserCloudCornerLast->points[j].intensity) >
              closestPointScan + 2.5) {
            break;
          }

          pointSqDis = (laserCloudCornerLast->points[j].x - pointSel.x) *
                           (laserCloudCornerLast->points[j].x - pointSel.x) +
                       (laserCloudCornerLast->points[j].y - pointSel.y) *
                           (laserCloudCornerLast->points[j].y - pointSel.y) +
                       (laserCloudCornerLast->points[j].z - pointSel.z) *
                           (laserCloudCornerLast->points[j].z - pointSel.z);

          if (int(laserCloudCornerLast->points[j].intensity) >
              closestPointScan) {
            if (pointSqDis < minPointSqDis2) {
              minPointSqDis2 = pointSqDis;
              minPointInd2 = j;
            }
          }
        }
        for (int j = closestPointInd - 1; j >= 0; j--) {
          if (int(laserCloudCornerLast->points[j].intensity) <
              closestPointScan - 2.5) {
            break;
          }

          pointSqDis = (laserCloudCornerLast->points[j].x - pointSel.x) *
                           (laserCloudCornerLast->points[j].x - pointSel.x) +
                       (laserCloudCornerLast->points[j].y - pointSel.y) *
                           (laserCloudCornerLast->points[j].y - pointSel.y) +
                       (laserCloudCornerLast->points[j].z - pointSel.z) *
                           (laserCloudCornerLast->points[j].z - pointSel.z);

          if (int(laserCloudCornerLast->points[j].intensity) <
              closestPointScan) {
            if (pointSqDis < minPointSqDis2) {
              minPointSqDis2 = pointSqDis;
              minPointInd2 = j;
            }
          }
        }
      }

      pointSearchCornerInd1[i] = closestPointInd;
      pointSearchCornerInd2[i] = minPointInd2;
    }

    if (pointSearchCornerInd2[i] >= 0) {
      PointType tripod1 =
          laserCloudCornerLast->points[pointSearchCornerInd1[i]];
      PointType tripod2 =
          laserCloudCornerLast->points[pointSearchCornerInd2[i]];

      float x0 = pointSel.x;
      float y0 = pointSel.y;
      float z0 = pointSel.z;
      float x1 = tripod1.x;
      float y1 = tripod1.y;
      float z1 = tripod1.z;
      float x2 = tripod2.x;
      float y2 = tripod2.y;
      float z2 = tripod2.z;

      float m11 = ((x0 - x1) * (y0 - y2) - (x0 - x2) * (y0 - y1));
      float m22 = ((x0 - x1) * (z0 - z2) - (x0 - x2) * (z0 - z1));
      float m33 = ((y0 - y1) * (z0 - z2) - (y0 - y2) * (z0 - z1));

      float a012 = sqrt(m11 * m11 + m22 * m22 + m33 * m33);

      float l12 = sqrt((x1 - x2) * (x1 - x2) + (y1 - y2) * (y1 - y2) +
                       (z1 - z2) * (z1 - z2));

      float la = ((y1 - y2) * m11 + (z1 - z2) * m22) / a012 / l12;

      float lb = -((x1 - x2) * m11 - (z1 - z2) * m33) / a012 / l12;

      float lc = -((x1 - x2) * m22 + (y1 - y2) * m33) / a012 / l12;

      float ld2 = a012 / l12;

      float s = 1;
      if (iterCount >= 5) {
        s = 1 - 1.8 * fabs(ld2);
      }

      if (s > 0.1 && ld2 != 0) {
        PointType coeff;
        coeff.x = s * la;
        coeff.y = s * lb;
        coeff.z = s * lc;
        coeff.intensity = s * ld2;

        laserCloudOri->push_back(cornerPointsSharp->points[i]);
        coeffSel->push_back(coeff);
      }
    }
  }
}

void FeatureAssociation::findCorrespondingSurfFeatures(int iterCount) {
  int surfPointsFlatNum = surfPointsFlat->points.size();

  for (int i = 0; i < surfPointsFlatNum; i++) {
    PointType pointSel;
    TransformToStart(&surfPointsFlat->points[i], &pointSel);

    if (iterCount % 5 == 0) {
      kdtreeSurfLast.nearestKSearch(pointSel, 1, pointSearchInd,
                                     pointSearchSqDis);
      int closestPointInd = -1, minPointInd2 = -1, minPointInd3 = -1;

      if (pointSearchSqDis[0] < nearestFeatureSearchSqDist) {
        closestPointInd = pointSearchInd[0];
        int closestPointScan =
            int(laserCloudSurfLast->points[closestPointInd].intensity);

        float pointSqDis, minPointSqDis2 = nearestFeatureSearchSqDist,
                          minPointSqDis3 = nearestFeatureSearchSqDist;
        for (int j = closestPointInd + 1; j < surfPointsFlatNum; j++) {
          if (int(laserCloudSurfLast->points[j].intensity) >
              closestPointScan + 2.5) {
            break;
          }

          pointSqDis = (laserCloudSurfLast->points[j].x - pointSel.x) *
                           (laserCloudSurfLast->points[j].x - pointSel.x) +
                       (laserCloudSurfLast->points[j].y - pointSel.y) *
                           (laserCloudSurfLast->points[j].y - pointSel.y) +
                       (laserCloudSurfLast->points[j].z - pointSel.z) *
                           (laserCloudSurfLast->points[j].z - pointSel.z);

          if (int(laserCloudSurfLast->points[j].intensity) <=
              closestPointScan) {
            if (pointSqDis < minPointSqDis2) {
              minPointSqDis2 = pointSqDis;
              minPointInd2 = j;
            }
          } else {
            if (pointSqDis < minPointSqDis3) {
              minPointSqDis3 = pointSqDis;
              minPointInd3 = j;
            }
          }
        }
        for (int j = closestPointInd - 1; j >= 0; j--) {
          if (int(laserCloudSurfLast->points[j].intensity) <
              closestPointScan - 2.5) {
            break;
          }

          pointSqDis = (laserCloudSurfLast->points[j].x - pointSel.x) *
                           (laserCloudSurfLast->points[j].x - pointSel.x) +
                       (laserCloudSurfLast->points[j].y - pointSel.y) *
                           (laserCloudSurfLast->points[j].y - pointSel.y) +
                       (laserCloudSurfLast->points[j].z - pointSel.z) *
                           (laserCloudSurfLast->points[j].z - pointSel.z);

          if (int(laserCloudSurfLast->points[j].intensity) >=
              closestPointScan) {
            if (pointSqDis < minPointSqDis2) {
              minPointSqDis2 = pointSqDis;
              minPointInd2 = j;
            }
          } else {
            if (pointSqDis < minPointSqDis3) {
              minPointSqDis3 = pointSqDis;
              minPointInd3 = j;
            }
          }
        }
      }

      pointSearchSurfInd1[i] = closestPointInd;
      pointSearchSurfInd2[i] = minPointInd2;
      pointSearchSurfInd3[i] = minPointInd3;
    }

    if (pointSearchSurfInd2[i] >= 0 && pointSearchSurfInd3[i] >= 0) {
      PointType tripod1 = laserCloudSurfLast->points[pointSearchSurfInd1[i]];
      PointType tripod2 = laserCloudSurfLast->points[pointSearchSurfInd2[i]];
      PointType tripod3 = laserCloudSurfLast->points[pointSearchSurfInd3[i]];

      float pa = (tripod2.y - tripod1.y) * (tripod3.z - tripod1.z) -
                 (tripod3.y - tripod1.y) * (tripod2.z - tripod1.z);
      float pb = (tripod2.z - tripod1.z) * (tripod3.x - tripod1.x) -
                 (tripod3.z - tripod1.z) * (tripod2.x - tripod1.x);
      float pc = (tripod2.x - tripod1.x) * (tripod3.y - tripod1.y) -
                 (tripod3.x - tripod1.x) * (tripod2.y - tripod1.y);
      float pd = -(pa * tripod1.x + pb * tripod1.y + pc * tripod1.z);

      float ps = sqrt(pa * pa + pb * pb + pc * pc);

      pa /= ps;
      pb /= ps;
      pc /= ps;
      pd /= ps;

      float pd2 = pa * pointSel.x + pb * pointSel.y + pc * pointSel.z + pd;

      float s = 1;
      if (iterCount >= 5) {
        s = 1 -
            1.8 * fabs(pd2) /
                sqrt(sqrt(pointSel.x * pointSel.x + pointSel.y * pointSel.y +
                          pointSel.z * pointSel.z));
      }

      if (s > 0.1 && pd2 != 0) {
        PointType coeff;
        coeff.x = s * pa;
        coeff.y = s * pb;
        coeff.z = s * pc;
        coeff.intensity = s * pd2;

        laserCloudOri->push_back(surfPointsFlat->points[i]);
        coeffSel->push_back(coeff);
      }
    }
  }
}

bool FeatureAssociation::calculateTransformationSurf(int iterCount) {
  int pointSelNum = laserCloudOri->points.size();

  Eigen::Matrix<float,Eigen::Dynamic,3> matA(pointSelNum, 3);
  Eigen::Matrix<float,3,Eigen::Dynamic> matAt(3,pointSelNum);
  Eigen::Matrix<float,3,3> matAtA;
  Eigen::VectorXf matB(pointSelNum);
  Eigen::Matrix<float,3,1> matAtB;
  Eigen::Matrix<float,3,1> matX;
  Eigen::Matrix<float,3,3> matP;

  float srx = sin(transformCur[0]);
  float crx = cos(transformCur[0]);
  float sry = sin(transformCur[1]);
  float cry = cos(transformCur[1]);
  float srz = sin(transformCur[2]);
  float crz = cos(transformCur[2]);
  float tx = transformCur[3];
  float ty = transformCur[4];
  float tz = transformCur[5];

  float a1 = crx * sry * srz;
  float a2 = crx * crz * sry;
  float a3 = srx * sry;
  float a4 = tx * a1 - ty * a2 - tz * a3;
  float a5 = srx * srz;
  float a6 = crz * srx;
  float a7 = ty * a6 - tz * crx - tx * a5;
  float a8 = crx * cry * srz;
  float a9 = crx * cry * crz;
  float a10 = cry * srx;
  float a11 = tz * a10 + ty * a9 - tx * a8;

  float b1 = -crz * sry - cry * srx * srz;
  float b2 = cry * crz * srx - sry * srz;
  float b5 = cry * crz - srx * sry * srz;
  float b6 = cry * srz + crz * srx * sry;

  float c1 = -b6;
  float c2 = b5;
  float c3 = tx * b6 - ty * b5;
  float c4 = -crx * crz;
  float c5 = crx * srz;
  float c6 = ty * c5 + tx * -c4;
  float c7 = b2;
  float c8 = -b1;
  float c9 = tx * -b2 - ty * -b1;

  for (int i = 0; i < pointSelNum; i++) {
    PointType pointOri = laserCloudOri->points[i];
    PointType coeff = coeffSel->points[i];

    float arx =
        (-a1 * pointOri.x + a2 * pointOri.y + a3 * pointOri.z + a4) * coeff.x +
        (a5 * pointOri.x - a6 * pointOri.y + crx * pointOri.z + a7) * coeff.y +
        (a8 * pointOri.x - a9 * pointOri.y - a10 * pointOri.z + a11) * coeff.z;

    float arz = (c1 * pointOri.x + c2 * pointOri.y + c3) * coeff.x +
                (c4 * pointOri.x - c5 * pointOri.y + c6) * coeff.y +
                (c7 * pointOri.x + c8 * pointOri.y + c9) * coeff.z;

    float aty = -b6 * coeff.x + c4 * coeff.y + b2 * coeff.z;

    float d2 = coeff.intensity;

    matA(i, 0) = arx;
    matA(i, 1) = arz;
    matA(i, 2) = aty;
    matB(i, 0) = -0.05 * d2;
  }

  matAt = matA.transpose();
  matAtA = matAt * matA;
  matAtB = matAt * matB;
  matX = matAtA.colPivHouseholderQr().solve(matAtB);

  if (iterCount == 0) {
    Eigen::Matrix<float,1,3> matE;
    Eigen::Matrix<float,3,3> matV;
    Eigen::Matrix<float,3,3> matV2;
    
    Eigen::SelfAdjointEigenSolver< Eigen::Matrix<float,3,3> > esolver(matAtA);
    matE = esolver.eigenvalues().real();
    matV = esolver.eigenvectors().real();
    matV2 = matV;

    isDegenerate = false;
    float eignThre[3] = {10, 10, 10};
    for (int i = 2; i >= 0; i--) {
      if (matE(0, i) < eignThre[i]) {
        for (int j = 0; j < 3; j++) {
          matV2(i, j) = 0;
        }
        isDegenerate = true;
      } else {
        break;
      }
    }
    matP = matV.inverse() * matV2;
  }

  if (isDegenerate) {
    Eigen::Matrix<float,3,1> matX2;
    matX2 = matX;
    matX = matP * matX2;
  }

  transformCur[0] += matX(0, 0);
  transformCur[2] += matX(1, 0);
  transformCur[4] += matX(2, 0);

  for (int i = 0; i < 6; i++) {
    if (std::isnan(transformCur[i])) transformCur[i] = 0;
  }

  float deltaR = sqrt(pow(RAD2DEG * (matX(0, 0)), 2) +
                      pow(RAD2DEG * (matX(1, 0)), 2));
  float deltaT = sqrt(pow(matX(2, 0) * 100, 2));

  if (deltaR < 0.1 && deltaT < 0.1) {
    return false;
  }
  return true;
}

bool FeatureAssociation::calculateTransformationCorner(int iterCount) {
  int pointSelNum = laserCloudOri->points.size();

  Eigen::Matrix<float,Eigen::Dynamic,3> matA(pointSelNum, 3);
  Eigen::Matrix<float,3,Eigen::Dynamic> matAt(3,pointSelNum);
  Eigen::Matrix<float,3,3> matAtA;
  Eigen::VectorXf matB(pointSelNum);
  Eigen::Matrix<float,3,1> matAtB;
  Eigen::Matrix<float,3,1> matX;
  Eigen::Matrix<float,3,3> matP;

  float srx = sin(transformCur[0]);
  float crx = cos(transformCur[0]);
  float sry = sin(transformCur[1]);
  float cry = cos(transformCur[1]);
  float srz = sin(transformCur[2]);
  float crz = cos(transformCur[2]);
  float tx = transformCur[3];
  float ty = transformCur[4];
  float tz = transformCur[5];

  float b1 = -crz * sry - cry * srx * srz;
  float b2 = cry * crz * srx - sry * srz;
  float b3 = crx * cry;
  float b4 = tx * -b1 + ty * -b2 + tz * b3;
  float b5 = cry * crz - srx * sry * srz;
  float b6 = cry * srz + crz * srx * sry;
  float b7 = crx * sry;
  float b8 = tz * b7 - ty * b6 - tx * b5;

  float c5 = crx * srz;

  for (int i = 0; i < pointSelNum; i++) {
    PointType pointOri = laserCloudOri->points[i];
    PointType coeff = coeffSel->points[i];

    float ary =
        (b1 * pointOri.x + b2 * pointOri.y - b3 * pointOri.z + b4) * coeff.x +
        (b5 * pointOri.x + b6 * pointOri.y - b7 * pointOri.z + b8) * coeff.z;

    float atx = -b5 * coeff.x + c5 * coeff.y + b1 * coeff.z;

    float atz = b7 * coeff.x - srx * coeff.y - b3 * coeff.z;

    float d2 = coeff.intensity;

    matA(i, 0) = ary;
    matA(i, 1) = atx;
    matA(i, 2) = atz;
    matB(i, 0) = -0.05 * d2;
  }

  matAt = matA.transpose();
  matAtA = matAt * matA;
  matAtB = matAt * matB;
  matX = matAtA.colPivHouseholderQr().solve(matAtB);

  if (iterCount == 0) {
    Eigen::Matrix<float,1, 3> matE;
    Eigen::Matrix<float,3, 3> matV;
    Eigen::Matrix<float,3, 3> matV2;

    Eigen::SelfAdjointEigenSolver< Eigen::Matrix<float,3,3> > esolver(matAtA);
    matE = esolver.eigenvalues().real();
    matV = esolver.eigenvectors().real();
    matV2 = matV;

    isDegenerate = false;
    float eignThre[3] = {10, 10, 10};
    for (int i = 2; i >= 0; i--) {
      if (matE(0, i) < eignThre[i]) {
        for (int j = 0; j < 3; j++) {
          matV2(i, j) = 0;
        }
        isDegenerate = true;
      } else {
        break;
      }
    }
    matP = matV.inverse() * matV2;
  }

  if (isDegenerate) {
    Eigen::Matrix<float,3,1> matX2;
    matX2 = matX;
    matX = matP * matX2;
  }

  transformCur[1] += matX(0, 0);
  transformCur[3] += matX(1, 0);
  transformCur[5] += matX(2, 0);

  for (int i = 0; i < 6; i++) {
    if (std::isnan(transformCur[i])) transformCur[i] = 0;
  }

  float deltaR = sqrt(pow(RAD2DEG * (matX(0, 0)), 2));
  float deltaT = sqrt(pow(matX(1, 0) * 100, 2) +
                      pow(matX(2, 0) * 100, 2));

  if (deltaR < 0.1 && deltaT < 0.1) {
    return false;
  }
  return true;
}

bool FeatureAssociation::calculateTransformation(int iterCount) {
  int pointSelNum = laserCloudOri->points.size();

  Eigen::Matrix<float,Eigen::Dynamic,6> matA(pointSelNum, 6);
  Eigen::Matrix<float,6,Eigen::Dynamic> matAt(6,pointSelNum);
  Eigen::Matrix<float,6,6> matAtA;
  Eigen::VectorXf matB(pointSelNum);
  Eigen::Matrix<float,6,1> matAtB;
  Eigen::Matrix<float,6,1> matX;
  Eigen::Matrix<float,6,6> matP;

  float srx = sin(transformCur[0]);
  float crx = cos(transformCur[0]);
  float sry = sin(transformCur[1]);
  float cry = cos(transformCur[1]);
  float srz = sin(transformCur[2]);
  float crz = cos(transformCur[2]);
  float tx = transformCur[3];
  float ty = transformCur[4];
  float tz = transformCur[5];

  float a1 = crx * sry * srz;
  float a2 = crx * crz * sry;
  float a3 = srx * sry;
  float a4 = tx * a1 - ty * a2 - tz * a3;
  float a5 = srx * srz;
  float a6 = crz * srx;
  float a7 = ty * a6 - tz * crx - tx * a5;
  float a8 = crx * cry * srz;
  float a9 = crx * cry * crz;
  float a10 = cry * srx;
  float a11 = tz * a10 + ty * a9 - tx * a8;

  float b1 = -crz * sry - cry * srx * srz;
  float b2 = cry * crz * srx - sry * srz;
  float b3 = crx * cry;
  float b4 = tx * -b1 + ty * -b2 + tz * b3;
  float b5 = cry * crz - srx * sry * srz;
  float b6 = cry * srz + crz * srx * sry;
  float b7 = crx * sry;
  float b8 = tz * b7 - ty * b6 - tx * b5;

  float c1 = -b6;
  float c2 = b5;
  float c3 = tx * b6 - ty * b5;
  float c4 = -crx * crz;
  float c5 = crx * srz;
  float c6 = ty * c5 + tx * -c4;
  float c7 = b2;
  float c8 = -b1;
  float c9 = tx * -b2 - ty * -b1;

  for (int i = 0; i < pointSelNum; i++) {
    PointType pointOri = laserCloudOri->points[i];
    PointType coeff = coeffSel->points[i];

    float arx =
        (-a1 * pointOri.x + a2 * pointOri.y + a3 * pointOri.z + a4) * coeff.x +
        (a5 * pointOri.x - a6 * pointOri.y + crx * pointOri.z + a7) * coeff.y +
        (a8 * pointOri.x - a9 * pointOri.y - a10 * pointOri.z + a11) * coeff.z;

    float ary =
        (b1 * pointOri.x + b2 * pointOri.y - b3 * pointOri.z + b4) * coeff.x +
        (b5 * pointOri.x + b6 * pointOri.y - b7 * pointOri.z + b8) * coeff.z;

    float arz = (c1 * pointOri.x + c2 * pointOri.y + c3) * coeff.x +
                (c4 * pointOri.x - c5 * pointOri.y + c6) * coeff.y +
                (c7 * pointOri.x + c8 * pointOri.y + c9) * coeff.z;

    float atx = -b5 * coeff.x + c5 * coeff.y + b1 * coeff.z;

    float aty = -b6 * coeff.x + c4 * coeff.y + b2 * coeff.z;

    float atz = b7 * coeff.x - srx * coeff.y - b3 * coeff.z;

    float d2 = coeff.intensity;

    matA(i, 0) = arx;
    matA(i, 1) = ary;
    matA(i, 2) = arz;
    matA(i, 3) = atx;
    matA(i, 4) = aty;
    matA(i, 5) = atz;
    matB(i, 0) = -0.05 * d2;
  }

  matAt = matA.transpose();
  matAtA = matAt * matA;
  matAtB = matAt * matB;
  matX = matAtA.colPivHouseholderQr().solve(matAtB);

  if (iterCount == 0) {
    Eigen::Matrix<float,1, 6> matE;
    Eigen::Matrix<float,6, 6> matV;
    Eigen::Matrix<float,6, 6> matV2;

    Eigen::SelfAdjointEigenSolver< Eigen::Matrix<float,6,6> > esolver(matAtA);
    matE = esolver.eigenvalues().real();
    matV = esolver.eigenvectors().real();
    matV2 = matV;

    isDegenerate = false;
    float eignThre[6] = {10, 10, 10, 10, 10, 10};
    for (int i = 5; i >= 0; i--) {
      if (matE(0, i) < eignThre[i]) {
        for (int j = 0; j < 6; j++) {
          matV2(i, j) = 0;
        }
        isDegenerate = true;
      } else {
        break;
      }
    }
    matP = matV.inverse() * matV2;
  }

  if (isDegenerate) {
    Eigen::Matrix<float,6,1> matX2;
    matX2 = matX;
    matX = matP * matX2;
  }

  transformCur[0] += matX(0, 0);
  transformCur[1] += matX(1, 0);
  transformCur[2] += matX(2, 0);
  transformCur[3] += matX(3, 0);
  transformCur[4] += matX(4, 0);
  transformCur[5] += matX(5, 0);

  for (int i = 0; i < 6; i++) {
    if (std::isnan(transformCur[i])) transformCur[i] = 0;
  }

  float deltaR = sqrt(pow(RAD2DEG * (matX(0, 0)), 2) +
                      pow(RAD2DEG * (matX(1, 0)), 2) +
                      pow(RAD2DEG * (matX(2, 0)), 2));
  float deltaT = sqrt(pow(matX(3, 0) * 100, 2) +
                      pow(matX(4, 0) * 100, 2) +
                      pow(matX(5, 0) * 100, 2));

  if (deltaR < 0.1 && deltaT < 0.1) {
    return false;
  }
  return true;
}

void FeatureAssociation::checkSystemInitialization() {
  pcl::PointCloud<PointType>::Ptr laserCloudTemp = cornerPointsLessSharp;
  cornerPointsLessSharp = laserCloudCornerLast;
  laserCloudCornerLast = laserCloudTemp;

  laserCloudTemp = surfPointsLessFlat;
  surfPointsLessFlat = laserCloudSurfLast;
  laserCloudSurfLast = laserCloudTemp;

  kdtreeCornerLast.setInputCloud(laserCloudCornerLast);
  kdtreeSurfLast.setInputCloud(laserCloudSurfLast);

  laserCloudCornerLastNum = laserCloudCornerLast->points.size();
  laserCloudSurfLastNum = laserCloudSurfLast->points.size();

  sensor_msgs::PointCloud2 laserCloudCornerLast2;
  pcl::toROSMsg(*laserCloudCornerLast, laserCloudCornerLast2);
  laserCloudCornerLast2.header.stamp = cloudHeader.stamp;
  laserCloudCornerLast2.header.frame_id = "/camera";
  _pub_cloud_corner_last.publish(laserCloudCornerLast2);

  sensor_msgs::PointCloud2 laserCloudSurfLast2;
  pcl::toROSMsg(*laserCloudSurfLast, laserCloudSurfLast2);
  laserCloudSurfLast2.header.stamp = cloudHeader.stamp;
  laserCloudSurfLast2.header.frame_id = "/camera";
  _pub_cloud_surf_last.publish(laserCloudSurfLast2);

  transformSum[0] += imuPitchStart;
  transformSum[2] += imuRollStart;

  systemInitedLM = true;
}

void FeatureAssociation::updateInitialGuess() {
  imuPitchLast = imuPitchCur;
  imuYawLast = imuYawCur;
  imuRollLast = imuRollCur;

  imuShiftFromStart = imuShiftFromStartCur;
  imuVeloFromStart = imuVeloFromStartCur;

  if (imuAngularFromStart.x() != 0 || imuAngularFromStart.y() != 0 ||
      imuAngularFromStart.z() != 0) {
    transformCur[0] = -imuAngularFromStart.y();
    transformCur[1] = -imuAngularFromStart.z();
    transformCur[2] = -imuAngularFromStart.x();
  }

  if (imuVeloFromStart.x() != 0 || imuVeloFromStart.y() != 0 ||
      imuVeloFromStart.z() != 0) {
    transformCur[3] -= imuVeloFromStart.x() * scanPeriod;
    transformCur[4] -= imuVeloFromStart.y() * scanPeriod;
    transformCur[5] -= imuVeloFromStart.z() * scanPeriod;
  }
}

void FeatureAssociation::updateTransformation() {
  if (laserCloudCornerLastNum < 10 || laserCloudSurfLastNum < 100) return;

  for (int iterCount1 = 0; iterCount1 < 25; iterCount1++) {
    laserCloudOri->clear();
    coeffSel->clear();

    findCorrespondingSurfFeatures(iterCount1);

    if (laserCloudOri->points.size() < 10) continue;
    if (calculateTransformationSurf(iterCount1) == false) break;
  }

  for (int iterCount2 = 0; iterCount2 < 25; iterCount2++) {
    laserCloudOri->clear();
    coeffSel->clear();

    findCorrespondingCornerFeatures(iterCount2);

    if (laserCloudOri->points.size() < 10) continue;
    if (calculateTransformationCorner(iterCount2) == false) break;
  }
}

void FeatureAssociation::integrateTransformation() {
  float rx, ry, rz, tx, ty, tz;
  AccumulateRotation(transformSum[0], transformSum[1], transformSum[2],
                     -transformCur[0], -transformCur[1], -transformCur[2], rx,
                     ry, rz);

  float x1 = cos(rz) * (transformCur[3] - imuShiftFromStart.x()) -
             sin(rz) * (transformCur[4] - imuShiftFromStart.y());
  float y1 = sin(rz) * (transformCur[3] - imuShiftFromStart.x()) +
             cos(rz) * (transformCur[4] - imuShiftFromStart.y());
  float z1 = transformCur[5] - imuShiftFromStart.z();

  float x2 = x1;
  float y2 = cos(rx) * y1 - sin(rx) * z1;
  float z2 = sin(rx) * y1 + cos(rx) * z1;

  tx = transformSum[3] - (cos(ry) * x2 + sin(ry) * z2);
  ty = transformSum[4] - y2;
  tz = transformSum[5] - (-sin(ry) * x2 + cos(ry) * z2);

  PluginIMURotation(rx, ry, rz, imuPitchStart, imuYawStart, imuRollStart,
                    imuPitchLast, imuYawLast, imuRollLast, rx, ry, rz);

  transformSum[0] = rx;
  transformSum[1] = ry;
  transformSum[2] = rz;
  transformSum[3] = tx;
  transformSum[4] = ty;
  transformSum[5] = tz;
}

void FeatureAssociation::adjustOutlierCloud() {
  PointType point;
  int cloudSize = outlierCloud->points.size();
  for (int i = 0; i < cloudSize; ++i) {
    point.x = outlierCloud->points[i].y;
    point.y = outlierCloud->points[i].z;
    point.z = outlierCloud->points[i].x;
    point.intensity = outlierCloud->points[i].intensity;
    outlierCloud->points[i] = point;
  }
}

void FeatureAssociation::publishOdometry() {
  geometry_msgs::Quaternion geoQuat = tf::createQuaternionMsgFromRollPitchYaw(
      transformSum[2], -transformSum[0], -transformSum[1]);

  laserOdometry.header.stamp = cloudHeader.stamp;
  laserOdometry.pose.pose.orientation.x = -geoQuat.y;
  laserOdometry.pose.pose.orientation.y = -geoQuat.z;
  laserOdometry.pose.pose.orientation.z = geoQuat.x;
  laserOdometry.pose.pose.orientation.w = geoQuat.w;
  laserOdometry.pose.pose.position.x = transformSum[3];
  laserOdometry.pose.pose.position.y = transformSum[4];
  laserOdometry.pose.pose.position.z = transformSum[5];
  pubLaserOdometry.publish(laserOdometry);

  laserOdometryTrans.stamp_ = cloudHeader.stamp;
  laserOdometryTrans.setRotation(
      tf::Quaternion(-geoQuat.y, -geoQuat.z, geoQuat.x, geoQuat.w));
  laserOdometryTrans.setOrigin(
      tf::Vector3(transformSum[3], transformSum[4], transformSum[5]));
  tfBroadcaster.sendTransform(laserOdometryTrans);
}

void FeatureAssociation::publishCloud() {
  sensor_msgs::PointCloud2 laserCloudOutMsg;

  auto Publish = [&](ros::Publisher &pub,
                     const pcl::PointCloud<PointType>::Ptr &cloud) {
    if (pub.getNumSubscribers() != 0) {
      pcl::toROSMsg(*cloud, laserCloudOutMsg);
      laserCloudOutMsg.header.stamp = cloudHeader.stamp;
      laserCloudOutMsg.header.frame_id = "/camera";
      pub.publish(laserCloudOutMsg);
    }
  };

  Publish(pubCornerPointsSharp, cornerPointsSharp);
  Publish(pubCornerPointsLessSharp, cornerPointsLessSharp);
  Publish(pubSurfPointsFlat, surfPointsFlat);
  Publish(pubSurfPointsLessFlat, surfPointsLessFlat);
}

void FeatureAssociation::publishCloudsLast() {
  updateImuRollPitchYawStartSinCos();

  int cornerPointsLessSharpNum = cornerPointsLessSharp->points.size();
  for (int i = 0; i < cornerPointsLessSharpNum; i++) {
    TransformToEnd(&cornerPointsLessSharp->points[i],
                   &cornerPointsLessSharp->points[i]);
  }

  int surfPointsLessFlatNum = surfPointsLessFlat->points.size();
  for (int i = 0; i < surfPointsLessFlatNum; i++) {
    TransformToEnd(&surfPointsLessFlat->points[i],
                   &surfPointsLessFlat->points[i]);
  }

  pcl::PointCloud<PointType>::Ptr laserCloudTemp = cornerPointsLessSharp;
  cornerPointsLessSharp = laserCloudCornerLast;
  laserCloudCornerLast = laserCloudTemp;

  laserCloudTemp = surfPointsLessFlat;
  surfPointsLessFlat = laserCloudSurfLast;
  laserCloudSurfLast = laserCloudTemp;

  laserCloudCornerLastNum = laserCloudCornerLast->points.size();
  laserCloudSurfLastNum = laserCloudSurfLast->points.size();

  if (laserCloudCornerLastNum > 10 && laserCloudSurfLastNum > 100) {
    kdtreeCornerLast.setInputCloud(laserCloudCornerLast);
    kdtreeSurfLast.setInputCloud(laserCloudSurfLast);
  }

  frameCount++;
  adjustOutlierCloud();

  if (frameCount >= skipFrameNum + 1) {
    frameCount = 0;
    sensor_msgs::PointCloud2 cloudTemp;

    auto Publish = [&](ros::Publisher &pub,
                       const pcl::PointCloud<PointType>::Ptr &cloud) {
      if (pub.getNumSubscribers() != 0) {
        pcl::toROSMsg(*cloud, cloudTemp);
        cloudTemp.header.stamp = cloudHeader.stamp;
        cloudTemp.header.frame_id = "/camera";
        pub.publish(cloudTemp);
      }
    };

    Publish(_pub_outlier_cloudLast, outlierCloud);
    Publish(_pub_cloud_corner_last, laserCloudCornerLast);
    Publish(_pub_cloud_surf_last, laserCloudSurfLast);
  }
}

void FeatureAssociation::runFeatureAssociation() {
  while (ros::ok()) {
    ProjectionOut projection;
    _input_channel.receive(projection);

    if( !ros::ok() ) break;

    //--------------
    std::lock_guard<std::mutex> lock(_imu_mutex);

    outlierCloud = projection.outlier_cloud;
    segmentedCloud = projection.segmented_cloud;
    segInfo = std::move(projection.seg_msg);

    cloudHeader = segInfo.header;
    timeScanCur = cloudHeader.stamp.toSec();

    /**  1. Feature Extraction  */
    adjustDistortion();

    calculateSmoothness();

    markOccludedPoints();

    extractFeatures();

    publishCloud();  // cloud for visualization

    // Feature Association
    if (!systemInitedLM) {
      checkSystemInitialization();
      continue;
    }

    updateInitialGuess();

    updateTransformation();

    integrateTransformation();

    publishOdometry();

    publishCloudsLast();  // cloud to mapOptimization

    //--------------
    cycle_count++;

    if (cycle_count == mappingFrequencyDivider) {
      cycle_count = 0;
      AssociationOut out;
      out.cloud_corner_last.reset(new pcl::PointCloud<PointType>());
      out.cloud_surf_last.reset(new pcl::PointCloud<PointType>());
      out.cloud_outlier_last.reset(new pcl::PointCloud<PointType>());

      *out.cloud_corner_last = *laserCloudCornerLast;
      *out.cloud_surf_last = *laserCloudSurfLast;
      *out.cloud_outlier_last = *outlierCloud;

      out.laser_odometry = laserOdometry;

      _output_channel.send(std::move(out));
    }
  }
}
