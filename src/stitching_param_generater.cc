//
// Created by s1nh.org on 2020/11/13.
// Modified from samples/cpp/stitching_detailed.cpp
//

/**
 * @file stitching_param_generater.cc
 * @brief 闁瑰嘲鍚嬬敮鎾矗閸屾稒娈堕柣銏㈠枑閸ㄦ岸宕抽妸銉ф澖闁?
 * 
 * 闁糕晞妗ㄧ花鐞宲enCV闁汇劌澧晅itching_detailed缂佲偓鏉炴壆浼愬ǎ鍥跺枟閺佸ジ鏁嶅畝鈧弫銈嗙鎼达絾鏅搁柟瀛樺姉濞村寮甸崫鍕暥濠㈣埖鐗曞顒勫极閼割兘鍋?
 * 闁搞儲鍎抽崕姘舵偋閻熸壆绐欓柕鍡曠瀹曠喐鎯旈弮鈧埀顑懐鍙愰梻鍐涧閹蜂即宕ｅΟ楦垮煂闁哄嫮濮撮惃鐘绘晬鐏炶壈绀嬮柛蹇嬪妽濞呮瑩骞忛崗鐓庡闁圭粯鍔掔欢鐢稿春閾忚鏀ㄩ柛娆忓€归弳鐔煎Υ?
 * 婵炲鍔嶉崜浼存晬濮樻剚鍤夋俊顖椻偓铏仴闁革负鍔岀紞瀣礈瀹ュ懍绱ｇ紒瀣儎閼垫垿寮甸鍥舵蕉濞达綀娉曢弫銈夋晬鐏炶偐绠介柣锝嗙懁娴滄帗寰勯崶銊ノ楅柡鍫灡缁便劍娼诲☉鏍ゅ亾?
 */

#include "stitching_param_generater.h"
#include "logger.h"

#include <iostream>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>


using namespace std;
using namespace cv;
using namespace cv::detail;

#define ENABLE_LOG 0
#define LOG(msg) do { std::ostringstream log_stream; log_stream << msg; Logger::GetInstance().Log(log_stream.str()); } while (false)
#define LOGLN(msg) LOG(msg)
// ============================================================================
// v3.3 (2026-07-14) SIFT -> ORB 鏀归€?- 鑷疄鐜?cv::detail::FeaturesFinder /
// FeaturesMatcher (NORM_HAMMING binary descriptor 璺緞).
//   - 纭害鏉? 鎻忚堪瀛愮被鍨?ORB binary (32 瀛楄妭 / kp), 涓嶈鍥為€€鍒?SIFT/SURF.
//   - 纭害鏉? 鍖归厤璺濈 NORM_HAMMING, 涓嶈鍥為€€鍒?L2.
//   - 杩欎袱涓被瀵?AffineBasedEstimator / HomographyBasedEstimator / BundleAdjuster
//     閮芥槸閫忔槑鐨?(瀹冧滑鍙湅 keypoints + matches, 涓嶇湅 descriptor 瀛楄妭甯冨眬).
// ============================================================================
namespace {

class OrbFeaturesFinder : public cv::detail::FeaturesFinder {
 public:
  explicit OrbFeaturesFinder(int n_features = 4000,
                             float scale_factor = 1.2f,
                             int n_levels = 8,
                             int edge_threshold = 31,
                             int first_level = 0,
                             int wta_k = 2,
                             int score_type = cv::ORB::HARRIS_SCORE,
                             int patch_size = 31)
      : orb_(cv::ORB::create(n_features, scale_factor, n_levels,
                              edge_threshold, first_level, wta_k,
                              score_type, patch_size)) {}

 protected:
  void find(const cv::Mat& image,
            cv::detail::ImageFeatures& features) override {
    features.img_idx = -1;
    features.img_size = image.size();
    features.keypoints.clear();
    features.descriptors.release();
    if (image.empty()) {
      return;
    }
    cv::Mat gray = image;
    if (gray.channels() != 1) {
      cv::cvtColor(gray, gray, cv::COLOR_BGR2GRAY);
    }
    if (gray.type() != CV_8U) {
      gray.convertTo(gray, CV_8U);
    }
    orb_->detectAndCompute(gray, cv::noArray(),
                           features.keypoints, features.descriptors);
  }

