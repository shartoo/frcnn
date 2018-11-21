// ------------------------------------------------------------------
// Decoupled-Classification-Refinement
// write by fyk
// ------------------------------------------------------------------

#include "dcr_proposal_target_layer.hpp"
#include "yaml-cpp/yaml.h"

namespace caffe {

namespace Frcnn {

using std::vector;

template <typename Dtype>
void DCRProposalTargetLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype> *> &bottom,
                                                 const vector<Blob<Dtype> *> &top) {
  this->rng_.reset(new Caffe::RNG(static_cast<unsigned int>(FrcnnParam::rng_seed)));
  this->_count_ = this->_bg_num_ = this->_fg_num_ = 0;

  config_n_classes_ = FrcnnParam::n_classes;
  YAML::Node params = YAML::Load(this->layer_param_.module_param().param_str());
  CHECK(params["sample_per_img"]) << "not found as parameter.";
  CHECK(params["hard_fp_score"]) << "not found as parameter.";
  sample_per_img_ = params["sample_per_img"].as<int>();
  hard_fp_score_ = params["hard_fp_score"].as<float>();
  string sample_method = params["sample"].as<string>();
  if (sample_method == "RCNN") sample_method_ = 1;
  else sample_method_ = 0; // DCRV1
  
  LOG(INFO) << "DCRProposalTargetLayer :: " << config_n_classes_ << " classes";
  LOG(INFO) << "DCRProposalTargetLayer :: LayerSetUp";
  // sampled rois (0, x1, y1, x2, y2)
  top[0]->Reshape(1, 5, 1, 1);
  // labels
  top[1]->Reshape(1, 1, 1, 1);
  // bbox_targets
  top[2]->Reshape(1, config_n_classes_ * 4, 1, 1);
}

template <typename Dtype>
void DCRProposalTargetLayer<Dtype>::Forward_cpu(
    const vector<Blob<Dtype> *> &bottom, const vector<Blob<Dtype> *> &top) {

  vector<Point4f<Dtype> > all_rois;
  for (int i = 0; i < bottom[0]->num(); i++) {
    all_rois.push_back(Point4f<Dtype>(
        bottom[0]->data_at(i,1,0,0),
        bottom[0]->data_at(i,2,0,0),
        bottom[0]->data_at(i,3,0,0),
        bottom[0]->data_at(i,4,0,0)));
    CHECK_EQ(bottom[0]->data_at(i,0,0,0), 0) << "Only single item batches are supported";
  }

  vector<Point4f<Dtype> > gt_boxes;
  vector<int> gt_labels;
  for (int i = 0; i < bottom[1]->num(); i++) {
    gt_boxes.push_back(Point4f<Dtype>(
        bottom[1]->data_at(i,0,0,0),
        bottom[1]->data_at(i,1,0,0),
        bottom[1]->data_at(i,2,0,0),
        bottom[1]->data_at(i,3,0,0)));
    gt_labels.push_back(bottom[1]->data_at(i,4,0,0));
    CHECK_GT(gt_labels[i], 0) << "Ground Truth Should Be Greater Than 0";
  }

  vector<Dtype> pred_probs;
  vector<int> pred_labels;
  const int cls_num = bottom[2]->channels();
  for (int i = 0; i < bottom[2]->num(); i++) {
    int max_score = 0; // prob
    int max_id = 0;
    for (int cls = 1; cls < cls_num; cls++) {
        float score = bottom[2]->cpu_data()[i * cls_num + cls];
        if (score > max_score) {
            max_score = score;
            max_id = cls;
        }
    }
    pred_probs.push_back(max_score);
    pred_labels.push_back(max_id);
  }

  // for Cascade R-CNN, screen out high IoU boxes, to remove redundant gt boxes, for that we sample rois from previous stage with GT, if we don't remove some of those good boxes, it is easy to overfit.
  /* const int stage = this->layer_param_.proposal_target_param().stage();
  if( stage != 0) {
    const float gt_iou_thr = this->layer_param_.proposal_target_param().gt_iou_thr();
    std::vector<std::vector<Dtype> > overlaps = get_ious(all_rois, gt_boxes, FrcnnParam::test_use_gpu_nms);
    std::vector<Dtype> max_overlaps(all_rois.size(), 0);
    for (int i = 0; i < all_rois.size(); ++ i) {
      for (int j = 0; j < gt_boxes.size(); ++ j) {
        if (max_overlaps[i] <= overlaps[i][j]) {
          max_overlaps[i] = overlaps[i][j];
        }
      }
    }
    vector<Point4f<Dtype> > valid_rois;
    for (int i = 0; i < all_rois.size(); i++) {
      if (max_overlaps[i] < gt_iou_thr) valid_rois.push_back(all_rois[i]);
    }
    all_rois = valid_rois;
  } */

  all_rois.insert(all_rois.end(), gt_boxes.begin(), gt_boxes.end());
  vector<Dtype> gt_probs(gt_labels.size(), 1);
  pred_probs.insert(pred_probs.end(), gt_probs.begin(), gt_probs.end());
  pred_labels.insert(pred_labels.end(), gt_labels.begin(), gt_labels.end());

  DLOG(ERROR) << "gt boxes size: " << gt_boxes.size();
  //const int num_images = 1;
  //const int rois_per_image = FrcnnParam::batch_size / num_images;
  const int rois_per_image = sample_per_img_;
  const int fg_rois_per_image = rois_per_image * FrcnnParam::fg_fraction;

  //Sample rois with classification labels and bounding box regression
  //targets
  vector<int> labels;
  vector<Point4f<Dtype> > rois;
  vector<vector<Point4f<Dtype> > > bbox_targets, bbox_inside_weights;

  switch(sample_method_){ 
  case 1:// the rcnn way
    _sample_rois(all_rois, gt_boxes, gt_labels, fg_rois_per_image, rois_per_image, labels, rois, bbox_targets, bbox_inside_weights);
    break;
  default:
    // the dcr way, choose fg+bg+hard-fp equally
    _sample_rois_fg_bg(all_rois, gt_boxes, gt_labels, pred_labels, pred_probs, rois_per_image, labels, rois, bbox_targets, bbox_inside_weights);
  }

  CHECK_EQ(labels.size(), rois.size());
  CHECK_EQ(labels.size(), bbox_targets.size());
  CHECK_EQ(labels.size(), bbox_inside_weights.size());
  const int batch_size = rois.size();
  DLOG(ERROR) << "top[0]-> " << batch_size << " , 5, 1, 1";
  // sampled rois
  top[0]->Reshape(batch_size, 5, 1, 1); // rois
  caffe_set(top[0]->count(), Dtype(0), top[0]->mutable_cpu_data());
  Dtype *rois_data = top[0]->mutable_cpu_data();
  for (int i = 0; i < batch_size; i++) {
    rois_data[ top[0]->offset(i,1,0,0) ] = rois[i][0];
    rois_data[ top[0]->offset(i,2,0,0) ] = rois[i][1];
    rois_data[ top[0]->offset(i,3,0,0) ] = rois[i][2];
    rois_data[ top[0]->offset(i,4,0,0) ] = rois[i][3];
  }
  // classification labels
  top[1]->Reshape(batch_size, 1, 1, 1);
  Dtype *label_data = top[1]->mutable_cpu_data();
  for (int i = 0; i < batch_size; i++) {
    label_data[ top[1]->offset(i,0,0,0) ] = labels[i];
  }
  // bbox_targets
  top[2]->Reshape(batch_size, this->config_n_classes_*4, 1, 1);
  caffe_set(top[2]->count(), Dtype(0), top[2]->mutable_cpu_data());
  Dtype *target_data = top[2]->mutable_cpu_data();
  for (int i = 0; i < batch_size; i++) {
    for (int j = 0; j < this->config_n_classes_; j++) {
      for (int cor = 0; cor < 4; cor++ ) {
        target_data[ top[2]->offset(i, j*4+cor, 0, 0) ] = bbox_targets[i][j][cor];
      }
    }
  }
  DLOG(INFO) << "DCRProposalTargetLayer::Forward_cpu End";
}

template <typename Dtype>
void DCRProposalTargetLayer<Dtype>::Backward_cpu(
    const vector<Blob<Dtype> *> &top, const vector<bool> &propagate_down,
    const vector<Blob<Dtype> *> &bottom) {
  for (int i = 0; i < propagate_down.size(); ++i) {
    if (propagate_down[i]) {
      NOT_IMPLEMENTED;
    }
  }
}

template <typename Dtype>
void DCRProposalTargetLayer<Dtype>::_sample_rois(const vector<Point4f<Dtype> > &all_rois, const vector<Point4f<Dtype> > &gt_boxes, 
        const vector<int> &gt_label, const int fg_rois_per_image, const int rois_per_image, vector<int> &labels, 
        vector<Point4f<Dtype> > &rois, vector<vector<Point4f<Dtype> > > &bbox_targets, vector<vector<Point4f<Dtype> > > &bbox_inside_weights) {
// Generate a random sample of RoIs comprising foreground and background examples.

  CHECK_EQ(gt_label.size(), gt_boxes.size());
  // overlaps: (rois x gt_boxes)
  //std::vector<std::vector<Dtype> > overlaps = get_ious(all_rois, gt_boxes, this->use_gpu_nms_in_forward_cpu);
  std::vector<std::vector<Dtype> > overlaps = get_ious(all_rois, gt_boxes, FrcnnParam::test_use_gpu_nms);
  //this->use_gpu_nms_in_forward_cpu = false; // restore
  std::vector<Dtype> max_overlaps(all_rois.size(), 0);
  std::vector<int> gt_assignment(all_rois.size(), -1);
  std::vector<int> _labels(all_rois.size());
  for (int i = 0; i < all_rois.size(); ++ i) {
    for (int j = 0; j < gt_boxes.size(); ++ j) {
      if (max_overlaps[i] <= overlaps[i][j]) {
        max_overlaps[i] = overlaps[i][j];
        gt_assignment[i] = j;       
      }
    }
  }
  DLOG(INFO) << "sample_rois : all_rois: " << all_rois.size() << ", gt_box: " << gt_boxes.size();
  for (size_t i = 0; i < all_rois.size(); ++i) {
    if (gt_assignment[i] >= 0 ) {
      CHECK_LT(gt_assignment[i], gt_label.size());
      _labels[i] = gt_label[gt_assignment[i]];
    } else {
      _labels[i] = 0;
    }
  }
  
  // Select foreground RoIs as those with >= FG_THRESH overlap
  std::vector<int> fg_inds;
  //const float fg_thr = FrcnnParam::fg_thresh + 0.1 * this->layer_param_.proposal_target_param().stage(); // 0.5, 0.6, 0.7
  const float fg_thr = FrcnnParam::fg_thresh;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= fg_thr) {
      fg_inds.push_back(i);
    }
  }
  // Guard against the case when an image has fewer than fg_rois_per_image
  // foreground RoIs
  const int fg_rois_per_this_image = std::min(fg_rois_per_image, int(fg_inds.size()));
  DLOG(INFO) << "fg_inds [PRE,AFT] : [" << fg_inds.size() << "," << fg_rois_per_this_image << "] FG_THRESH : " << FrcnnParam::fg_thresh;
  // Sample foreground regions without replacement
  if (fg_inds.size() > 0) {
    shuffle(fg_inds.begin(), fg_inds.end(), (caffe::rng_t *) this->rng_->generator());
    fg_inds.resize(fg_rois_per_this_image);
  }
  
  // Select background RoIs as those within [BG_THRESH_LO, BG_THRESH_HI)
  std::vector<int> bg_inds;
  //const float bg_thr_hi = FrcnnParam::bg_thresh_hi + 0.1 * this->layer_param_.proposal_target_param().stage();
  const float bg_thr_hi = FrcnnParam::bg_thresh_hi;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= FrcnnParam::bg_thresh_lo 
        && max_overlaps[i] < bg_thr_hi) {
      bg_inds.push_back(i);
    }
  }
  // Compute number of background RoIs to take from this image (guarding against there being fewer than desired)
  const int bg_rois_per_this_image = std::min(rois_per_image-fg_rois_per_this_image, int(bg_inds.size()));
  DLOG(INFO) << "bg_inds [PRE,AFT] : [" << bg_inds.size() << "," << bg_rois_per_this_image << "] BG_THRESH : [" << FrcnnParam::bg_thresh_lo << ", " << FrcnnParam::bg_thresh_hi << ")" ;
  // Sample background regions without replacement
  if (bg_inds.size() > 0) {
    shuffle(bg_inds.begin(), bg_inds.end(), (caffe::rng_t *) this->rng_->generator());
    bg_inds.resize(bg_rois_per_this_image);
  }

  // The indices that we're selecting (both fg and bg)
  std::vector<int> keep_inds(fg_inds);
  keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.end());

  // fyk add for solving the problem of zero roi problem, better than setting bg_thresh_lo to 0 in config
  // Need more?
  int remaining = rois_per_image - keep_inds.size();
  if (remaining > 0) {
    // Looks like we don't have enough samples to maintain the desired
    // balance. Reduce requirements and fill in the rest. This is
    // likely different from the Mask RCNN paper.
    // There is a small chance we have neither fg nor bg samples.
    if (keep_inds.size() == 0) {
        // Pick bg regions with easier IoU threshold
        for (int i = 0; i < all_rois.size(); ++i) {
          if (max_overlaps[i] < FrcnnParam::bg_thresh_lo) { 
            bg_inds.push_back(i);
          }
        }
        if (bg_inds.size() > 0) {
            shuffle(bg_inds.begin(), bg_inds.end(), (caffe::rng_t *) this->rng_->generator());
            bg_inds.resize(rois_per_image);
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.end());
        }
    }else{
        if (bg_inds.size() == 0) {
            for (int i = 0; i < all_rois.size(); ++i) {
	      // Negative ROIs are those with max IoU 0.1-0.5 (hard example mining)
	      // To hard example mine or not to hard example mine, that's the question
              if (max_overlaps[i] < FrcnnParam::bg_thresh_hi) {
                bg_inds.push_back(i);
              }
            }
	}
        // Fill the rest with repeated bg rois.
        if (bg_inds.size() > 0) while (remaining > 0) {
            int rmin = std::min(remaining, (int)bg_inds.size());
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.begin() + rmin);
            remaining -= rmin;
        }
    }
  }

  // Select sampled values from various arrays:
  labels.resize(keep_inds.size());
  rois.resize(keep_inds.size());
  std::vector<Point4f<Dtype> > _gt_boxes(keep_inds.size());
  for (size_t i = 0; i < keep_inds.size(); ++ i) {
    labels[i] = _labels[keep_inds[i]];
    rois[i] = all_rois[keep_inds[i]];
    _gt_boxes[i] =
        gt_assignment[keep_inds[i]] >= 0 ? gt_boxes[gt_assignment[keep_inds[i]]] : Point4f<Dtype>();
    // Clamp labels for the background RoIs to 0
    if ( i >= fg_rois_per_this_image ) 
        labels[i] = 0;
  }

