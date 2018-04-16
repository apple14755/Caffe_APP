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
#include <ctime>
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
  tmp_.clear(); // @ming, for saving reg*weight
  history_reg_.clear(); // @ming
  for (int i = 0; i < net_params.size(); ++i) {
    const vector<int>& shape = net_params[i]->shape();
    history_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
    update_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
    temp_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape)));
    history_reg_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape))); // @mingsuntse
    tmp_.push_back(shared_ptr<Blob<Dtype> >(new Blob<Dtype>(shape))); // @mingsuntse
  }
}

template <typename Dtype>
void SGDSolver<Dtype>::ClipGradients() {
  const Dtype clip_gradients = this->param_.clip_gradients();
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
  #ifdef ShowTimingLog
  cout << "ApplyUpdate start timing" << endl;
  clock_t t1 = clock();
  #endif
  
  CHECK(Caffe::root_solver()); /// 更新梯度是由主solver来做的
  Dtype rate = GetLearningRate();
  if (this->param_.display() && this->iter_ % this->param_.display() == 0) {
    LOG(INFO) << "Iteration " << this->iter_ << ", lr = " << rate;
  }
  ClipGradients();
  #ifdef ShowTimingLog
  cout << "  after clip_gradients: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;
  #endif
  if (APP::step_ % 1 == 0) {  
    ClearHistory(); // There is no need to ClearHistory each iteration.
  }
  #ifdef ShowTimingLog
  cout << "  after ClearHistory: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;
  #endif
  for (int param_id = 0; param_id < this->net_->learnable_params().size();
       ++param_id) {
    Normalize(param_id);
    Regularize(param_id);
    ComputeUpdateValue(param_id, rate);
  }
  #ifdef ShowTimingLog
  cout << "  after ComputeUpdateValue etc.: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;
  #endif
  this->net_->Update();
  #ifdef ShowTimingLog
  cout << "Time in ApplyUpdate: " << (double)(clock() - t1) / CLOCKS_PER_SEC << endl;
  #endif
  
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
      CHECK_LE(this->param_.wd_end(), 1) << "Error: wd_end must be in [0, 1]";
      
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

          } else if (this->param_.dwd_mode() == "adaptive") {
            const int num_pruned = *std::max_element(APP::num_pruned_col.begin(), APP::num_pruned_col.end()); // 9 is the size, TODO: replace it using vector
            const int num_to_prune = APP::max_num_column_to_prune;
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
    
      } else if (regularization_type == "SSL") {
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());

        // some occasions to return
        const int L = param_id / 2; // TODO: improve
        bool IF_find_layer_name = false;
        std::map<string,int>::iterator it;
        string layer_name;
        for (it = APP::layer_index.begin(); it != APP::layer_index.end(); ++it) {
            if (it->second == L) {
                IF_find_layer_name = true;
                layer_name = it->first;
                break;
            }
        }
        if (!IF_find_layer_name) { return; }
        if (APP::iter_prune_finished[L] != INT_MAX) { return; }
        const vector<int>& shape = net_params[param_id]->shape();
        if (shape.size() != 4) { return; } // do not reg biases
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const Dtype col_reg = APP::col_reg;
        const int count = net_params[param_id]->count();
        const int num_row = net_params[param_id]->shape()[0];
        const int num_col = count / num_row;
      
        Dtype* sqrted_energy = (Dtype*) malloc (sizeof(Dtype*) * count); // demoninator of SSL reg
        for (int j = 0; j < num_col; ++j) {
          Dtype sum = 0;
          for (int i = 0; i < num_row; ++i) { 
            sum += weight[i * num_col + j] * weight[i * num_col + j]; 
          } 
          for (int i = 0; i < num_row; ++i) { 
            sqrted_energy[i * num_col + j] = (sum == 0) ? 1 : sqrt(sum); 
          }
          
          if (j < NUM_SHOW) {
            const string mark = (j < 9) ? "c " : "c";
            cout << layer_name << "-" << mark << j+1 << ": " << 1 / sqrted_energy[j] << endl;
          }
        }
        const Dtype* sqrted_energy_const =  sqrted_energy;
          
        // add SSL reg
        Dtype* scaled_weight = (Dtype*) malloc (sizeof(Dtype*) * count);
        caffe_div(count, 
                  net_params[param_id]->cpu_data(), 
                  sqrted_energy_const, 
                  scaled_weight); // degug here
        const Dtype* scaled_weight_const = scaled_weight;
        
        caffe_axpy(count, 
                   col_reg, 
                   scaled_weight_const, 
                   net_params[param_id]->mutable_cpu_diff()); // degug here
        
        free(scaled_weight);
        free(sqrted_energy);

      // ******************************************************************************************
      } else if (regularization_type == "SelectiveReg") {
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // if return 
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
        
        #ifdef ShowTimingLog
        cout << layer_name << " Reg_Col start timing" << endl;
        clock_t t1 = clock();
        #endif

        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        const int num_row = net_params[param_id]->shape()[0];
        const int num_col = count / num_row;
        const int num_col_to_prune = ceil(num_col * APP::prune_ratio[L]);
        const int num_pruned_col = APP::num_pruned_col[L];
        if (num_pruned_col >= num_col_to_prune) { return; }
        
        if (APP::prune_interval == 0) {
            cout << "Wrong: prune_interval not set, please check" << endl;
            exit(1);
        }
        if (APP::step_ % APP::prune_interval == 0) {
            // ***********************************************************
            // Sort 01: sort by L1-norm
            typedef std::pair<Dtype, int> mypair;
            vector<mypair> col_score(num_col);
            for (int j = 0; j < num_col; ++j) { //j: column number
                col_score[j].second = j;
                if (APP::IF_col_pruned[L][j][0]) {
                    col_score[j].first = -1; //APP::history_rank[L][j]; // make the pruned sink down
                    continue;
                }
                col_score[j].first = 0;
                for (int i = 0; i < num_row; ++i) {
                    col_score[j].first += fabs(weight[i * num_col + j]); //How to speedup??
                }
            }
            sort(col_score.begin(), col_score.end());
            #ifdef ShowTimingLog
            cout  << "  after 1st sort: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
            #endif
            
            // Make new criteria by rank: history_rank
            const int n = this->iter_ + 1; // No.n iter (n starts from 1)
            for (int j = 0; j < num_col; ++j) { // j: rank
                const int col_of_rank_j = col_score[j].second;
                if (APP::IF_col_pruned[L][col_of_rank_j][0]) { continue; }
                APP::history_rank[L][col_of_rank_j] = ((n-1) * APP::history_rank[L][col_of_rank_j] + j) / n;
            }
            
            // ***********************************************************
            // Sort 02: sort by history_rank
            vector<mypair> col_history_rank(num_col); // the history_rank of each column, history_rank is like the new score
            for (int j = 0; j < num_col; ++j) { // j: column number
                col_history_rank[j].first  = APP::history_rank[L][j];
                col_history_rank[j].second = j;
                if (APP::IF_col_pruned[L][j][0]) { 
                    col_history_rank[j].first = APP::history_rank[L][j]; // make the pruned sink down
                } 
            }
            sort(col_history_rank.begin(), col_history_rank.end());
            
            #ifdef ShowTimingLog
            cout  << "  after 2nd sort: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
            #endif

            // Print: Check rank, j is column number --------------------
            // char iter[10];
            // sprintf(iter, "%6d", this->iter_ + 1); // max_iter should be in [0, 999999]
            // cout << iter << "-" << layer_name << "hrank:";
            // for (int j = 0; j < num_col; ++j) { // j: column number
                // cout << "  ";
                // char s[50];
                // if (APP::IF_col_pruned[L][j][0]) { 
                    // sprintf(s, "%7.0f", APP::history_rank[L][j]);
                // } else {
                    // sprintf(s, "%7.2f", APP::history_rank[L][j]);
                // }
                // cout << s;
            // }
            // cout << "\n";
            
            // cout << iter << "-" << layer_name << "rank(by_hrank):";
            // for (int j = 0; j < num_col; ++j) { // j: rank
                // cout << "  ";
                // char s[50];
                // const int prune_mark = APP::IF_col_pruned[L][col_history_rank[j].second][0] ? 0 : 1;
                // sprintf(s, "%4d-%d", col_history_rank[j].second, prune_mark);
                // cout << s;
            // }
            // cout << "\n";
            // -----------------------------------------------------------
            
        
            // compute reg multiplier for those "bad" columns, "good" columns are spared with zero reg.
            const Dtype AA = APP::AA;
            const Dtype kk = APP::kk;
            const Dtype alpha = log(2/kk) / (num_col_to_prune - num_pruned_col + 1);
            const Dtype N1 = -log(kk)/alpha;
            Dtype Delta = 0, old_reg = 0, new_reg = 0;
            
            Dtype* muhistory_reg_ = history_reg_[param_id]->mutable_cpu_data();
            for (int j = 0; j < num_col - num_pruned_col; ++j) { // j: rank 
                const int col_of_rank_j = col_history_rank[j + num_pruned_col].second; // Note the real rank is j + num_pruned_col
                Delta = j < N1 ? AA * exp(-alpha * j) : -AA * exp(-alpha * (2*N1-j)) + 2*kk*AA;
                old_reg = APP::history_reg[L][col_of_rank_j];
                new_reg = std::max(old_reg + Delta, Dtype(0));
                APP::history_reg[L][col_of_rank_j] = new_reg;
                for (int i = 0; i < num_row; ++i) {
                    muhistory_reg_[i * num_col + col_of_rank_j] = new_reg;
                }
            }
            // cudaMemcpy(muhistory_reg_, this->history_reg2, sizeof(Dtype) * count, cudaMemcpyHostToDevice);
            
        }
        #ifdef ShowTimingLog
        cout  << "  after calculate reg term: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif
        
        /*
        //Check reg
        if (APP::step_ % 1 == 0) {
            std::cout << layer_name << "  selective regs: " << std::endl;
            for (int j = 0; j < APP::show_num_weight; ++j) {
                const string mark = j < 9 ? " c" : "c";
                cout << mark << j+1 << ":    " << history_reg_[param_id]->cpu_data()[j] << endl;
            }
        }
        */
                
        //Apply Reg
        caffe_gpu_mul(count,
                      net_params[param_id]->gpu_data(),
                      history_reg_[param_id]->gpu_data(),
                      tmp_[param_id]->mutable_gpu_data());
        #ifdef ShowTimingLog
        cout << "  after gpu mul: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif
        
        caffe_gpu_add(count,
                      tmp_[param_id]->gpu_data(),
                      net_params[param_id]->gpu_diff(),
                      net_params[param_id]->mutable_gpu_diff()); 
        #ifdef ShowTimingLog
        cout << "  after gpu add, end of Reg_Col: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif
        
      } else if (regularization_type == "Reg_Weight") {
        // SR used to compress large DNN, not using ranking 
        // add weight decay, weight decay still used
        caffe_gpu_axpy(net_params[param_id]->count(),
                       local_decay,
                       net_params[param_id]->gpu_data(),
                       net_params[param_id]->mutable_gpu_diff());    
        
        
        // If return
        const string layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
        #ifdef ShowTimingLog
        cout << layer_name << " Reg_Weight start timing" << endl;
        clock_t t1 = clock();
        #endif
        
        const Dtype* weight = net_params[param_id]->cpu_data();
        const int count = net_params[param_id]->count();
        
        if (APP::step_ % APP::prune_interval == 0) {
            // estimate threhold score
            const Dtype score_min = 0;
            Dtype score_max = -1;
            for (int i = 0; i < count; ++i) {
                if (fabs(weight[i]) > score_max) {
                    score_max = fabs(weight[i]);
                }
            }
            cout << endl;
            #ifdef ShowTimingLog
            cout  << "  after calculate score max/min: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
            #endif
            
            const Dtype u = (score_max + score_min) / 2; // mean
            const Dtype sigma = (score_max - score_min) / 8; //stddev assumption: all weights are included in 4-sigma scope
            const Dtype prune_ratio = (APP::prune_ratio[L] < 0.5) ? 1 - APP::prune_ratio[L] : APP::prune_ratio[L]; // the lookup table only contains half of the normal distribution
            const Dtype normalized_prune_ratio = round(prune_ratio / 0.05) * 0.05; // e.g. 0.63 -> 0.65; 0.05 is the prune ratio step 
            const int index = int((normalized_prune_ratio - 0.5) / 0.05);
            const Dtype normal_lookup_table[10] = {0, 0.125, 0.255, 0.385, 0.525, 0.675, 0.845, 1.035, 1.285, 1.645};
            const Dtype score_thr = APP::prune_ratio[L] > 0.5
                                        ? u + normal_lookup_table[index] * sigma
                                        : u - normal_lookup_table[index] * sigma;
            if (score_thr > score_max || score_thr < score_min) {
                cout << "Wrong: score_thr in Reg_Weight is out of range." << endl;
                exit(1);
            }
            cout << layer_name << " u=" << u << " sigma=" << sigma << " | score_thr=" << score_thr << " score_max=" << score_max << " score_min=" << score_min << endl;

            //Assign Delta
            const Dtype AA = APP::AA;
            const Dtype k1 = AA / (score_thr - score_min);
            const Dtype k2 = AA / (score_max - score_thr);
            Dtype* muhistory_reg_ = history_reg_[param_id]->mutable_cpu_data();
            for (int i = 0; i < count; ++i) {
                const Dtype Delta = fabs(weight[i]) < score_thr 
                                        ? AA - k1 * (fabs(weight[i]) - score_min)
                                        : k2 * (score_thr - fabs(weight[i]));
                muhistory_reg_[i] = max(muhistory_reg_[i] + Delta, Dtype(0));
            }
        }
        #ifdef ShowTimingLog
        cout  << "  after calculate reg term: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif
        
        
        //Apply Reg
        caffe_gpu_mul(count,
                      net_params[param_id]->gpu_data(),
                      history_reg_[param_id]->gpu_data(),
                      tmp_[param_id]->mutable_gpu_data());
        #ifdef ShowTimingLog
        cout << "  after gpu mul: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif
        
        caffe_gpu_add(count,
                      tmp_[param_id]->gpu_data(),
                      net_params[param_id]->gpu_diff(),
                      net_params[param_id]->mutable_gpu_diff()); 
        #ifdef ShowTimingLog
        cout << "  after gpu add, end of Reg_Weight: " << (double)(clock() - t1)/CLOCKS_PER_SEC << "s" << endl;
        #endif

      } else if (regularization_type == "ForcePositiveWeight") {
        caffe_gpu_axpy(net_params[param_id]->count(),
            local_decay,
            net_params[param_id]->gpu_data(),
            net_params[param_id]->mutable_gpu_diff());
            
        const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
        const int L = GetLayerIndex(param_id);
        if (L == -1) { return; }
            
        const Dtype* weight = net_params[param_id]->cpu_data();
        Dtype* muhistory_reg_ = history_reg_[param_id]->mutable_cpu_data();
        const int count = net_params[param_id]->count();
        
        if (history_reg_[param_id]->count() != APP::history_reg[L].size()) {
            cout << "Wrong: reg size does not equal! layer: " << layer_name << ", its layer_index: " << L << endl;
            cout << history_reg_[param_id]->count() << " " << count << " " << APP::history_reg[L].size() << endl;
            exit(1);
        }
        Dtype local_reg_multiplier = 1.0;
        if (L == 0) { 
            local_reg_multiplier *= 4; 
        }
        for (int i = 0; i < count; ++i) {
            if (weight[i] < 0) {
                muhistory_reg_[i] = local_reg_multiplier * min((Dtype)APP::AA, (Dtype)(APP::step_ * 5e-5));
            } else if (weight[i] > 0) {
                muhistory_reg_[i] = -1.0* min((Dtype)0.1 * local_decay / weight[i], (Dtype)10) ;
                // if (i < 20) { cout << muhistory_reg_[i] << " "; }
            } else {
                muhistory_reg_[i] = 0;
            }
            APP::history_reg[L][i] = muhistory_reg_[i];
        }
        //cout << endl;
        
        // Apply Reg
        caffe_gpu_mul(count,
                      net_params[param_id]->gpu_data(),
                      history_reg_[param_id]->gpu_data(),
                      tmp_[param_id]->mutable_gpu_data());
        
        caffe_gpu_add(count,
                      tmp_[param_id]->gpu_data(),
                      net_params[param_id]->gpu_diff(),
                      net_params[param_id]->mutable_gpu_diff());          
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


// template <typename Dtype>
// void SGDSolver<Dtype>::ClearHistory(const int& L) {
    // const int L = APP<Dtype>::layer_index[layer_name];
    // const int count = APP<Dtype>::masks[L].size();
    // Dtype* tmp = new Dtype[count]; /// TODEBUG: Why cannot use bool?
    // for (int k = 0; k < count; ++k) {
        // tmp[k] = APP<Dtype>::masks[L][k];
    // }
    // caffe_mul(count, 
              // (const Dtype*) tmp, 
              // history_[param_id]->cpu_data(), 
              // history_[param_id]->mutable_cpu_data());
    // delete[] tmp;
// }


template <typename Dtype>
void SGDSolver<Dtype>::ClearHistory() {
    const vector<shared_ptr<Layer<Dtype> > >& layers = this->net_->layers();
    int param_id = 0;
    for (int i = 0; i < layers.size(); ++i) {
        // As long as layer i has masks, its history_ should be cleared. 
        // But only clear history_ of weights, since we only have masks for weights.
        // So the key is to relate layer i with corresponding param_id.
        const string layer_name = layers[i]->layer_param().name();
        if (APP::layer_index.count(layer_name)) {
            const int L = APP::layer_index[layer_name];
            const int count = APP::masks[L].size();
            while (history_[param_id]->count() != count) { 
                ++ param_id; // jump over biases. Note this may be at risk!! Equal count doesn't mean the same thing.
            }
            for (int i = 0; i < count; ++i) {
                if (!APP::masks[L][i]) {
                    history_[param_id]->mutable_cpu_data()[i] = 0;
                }
            }
            ++ param_id;
        }
    }
}


template <typename Dtype>
int SGDSolver<Dtype>::GetLayerIndex(const int& param_id) {
    // Three occasions to return, `-1` means return
    // 1. Get layer index and layer name, if not registered, don't reg it.
    const string& layer_name = this->net_->layer_names()[this->net_->param_layer_indices()[param_id].first];
    if (APP::layer_index.count(layer_name) == 0) { return -1; }
    const int L = APP::layer_index[layer_name];
    
    // 2.
    const bool IF_want_prune  = APP::prune_method != "None" && APP::prune_ratio[L] > 0;
    const bool IF_been_pruned = APP::pruned_ratio[L] > 0;
    const bool IF_enough_iter = APP::step_ >= APP::prune_begin_iter+1;
    const bool IF_prune = IF_want_prune && (IF_been_pruned || IF_enough_iter);
    if (!(IF_prune && APP::iter_prune_finished[L] == INT_MAX)) { return -1; }
    
    // 3. Do not reg biases
    const vector<int>& shape = this->net_->learnable_params()[param_id]->shape();
    if (shape.size() == 1) { return -1; }
    
    return L;
    
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