 private:
  cv::Ptr<cv::ORB> orb_;
};

class OrbPairwiseMatcher : public cv::detail::FeaturesMatcher {
 public:
  explicit OrbPairwiseMatcher(float match_conf = 0.65f, int knn = 2)
      : cv::detail::FeaturesMatcher(false),
        match_conf_(match_conf),
        knn_(knn) {}

  void match(const cv::detail::ImageFeatures& features1,
             const cv::detail::ImageFeatures& features2,
             cv::detail::MatchesInfo& matches_info) override {
    matches_info = cv::detail::MatchesInfo{};
    matches_info.src_img_idx = features1.img_idx;
    matches_info.dst_img_idx = features2.img_idx;

    if (features1.descriptors.empty() ||
        features1.descriptors.type() != CV_8U ||
        features2.descriptors.empty() ||
        features2.descriptors.type() != CV_8U) {
      return;
    }

    cv::BFMatcher matcher(cv::NORM_HAMMING);
    std::vector<std::vector<cv::DMatch>> knn_matches;
    matcher.knnMatch(features1.descriptors, features2.descriptors,
                     knn_matches, knn_);

    std::vector<cv::DMatch> good;
    good.reserve(knn_matches.size());
    for (const auto& m : knn_matches) {
      if (m.size() < 2) continue;
      // Lowe ratio test - binary descriptor 甯哥敤 0.65, 姣?SIFT (0.7) 涓ユ牸.
      if (m[0].distance >= m[1].distance * match_conf_) continue;
      good.push_back(m[0]);
    }
    matches_info.matches = std::move(good);
    // Affine/Homography estimator 鍐呴儴浼氳窇 RANSAC 閲嶇畻 inliers_mask / H / confidence.
  }

 private:
  float match_conf_;
  int knn_;
};

}  // namespace

/**
 * @brief StitchingParamGenerator闁哄瀚伴埀顒傚Т閸ら亶寮?
 * 闁告帗绻傞～鎰板礌閺嶃劌顏婚柟鎭掑劚瀵剟寮幍顔芥櫢闁瑰瓨鍔曞▍鎺楁晬鐏炴儳鈷旈悶娑樿嫰閻ｎ剟寮€靛憡鐣遍柛娆忓€归弳鐔兼偨閻旂鐏囨繛缈犺兌閳诲ジ鏁?
 * 1. 闁告帗绻傞～鎰板礌閺嵮冩暥閻庢稒顭囬埞鏍⒒?
 * 2. 闁伙絿顭堣ぐ澶愬冀閳╁喚鍔€
 * 3. 闁烩晛鎲″┃鈧柛娆忓€归弳鐔稿閹峰矈鍚€
 * 4. 闁告瑦锚閼镐即宕抽妸銉ョ仴濠殿喖顑呯€?
 * @param image_vector 閺夊牊鎸搁崣鍡涙儍閸曨偒妯嬮悽顖嗗啯绂堥柛宥呯箰閹粓鏌岃箛銉х閹煎瓨鏌ㄧ€垫﹢宕ラ銏㈢闁瑰嘲鍚嬬敮鎾儍閸曨剙顣查柡鍫濐槸濞存﹢宕?
 */
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
  camera_params_vector_ = vector<cv::detail::CameraParams>(num_img_);
  final_xmap_vector_ = vector<cv::Mat>(num_img_);
  final_ymap_vector_ = vector<cv::Mat>(num_img_);

//      projected_image_bbox_ = vector<cv::Rect>(num_img_);
  projected_image_roi_vect_refined_ = vector<cv::Rect>(num_img_);
  projected_image_roi_vect_normalized_ = vector<cv::Rect>(num_img_);

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

