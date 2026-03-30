//
// Created by s1nh.org on 2020/11/13.
// Modified from samples/cpp/stitching_detailed.cpp
//

#include "stitching_param_generater.h"
#include "logger.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>


using namespace std;
using namespace cv;
using namespace cv::detail;

#define ENABLE_LOG 0
#define LOG(msg) do { std::ostringstream log_stream; log_stream << msg; Logger::GetInstance().Log(log_stream.str()); } while (false)
#define LOGLN(msg) LOG(msg)


StitchingParamGenerator::StitchingParamGenerator(
    const vector<cv::Mat>& image_vector) {
  num_img_ = image_vector.size();

  image_vector_ = image_vector;
  mask_vector_ = vector<cv::UMat>(num_img_);
  mask_warped_vector_ = vector<cv::UMat>(num_img_);
  image_size_vector_ = vector<cv::Size>(num_img_);
  image_warped_size_vector_ = vector<cv::Size>(num_img_);
  reproj_xmap_vector_ = vector<cv::UMat>(num_img_);
  reproj_ymap_vector_ = vector<cv::UMat>(num_img_);
  camera_params_vector_ =
      vector<cv::detail::CameraParams>(camera_params_vector_);

//      projected_image_bbox_ = vector<cv::Rect>(num_img_);
  projected_image_roi_vect_refined_ = vector<cv::Rect>(num_img_);

  for (size_t img_idx = 0; img_idx < num_img_; img_idx++) {
    image_size_vector_[img_idx] = image_vector_[img_idx].size();
  }

  vector<cv::UMat> undist_xmap_vector;
  vector<cv::UMat> undist_ymap_vector;

  InitUndistortMap();

  for (size_t img_idx = 0; img_idx < num_img_; ++img_idx) {
    cv::remap(image_vector_[img_idx],
              image_vector_[img_idx],
              undist_xmap_vector_[img_idx],
              undist_ymap_vector_[img_idx],
              cv::INTER_LINEAR);
  }

  InitCameraParam();
  InitWarper();
}

void StitchingParamGenerator::InitCameraParam() {
  Ptr<Feature2D> finder;
  finder = SIFT::create();
  vector<ImageFeatures> features(num_img_);
  vector<Size> full_img_sizes(num_img_);
  for (int i = 0; i < num_img_; ++i) {
    computeImageFeatures(finder, image_vector_[i], features[i]);
    features[i].img_idx = i;
    LOGLN(
        "Features in image #" << i + 1 << ": " << features[i].keypoints.size());
  }
  LOG("Pairwise matching");
  vector<MatchesInfo> pairwise_matches;
  Ptr<FeaturesMatcher> matcher;
  if (matcher_type == "affine")
    matcher = makePtr<AffineBestOf2NearestMatcher>(false, try_cuda, match_conf);
  else if (range_width == -1)
    matcher = makePtr<BestOf2NearestMatcher>(try_cuda, match_conf);
  else
    matcher = makePtr<BestOf2NearestRangeMatcher>(range_width, try_cuda,
                                                  match_conf);
  (*matcher)(features, pairwise_matches);
  matcher->collectGarbage();
  // Check if we should save matches graph
  if (save_graph) {
    LOGLN("Saving matches graph...");
    ofstream f(save_graph_to.c_str());
    f << matchesGraphAsString(img_names, pairwise_matches, conf_thresh);
  }
  Ptr<Estimator> estimator;
  if (estimator_type == "affine")
    estimator = makePtr<AffineBasedEstimator>();
  else
    estimator = makePtr<HomographyBasedEstimator>();
  if (!(*estimator)(features, pairwise_matches, camera_params_vector_)) {
    Logger::GetInstance().LogError("Homography estimation failed.");
    assert(false);
  }
  for (auto& i : camera_params_vector_) {
    Mat R;
    i.R.convertTo(R, CV_32F);
    i.R = R;
  }
  Ptr<detail::BundleAdjusterBase> adjuster;
  if (ba_cost_func == "reproj")
    adjuster = makePtr<detail::BundleAdjusterReproj>();
  else if (ba_cost_func == "ray")
    adjuster = makePtr<detail::BundleAdjusterRay>();
  else if (ba_cost_func == "affine")
    adjuster =
        makePtr<detail::BundleAdjusterAffinePartial>();
  else if (ba_cost_func == "no") adjuster = makePtr<NoBundleAdjuster>();
  else {
    Logger::GetInstance().LogError(
        "Unknown bundle adjustment cost function: '" + ba_cost_func + "'.");
    assert(false);
  }
  adjuster->setConfThresh(conf_thresh);
  Mat_<uchar> refine_mask = Mat::zeros(3, 3, CV_8U);
  if (ba_refine_mask[0] == 'x') refine_mask(0, 0) = 1;
  if (ba_refine_mask[1] == 'x') refine_mask(0, 1) = 1;
  if (ba_refine_mask[2] == 'x') refine_mask(0, 2) = 1;
  if (ba_refine_mask[3] == 'x') refine_mask(1, 1) = 1;
  if (ba_refine_mask[4] == 'x') refine_mask(1, 2) = 1;
  adjuster->setRefinementMask(refine_mask);
  if (!(*adjuster)(features, pairwise_matches, camera_params_vector_)) {
    Logger::GetInstance().LogError("Camera parameters adjusting failed.");
    assert(false);
  }

  vector<Mat> rmats;
  for (size_t i = 0; i < camera_params_vector_.size(); ++i)
    rmats.push_back(camera_params_vector_[i].R.clone());
  waveCorrect(rmats, wave_correct);
  for (size_t i = 0; i < camera_params_vector_.size(); ++i) {
    camera_params_vector_[i].R = rmats[i];
    LOGLN("Initial camera intrinsics #"
              << i + 1 << ":\nK:\n"
              << camera_params_vector_[i].K()
              << "\nR:\n" << camera_params_vector_[i].R);
  }
}


