/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   Tracker.h
 * @brief  Class describing temporal tracking
 * @author Antoni Rosinol
 * @author Luca Carlone
 */

// TODO(Toni): put tracker in another folder.
#pragma once

#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/highgui/highgui_c.h>

#include <gtsam/geometry/StereoCamera.h>

#include "kimera-vio/frontend/FeatureSelector.h"
#include "kimera-vio/frontend/Frame.h"
#include "kimera-vio/frontend/StereoFrame.h"
#include "kimera-vio/frontend/Tracker-definitions.h"
#include "kimera-vio/utils/Macros.h"
#include "kimera-vio/utils/UtilsOpenCV.h"

// implementation of feature selector, still within the tracker class
#include <gtsam/nonlinear/Marginals.h>

namespace VIO {

enum class OpticalFlowPredictorType {
  kStatic = 0,
  kRotational = 1,
};

class OpticalFlowPredictor {
 public:
  KIMERA_POINTER_TYPEDEFS(OpticalFlowPredictor);
  KIMERA_DELETE_COPY_CONSTRUCTORS(OpticalFlowPredictor);
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  OpticalFlowPredictor() = default;
  virtual ~OpticalFlowPredictor() = default;

  /**
   * @brief predictFlow Predicts optical flow for a set of image keypoints.
   * The optical flow determines the position of image features in consecutive
   * frames.
   * @param prev_kps: keypoints in previous (reference) image
   * @param next_kps: keypoints in next image
   * @return true if flow could be determined successfully
   */
  virtual bool predictFlow(const KeypointsCV& prev_kps,
                           KeypointsCV* next_kps) = 0;
};

/**
 * @brief The SillyOpticalFlowPredictor class just assumes that the camera
 * did not move and so the features on the previous frame remain at the same
 * pixel positions in the current frame.
 */
class SillyOpticalFlowPredictor : public OpticalFlowPredictor {
 public:
  KIMERA_POINTER_TYPEDEFS(SillyOpticalFlowPredictor);
  KIMERA_DELETE_COPY_CONSTRUCTORS(SillyOpticalFlowPredictor);
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  SillyOpticalFlowPredictor() = default;
  virtual ~SillyOpticalFlowPredictor() = default;

  bool predictFlow(const KeypointsCV& prev_kps,
                   KeypointsCV* next_kps) override {
    *CHECK_NOTNULL(next_kps) = prev_kps;
    return true;
  }
};

/**
 * @brief The RotationalOpticalFlowPredictor class predicts optical flow
 * by using a guess of inter-frame rotation and assumes no translation btw
 * frames.
 */
class RotationalOpticalFlowPredictor : public OpticalFlowPredictor {
 public:
  KIMERA_POINTER_TYPEDEFS(RotationalOpticalFlowPredictor);
  KIMERA_DELETE_COPY_CONSTRUCTORS(RotationalOpticalFlowPredictor);
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  RotationalOpticalFlowPredictor(const cv::Matx33f& K)
      : K_(K), K_inverse_(K.inv()) {}
  virtual ~RotationalOpticalFlowPredictor() = default;

  inline bool updateInterFrameRotation(const gtsam::Rot3& rotation) {
    inter_frame_rotation_ = rotation;
  }

  bool predictFlow(const KeypointsCV& prev_kps,
                   KeypointsCV* next_kps) override {
    CHECK_NOTNULL(next_kps)->clear();

    // lf_R_f is a relative rotation which takes a vector from the last frame to
    // the current frame.
    cv::Matx33f R =
        UtilsOpenCV::gtsamMatrix3ToCvMat(inter_frame_rotation_.matrix());
    cv::Matx33f H = K_ * R * K_inverse_;
    const size_t n_kps = prev_kps.size();
    next_kps->resize(n_kps);
    for (size_t i = 0; i < n_kps; ++i) {
      // Backproject last frame's corners to bearing vectors
      cv::Vec3f p1(prev_kps[i].x, prev_kps[i].y, 1.0f);

      // Rotate bearing vectors to current frame
      cv::Vec3f p2 = H * p1;

      // Project predicted bearing vectors to 2D again
      if (p2[2] > 0.0f) {
        next_kps->at(i) = cv::Point2f(p2[0] / p2[2], p2[1] / p2[2]);
      } else {
        // Projection failed, keep old corner
        next_kps->at(i) = prev_kps[i];
      }
    }
    return true;
  }

 private:
  const cv::Matx33f K_;
  const cv::Matx33f K_inverse_;
  gtsam::Rot3 inter_frame_rotation_;
};

class OpticalFlowPredictorFactory {
 public:
  template <class... Args>
  static OpticalFlowPredictor::UniquePtr makeOpticalFlowPredictor(
      const OpticalFlowPredictorType& optical_flow_predictor_type,
      Args&&... args) {
    switch (optical_flow_predictor_type) {
      case OpticalFlowPredictorType::kStatic: {
        return VIO::make_unique<SillyOpticalFlowPredictor>();
      }
      case OpticalFlowPredictorType::kRotational: {
        return VIO::make_unique<RotationalOpticalFlowPredictor>(
            std::forward<Args>(args)...);
      }
      default: {
        LOG(FATAL) << "Unknown OpticalFlowPredictorType: "
                   << static_cast<int>(optical_flow_predictor_type);
      }
    }
  }
};

class Tracker {
public:
  // Constructor
 Tracker(const VioFrontEndParams& tracker_params_,
         const CameraParams& camera_params);