#ifdef DEBUG
  DLOG(INFO) << "num fg : " << labels.size() - std::count(labels.begin(), labels.end(), 0);
  DLOG(INFO) << "num bg : " << std::count(labels.begin(), labels.end(), 0);
  CHECK_EQ(std::count(labels.begin(), labels.end(), -1), 0);
  this->_count_ += 1;
  this->_fg_num_ += labels.size() - std::count(labels.begin(), labels.end(), 0);
  this->_bg_num_ += std::count(labels.begin(), labels.end(), 0);
  DLOG(INFO) << "num fg avg : " << this->_fg_num_ * 1. / this->_count_;
  DLOG(INFO) << "num bg avg : " << this->_bg_num_ * 1. / this->_count_;
  DLOG(INFO) << "ratio : " << float(this->_fg_num_) / float(this->_bg_num_+FrcnnParam::eps);
  DLOG(INFO) << "FrcnnParam::bbox_normalize_targets : " << (FrcnnParam::bbox_normalize_targets ? "True" : "False");
  DLOG(INFO) << "FrcnnParam::bbox_normalize_means : " << FrcnnParam::bbox_normalize_means[0] << ", " << FrcnnParam::bbox_normalize_means[1]
        << ", " << FrcnnParam::bbox_normalize_means[2] << ", " << FrcnnParam::bbox_normalize_means[3];
  DLOG(INFO) << "FrcnnParam::bbox_normalize_stds : " << FrcnnParam::bbox_normalize_stds[0] << ", " << FrcnnParam::bbox_normalize_stds[1]
        << ", " << FrcnnParam::bbox_normalize_stds[2] << ", " << FrcnnParam::bbox_normalize_stds[3];