void StitchingParamGenerator::InitWarper() {

  vector<double> focals;
  float median_focal_length;
  reproj_xmap_vector_ = vector<UMat>(num_img_);
  reproj_xmap_vector_ = vector<UMat>(num_img_);


  for (size_t i = 0; i < camera_params_vector_.size(); ++i) {
    LOGLN("Camera #" << i + 1 << ":\nK:\n" << camera_params_vector_[i].K()
                     << "\nR:\n" << camera_params_vector_[i].R);
    focals.push_back(camera_params_vector_[i].focal);
  }
  sort(focals.begin(), focals.end());
  if (focals.size() % 2 == 1)
    median_focal_length = static_cast<float>(focals[focals.size() / 2]);
  else
    median_focal_length =
        static_cast<float>(focals[focals.size() / 2 - 1] +
                           focals[focals.size() / 2]) * 0.5f;


  Ptr<WarperCreator> warper_creator;
#ifdef HAVE_OPENCV_CUDAWARPING
  if (try_cuda && cuda::getCudaEnabledDeviceCount() > 0) {
    if (warp_type == "plane")
      warper_creator = makePtr<cv::PlaneWarperGpu>();
    else if (warp_type == "cylindrical")
      warper_creator = makePtr<cv::CylindricalWarperGpu>();
    else if (warp_type == "spherical")
      warper_creator = makePtr<cv::SphericalWarperGpu>();
  } else
#endif
  {
    if (warp_type == "plane")
      warper_creator = makePtr<cv::PlaneWarper>();
    else if (warp_type == "affine")
      warper_creator = makePtr<cv::AffineWarper>();
    else if (warp_type == "cylindrical")
      warper_creator = makePtr<cv::CylindricalWarper>();
    else if (warp_type == "spherical")
      warper_creator = makePtr<cv::SphericalWarper>();
    else if (warp_type == "fisheye")
      warper_creator = makePtr<cv::FisheyeWarper>();
    else if (warp_type == "stereographic")
      warper_creator = makePtr<cv::StereographicWarper>();
    else if (warp_type == "compressedPlaneA2B1")
      warper_creator = makePtr<cv::CompressedRectilinearWarper>(2.0f, 1.0f);
    else if (warp_type == "compressedPlaneA1.5B1")
      warper_creator = makePtr<cv::CompressedRectilinearWarper>(1.5f, 1.0f);
    else if (warp_type == "compressedPlanePortraitA2B1")
      warper_creator =
          makePtr<cv::CompressedRectilinearPortraitWarper>(2.0f, 1.0f);
    else if (warp_type == "compressedPlanePortraitA1.5B1")
      warper_creator =
          makePtr<cv::CompressedRectilinearPortraitWarper>(1.5f, 1.0f);
    else if (warp_type == "paniniA2B1")
      warper_creator = makePtr<cv::PaniniWarper>(2.0f, 1.0f);
    else if (warp_type == "paniniA1.5B1")
      warper_creator = makePtr<cv::PaniniWarper>(1.5f, 1.0f);
    else if (warp_type == "paniniPortraitA2B1")
      warper_creator = makePtr<cv::PaniniPortraitWarper>(2.0f, 1.0f);
    else if (warp_type == "paniniPortraitA1.5B1")
      warper_creator = makePtr<cv::PaniniPortraitWarper>(1.5f, 1.0f);
    else if (warp_type == "mercator")
      warper_creator = makePtr<cv::MercatorWarper>();
    else if (warp_type == "transverseMercator")
      warper_creator = makePtr<cv::TransverseMercatorWarper>();
  }
  if (!warper_creator) {
    Logger::GetInstance().LogError(
        "Can't create the following warper '" + warp_type + "'");
    assert(false);
  }
  rotation_warper_ =
      warper_creator->create(static_cast<float>(median_focal_length));
  LOGLN("warped_image_scale: " << median_focal_length);


  Rect rect;
  vector<cv::Point> image_point_vect(num_img_);

  for (int img_idx = 0; img_idx < num_img_; ++img_idx) {
    Mat_<float> K;
    camera_params_vector_[img_idx].K().convertTo(K, CV_32F);
    rect = rotation_warper_->buildMaps(image_size_vector_[img_idx], K,
                                       camera_params_vector_[img_idx].R,
                                       reproj_xmap_vector_[img_idx],
                                       reproj_ymap_vector_[img_idx]);
    Point point(rect.x, rect.y);

    image_point_vect[img_idx] = point;
  }


  // Prepare images masks
  for (int img_idx = 0; img_idx < num_img_; ++img_idx) {
    mask_vector_[img_idx].create(image_vector_[img_idx].size(), CV_8U);
    mask_vector_[img_idx].setTo(Scalar::all(255));
    remap(mask_vector_[img_idx],
          mask_warped_vector_[img_idx],
          reproj_xmap_vector_[img_idx],
          reproj_ymap_vector_[img_idx],
          INTER_NEAREST);
    image_warped_size_vector_[img_idx] = mask_warped_vector_[img_idx].size();

  }


  timelapser_ = Timelapser::createDefault(timelapse_type);
  blender_ = Blender::createDefault(Blender::NO);
  timelapser_->initialize(image_point_vect, image_size_vector_);
  blender_->prepare(image_point_vect, image_size_vector_);

  vector<cv::Rect> projected_image_roi_vect = vector<cv::Rect>(num_img_);

  // Update corners and sizes
  // TODO(duchengyao): Figure out what bias means.
  Point roi_tl_bias(999999, 999999);
  for (int i = 0; i < num_img_; ++i) {
    // Update corner and size
    Size sz = image_vector_[i].size();
    Mat K;
    camera_params_vector_[i].K().convertTo(K, CV_32F);
    Rect roi = rotation_warper_->warpRoi(sz, K, camera_params_vector_[i].R);
    LOG("roi" << roi);
    roi_tl_bias.x = min(roi.tl().x, roi_tl_bias.x);
    roi_tl_bias.y = min(roi.tl().y, roi_tl_bias.y);
    projected_image_roi_vect[i] = roi;
  }
  full_image_size_ = Point(0, 0);
  Point y_range = Point(-9999999, 999999);
  for (int i = 0; i < num_img_; ++i) {
    projected_image_roi_vect[i] -= roi_tl_bias;
    Point tl = projected_image_roi_vect[i].tl();
    Point br = projected_image_roi_vect[i].br();

    full_image_size_.x = max(br.x, full_image_size_.x);
    full_image_size_.y = max(br.y, full_image_size_.y);
    y_range.x = max(y_range.x, tl.y);
    y_range.y = min(y_range.y, br.y);
  }
  for (int i = 0; i < num_img_; ++i) {
    Rect global_rect = projected_image_roi_vect[i];
    Rect local_rect = global_rect;
    local_rect.height =
        global_rect.height -
        (global_rect.br().y - y_range.y + y_range.x - global_rect.tl().y);
    local_rect.y = y_range.x - global_rect.y;
    local_rect.x = 0;
    projected_image_roi_vect_refined_[i] = local_rect;
    LOGLN("normalized global roi[" << i << "] " << global_rect);
    LOGLN("normalized local roi[" << i << "] " << local_rect);
  }

  for (int i = 0; i < num_img_ - 1; ++i) {

    Rect rect_left = projected_image_roi_vect_refined_[i];
    int offset = (projected_image_roi_vect[i].br().x -
                  projected_image_roi_vect[i + 1].tl().x) / 2;
    rect_left.width -= offset;
    Rect rect_right = projected_image_roi_vect_refined_[i + 1];
    rect_right.width -= offset;
    rect_right.x += offset;
    projected_image_roi_vect_refined_[i] = rect_left;
    projected_image_roi_vect_refined_[i + 1] = rect_right;

  }
  for (int i = 0; i < num_img_; ++i) {
    LOGLN("refined roi[" << i << "] " << projected_image_roi_vect_refined_[i]);
  }
}