/**
 * @brief 闁告帗绻傞～鎰板礌閺嶎偅绁查柡鍫濇惈瀵剟寮?
 * 
 * 闁圭瑳鍡╂斀濞寸姰鍎扮粭鍛潰閵夆晩鈧啴鏁?
 * 1. 濞达綀娉曢弫顥笽FT闁绘鎳撶欢娑㈠箵閹邦剙绲垮鑸佃壘缁卞爼宕堕幆褍鍓奸柣銊ュ婢规帒顕ユ担鍝勪化
 * 2. 閺夆晜绋栭、鎴︽偋閻熸壆绐欓柣鎰嚀鐏忣噣鏌婂蹇曠闁哄秷顫夊畵涔礱tcher_type闂侇偄顦扮€氥劑宕犺ぐ鎺戝赋缂佹稒鐗滈弳鎰版晬?
 * 3. 闁糕晞妗ㄧ花顒勫礌瑜版帒甯抽柣銊ュ婢规帒顕ユ担鍝勪化濞村吋濯介鎼佹儎閸涘﹥绨氶梻鍌氼嚟濞堟垿宕￠弴鐐靛畨闁诡儸鍛彁闂?
 * 4. 闁稿繐顦板顐︾嵁閸愬弶鈻曢柨娑樻箰undle Adjustment闁挎稑顦槐顓㈠礌閺嶎偅绁查柡鍫濇惈瀵剟寮?
 * 5. 婵炲鍨归懜浼村冀閳╁喚鍔€濞寸姰鍎茬粔鐑芥⒔閵堝應鍋撻悙顒佺仒
 */