#endif

  //def _compute_targets(ex_rois, gt_rois, labels):
  CHECK_EQ(rois.size(), _gt_boxes.size());
  CHECK_EQ(rois.size(), labels.size());
  std::vector<Point4f<Dtype> > bbox_targets_data = bbox_transform(rois, _gt_boxes);
  if ( FrcnnParam::bbox_normalize_targets ) {
    // Optionally normalize targets by a precomputed mean and stdev
    for (size_t index = 0; index < bbox_targets_data.size(); index ++) {
      for (int j = 0; j < 4; j ++) {
        bbox_targets_data[index][j] = (bbox_targets_data[index][j]-FrcnnParam::bbox_normalize_means[j]) / FrcnnParam::bbox_normalize_stds[j];
      }
    }
  }

  // Compute boxes target 
  bbox_targets = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  bbox_inside_weights = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  for (size_t i = 0; i < labels.size(); ++i) if (labels[i] > 0) {
    int cls = labels[i];
    //get bbox_targets and bbox_inside_weights
    bbox_targets[i][cls] = bbox_targets_data[i];
  }

}

template <typename Dtype>
void DCRProposalTargetLayer<Dtype>::_sample_rois_fg_bg(const vector<Point4f<Dtype> > &all_rois, const vector<Point4f<Dtype> > &gt_boxes, 
        const vector<int> &gt_label, const vector<int> &pred_label, const vector<Dtype> &pred_probs, const int rois_per_image, vector<int> &labels, 
        vector<Point4f<Dtype> > &rois, vector<vector<Point4f<Dtype> > > &bbox_targets, vector<vector<Point4f<Dtype> > > &bbox_inside_weights) {
// Generate a random sample of RoIs comprising foreground and background examples and **hard false positive**.

  CHECK_EQ(gt_label.size(), gt_boxes.size());
  // overlaps: (rois x gt_boxes)
  std::vector<std::vector<Dtype> > overlaps = get_ious(all_rois, gt_boxes, FrcnnParam::test_use_gpu_nms);
  std::vector<Dtype> max_overlaps(all_rois.size(), 0);
  std::vector<int> gt_assignment(all_rois.size(), -1);
  std::vector<int> _labels(all_rois.size());
  for (int i = 0; i < all_rois.size(); ++ i) {
    for (int j = 0; j < gt_boxes.size(); ++ j) {
      if (max_overlaps[i] <= overlaps[i][j]) {
        max_overlaps[i] = overlaps[i][j];
        gt_assignment[i] = j;       
      }
    }
  }
  DLOG(INFO) << "sample_rois : all_rois: " << all_rois.size() << ", gt_box: " << gt_boxes.size();
  for (size_t i = 0; i < all_rois.size(); ++i) {
    if (gt_assignment[i] >= 0 ) {
      CHECK_LT(gt_assignment[i], gt_label.size());
      _labels[i] = gt_label[gt_assignment[i]];
    } else {
      _labels[i] = 0;
    }
  }
  
  // Select foreground RoIs as those FG_THRESH <= IOU < 1 and predict right, not include groud truth.
  std::vector<int> fg_inds;
  //const float fg_thr = FrcnnParam::fg_thresh + 0.1 * this->layer_param_.proposal_target_param().stage(); // 0.5, 0.6, 0.7
  const float fg_thr = FrcnnParam::fg_thresh;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= fg_thr && max_overlaps[i] < 1 && pred_label[i] == _labels[i]) {
      fg_inds.push_back(i);
    }
  }
  // Select background RoIs as those within [BG_THRESH_LO, BG_THRESH_HI)
  std::vector<int> bg_inds;
  //const float bg_thr_hi = FrcnnParam::bg_thresh_hi + 0.1 * this->layer_param_.proposal_target_param().stage();
  const float bg_thr_hi = FrcnnParam::bg_thresh_hi;
  for (int i = 0; i < all_rois.size(); ++i) {
    if (max_overlaps[i] >= FrcnnParam::bg_thresh_lo 
        && max_overlaps[i] < bg_thr_hi) {
      bg_inds.push_back(i);
    }
  }
  
  set<int> ind_set;
  // hard false positive
  for (int i = 0; i < all_rois.size(); ++i) {
    if ( ( (pred_probs[i] >= hard_fp_score_) && (pred_label[i] != _labels[i] || max_overlaps[i] < fg_thr) )
        || (max_overlaps[i] >= fg_thr && pred_label[i] != _labels[i]) ) {
        ind_set.insert(i);
    }
  }
  // The indices that we're selecting (both fg and bg and hard-fp)
  ind_set.insert(fg_inds.begin(), fg_inds.end());
  ind_set.insert(bg_inds.begin(), bg_inds.end());
  std::vector<int> keep_inds(ind_set.begin(), ind_set.end());

  // fyk add for solving the problem of zero roi problem, better than setting bg_thresh_lo to 0 in config
  // Need more?
  int remaining = rois_per_image - keep_inds.size();
  if (remaining < 0) {
    shuffle(keep_inds.begin(), keep_inds.end(), (caffe::rng_t *) this->rng_->generator());
    keep_inds.resize(rois_per_image);
  }
  else if (remaining > 0) {
    // Looks like we don't have enough samples to maintain the desired
    // balance. Reduce requirements and fill in the rest. This is
    // likely different from the Mask RCNN paper.
    // There is a small chance we have neither fg nor bg samples.
    if (keep_inds.size() == 0) {
        // Pick bg regions with easier IoU threshold
        for (int i = 0; i < all_rois.size(); ++i) {
          if (max_overlaps[i] < FrcnnParam::bg_thresh_lo) { 
            bg_inds.push_back(i);
          }
        }
        if (bg_inds.size() > 0) {
            shuffle(bg_inds.begin(), bg_inds.end(), (caffe::rng_t *) this->rng_->generator());
            bg_inds.resize(rois_per_image);
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.end());
        }
    }else{
        if (bg_inds.size() == 0) {
            for (int i = 0; i < all_rois.size(); ++i) {
	      // Negative ROIs are those with max IoU 0.1-0.5 (hard example mining)
	      // To hard example mine or not to hard example mine, that's the question
              if (max_overlaps[i] < FrcnnParam::bg_thresh_hi) {
                bg_inds.push_back(i);
              }
            }
	}
        // Fill the rest with repeated bg rois.
        if (bg_inds.size() > 0) while (remaining > 0) {
            int rmin = std::min(remaining, (int)bg_inds.size());
            keep_inds.insert(keep_inds.end(), bg_inds.begin(), bg_inds.begin() + rmin);
            remaining -= rmin;
        }
    }
  }

  // Select sampled values from various arrays:
  labels.resize(keep_inds.size());
  rois.resize(keep_inds.size());
  std::vector<Point4f<Dtype> > _gt_boxes(keep_inds.size());
  for (size_t i = 0; i < keep_inds.size(); ++ i) {
    labels[i] = _labels[keep_inds[i]];
    rois[i] = all_rois[keep_inds[i]];
    _gt_boxes[i] =
        gt_assignment[keep_inds[i]] >= 0 ? gt_boxes[gt_assignment[keep_inds[i]]] : Point4f<Dtype>();
  }

  //def _compute_targets(ex_rois, gt_rois, labels):
  CHECK_EQ(rois.size(), _gt_boxes.size());
  CHECK_EQ(rois.size(), labels.size());
  std::vector<Point4f<Dtype> > bbox_targets_data = bbox_transform(rois, _gt_boxes);
  if ( FrcnnParam::bbox_normalize_targets ) {
    // Optionally normalize targets by a precomputed mean and stdev
    for (size_t index = 0; index < bbox_targets_data.size(); index ++) {
      for (int j = 0; j < 4; j ++) {
        bbox_targets_data[index][j] = (bbox_targets_data[index][j]-FrcnnParam::bbox_normalize_means[j]) / FrcnnParam::bbox_normalize_stds[j];
      }
    }
  }

  // Compute boxes target 
  bbox_targets = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  bbox_inside_weights = std::vector<std::vector<Point4f<Dtype> > >(
          keep_inds.size(), std::vector<Point4f<Dtype> >(this->config_n_classes_));
  for (size_t i = 0; i < labels.size(); ++i) if (labels[i] > 0) {
    int cls = labels[i];
    //get bbox_targets and bbox_inside_weights
    bbox_targets[i][cls] = bbox_targets_data[i];
  }

}

INSTANTIATE_CLASS(DCRProposalTargetLayer);
EXPORT_LAYER_MODULE_CLASS(DCRProposalTarget);
//REGISTER_LAYER_CLASS(DCRProposalTarget);

} // namespace frcnn

} // namespace caffe