void StitchingParamGenerator::InitUndistortMap() {
  std::vector<double> cam_focal_vector(num_img_);

  std::vector<cv::UMat> r_vector(num_img_);
  std::vector<cv::UMat> k_vector(num_img_);
  std::vector<std::vector<double>> d_vector(num_img_);
  std::vector<cv::Size> calib_size_vector(num_img_);

  undist_xmap_vector_ = std::vector<cv::UMat>(num_img_);
  undist_ymap_vector_ = std::vector<cv::UMat>(num_img_);

  for (size_t i = 0; i < num_img_; i++) {
    cv::FileStorage fs_read(
        "../params/camchain_" + std::to_string(i) + ".yaml",

        cv::FileStorage::READ);
    if (!fs_read.isOpened()) {
      std::ostringstream error_stream;
      error_stream << __FILE__ << ":" << __LINE__
                   << ":loadParams falied. 'camera.yml' does not exist";
      Logger::GetInstance().LogError(error_stream.str());
      return;
    }
    cv::Mat R, K;
    fs_read["KMat"] >> K;
    K.copyTo(k_vector[i]);
    fs_read["D"] >> d_vector[i];
    fs_read["RMat"] >> R;
    R.copyTo(r_vector[i]);
    fs_read["focal"] >> cam_focal_vector[i];
    int width = 0;
    int height = 0;
    fs_read["width"] >> width;
    fs_read["height"] >> height;
    if (width <= 0 || height <= 0) {
      cv::FileNode resolution_node = fs_read["resolution"];
      if (!resolution_node.empty() && resolution_node.isSeq() &&
          resolution_node.size() >= 2) {
        width = static_cast<int>(resolution_node[0]);
        height = static_cast<int>(resolution_node[1]);
      }
    }
    calib_size_vector[i] = cv::Size(width, height);
  }

  for (size_t i = 0; i < num_img_; i++) {
    cv::Mat K;
    cv::UMat R;
    cv::UMat NONE;
    k_vector[i].copyTo(K);
    K.convertTo(K, CV_32F);
    cv::UMat::eye(3, 3, CV_32F).convertTo(R, CV_32F);

    cv::Size input_size = image_size_vector_[i];
    cv::Size calib_size = calib_size_vector[i];
    if (input_size.width <= 0 || input_size.height <= 0) {
      input_size = calib_size;
    }
    if (calib_size.width > 0 && calib_size.height > 0 &&
        input_size != calib_size) {
      float scale_x =
          static_cast<float>(input_size.width) / calib_size.width;
      float scale_y =
          static_cast<float>(input_size.height) / calib_size.height;
      K.at<float>(0, 0) *= scale_x;
      K.at<float>(1, 1) *= scale_y;
      K.at<float>(0, 2) *= scale_x;
      K.at<float>(1, 2) *= scale_y;
      LOGLN("Scale intrinsics for image #" << i + 1
                                          << " from calibration size "
                                          << calib_size << " to input size "
                                          << input_size);
    }

    cv::initUndistortRectifyMap(
        K, d_vector[i], R, NONE, input_size,
        CV_32FC1, undist_xmap_vector_[i], undist_ymap_vector_[i]);
  }

}

void StitchingParamGenerator::GetReprojParams(
    vector<cv::UMat>& undist_xmap_vector,
    vector<cv::UMat>& undist_ymap_vector,
    vector<cv::UMat>& reproj_xmap_vector,
    vector<cv::UMat>& reproj_ymap_vector,
    vector<cv::Rect>& projected_image_roi_vect_refined) {


  undist_xmap_vector = undist_xmap_vector_;
  undist_ymap_vector = undist_ymap_vector_;
  reproj_xmap_vector = reproj_xmap_vector_;
  reproj_ymap_vector = reproj_ymap_vector_;
  projected_image_roi_vect_refined = projected_image_roi_vect_refined_;

}