void StitchingParamGenerator::InitCameraParam() {
  // v3.3 (2026-07-14): SIFT -> ORB.
  //   原实现: SIFT::create() + AffineBestOf2NearestMatcher (L2 float desc).
  //   新实现: OrbFeaturesFinder (binary 32 字节) + OrbPairwiseMatcher
  //           (BFMatcher NORM_HAMMING + KNN(2) + Lowe ratio).
  //   AffineBasedEstimator / HomographyBasedEstimator / BundleAdjuster 对
  //   描述子字节布局透明, 不需要改.
  Ptr<FeaturesFinder> finder = makePtr<OrbFeaturesFinder>(4000);
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
  // matcher_type / range_width 在新 ORB 路径下都走 OrbPairwiseMatcher.
  // 保留字段名以保持 yaml / 代码可读, 后续若需要不同匹配策略可在此分发.
  Ptr<FeaturesMatcher> matcher = makePtr<OrbPairwiseMatcher>(match_conf);
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
    throw std::runtime_error("Homography estimation failed.");
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
    throw std::runtime_error("Unknown bundle adjustment cost function.");
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
    throw std::runtime_error("Camera parameters adjusting failed.");
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


/**
 * @brief 闁告帗绻傞～鎰板礌閺嵮冪秮鐟滆埇鍨瑰▍?
 * 
 * 闁圭瑳鍡╂斀濞寸姰鍎扮粭鍛潰閵夆晩鈧啴鏁?
 * 1. 閻犱緤绱曢悾缁樼▔椤撴繄绉撮柣鎺炵畳缁愭盯鎮介妸銈囪壘闁告瑦锚閼?
 * 2. 闁告帗绋戠紓鎾诲矗濡缚鍩岄柛锝庣厜缁辨瑩寮ㄩ娑樼槷妤犵偞濞婂浼村Υ娴ｅ憡濡囬柡灞肩┒閳ь兛鑳堕幃鍡涙閵忋垻鎼煎鑸垫皑椤帡宕ｅΟ楦垮煂闁哄倻鎳撶槐锟犳晬?
 * 3. 濞戞挾鍎ら惁锟犵嵁閸涱厽绂堥柛宥呯箲閻庮垰顕欓崫鍕秮鐟滆埇鍨哄Σ褏浜?
 * 4. 闁汇垻鍠愰崹姘跺炊閹冨壖闁硅　鏅濋悥婊堢嵁閹壆绠婚悶娑樿嫰瑜板銇?
 * 5. 閻犱緤绱曢悾濠氬箯閸忕厧澶嶉柛姘捣濞堟垿宕楅妸锔界彲闁搞儳鍐琌I闁挎稑鐗婇崝鍛村礂绾惧褰柛鏍ф惈閻撴瑩鏁?
 * 6. 閻庨潧婀卞ù澶愭焽鐠囧弶绂堥柛宥呯箺缁绘鎮板畝鍐彾闁伙絽鐭侀惃鐔煎极缂堢姳绨版繛鎴濈墦濞呭酣鏌屽鍛秾
 */
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
    throw std::runtime_error("Unable to create requested warper.");
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

  for (int img_idx = 0; img_idx < num_img_; ++img_idx) {
    cv::Mat undist_x, undist_y, reproj_x, reproj_y;
    undist_xmap_vector_[img_idx].copyTo(undist_x);
    undist_ymap_vector_[img_idx].copyTo(undist_y);
    reproj_xmap_vector_[img_idx].copyTo(reproj_x);
    reproj_ymap_vector_[img_idx].copyTo(reproj_y);
    cv::remap(undist_x,
              final_xmap_vector_[img_idx],
              reproj_x,
              reproj_y,
              cv::INTER_LINEAR,
              cv::BORDER_CONSTANT,
              cv::Scalar(-1.0f));
    cv::remap(undist_y,
              final_ymap_vector_[img_idx],
              reproj_x,
              reproj_y,
              cv::INTER_LINEAR,
              cv::BORDER_CONSTANT,
              cv::Scalar(-1.0f));
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
    projected_image_roi_vect_normalized_[i] = projected_image_roi_vect[i];
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


/**
 * @brief 闁告帗绻傞～鎰板礌閺嶎偅姣勯柛娆惿戦悧搴☆潰閿濆棙衼閻?
 * 
 * 闁圭瑳鍡╂斀濞寸姰鍎扮粭鍛潰閵夆晩鈧啴鏁?
 * 1. 濞寸姴绐媋rams闁哄倸娲ｅ▎銏″緞閻熸澘顫ｉ弶鐐电ゼAML闁哄秴娲ら悾楣冨棘閸ワ附顐介柨娑樻綇amchain_*.yaml闁?
 * 2. 閻犲洩顕цぐ鍥儎閸涘﹥绨氶柛鎰噹瀵剟鎯岄埡鍛枅闁挎稑婀燤at闁挎稑顦埀顑胯兌閺嗏晠宕ｅΟ濂稿厙闁轰礁搴滅槐姗犻柨娑橆槶閳ь兛绀侀ˇ濠氬矗閸岋妇绀凴Mat闁?
 * 3. 濠㈣泛瀚幃濠囧礆閸℃岸鍝洪柣婊冩川缂傚寮ㄩ幘鍛濠碘€冲€归悘澶嬫綇閹惧啿寮抽柛鎺戞妞存悂鎮抽崶锔剧憿闁哄秴娲ら悾楣冨礆閸℃岸鍝洪柣婊冩矗缁楀宕ュ畝瀣
 * 4. 闁汇垻鍠愰崹姘舵偩缁嬪灝缍侀柡宥佸墲椤掓粓鎯冮崟顐ｇ稄闁哄秴娲﹀Σ褏浜搁崟鍓佺闁活潿鍔嬬花鐞::remap闁?
 * 
 * 闁哄秴娲ら悾楣冨棘閸ワ附顐介柡宥囧帶缁憋繝鏁嶉崸顪ams/camchain_i.yaml闁挎稑顧€缁?
 * - KMat: 3x3闁烩晛鎲″┃鈧柛鎰噹瀵剟鎯岄埡鍛枅
 * - D: 闁伙絿顭堣ぐ澶屽寲缂佹ɑ娈堕柛姘灴閸?
 * - RMat: 3x3闁哄啫顑堝ù鍡涙儗閳哄懏鈻堥柨娑樼墢濞村鈧敻鈧稓鑹剧紒妤婂厸缁斿瓨绋夐鍡樼ゲ闁哄牏灏ㄧ槐?
 * - focal: 闁绘帪绠掔粣?
 * - width/height: 闁哄秴娲ら悾楣冨籍閸撲焦鐣遍柛銉﹀劤閸庢岸宕氶崱姘跺摵闁?
 */
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

/**
 * @brief 闁兼儳鍢茶ぐ鍥箥閳ь剟寮垫径鎰闁硅埖娲栨總鏍矗閸屾稒娈?
 * 
 * 閺夆晜鏌ㄥú鏍焻濮樺磭绠栭柛鎺撶箓椤劙宕犻弽顒傜畺缂佸顑堥鍝ョ不濡も偓缁堕亶宕氶幍顔界暠闁圭鍋撻柡鍫濐槸瀵剟寮敮顔剧濞撴碍绋戦ˇ濠氭焾閵娿倕鈻忛柣銏╃厜缁?
 * - 闁伙絿顭堣ぐ澶愬冀閳╁喚鍔€闁哄嫮濮撮惃鐘绘晬濮樿鲸鏆忓ù婊冪┋v::remap閺夆晜绋栭、鎴﹀炊閹冨壖闁哄秮鍓濋?
 * - 闂佹彃绉垫慨鍥亹鏉堛劍衼閻忓繐瀚哥槐浼存偨閵娿倗鑹綾v::remap閺夆晜绋栭、鎴犳喆閸屾粌浠柛娆惿戝畷鏌ユ晬閸儮鍋撹箛姘兼綊闁硅埖娲栨總鏍晬?
 * - 缂侇喗鍎崇€垫煡宕ユ惔锝嗙暠ROI闁挎稒纰嶉惁鈩冪▔椤忓嫭绂堥柛宥呯箰濠€顏堝箯閸忕厧澶嶉柣銏ｎ嚙缁旈攱绋夐鐘崇暠濞达絽绉堕悿鍡涘椽鐏炲浜ｉ悘?
 * 
 * @param undist_xmap_vector 閺夊牊鎸搁崵顓㈡晬濮樿鲸姣勯柛娆惿戦悧搴☆潰閿濆洦鐣盭闁秆勫姈閻栵綁寮伴悩鑼
 * @param undist_ymap_vector 閺夊牊鎸搁崵顓㈡晬濮樿鲸姣勯柛娆惿戦悧搴☆潰閿濆洦鐣盰闁秆勫姈閻栵綁寮伴悩鑼
 * @param reproj_xmap_vector 閺夊牊鎸搁崵顓㈡晬濮樿泛娅㈤柟鑸垫礀婵傛牠鎯冮崚瀣锤閹邦厾鍨奸柡鍕Т閻?
 * @param reproj_ymap_vector 閺夊牊鎸搁崵顓㈡晬濮樿泛娅㈤柟鑸垫礀婵傛牠鎯冮崚宀勫锤閹邦厾鍨奸柡鍕Т閻?
 * @param projected_image_roi_vect_refined 閺夊牊鎸搁崵顓㈡晬濮樿京缈遍柛鏍ㄧ墪閹鎯冮崟顐ｇ闁稿秴绮篛I闁挎稑鐗婇惁鈩冪▔椤忓嫭绂堥柛宥呯箰濠€顏堝礂閵婏附鐝柛銉ュ綖閼垫垿鎯冮崟顏嗙Т缂傚喚鍣槐?
 */
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

void StitchingParamGenerator::GetWarpData(StitchingWarpData& warp_data) {
  warp_data = StitchingWarpData{};
  warp_data.entries.resize(num_img_);

  int min_x = std::numeric_limits<int>::max();
  int min_y = std::numeric_limits<int>::max();
  int max_x = std::numeric_limits<int>::min();
  int max_y = std::numeric_limits<int>::min();

  for (size_t i = 0; i < num_img_; ++i) {
    if (final_xmap_vector_[i].empty() || final_ymap_vector_[i].empty()) {
      continue;
    }

    cv::Rect roi = projected_image_roi_vect_refined_[i];
    if (roi.width <= 0 || roi.height <= 0) {
      roi = projected_image_roi_vect_normalized_[i];
    }
    if (roi.width <= 0 || roi.height <= 0) {
      continue;
    }

    roi &= cv::Rect(0, 0, final_xmap_vector_[i].cols, final_xmap_vector_[i].rows);
    roi.x &= ~1;
    roi.y &= ~1;
    roi.width &= ~1;
    roi.height &= ~1;
    if (roi.width <= 0 || roi.height <= 0) {
      continue;
    }

    if (roi.width < 2 || roi.height < 2) {
      continue;
    }

    warp_data.entries[i].xmap = final_xmap_vector_[i](roi).clone();
    warp_data.entries[i].ymap = final_ymap_vector_[i](roi).clone();
    warp_data.entries[i].roi = roi;

    min_x = std::min(min_x, roi.x);
    min_y = std::min(min_y, roi.y);
    max_x = std::max(max_x, roi.x + roi.width);
    max_y = std::max(max_y, roi.y + roi.height);
  }

  if (min_x == std::numeric_limits<int>::max() ||
      min_y == std::numeric_limits<int>::max()) {
    return;
  }

  for (size_t i = 0; i < warp_data.entries.size(); ++i) {
    if (!warp_data.entries[i].valid()) {
      continue;
    }
    warp_data.entries[i].roi.x -= min_x;
    warp_data.entries[i].roi.y -= min_y;
  }

  warp_data.panorama_size = cv::Size(max_x - min_x, max_y - min_y);
}
