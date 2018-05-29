 #include "caffe/adaptive_probabilistic_pruning.hpp"

using namespace std;
namespace caffe {
 

    /// --------------------------------
    // 1 Params passed from solver to layer, not initialized here
    string APP::prune_method = "None"; /// initialized for caffe test, which has no solver but this info is still needed in layer.
    string APP::prune_unit;
    string APP::prune_coremthd;
    string APP::criteria;
    int APP::num_once_prune;
    int APP::prune_interval;
    float APP::rgamma;
    float APP::rpower;
    float APP::cgamma;
    float APP::cpower;
    int APP::prune_begin_iter;
    int APP::iter_size;
    float APP::score_decay = 0;
    int APP::reg_cushion_iter = 2000;
    float history_rank_momentum = 0.999;
    float APP::AA;
    float APP::kk;
    float APP::speedup;
    float APP::compRatio;
    bool APP::IF_speedup_count_fc;
    bool APP::IF_compr_count_conv;
    bool APP::IF_update_row_col;
    vector<bool> APP::IF_update_row_col_layer;
    bool APP::IF_eswpf;
    float APP::prune_threshold;
    float APP::target_reg;
    int APP::num_iter_reg;
    
    // 2.1 Info shared among layers
    int APP::inner_iter = 0;
    int APP::step_ = -1;
    map<string, int> APP::layer_index;
    int APP::layer_cnt = 0;
    int APP::conv_layer_cnt = 0;
    int APP::fc_layer_cnt = 0;
    vector<int> APP::filter_area;
    vector<int> APP::group;
    vector<int> APP::priority;
    
    
    // 2.2 Pruning state (key)
    vector<int> APP::num_pruned_weight;
    vector<float> APP::num_pruned_col;
    vector<int>   APP::num_pruned_row;
    vector<int>   APP::pruned_rows; /// used in UpdateNumCol
    vector<vector<bool> > APP::masks;
    vector<vector<bool> > APP::IF_weight_pruned;
    vector<vector<bool> > APP::IF_row_pruned;
    vector<vector<vector<bool> > > APP::IF_col_pruned;
    vector<vector<float> > APP::history_prob;
    vector<vector<float> > APP::history_reg;
    vector<vector<float> > APP::history_score;    
    vector<vector<float> > APP::history_rank;
    vector<vector<float> > APP::hhistory_rank;
    vector<int> APP::iter_prune_finished;
    vector<float> APP::prune_ratio;
    vector<float> APP::delta;
    vector<float> APP::pruned_ratio;
    vector<float> APP::pruned_ratio_col;
    vector<float> APP::pruned_ratio_row;
    vector<float> APP::GFLOPs;
    vector<float> APP::num_param;
    bool APP::IF_speedup_achieved = false;
    bool APP::IF_compRatio_achieved = false;
    bool APP::IF_alpf = false; /// if all layer prune finished
    vector<float> APP::reg_to_distribute;
    int APP::num_bit = 4;
    

    // 3. Logging
    int APP::num_log;
    vector<vector<vector<float> > > APP::log_weight;
    vector<vector<vector<float> > > APP::log_diff;
    vector<vector<int> > APP::log_index;
    string APP::snapshot_prefix;
    string APP::prune_state_dir = "/PruneStateSnapshot/";
    int APP::show_layer = 1; 
    int APP::show_num_layer = 20;
    int APP::show_num_weight = 20; 
    int APP::show_interval = 10;
    long APP::first_time = 0;
    long APP::last_time = 0;
    int APP::first_iter = 0;
    int APP::num_negative = 0;
    /// --------------------------------

    // use window proposal or score decay ----- legacy
    int APP::window_size = 40;
    bool APP::use_score_decay = true;
    float APP::score_decay_rate = 0.88;
    
    // selective reg ----- legacy
    bool APP::use_selective_reg = false; // default is false
    float APP::reg_decay = 0.59;
    
    // the penalty ratio of column regularization
    float APP::col_reg = 0.05; //0.0075; // 0.0008;  
    float APP::diff_reg = 0.00001; 
        
    // Decrease-Weight-Decay ----- legacy
    int APP::max_num_column_to_prune = 0; // If "adaptive" used, this must be provided.
    // When to Prune or Reg etc.
    int APP::when_to_col_reg = 7654321; // when to apply col reg, SSL or SelectiveReg

    // Adaptive SPP
    float APP::loss = 0;
    float APP::loss_decay = 0.7;
    float APP::Delta_loss_history = 0;
    float APP::learning_speed = 0;

}
