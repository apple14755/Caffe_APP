#include <string>
#include <vector>

#include "caffe/sgd_solvers.hpp"
#include "caffe/util/hdf5.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/upgrade_proto.hpp"
#include "caffe/util/math_functions.hpp"
#include <numeric>
#include "caffe/adaptive_probabilistic_pruning.hpp"
#include <cmath>
#include <algorithm>
#include <fstream>
#define NUM_SHOW 20

namespace caffe {

// Return the current learning rate. The currently implemented learning rate
// policies are as follows:
//    - fixed: always return base_lr.
//    - step: return base_lr * gamma ^ (floor(iter / step))
//    - exp: return base_lr * gamma ^ iter
//    - inv: return base_lr * (1 + gamma * iter) ^ (- power)
//    - multistep: similar to step but it allows non uniform steps defined by
//      stepvalue
//    - poly: the effective learning rate follows a polynomial decay, to be
//      zero by the max_iter. return base_lr (1 - iter/max_iter) ^ (power)
//    - sigmoid: the effective learning rate follows a sigmod decay
//      return base_lr ( 1/(1 + exp(-gamma * (iter - stepsize))))
//
// where base_lr, max_iter, gamma, step, stepvalue and power are defined
// in the solver parameter protocol buffer, and iter is the current iteration.
template <typename Dtype>
Dtype SGDSolver<Dtype>::GetLearningRate() {
  Dtype rate;
  const string& lr_policy = this->param_.lr_policy();
  if (lr_policy == "fixed") {
    rate = this->param_.base_lr();
  } else if (lr_policy == "step") {
    this->current_step_ = this->iter_ / this->param_.stepsize();
    rate = this->param_.base_lr() *
        pow(this->param_.gamma(), this->current_step_);
  } else if (lr_policy == "exp") {
    rate = this->param_.base_lr() * pow(this->param_.gamma(), this->iter_);
  } else if (lr_policy == "inv") {
    rate = this->param_.base_lr() *
        pow(Dtype(1) + this->param_.gamma() * this->iter_,
            - this->param_.power());
  } else if (lr_policy == "multistep") {
    if (this->current_step_ < this->param_.stepvalue_size() &&
          this->iter_ >= this->param_.stepvalue(this->current_step_)) {
      this->current_step_++;
      LOG(INFO) << "MultiStep Status: Iteration " <<
      this->iter_ << ", step = " << this->current_step_;
    }
    rate = this->param_.base_lr() *
        pow(this->param_.gamma(), this->current_step_);
  } else if (lr_policy == "poly") {
    rate = this->param_.base_lr() * pow(Dtype(1.) -
        (Dtype(this->iter_) / Dtype(this->param_.max_iter())),
        this->param_.power());
  } else if (lr_policy == "sigmoid") {
    rate = this->param_.base_lr() * (Dtype(1.) /
        (Dtype(1.) + exp(-this->param_.gamma() * (Dtype(this->iter_) -
          Dtype(this->param_.stepsize())))));
  } else {
    LOG(FATAL) << "Unknown learning rate policy: " << lr_policy;
  }
  return rate;
}

template <typename Dtype>
void SGDSolver<Dtype>::PreSolve() {
  // Initialize the history
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  history_.clear();
  update_.clear();
  temp_.clear();
  for (int i = 0; i < net_params.size(); ++i) {
    const vector<int>& shape = net_params[i]->shape();
    history_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
    update_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
    temp_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));    
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::ClipGradients() {
  const Dtype clip_gradients = this->param_.clip_gradients();
  // cout << "clip_gradients: " << clip_gradients << endl; // WANGHUAN
  if (clip_gradients < 0) { return; }
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  Dtype sumsq_diff = 0;
  for (int i = 0; i < net_params.size(); ++i) {
    sumsq_diff += net_params[i]->sumsq_diff();
  }
  const Dtype l2norm_diff = std::sqrt(sumsq_diff);
  if (l2norm_diff > clip_gradients) {
    Dtype scale_factor = clip_gradients / l2norm_diff;
    LOG(INFO) << "Gradient clipping: scaling down gradients (L2 norm "
        << l2norm_diff << " > " << clip_gradients << ") "
        << "by scale factor " << scale_factor;
    for (int i = 0; i < net_params.size(); ++i) {
      net_params[i]->scale_diff(scale_factor);
    }
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::ApplyUpdate() {
  CHECK(Caffe::root_solver()); /// 更新梯度是由主solver来做的
  Dtype rate = GetLearningRate();
  if (this->param_.display() && this->iter_ % this->param_.display() == 0) {
    LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
  }
  // ClearHistory();
  ClipGradients();
  for (int param_id = 0; param_id < this->net_->learnable_params().size();
       ++param_id) {
    ClearHistory(param_id); // WANGHUAN
    Normalize(param_id);
    Regularize(param_id);
    ComputeUpdateValue(param_id, rate);
  }
  this->net_->Update();
}

template <typename Dtype>
void SGDSolver<Dtype>::Normalize(int param_id) {
  if (this->param_.iter_size() == 1) { return; }
  // Scale gradient to counterbalance accumulation.
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  const Dtype accum_normalization = Dtype(1.) / this->param_.iter_size();
  switch (Caffe::mode()) {
  case Caffe::CPU: {
    caffe_scal(net_params[param_id]->count(), accum_normalization,
        net_params[param_id]->mutable_cpu_diff());
    break;
  }
  case Caffe::GPU: {
#ifndef CPU_ONLY
    caffe_gpu_scal(net_params[param_id]->count(), accum_normalization,
        net_params[param_id]->mutable_gpu_diff());
#else
    NO_GPU;
#endif
    break;
  }
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}


template <typename Dtype>
// typedef std::pair<Dtype, int> mypair;
bool SGDSolver<Dtype>::Comparator(const std::pair<Dtype, int>& left, const std::pair<Dtype, int>& right) { 
    return (left.first < right.first); 
}    

template <typename Dtype>
void SGDSolver<Dtype>::Regularize(int param_id) {    
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  const vector<float>& net_params_weight_decay =
      this->net_->params_weight_decay();
  Dtype weight_decay = this->param_.weight_decay();
  string regularization_type = this->param_.regularization_type();

  // ------------------------------------------------
  // Decrease-Weight-Decay Mode, WANGHUAN
  Dtype current_wd = weight_decay; // default
  if (this->param_.dwd_mode() != "None") {
      CHECK_GE(this->param_.wd_end(), 0) << "Error: wd_end must be in [0, 1]";
      // CHECK_LE(this->param_.wd_end(), 1) << "Error: wd_end must be in [0, 1]"; // weight decay can go up, when wd_end > 1
      
      const int begin = this->param_.dwd_begin_iter();      
      if (this->iter_ >= begin) {
          if (this->param_.dwd_mode() == "linearly") {
            const int end   = this->param_.dwd_end_iter();
            CHECK_GT(end, begin) << "Error: dwd_end_iter must be larger than dwd_begin_iter.";
            current_wd = weight_decay * (1 - (1 - this->param_.wd_end()) / (end - begin) * (std::min(this->iter_, end) - begin));
          
          } else if (this->param_.dwd_mode() == "step_linearly") {
            const int end   = this->param_.dwd_end_iter();
            CHECK_GT(end, begin) << "Error: dwd_end_iter must be larger than dwd_begin_iter.";
            const int tmp_iter = (std::min(this->iter_, end) - begin) / this->param_.dwd_step() * this->param_.dwd_step();
            current_wd = weight_decay * (1 - (1 - this->param_.wd_end()) / (end - begin) * tmp_iter);

          } else if (this->param_.dwd_mode() == "adaptive") { // legacy
            const int num_pruned = *std::max_element(APP<Dtype>::num_pruned_col.begin(), APP<Dtype>::num_pruned_col.end()); // 9 is the size, TODO: replace it using vector
            const int num_to_prune = APP<Dtype>::max_num_column_to_prune;
            current_wd = weight_decay * (1 - (1 - this->param_.wd_end()) / num_to_prune * num_pruned);
          }
      }
  }
  
  // ------------------------------------------------
  Dtype local_decay = current_wd * net_params_weight_decay[param_id];
  
  
  
  switch (Caffe::mode()) {
  case Caffe::CPU: {
    if (local_decay) {
      if (regularization_type == "L2") {
        // add weight decay
        caffe_axpy(net_params[param_id]->count(),
            local_decay,
            net_params[param_id]->cpu_data(),
            net_params[param_id]->mutable_cpu_diff());

      } else if (regularization_type == "L1") {
        caffe_cpu_sign(net_params[param_id]->count(),
            net_params[param_id]->cpu_data(),
            temp_[param_id]->mutable_cpu_data());


        // compute sign, saved in temp_[param_id]->mutable_cpu_data()
        caffe_axpy(net_params[param_id]->count(),
            local_decay,
            temp_[param_id]->cpu_data(),
            net_params[param_id]->mutable_cpu_diff());

      } else {
        LOG(FATAL) << "Unknown regularization type: " << regularization_type;
      }
    }
    break;
  }
  case Caffe::GPU: {
#ifndef CPU_ONLY
    if (local_decay) {
      if (regularization_type == "L2") {
        // add weight decay
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());

      } else if (regularization_type == "L1") {
        caffe_gpu_sign(net_params[param_id]->count(),
            net_params[param_id]->gpu_data(),
            temp_[param_id]->mutable_gpu_data());

        caffe_gpu_axpy(net_params[param_id]->count(),
            local_decay,
            temp_[param_id]->gpu_data(),
            net_params[param_id]->mutable_gpu_diff());

        // print cpu_diff after L1 reg
        for (int i = 0; i < 10; i++) {
          std::cout << *(net_params[param_id]->cpu_diff() + i) << " ";
        }
        std::cout << std::endl;
    
      } else if (regularization_type == "SSL" || regularization_type == "SSL_discriminative") {
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());

        
        // Occasions to return
        // 1. Get layer index and layer name, if not registered, don't reg it.
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        if (APP<Dtype>::layer_index.count(layer_name) == 0) { return; }
        const int L = APP<Dtype>::layer_index[layer_name];
        
        // 2.
        const bool IF_want_prune  = APP<Dtype>::prune_method != "None" && APP<Dtype>::prune_ratio[L] > 0;
        const bool IF_been_pruned = APP<Dtype>::pruned_ratio[L] > 0;
        const bool IF_enough_iter = APP<Dtype>::step_ >= APP<Dtype>::prune_begin_iter+1;
        const bool IF_prune = IF_want_prune && (IF_been_pruned || IF_enough_iter);
        if (!(IF_prune && APP<Dtype>::iter_prune_finished[L] == INT_MAX)) { return; }
        
        // 3. Do not reg biases
        const vector<int>& shape = net_params[param_id]->shape();
        if (shape.size() == 1) { return; } 
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        const int num_row = net_params[param_id]->shape()[0];
        const int num_col = count / num_row;
        vector<Dtype> reg_multiplier(count, APP<Dtype>::AA); // Note!! use AA to set column reg, weight decay remains weight decay.
      
        Dtype* sqrted_energy = (Dtype*) malloc (sizeof(Dtype*) * count); // demoninator of SSL reg
        typedef std::pair<Dtype, int> mypair;
        vector<mypair> col_score(num_col);
        cout << "ave-magnitude_col " << this->iter_ << " " << layer_name << ":";
        for (int j = 0; j < num_col; ++j) {
          Dtype sum  = 0;
          Dtype sum2 = 0;
          for (int i = 0; i < num_row; ++i) { 
            sum  += weight[i * num_col + j] * weight[i * num_col + j];
            sum2 += fabs(weight[i * num_col + j]);
          }
          cout << " " << sum2/num_row;
          
          // col score
          col_score[j].first  = APP<Dtype>::IF_col_pruned[L][j][0] ? INT_MAX : sum2;
          col_score[j].second = j; 
          
          for (int i = 0; i < num_row; ++i) {
            sqrted_energy[i * num_col + j] = (sum == 0) ? 1 : sqrt(sum); // 1 for to avoid divide zero
          }
        }
        cout << endl;
        
        // "SSL_discriminative" doesn't reg the `good` columns.
        if (regularization_type == "SSL_discriminative") {
            sort(col_score.begin(), col_score.end());
            const int num_col_ = num_col - APP<Dtype>::num_pruned_col[L];
            const int num_col_to_prune_ = ceil(num_col * APP<Dtype>::prune_ratio[L]) - APP<Dtype>::num_pruned_col[L];
            for (int rk = num_col_to_prune_; rk < num_col_; ++rk) {
                const int col_of_rank_rk = col_score[rk].second;
                for (int i = 0; i < num_row; ++i) {
                    reg_multiplier[i * num_col + col_of_rank_rk] = 0;
                }
            }
        }

        // add SSL reg
        Dtype* scaled_weight = (Dtype*) malloc (sizeof(Dtype*) * count);
        caffe_div(count, 
                  net_params[param_id]->cpu_data(), 
                  (const Dtype*) sqrted_energy, 
                  scaled_weight); // degug here

        // Add Reg
        for (int i = 0; i < count; ++i) {
            net_params[param_id]->mutable_cpu_diff()[i] += reg_multiplier[i] * scaled_weight[i];
        }

        free(scaled_weight);
        free(sqrted_energy);
      // ******************************************************************************************
      } else if (regularization_type == "OptimalReg") {
       // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // some occasions to return
        const int L = param_id / 2; // TODO: improve, if no biase, this will cause error.
        bool IF_find_layer_name = false;
        std::map<string,int>::iterator it;
        string layer_name;
        for (it = APP<Dtype>::layer_index.begin(); it != APP<Dtype>::layer_index.end(); ++it) {
            if (it->second == L) {
                IF_find_layer_name = true;
                layer_name = it->first;
                break;
            }
        }
        if (!IF_find_layer_name) { return; }
        if (APP<Dtype>::iter_prune_finished[L] != INT_MAX) { return; }
        const vector<int>& shape = net_params[param_id]->shape();
        if (shape.size() != 4) { return; } // do not reg biases and fc layer
        
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        const int num_row = net_params[param_id]->shape()[0];
        const int num_col = count / num_row;
        const int num_col_to_prune = ceil(num_col * APP<Dtype>::prune_ratio[L]);
        const int num_col_pruned = APP<Dtype>::num_pruned_col[L];
        if (num_col_pruned >= num_col_to_prune) { return; } // there is `/(num_col_to_prune - num_col_pruned)` below, the denominator cannot be zero
        
        // ************************** ************************** 
        // Sort 01: sort by L1-norm
        typedef std::pair<Dtype, int> mypair;
        vector<mypair> col_score(num_col);
        for (int j = 0; j < num_col; ++j) { // j: column number
            col_score[j].second = j;
            col_score[j].first  = 0;
            for (int i = 0; i < num_row; ++i) {
                col_score[j].first += -fabs(weight[i * num_col + j]); // !! Note the minus, to make descneding order: the smaller rank, the more important
            }
            if (APP<Dtype>::IF_col_pruned[L][j][0]) { // TODEBUG: fix this [0]
                col_score[j].first = 1;
            }            
        }
        sort(col_score.begin(), col_score.end());
        
        const int n = this->iter_ + 1; // No.n iter (n starts from 1)
        for (int j = 0; j < num_col; ++j) { // j: rank
            const int col_of_rank_j = col_score[j].second;
            APP<Dtype>::hrank[L][col_of_rank_j] = ((n-1) * APP<Dtype>::hrank[L][col_of_rank_j] + j) / n;
        }
        
        // ************************** **************************
        // Sort 02: sort by history_rank
        vector<mypair> col_hrank(num_col); // the history_rank of each column
        for (int j = 0; j < num_col; ++j) { // j: column number
            col_hrank[j].first  = APP<Dtype>::hrank[L][j];
            col_hrank[j].second = j;
            if (APP<Dtype>::IF_col_pruned[L][j][0]) {
                col_hrank[j].first = INT_MAX;
            }
        }
        sort(col_hrank.begin(), col_hrank.end()); // col_hrank is like col_score
        
        // for print 
        vector<int> col_rank(num_col); // the rank of each column 
        for (int j = 0; j < num_col; ++j) { // j: rank
            const int col_of_rank_j = col_hrank[j].second; 
            col_rank[col_of_rank_j] = j;
        }
        
        // Print: Check rank, j is column number --------------------
        char iter[10];
        sprintf(iter, "%6d", this->iter_ + 1);
        cout << iter << "-hrank-" << layer_name << ":";
        for (int j = 0; j < num_col; ++j) {
            cout << "  ";
            char s[10];
            sprintf(s, "%6.2f", APP<Dtype>::hrank[L][j]);
            cout << s;
        }
        cout << endl;
        
        cout << iter << "-rank(by_hrank)-" << layer_name << ":";
        for (int j = 0; j < num_col; ++j) { // j: column number
            cout << "  ";
            char s[10];
            sprintf(s, "%3d", col_rank[j]);
            cout << s;
        }
        cout << endl;
        // ----------------------------------------------------
        
        
        // Key part: distribute Reg quota to columns
        // Only one goal: distribute Reg quota within `num_iter_reg` iters
        const int num_iter_reg_left = APP<Dtype>::num_iter_reg - this->iter_;
        if (num_iter_reg_left < 0) { cout << "num_iter_reg_left < 0, wrong" << endl;}
        const int num_col_to_prune_left = num_col_to_prune - num_col_pruned;
        
        const Dtype reg_quota_end = (APP<Dtype>::reg_to_distribute[L] * 2) / num_iter_reg_left / (num_col_to_prune_left + 1); // adjust reg adaptively
        const Dtype d = (num_col_to_prune_left - 1) * reg_quota_end / (num_iter_reg_left - 1);
        const Dtype reg_quota_now = (num_col_to_prune_left - 1) * d + reg_quota_end; // reg quota for current iter
        vector<Dtype> reg_multiplier(count, APP<Dtype>::target_reg);
        
        // distribute quota according to column criteria
        Dtype hrank_sum = 0;
        for (int j = num_col - num_col_to_prune; j < num_col - num_col_pruned; ++j) {
            hrank_sum += col_hrank[j].first;
        }
        const Dtype k = reg_quota_now / hrank_sum;
        
        Dtype reg_sum = 0;
        for (int j = num_col - num_col_to_prune; j < num_col - num_col_pruned; ++j) { // j: rank
            const int col_of_rank_j = col_hrank[j].second;
            const Dtype Delta   = k * col_hrank[j].first;
            const Dtype old_reg = APP<Dtype>::history_reg[L][col_of_rank_j];
            const Dtype new_reg = std::min(std::max(old_reg + Delta, Dtype(0)), Dtype(APP<Dtype>::target_reg));

            APP<Dtype>::history_reg[L][col_of_rank_j] = new_reg;
            reg_sum += new_reg;
            for (int i = 0; i < num_row; ++i) {
                reg_multiplier[i * num_col + col_of_rank_j] = new_reg;
            }
        }
        APP<Dtype>::reg_to_distribute[L] = num_col_to_prune * APP<Dtype>::target_reg - reg_sum;
        
        for (int j = 0; j < num_col - num_col_to_prune; ++j) { // j: rank
            const int col_of_rank_j = col_hrank[j].second;
            const Dtype Delta   = k * (col_hrank[j].first - col_hrank[num_col-num_col_to_prune-1].first); // Delta < 0
            const Dtype old_reg = APP<Dtype>::history_reg[L][col_of_rank_j];
            const Dtype new_reg = std::min(std::max(old_reg + Delta, Dtype(0)), Dtype(APP<Dtype>::target_reg));

            APP<Dtype>::history_reg[L][col_of_rank_j] = new_reg;
            for (int i = 0; i < num_row; ++i) {
                reg_multiplier[i * num_col + col_of_rank_j] = new_reg;
            }
        
        }
        
        // Check reg
        char* mthd = new char[strlen(APP<Dtype>::prune_method.c_str()) + 1];
        strcpy(mthd, APP<Dtype>::prune_method.c_str());
        strtok(mthd, "_"); // mthd is like "Reg_Col", the first split is `Reg`
        const char* row_or_col = strtok(NULL, "_");
        const string mark = (strcmp(row_or_col, "Col")) ? "r" : "c";
        const int stride = (strcmp(row_or_col, "Col")) ? num_col : 1;
        
        if (APP<Dtype>::step_ % 10 == 0) {
            std::cout << layer_name << " optimal reg:" << endl;
            for (int j = 0; j < NUM_SHOW; ++j) {
                const string mark2 = j < 9 ? mark + " " : mark;
                std::cout << mark2 << j+1 << ":    " << reg_multiplier[j * stride] << std::endl;
            }
        }

        // Add Reg
        for (int i = 0; i < count; ++i) {
            net_params[param_id]->mutable_cpu_diff()[i] += reg_multiplier[i] * weight[i];
        }

      // ******************************************************************************************
      } else if (regularization_type == "SelectiveReg" || regularization_type == "Reg_Col") {
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // If return
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
        
        /* print for check
        cout << param_id << "  layer_name: " << layer_name << endl;
        cout << "  num_param_layer_indices: " << this->net_->param_layer_indices().size() << endl; 
        cout << "  num_layer: "               << this->net_->layer_names().size() << endl;
        cout << "  num_blob: "                << this->net_->blob_names().size()  << endl;
        cout << "  num_learnable_param: "     << this->net_->learnable_params().size() << endl;

        // My layer_index only contains conv and fc layers, while caffe's layer_index contains literally all layers.
        map<string, int> layer_names_index = this->net_->layer_names_index();
        cout << "my layer_index: " << L 
             << "  caffe's layer_index: " << layer_names_index[layer_name] << endl;
        */

        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count   = net_params[param_id]->count();
        const int num_row = net_params[param_id]->shape()[0];
        const int num_col = count / num_row;
        
        const int num_pruned_col = APP<Dtype>::num_pruned_col[L];
        const int num_col_to_prune_ = ceil(num_col * APP<Dtype>::prune_ratio[L]) - num_pruned_col;
        const int num_col_ = num_col - num_pruned_col;
        assert (num_col_to_prune_ > 0);
        const Dtype AA = APP<Dtype>::AA;
        vector<Dtype> reg_multiplier(count, -1);
        
        // (1) Reg-rank_Col
        if (APP<Dtype>::prune_coremthd == "Reg-rank") {
            // ***********************************************************
            // Sort 01: sort by L1-norm
            typedef std::pair<Dtype, int> mypair;
            vector<mypair> col_score(num_col); // score of each column
            for (int j = 0; j < num_col; ++j) {
                col_score[j].second = j;
                if (APP<Dtype>::IF_col_pruned[L][j][0]) { // TODEBUG: fix this [0]
                    col_score[j].first = APP<Dtype>::hrank[L][j]; // make the pruned sink down
                    continue;
                } 
                col_score[j].first  = 0;
                for (int i = 0; i < num_row; ++i) {
                    col_score[j].first += fabs(weight[i * num_col + j]);
                }           
            }
            sort(col_score.begin(), col_score.end());
            // Make new criteria by rank: history_rank
            const int n = this->iter_ + 1; // No.n iter (n starts from 1)
            for (int rk = 0; rk < num_col; ++rk) {
                const int col_of_rank_rk = col_score[rk].second;
                if (APP<Dtype>::IF_col_pruned[L][col_of_rank_rk][0]) { continue; }
                APP<Dtype>::hrank[L][col_of_rank_rk] = ((n-1) * APP<Dtype>::hrank[L][col_of_rank_rk] + rk) / n;
            }
            
            // put this here, to let the hrank able to update even when not pruning
            if (this->iter_ % APP<Dtype>::prune_interval != 0) { return; }
            
            // ***********************************************************
            // Sort 02: sort by history_rank
            vector<mypair> col_hrank(num_col); // the history_rank of each column, history_rank is like the new score
            cout << "ave-magnitude_col " << this->iter_ << " " << layer_name << ":";
            for (int j = 0; j < num_col; ++j) {
                
                Dtype sum = 0; // for print ave magnitude
                for (int i = 0; i < num_row; ++i) {
                    sum += fabs(weight[i * num_col + j]);
                }
                cout << " " << sum/num_row;
                
                col_hrank[j].first  = APP<Dtype>::hrank[L][j];
                col_hrank[j].second = j;
            }
            sort(col_hrank.begin(), col_hrank.end());
            cout << endl;
            
            // for print
            vector<int> col_rank(num_col);
            for (int rk = 0; rk < num_col; ++rk) {
                const int col_of_rank_rk = col_hrank[rk].second;
                col_rank[col_of_rank_rk] = rk;
            }
            
            // Print: Check rank, j is column number
            if (this->iter_ % 20 == 0) {
                char iter[10]; sprintf(iter, "%6d", this->iter_ + 1); // max_iter should be in [0, 999999]
                cout << iter << "-" << layer_name << "hrank:";
                for (int j = 0; j < num_col; ++j) {
                    cout << "  ";
                    char s[50];
                    if (APP<Dtype>::IF_col_pruned[L][j][0]) { 
                        sprintf(s, "%7.0f", APP<Dtype>::hrank[L][j]);
                    } else {
                        sprintf(s, "%7.2f", APP<Dtype>::hrank[L][j]);
                    }
                    cout << s;
                }
                cout << endl;
                
                cout << iter << "-" << layer_name << "rank(by_hrank):";
                for (int rk = 0; rk < num_col; ++rk) {
                    cout << "  ";
                    char s[50];
                    const int prune_mark = APP<Dtype>::IF_col_pruned[L][col_hrank[rk].second][0] ? 0 : 1;
                    sprintf(s, "%4d-%d", col_hrank[rk].second, prune_mark);
                    cout << s;
                }
                cout << endl;
            }
            
           

            // check order            
            /* TODEBUG: why this code generate two `_order.txt`? 
            ostringstream stream;
            stream << APP<Dtype>::snapshot_prefix << layer_name << "_order.txt";
            const char* dd = stream.str().c_str();
            ofstream col_score_order(dd, ofstream::app); // dd must be char*, ofstream::app
            if (!col_score_order.is_open()) { 
                cerr << "file open failed: layer_name = " << layer_name << endl; 
            } else {
            */
            
            // Punishment Function
            if (APP<Dtype>::IF_scheme1_when_Reg_rank) {
                // scheme 1
                const Dtype kk = APP<Dtype>::kk;
                const Dtype alpha = log(2/kk) / (num_col_to_prune_ + 1);
                const Dtype N1 = -log(kk)/alpha; // symmetry point
                
                for (int j = 0; j < num_col_; ++j) { // j: rank 
                    const int col_of_rank_j = col_hrank[j + num_pruned_col].second; // Note the real rank is j + num_pruned_col
                    const Dtype Delta = j < N1 ? AA * exp(-alpha * j) : -AA * exp(-alpha * (2*N1-j)) + 2*kk*AA;
                    const Dtype old_reg = APP<Dtype>::history_reg[L][col_of_rank_j];
                    const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                    APP<Dtype>::history_reg[L][col_of_rank_j] = new_reg;
                    for (int i = 0; i < num_row; ++i) {
                        reg_multiplier[i * num_col + col_of_rank_j] = new_reg;
                    }
                    
                    // Print
                    if (new_reg < old_reg) {
                        cout << "reduce reg: " << layer_name << "-" << col_of_rank_j 
                             << "  old reg: "  << old_reg
                             << "  new reg: "  << new_reg << endl;
                    }
                }
            } else {
                // scheme 2, the dis-continual function
                const Dtype kk2 = APP<Dtype>::kk2;
                const Dtype alpha1 = (num_col_to_prune_ == 1)          ? 0 : log(1/kk2) / (num_col_to_prune_ - 1);
                const Dtype alpha2 = (num_col_to_prune_ == num_col_-1) ? 0 : log(1/kk2) / (num_col_-1 - num_col_to_prune_);

                for (int j = 0; j < num_col_; ++j) { // j: rank 
                    const int col_of_rank_j = col_hrank[j + num_pruned_col].second; // Note the real rank is j + num_pruned_col
                    const Dtype Delta = j < num_col_to_prune_ ? AA * exp(-alpha1 * j) : -AA * exp(-alpha2 * (num_col_-1 - j));
                    const Dtype old_reg = APP<Dtype>::history_reg[L][col_of_rank_j];
                    const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                    APP<Dtype>::history_reg[L][col_of_rank_j] = new_reg;
                    for (int i = 0; i < num_row; ++i) {
                        reg_multiplier[i * num_col + col_of_rank_j] = new_reg;
                    }
                }
            }
        
        // (2) Reg-L1_Col
        } else if (APP<Dtype>::prune_coremthd == "Reg-L1") {
            typedef std::pair<Dtype, int> mypair;
            vector<mypair> col_score(num_col);
            for (int j = 0; j < num_col; ++j) {
                col_score[j].second = j;
                if (APP<Dtype>::IF_col_pruned[L][j][0]) {
                    col_score[j].first = INT_MAX;
                    continue;
                }
                col_score[j].first  = 0;
                for (int i = 0; i < num_row; ++i) {
                    col_score[j].first += fabs(weight[i * num_col + j]);
                }
            }
            sort(col_score.begin(), col_score.end());
            
            const Dtype k_L1 = (num_col_to_prune_ == 0) ? 0 : AA / (col_score[num_col_to_prune_].first - col_score[0].first);
            cout << "step: " << this->iter_ + 1 << "  " << layer_name << " k_L1: " << k_L1 << endl;
            for (int rk = 0; rk < num_col_; ++rk) {
                const int col_of_rank_rk = col_score[rk].second;
                const Dtype Delta = AA - k_L1 * (col_score[rk].first - col_score[0].first);
                const Dtype old_reg = APP<Dtype>::history_reg[L][col_of_rank_rk];
                const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                APP<Dtype>::history_reg[L][col_of_rank_rk] = new_reg;
                
                for (int i = 0; i < num_row; ++i) {
                    reg_multiplier[i * num_col + col_of_rank_rk] = new_reg;
                }
                // Print
                if (new_reg < old_reg) {
                    cout << "reduce reg: " << layer_name << "-" << col_of_rank_rk 
                         << "  old reg: "  << old_reg
                         << "  new reg: "  << new_reg << endl;
                }
            }
            
            // Print
            const int num_show = NUM_SHOW > num_col ? num_col : NUM_SHOW;
            cout << layer_name << "-score: "; for (int rk = 0; rk < num_show; ++rk) { cout << col_score[rk].first  << " "; } cout << endl;
            cout << layer_name << "  -col: "; for (int rk = 0; rk < num_show; ++rk) { cout << col_score[rk].second << " "; } cout << endl;
            cout << layer_name << "  -reg: "; for (int rk = 0; rk < num_show; ++rk) { cout << APP<Dtype>::history_reg[L][col_score[rk].second] << " "; } cout << endl;
        }
        
        // apply reg
        for (int i = 0; i < count; ++i) {
            net_params[param_id]->mutable_cpu_diff()[i] += reg_multiplier[i] * weight[i];
        }
      
      // ******************************************************************************************
      // Got idea from cvpr rebuttal, improve SelectiveReg: 1) use L1-norm rather than rank, 2) row prune
      } else if (regularization_type == "Reg_Row") {
          
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        // Three occasions to return
        // 1. Get layer index and layer name, if not registered, don't reg it.
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        if (APP<Dtype>::layer_index.count(layer_name) == 0) { return; }
        const int L = APP<Dtype>::layer_index[layer_name];
        
        // 2.
        const bool IF_want_prune  = APP<Dtype>::prune_method != "None" && APP<Dtype>::prune_ratio[L] > 0;
        const bool IF_been_pruned = APP<Dtype>::pruned_ratio[L] > 0;
        const bool IF_enough_iter = APP<Dtype>::step_ >= APP<Dtype>::prune_begin_iter+1;
        const bool IF_prune = IF_want_prune && (IF_been_pruned || IF_enough_iter);
        if (!(IF_prune && APP<Dtype>::iter_prune_finished[L] == INT_MAX)) { return; }
        
        // 3. Do not reg biases
        const vector<int>& shape = net_params[param_id]->shape();
        if (shape.size() == 1) { return; }
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count     = net_params[param_id]->count();
        const int num_row   = net_params[param_id]->shape()[0];
        const int num_col   = count / num_row;

        vector<Dtype> reg_multiplier(count, -1);
        const Dtype AA = APP<Dtype>::AA;
        const int num_pruned_row    = APP<Dtype>::num_pruned_row[L];
        const int num_row_to_prune_ = ceil(num_row * APP<Dtype>::prune_ratio[L]) - num_pruned_row;
        const int num_row_          = num_row - num_pruned_row;
        
        if (APP<Dtype>::prune_coremthd == "Reg-rank") {
            // Sort 01: sort by L1-norm
            typedef std::pair<Dtype, int> mypair;
            vector<mypair> row_score(num_row);
            for (int i = 0; i < num_row; ++i) {
                row_score[i].second = i;
                if (APP<Dtype>::IF_row_pruned[L][i]) {
                    row_score[i].first = APP<Dtype>::hrank[L][i]; // make the pruned row sink down
                    continue;
                }
                row_score[i].first  = 0;
                for (int j = 0; j < num_col; ++j) {
                    row_score[i].first += fabs(weight[i * num_col + j]);
                }
            }
            sort(row_score.begin(), row_score.end()); // in ascending order
            
            // Make new criteria by rank: history_rank
            const int n = this->iter_ + 1; // No.n iter (n starts from 1)
            for (int rk = 0; rk < num_row; ++rk) {
                const int row_of_rank_rk = row_score[rk].second;
                if (APP<Dtype>::IF_row_pruned[L][row_of_rank_rk]) { continue; }
                APP<Dtype>::hrank[L][row_of_rank_rk] = ((n-1) * APP<Dtype>::hrank[L][row_of_rank_rk] + rk) / n;
            }
            
            // Sort 02: sort by history_rank
            vector<mypair> row_hrank(num_row);
            // cout << "ave-magnitude_row " << this->iter_ << " " << layer_name << ":";
            for (int i = 0; i < num_row; ++i) {
                
                // print 
                // Dtype sum = 0;
                // for (int j = 0; i < num_col; ++j) {
                    // sum += fabs(weight[i * num_col +j]);
                // }
                // cout << " " << sum/num_col;

                row_hrank[i].first  = APP<Dtype>::hrank[L][i];
                row_hrank[i].second = i;
            }
            //cout << endl;
            sort(row_hrank.begin(), row_hrank.end());
            
            // Print: Check rank
            if (this->iter_ % 20 == 0) {
                char iter[10]; sprintf(iter, "%6d", this->iter_ + 1); // max_iter should be in [0, 999999]
                cout << iter << "-" << layer_name << "hrank:";
                for (int i = 0; i < num_row; ++i) {
                    cout << "  ";
                    char s[50];
                    if (APP<Dtype>::IF_row_pruned[L][i]) { 
                        sprintf(s, "%7.0f", APP<Dtype>::hrank[L][i]);
                    } else {
                        sprintf(s, "%7.2f", APP<Dtype>::hrank[L][i]);
                    }
                    cout << s;
                }
                cout << endl;
                
                cout << iter << "-" << layer_name << "rank(by_hrank):";
                for (int rk = 0; rk < num_row; ++rk) {
                    cout << "  ";
                    char s[50];
                    const int prune_mark = APP<Dtype>::IF_row_pruned[L][row_hrank[rk].second] ? 0 : 1;
                    sprintf(s, "%4d-%d", row_hrank[rk].second, prune_mark);
                    cout << s;
                }
                cout << endl;
            }
            
            // Punishment Function 
            assert (num_row_to_prune_ > 0);
            if (APP<Dtype>::IF_scheme1_when_Reg_rank) {
                // scheme 1
                const Dtype kk = APP<Dtype>::kk;
                const Dtype alpha = log(2/kk) / (num_row_to_prune_ + 1);
                const Dtype N1 = -log(kk)/alpha; // symmetry point
                
                for (int rk = 0; rk < num_row_; ++rk) {
                    const int row_of_rank_rk = row_hrank[rk + num_pruned_row].second; // Note the real rank is j + num_pruned_col
                    const Dtype Delta = rk < N1 ? AA * exp(-alpha * rk) : -AA * exp(-alpha * (2*N1-rk)) + 2*kk*AA;
                    const Dtype old_reg = APP<Dtype>::history_reg[L][row_of_rank_rk];
                    const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                    APP<Dtype>::history_reg[L][row_of_rank_rk] = new_reg;
                    for (int j = 0; j < num_col; ++j) {
                        reg_multiplier[row_of_rank_rk * num_col + j] = new_reg;
                    }
                    // Print
                    if (new_reg < old_reg) {
                        cout << "reduce reg: " << layer_name << "-" << row_of_rank_rk 
                             << "  old reg: "  << old_reg
                             << "  new reg: "  << new_reg << endl;
                    }
                }
            } else {
                // scheme 2, the dis-continual function
                const Dtype kk2 = APP<Dtype>::kk2;
                const Dtype alpha1 = (num_row_to_prune_ == 1)          ? 0 : log(1/kk2) / (num_row_to_prune_ - 1);
                const Dtype alpha2 = (num_row_to_prune_ == num_row_-1) ? 0 : log(1/kk2) / (num_row_-1 - num_row_to_prune_);

                for (int rk = 0; rk < num_row_; ++rk) {
                    const int row_of_rank_rk = row_hrank[rk + num_pruned_row].second; // Note the real rank is j + num_pruned_col
                    const Dtype Delta = rk < num_row_to_prune_ ? AA * exp(-alpha1 * rk) : -AA * exp(-alpha2 * (num_row_-1 - rk));
                    const Dtype old_reg = APP<Dtype>::history_reg[L][row_of_rank_rk];
                    const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                    APP<Dtype>::history_reg[L][row_of_rank_rk] = new_reg;
                    for (int j = 0; j < num_col; ++j) {
                        reg_multiplier[row_of_rank_rk * num_col + j] = new_reg;
                    }
                }
            }
        
        // use L1-norm rather than rank
        } else if (APP<Dtype>::prune_coremthd == "Reg-L1") {
            // sort by L1-norm
            typedef std::pair<Dtype, int> mypair;
            vector<mypair> row_score(num_row);
            for (int i = 0; i < num_row; ++i) {
                row_score[i].second = i;
                if (APP<Dtype>::IF_row_pruned[L][i]) {
                    row_score[i].first = INT_MAX; // make the pruned row float up
                    continue;
                }
                row_score[i].first  = 0;
                for (int j = 0; j < num_col; ++j) {
                    row_score[i].first += fabs(weight[i * num_col + j]);
                }
            }
            sort(row_score.begin(), row_score.end()); // in ascending order
            
            // Punishment Function
            assert (num_row_to_prune_ > 0 && num_row_to_prune_ < num_row);
            cout << "num_row_to_prune_: " <<num_row_to_prune_ << endl;
            const Dtype k_L1 = num_row_to_prune_ == 0 ? 0 : AA / (row_score[num_row_to_prune_].first - row_score[0].first);
            
            cout << "k_L1: " << k_L1 << endl;
            for (int rk = 0; rk < num_row_; ++rk) {
                const int row_of_rank_rk = row_score[rk].second;
                const Dtype Delta = AA - k_L1 * (row_score[rk].first - row_score[0].first);
                const Dtype old_reg = APP<Dtype>::history_reg[L][row_of_rank_rk];
                const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
                APP<Dtype>::history_reg[L][row_of_rank_rk] = new_reg;
                
                for (int j = 0; j < num_col; ++j) {
                    reg_multiplier[row_of_rank_rk * num_col + j] = new_reg;
                }

                // Print
                if (new_reg < old_reg) {
                    cout << "reduce reg: " << layer_name << "-" << row_of_rank_rk 
                         << "  old reg: "  << old_reg
                         << "  new reg: "  << new_reg << endl;
                }
            }
            
            // Print
            const int num_show = 10000 > num_row ? num_row : NUM_SHOW;
            cout << "score: ";   for (int rk = 0; rk < num_show; ++rk) { cout << row_score[rk].first  << " "; }
            cout << "\n  row: "; for (int rk = 0; rk < num_show; ++rk) { cout << row_score[rk].second << " "; }
            cout << "\n  reg: "; for (int rk = 0; rk < num_show; ++rk) { cout << APP<Dtype>::history_reg[L][row_score[rk].second] << " "; }
            cout << endl;
        }

        // apply reg
        for (int i = 0; i < count; ++i) {
            net_params[param_id]->mutable_cpu_diff()[i] += reg_multiplier[i] * weight[i];
        }
     
      // ******************************************************************************************
      // use SelectiveReg to do compression
      } else if (regularization_type == "SelectiveRegCompression") {
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // If return
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        const int num_weight_to_prune = ceil(count * APP<Dtype>::prune_ratio[L]);
        const int num_pruned_weight = APP<Dtype>::num_pruned_weight[L];
        // if (num_pruned_weight >= num_weight_to_prune) { return; }
        
        // ***********************************************************
        // Sort 01: sort by L1-norm
        typedef std::pair<Dtype, int> mypair;
        vector<mypair> w_score(count); // score of each column
        for (int i = 0; i < count; ++i) {
            w_score[i].second = i;
            APP<Dtype>::hscore[L][i]  = fabs(weight[i]);// + APP<Dtype>::hscore[L][i] * 0.9; // use momentum for score
            w_score[i].first   = APP<Dtype>::hscore[L][i];
        }
        sort(w_score.begin(), w_score.end());
        
        
        // ***********************************************************
        // Make new criteria by history_rank
        const int n = this->iter_ + 1; // No.n iter (n starts from 1)
        for (int rk = 0; rk < count; ++rk) {
            const int w_of_rank_rk = w_score[rk].second;
            if (APP<Dtype>::IF_weight_pruned[L][w_of_rank_rk]) { continue; } // don't update the hrank of the pruned
            //APP<Dtype>::hrank[L][w_of_rank_rk] = ((n-1) * APP<Dtype>::hrank[L][w_of_rank_rk] + rk) / n;
            APP<Dtype>::hrank[L][w_of_rank_rk] = APP<Dtype>::hrank[L][w_of_rank_rk] ? APP<Dtype>::hrank_momentum * APP<Dtype>::hrank[L][w_of_rank_rk] + (1-APP<Dtype>::hrank_momentum) * rk : rk;
        }

        // Sort 02: sort by history_rank
        vector<mypair> w_hrank(count);
        for (int i = 0; i < count; ++i) {
            w_hrank[i].second = i;
            w_hrank[i].first  = APP<Dtype>::hrank[L][i];
        }
        sort(w_hrank.begin(), w_hrank.end());
                
        // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        // Test: hhrank
        const bool IF_use_hhrank = false;
        vector<mypair> w_hhrank(count);
        if (IF_use_hhrank) {
            // Make new criteria by hhrank
            for (int rk = 0; rk < count; ++rk) {
                const int w_of_rank_rk = w_hrank[rk].second;
                if (APP<Dtype>::IF_weight_pruned[L][w_of_rank_rk]) { continue; }
                APP<Dtype>::hhrank[L][w_of_rank_rk] = ((n-1) * APP<Dtype>::hhrank[L][w_of_rank_rk] + rk) / n;
            }
            
            // Sort 03: sort by hhrank
            for (int i = 0; i < count; ++i) {
                w_hhrank[i].second = i;
                w_hhrank[i].first  = APP<Dtype>::hhrank[L][i];
            }
            sort(w_hhrank.begin(), w_hhrank.end());
        }
        // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
        
        
        // Print: Check rank, j is column number
        char iter[10]; sprintf(iter, "%6d", this->iter_ + 1); // max_iter should be in [0, 999999]
        cout << iter << "-" << layer_name << (IF_use_hhrank ? "hhrank:" : "hrank");
        for (int i = 0; i < 100; ++i) { // why i cannot be 1000-1200?
            cout << "  ";
            char s[50];
            if (APP<Dtype>::IF_weight_pruned[L][i]) { 
                sprintf(s, "%9.0f", (IF_use_hhrank ? APP<Dtype>::hhrank[L][i] : APP<Dtype>::hrank[L][i]));
            } else {
                sprintf(s, "%9.2f", (IF_use_hhrank ? APP<Dtype>::hhrank[L][i] : APP<Dtype>::hrank[L][i]));
            }
            cout << s;
        }
        cout << endl;
        
        cout << iter << "-" << layer_name << (IF_use_hhrank ? "rank(by_hhrank):" : "rank(by_hrank):");
        for (int rk = 0; rk < 100; ++rk) {
            cout << "  ";
            char s[50];
            const int w_of_rank_rk = IF_use_hhrank ? w_hhrank[rk].second : w_hrank[rk].second;
            const int prune_mark = APP<Dtype>::IF_weight_pruned[L][w_of_rank_rk] ? 0 : 1;
            sprintf(s, "%6d-%d", w_of_rank_rk, prune_mark);
            cout << s;
        }
        cout << endl;

        if (APP<Dtype>::iter_prune_finished[L] != INT_MAX) { return; }
        
        // compute reg multiplier for those "bad" columns, "good" columns are spared with zero reg.
        const Dtype AA = (this->iter_ < APP<Dtype>::reg_cushion_iter) ? ((this->iter_+1)*1.0/APP<Dtype>::reg_cushion_iter * APP<Dtype>::AA) : APP<Dtype>::AA; // TODO: replace this 2000 with more consideration
        const Dtype kk = APP<Dtype>::kk;
        const Dtype alpha = log(2/kk) / (num_weight_to_prune - num_pruned_weight + 1);
        const Dtype N1 = -log(kk)/alpha;
        vector<Dtype> reg_multiplier(count, -1);
        
        for (int rk = 0; rk < count - num_pruned_weight; ++rk) {
            const int w_of_rank_rk = IF_use_hhrank ? w_hhrank[rk + num_pruned_weight].second : w_hrank[rk + num_pruned_weight].second; // Note the real rank is j + num_pruned_col
            const Dtype Delta = rk < N1 ? AA * exp(-alpha * rk) : -AA * exp(-alpha * (2*N1-rk)) + 2*kk*AA;
            const Dtype old_reg = APP<Dtype>::history_reg[L][w_of_rank_rk];
            const Dtype new_reg = std::max(old_reg + Delta, Dtype(0));
            APP<Dtype>::history_reg[L][w_of_rank_rk] = new_reg;
            reg_multiplier[w_of_rank_rk] = new_reg;
        }
        
        for (int i = 0; i < count; ++i) {
            net_params[param_id]->mutable_cpu_diff()[i] += reg_multiplier[i] * weight[i];
        }
        
      } else if (regularization_type == "SR_Weight") {
        // SR used to compress large DNN, not using ranking 
        
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // If return
        // const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        const int num_weight_to_prune = ceil(count * APP<Dtype>::prune_ratio[L]);
        const int num_pruned_weight = APP<Dtype>::num_pruned_weight[L];
        
        //TODO 20180321
        
      } else {    
          LOG(FATAL) << "Unknown regularization type: " << regularization_type;
      }

    }
#else
    NO_GPU;
#endif
    break;
  }
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}


#ifndef CPU_ONLY
template <typename Dtype>
void sgd_update_gpu(int N, Dtype* g, Dtype* h, Dtype momentum,
    Dtype local_rate);
#endif

template <typename Dtype>
void SGDSolver<Dtype>::ComputeUpdateValue(int param_id, Dtype rate) {
  const vector<Blob<Dtype>*>& net_params = this->net_->learnable_params();
  const vector<float>& net_params_lr = this->net_->params_lr();
  Dtype momentum = this->param_.momentum();
  Dtype local_rate = rate * net_params_lr[param_id];
  // Compute the update to history, then copy it to the parameter diff.
  switch (Caffe::mode()) {
  case Caffe::CPU: {
    caffe_cpu_axpby(net_params[param_id]->count(), local_rate,
              net_params[param_id]->cpu_diff(), momentum,
              history_[param_id]->mutable_cpu_data()); /// history = momentum * history + lrate * diff
    caffe_copy(net_params[param_id]->count(),
        history_[param_id]->cpu_data(),
        net_params[param_id]->mutable_cpu_diff());
    break;
  }
  case Caffe::GPU: {
#ifndef CPU_ONLY
    sgd_update_gpu(net_params[param_id]->count(),
        net_params[param_id]->mutable_gpu_diff(),
        history_[param_id]->mutable_gpu_data(),
        momentum, local_rate);
#else
    NO_GPU;
#endif
    break;
  }
  default:
    LOG(FATAL) << "Unknown caffe mode: " << Caffe::mode();
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::ClearHistory(int param_id) {
    const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
    if (APP<Dtype>::layer_index.count(layer_name) == 0 || history_[param_id]->shape().size() == 1) { return; }
    const int L = APP<Dtype>::layer_index[layer_name];
    const int count = APP<Dtype>::masks[L].size();
    Dtype* tmp = new Dtype[count]; /// TODEBUG: Why cannot use bool?
    for (int k = 0; k < count; ++k) {
        tmp[k] = APP<Dtype>::masks[L][k];
    }
    caffe_mul(count, 
              (const Dtype*) tmp, 
              history_[param_id]->cpu_data(), 
              history_[param_id]->mutable_cpu_data());
    delete[] tmp;
}

template <typename Dtype>
void SGDSolver<Dtype>::ClearHistory() {
    const vector<shared_ptr<Layer<Dtype> > >& layers = this->net_->layers();
    int param_id = 0;
    for (int i = 0; i < layers.size(); ++i) {
    /// As long as layer i has masks, its history_ should be cleared. But only clear history_ of weights, since we only have masks for weights.
    /// So the key is to relate layer i with corresponding param_id.
        const int count = layers[i]->masks_.size();
        
        if (count) { 
            while (history_[param_id]->count() != count) { 
                ++ param_id; /// jump over biases
            }
            Dtype* tmp = new Dtype[count]; /// TODEBUG: Why cannot use bool?
            for (int k = 0; k < count; ++k) {
                tmp[k] = layers[i]->masks_[k];
            }
            caffe_mul(count, 
                      (const Dtype*) tmp, 
                      history_[param_id]->cpu_data(), 
                      history_[param_id]->mutable_cpu_data());
            delete[] tmp;
            ++ param_id;
        }
    }
}



template <typename Dtype>
void SGDSolver<Dtype>::SnapshotSolverState(const string& model_filename) {
  switch (this->param_.snapshot_format()) {
    case caffe::SolverParameter_SnapshotFormat_BINARYPROTO:
      SnapshotSolverStateToBinaryProto(model_filename);
      break;
    case caffe::SolverParameter_SnapshotFormat_HDF5:
      SnapshotSolverStateToHDF5(model_filename);
      break;
    default:
      LOG(FATAL) << "Unsupported snapshot format.";
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::SnapshotSolverStateToBinaryProto(
    const string& model_filename) {
  SolverState state;
  state.set_iter(this->iter_);
  state.set_learned_net(model_filename);
  state.set_current_step(this->current_step_);
  state.clear_history();
  for (int i = 0; i < history_.size(); ++i) {
    // Add history
    BlobProto* history_blob = state.add_history();
    history_[i]->ToProto(history_blob);
  }
  string snapshot_filename = Solver<Dtype>::SnapshotFilename(".solverstate");
  LOG(INFO)
    << "Snapshotting solver state to binary proto file " << snapshot_filename;
  WriteProtoToBinaryFile(state, snapshot_filename.c_str());
}

template <typename Dtype>
void SGDSolver<Dtype>::SnapshotSolverStateToHDF5(
    const string& model_filename) {
  string snapshot_filename =
      Solver<Dtype>::SnapshotFilename(".solverstate.h5");
  LOG(INFO) << "Snapshotting solver state to HDF5 file " << snapshot_filename;
  hid_t file_hid = H5Fcreate(snapshot_filename.c_str(), H5F_ACC_TRUNC,
      H5P_DEFAULT, H5P_DEFAULT);
  CHECK_GE(file_hid, 0)
      << "Couldn't open " << snapshot_filename << " to save solver state.";
  hdf5_save_int(file_hid, "iter", this->iter_);
  hdf5_save_string(file_hid, "learned_net", model_filename);
  hdf5_save_int(file_hid, "current_step", this->current_step_);
  hid_t history_hid = H5Gcreate2(file_hid, "history", H5P_DEFAULT, H5P_DEFAULT,
      H5P_DEFAULT);
  CHECK_GE(history_hid, 0)
      << "Error saving solver state to " << snapshot_filename << ".";
  for (int i = 0; i < history_.size(); ++i) {
    ostringstream oss;
    oss << i;
    hdf5_save_nd_dataset<Dtype>(history_hid, oss.str(), *history_[i]);
  }
  H5Gclose(history_hid);
  H5Fclose(file_hid);
}

template <typename Dtype>
void SGDSolver<Dtype>::RestoreSolverStateFromBinaryProto(
    const string& state_file) {
  SolverState state;
  ReadProtoFromBinaryFile(state_file, &state);
  this->iter_ = state.iter();
  std::cout << " -- restore proto -- " << std::endl;
  
  if (state.has_learned_net()) {
    NetParameter net_param;
    ReadNetParamsFromBinaryFileOrDie(state.learned_net().c_str(), &net_param);
    this->net_->CopyTrainedLayersFrom(net_param);
  }
  this->current_step_ = state.current_step();
  CHECK_EQ(state.history_size(), history_.size())
      << "Incorrect length of history blobs.";
  LOG(INFO) << "SGDSolver: restoring history";
  for (int i = 0; i < history_.size(); ++i) {
    history_[i]->FromProto(state.history(i));
  }
}

template <typename Dtype>
int SGDSolver<Dtype>::GetLayerIndex(const int& param_id) {
    // Three occasions to return, `-1` means return
    // 1. Get layer index and layer name, if not registered, don't reg it.
    const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
    if (APP<Dtype>::layer_index.count(layer_name) == 0) { return -1; }
    const int L = APP<Dtype>::layer_index[layer_name];
    
    // 2.
    const bool IF_want_prune  = APP<Dtype>::prune_method != "None" && APP<Dtype>::prune_ratio[L] > 0;
    const bool IF_been_pruned = APP<Dtype>::pruned_ratio[L] > 0;
    const bool IF_enough_iter = APP<Dtype>::step_ >= APP<Dtype>::prune_begin_iter+1;
    const bool IF_prune = IF_want_prune && (IF_been_pruned || IF_enough_iter);
    if (!(IF_prune && APP<Dtype>::iter_prune_finished[L] == INT_MAX)) { return -1; }
    
    // 3. Do not reg biases
    const vector<int>& shape = this->net_->learnable_params()[param_id]->shape();
    if (shape.size() == 1) { return -1; }
    
    return L;
    
}

template <typename Dtype>
void SGDSolver<Dtype>::RestoreSolverStateFromHDF5(const string& state_file) {
  hid_t file_hid = H5Fopen(state_file.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  CHECK_GE(file_hid, 0) << "Couldn't open solver state file " << state_file;
  this->iter_ = hdf5_load_int(file_hid, "iter");
  if (H5LTfind_dataset(file_hid, "learned_net")) {
    string learned_net = hdf5_load_string(file_hid, "learned_net");
    this->net_->CopyTrainedLayersFrom(learned_net);
  }
  this->current_step_ = hdf5_load_int(file_hid, "current_step");
  hid_t history_hid = H5Gopen2(file_hid, "history", H5P_DEFAULT);
  CHECK_GE(history_hid, 0) << "Error reading history from " << state_file;
  int state_history_size = hdf5_get_num_links(history_hid);
  CHECK_EQ(state_history_size, history_.size())
      << "Incorrect length of history blobs.";
  for (int i = 0; i < history_.size(); ++i) {
    ostringstream oss;
    oss << i;
    hdf5_load_nd_dataset<Dtype>(history_hid, oss.str().c_str(), 0,
                                kMaxBlobAxes, history_[i].get());
  }
  H5Gclose(history_hid);
  H5Fclose(file_hid);
}

INSTANTIATE_CLASS(SGDSolver);
REGISTER_SOLVER_CLASS(SGD);

}  // namespace caffe