 // Tracker parameters.
 const VioFrontEndParams tracker_params_;
 // Camera params for the camera used to track: currently we only use K if the
 // rotational optical flow predictor is used
 const CameraParams camera_params_;

 // Mask for features.
 cv::Mat camMask_;

 // Counters.
 int landmark_count_;  // incremental id assigned to new landmarks

public:
  void featureTracking(Frame* ref_frame,
                       Frame* cur_frame);
  void featureDetection(Frame* cur_frame);

  std::pair<TrackingStatus, gtsam::Pose3>
  geometricOutlierRejectionMono(Frame* ref_frame,
                                Frame* cur_frame);

  std::pair<TrackingStatus, gtsam::Pose3>
  geometricOutlierRejectionStereo(StereoFrame& ref_frame,
                                  StereoFrame& cur_frame);

  // Contrarily to the previous 2 this also returns a 3x3 covariance for the
  // translation estimate.
  std::pair<TrackingStatus, gtsam::Pose3>
  geometricOutlierRejectionMonoGivenRotation(
      Frame* ref_frame,
      Frame* cur_frame,
      const gtsam::Rot3& R);

  std::pair< std::pair<TrackingStatus,gtsam::Pose3> , gtsam::Matrix3 >
  geometricOutlierRejectionStereoGivenRotation(
      StereoFrame& ref_stereoFrame,
      StereoFrame& cur_stereoFrame,
      const gtsam::Rot3& R);

  void removeOutliersMono(
      Frame* ref_frame,
      Frame* cur_frame,
      const std::vector<std::pair<size_t, size_t>>& matches_ref_cur,
      const std::vector<int>& inliers,
      const int iterations);

  void removeOutliersStereo(
      StereoFrame& ref_stereoFrame,
      StereoFrame& cur_stereoFrame,
      const std::vector<std::pair<size_t, size_t>>& matches_ref_cur,
      const std::vector<int>& inliers,
      const int iterations);

  void checkStatusRightKeypoints(
      const std::vector<KeypointStatus>& right_keypoints_status);

  /* ---------------------------- CONST FUNCTIONS --------------------------- */
  // returns frame with markers
  cv::Mat displayFrame(
      const Frame& ref_frame,
      const Frame& cur_frame,
      bool write_frame = false,
      const std::string& img_title = "",
      const KeypointsCV& extra_corners_gray = KeypointsCV(),
      const KeypointsCV& extra_corners_blue = KeypointsCV()) const;

  /* ---------------------------- STATIC FUNCTIONS -------------------------- */
  static void findOutliers(
      const std::vector<std::pair<size_t, size_t>>& matches_ref_cur,
      std::vector<int> inliers,
      std::vector<int>* outliers);

  static void findMatchingKeypoints(
      const Frame& ref_frame,
      const Frame& cur_frame,
      std::vector<std::pair<size_t, size_t>>* matches_ref_cur);

  static void findMatchingStereoKeypoints(
      const StereoFrame& ref_stereoFrame,
      const StereoFrame& cur_stereoFrame,
      std::vector<std::pair<size_t, size_t>>* matches_ref_cur_stereo);

  static void findMatchingStereoKeypoints(
      const StereoFrame& ref_stereoFrame,
      const StereoFrame& cur_stereoFrame,
      const std::vector<std::pair<size_t, size_t>>& matches_ref_cur_mono,
      std::vector<std::pair<size_t, size_t>>* matches_ref_cur_stereo);

  static double computeMedianDisparity(const Frame& ref_frame,
                                       const Frame& cur_frame);

  // Returns landmark_count (updated from the new keypoints),
  // and nr or extracted corners.
  std::pair<KeypointsCV, std::vector<double>>
  featureDetection(const Frame& cur_frame,
                   const cv::Mat& cam_mask,
                   const int need_n_corners);

  static std::pair< Vector3, Matrix3 > getPoint3AndCovariance(
      const StereoFrame& stereoFrame,
      const gtsam::StereoCamera& stereoCam,
      const size_t pointId,
      const gtsam::Matrix3& stereoPtCov,
      boost::optional<gtsam::Matrix3> Rmat = boost::none);

  // Get tracker info
  inline DebugTrackerInfo getTrackerDebugInfo() { return debug_info_; }

 private:
  OpticalFlowPredictor::UniquePtr optical_flow_predictor_;

  // Debug info.
  DebugTrackerInfo debug_info_;

  // This is not const as for debugging we want to redirect the image save path
  // where we like.
  std::string output_images_path_;
};

}  // namespace VIO
