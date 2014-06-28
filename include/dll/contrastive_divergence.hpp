//=======================================================================
// Copyright (c) 2014 Baptiste Wicht
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://opensource.org/licenses/MIT)
//=======================================================================

#ifndef DBN_CONTRASTIVE_DIVERGENCE_HPP
#define DBN_CONTRASTIVE_DIVERGENCE_HPP

#include "assert.hpp"
#include "batch.hpp"
#include "decay_type.hpp"

namespace dll {

template<typename RBM>
struct base_cd_trainer {
    typedef RBM rbm_t;

    static constexpr const auto num_hidden = rbm_t::num_hidden;
    static constexpr const auto num_visible = rbm_t::num_visible;

    typedef typename rbm_t::weight weight;

    //Gradients
    etl::fast_matrix<weight, num_visible, num_hidden> w_grad;
    etl::fast_vector<weight, num_visible> vbias_grad;
    etl::fast_vector<weight, num_hidden> hbias_grad;

    //{{{ Momentum

    //Compute sizes so that collections are empty if Momentum not enabled
    static constexpr const std::size_t num_visible_mom = rbm_t::Momentum ? num_visible : 0;
    static constexpr const std::size_t num_hidden_mom = rbm_t::Momentum ? num_hidden : 0;

    etl::fast_matrix<weight, num_visible_mom, num_hidden_mom> w_inc;
    etl::fast_vector<weight, num_visible_mom> a_inc;
    etl::fast_vector<weight, num_hidden_mom> b_inc;

    //}}} Momentum end

    //{{{ Sparsity

    weight q_old;
    weight q_batch;
    weight q_t;

    //}}} Sparsity end

    template<bool M = rbm_t::Momentum, disable_if_u<M> = detail::dummy>
    base_cd_trainer() : q_old(0.0) {
        static_assert(!rbm_t::Momentum, "This constructor should only be used without momentum support");
    }

    template<bool M = rbm_t::Momentum, enable_if_u<M> = detail::dummy>
    base_cd_trainer() : w_inc(0.0), a_inc(0.0), b_inc(0.0), q_old(0.0) {
        static_assert(rbm_t::Momentum, "This constructor should only be used with momentum support");
    }

    template<typename T1, typename T2, bool M = rbm_t::Momentum, enable_if_u<M> = detail::dummy>
    T2& get_fgrad(T1& , T2& inc){
        return inc;
    }

    template<typename T1, typename T2, bool M = rbm_t::Momentum, disable_if_u<M> = detail::dummy>
    T1& get_fgrad(T1& grad, T2& ){
        return grad;
    }

    void update_weights(RBM& rbm){
        auto learning_rate = rbm.learning_rate;

        //Update momentum gradients
        if(rbm_t::Momentum){
            auto momentum = rbm.momentum;

            w_inc = momentum * w_inc + (1 - momentum) * w_grad;
            a_inc = momentum * a_inc + (1 - momentum) * vbias_grad;
            b_inc = momentum * b_inc + (1 - momentum) * hbias_grad;
        }

        //Penalty to be applied to weights and hidden biases
        weight h_penalty = 0.0;

        //Update sparsity
        if(rbm_t::Sparsity){
            auto decay_rate = rbm.decay_rate;
            auto p = rbm.sparsity_target;
            auto cost = rbm.sparsity_cost;

            q_t = decay_rate * q_old + (1.0 - decay_rate) * q_batch;

            h_penalty = cost * (q_t - p);
        }

        //The final gradients;
        const auto& w_fgrad = get_fgrad(w_grad, w_inc);
        const auto& a_fgrad = get_fgrad(vbias_grad, a_inc);
        const auto& b_fgrad = get_fgrad(hbias_grad, b_inc);

        //Weight decay is applied on biases only on demand
        //Note: According to G. Hinton, Weight Decay should not be applied to
        //biases by default due to their limited number and therefore their weak
        //contribution to overfitting

        //Update weights

        if(rbm_t::Decay == DecayType::L1 || rbm_t::Decay == DecayType::L1_FULL){
            rbm.w += learning_rate * (w_fgrad - rbm.weight_cost * abs(rbm.w) - h_penalty);
        } else if(rbm_t::Decay == DecayType::L2 || rbm_t::Decay == DecayType::L2_FULL){
            rbm.w += learning_rate * (w_fgrad - rbm.weight_cost * rbm.w - h_penalty);
        } else {
            rbm.w += learning_rate * w_fgrad - h_penalty;
        }

        //Update hidden biases

        if(rbm_t::Decay == DecayType::L1_FULL){
            rbm.b += learning_rate * (b_fgrad - rbm.weight_cost * abs(rbm.b) - h_penalty);
        } else if(rbm_t::Decay == DecayType::L2_FULL){
            rbm.b += learning_rate * (b_fgrad - rbm.weight_cost * rbm.b - h_penalty);
        } else {
            rbm.b += learning_rate * b_fgrad - h_penalty;
        }

        //Update visible biases

        if(rbm_t::Decay == DecayType::L1_FULL){
            rbm.a += learning_rate * (a_fgrad - rbm.weight_cost * abs(rbm.a));
        } else if(rbm_t::Decay == DecayType::L2_FULL){
            rbm.a += learning_rate * (a_fgrad - rbm.weight_cost * rbm.a);
        } else {
            rbm.a += learning_rate * a_fgrad;
        }

        //Check for NaN
        nan_check_3(rbm.w, rbm.a, rbm.b);
    }
};

template<std::size_t K, typename RBM>
struct cd_trainer : base_cd_trainer<RBM> {
private:
    static_assert(K > 0, "CD-0 is not a valid training method");

    typedef RBM rbm_t;
    typedef typename rbm_t::weight weight;

    using base_cd_trainer<RBM>::num_visible;
    using base_cd_trainer<RBM>::num_hidden;

    using base_cd_trainer<RBM>::w_grad;
    using base_cd_trainer<RBM>::vbias_grad;
    using base_cd_trainer<RBM>::hbias_grad;

    using base_cd_trainer<RBM>::q_batch;

public:
    cd_trainer() : base_cd_trainer<RBM>() {
        //Nothing else to init here
    }

    template<typename T>
    weight train_batch(const dll::batch<T>& batch, RBM& rbm){
        dll_assert(batch.size() <= static_cast<typename dll::batch<T>::size_type>(rbm_t::BatchSize), "Invalid size");
        dll_assert(batch[0].size() == num_visible, "The size of the training sample must match visible units");

        //Size of a minibatch
        auto n_samples = static_cast<weight>(batch.size());

        //Clear the gradients
        vbias_grad = 0.0;
        hbias_grad = 0.0;
        w_grad = 0.0;

        //Reset mean activation probability if necessary
        if(rbm_t::Sparsity){
            q_batch = 0.0;
        }

        for(auto& items : batch){
            rbm.v1 = items;

            //First step
            rbm.activate_hidden(rbm.h1_a, rbm.h1_s, rbm.v1, rbm.v1);

            //CD-1
            rbm.activate_visible(rbm.h1_a, rbm.h1_s, rbm.v2_a, rbm.v2_s);
            rbm.activate_hidden(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);

            //CD-k
            for(std::size_t k = 1; k < K; ++k){
                rbm.activate_visible(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);
                rbm.activate_hidden(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);
            }

            for(size_t i = 0; i < num_visible; ++i){
                for(size_t j = 0; j < num_hidden; ++j){
                    w_grad(i, j) += rbm.h1_a(j) * rbm.v1(i) - rbm.h2_a(j) * rbm.v2_a(i);
                }
            }

            vbias_grad += rbm.v1 - rbm.v2_a;
            hbias_grad += rbm.h1_a - rbm.h2_a;

            if(rbm_t::Sparsity){
                q_batch += sum(rbm.h2_a);
            }
        }

        //Keep only the mean of the gradients
        w_grad /= n_samples;
        vbias_grad /= n_samples;
        hbias_grad /= n_samples;

        //Compute the mean activation probabilities
        if(rbm_t::Sparsity){
            q_batch /= n_samples * num_hidden;
        }

        nan_check_3(w_grad, vbias_grad, hbias_grad);

        //Update the weights and biases based on the gradients
        this->update_weights(rbm);

        //Compute the reconstruction error

        weight error = 0.0;
        for(size_t i = 0; i < num_visible; ++i){
            error += vbias_grad(i) * vbias_grad(i);
        }
        error = sqrt(error / num_visible);

        return error;
    }
};

template<std::size_t K, typename RBM>
struct persistent_cd_trainer : base_cd_trainer<RBM> {
private:
    static_assert(K > 0, "PCD-0 is not a valid training method");

    typedef RBM rbm_t;
    typedef typename rbm_t::weight weight;

    using base_cd_trainer<RBM>::num_visible;
    using base_cd_trainer<RBM>::num_hidden;

    using base_cd_trainer<RBM>::w_grad;
    using base_cd_trainer<RBM>::vbias_grad;
    using base_cd_trainer<RBM>::hbias_grad;

    using base_cd_trainer<RBM>::q_batch;

    std::vector<etl::fast_vector<weight, num_hidden>> p_h_a;
    std::vector<etl::fast_vector<weight, num_hidden>> p_h_s;

public:
    persistent_cd_trainer() : base_cd_trainer<RBM>() {
        //Nothing else to init here
    }

    template<typename T>
    weight train_batch(const dll::batch<T>& batch, RBM& rbm){
        dll_assert(batch.size() <= static_cast<typename dll::batch<T>::size_type>(rbm_t::BatchSize), "Invalid size");
        dll_assert(batch[0].size() == num_visible, "The size of the training sample must match visible units");

        //Size of a minibatch
        auto n_samples = static_cast<weight>(batch.size());

        //Clear the gradients
        vbias_grad = 0.0;
        hbias_grad = 0.0;
        w_grad = 0.0;

        //Reset mean activation probability if necessary
        if(rbm_t::Sparsity){
            q_batch = 0.0;
        }

        bool init = p_h_a.empty();;
        if(init){
            p_h_a.resize(static_cast<typename dll::batch<T>::size_type>(rbm_t::BatchSize));
            p_h_s.resize(static_cast<typename dll::batch<T>::size_type>(rbm_t::BatchSize));
        }

        for(std::size_t i = 0; i < batch.size(); ++i){
            auto& items = batch[i];

            rbm.v1 = items;

            //First step
            rbm.activate_hidden(rbm.h1_a, rbm.h1_s, rbm.v1, rbm.v1);

            if(init){
                p_h_a[i] = rbm.h1_a;
                p_h_s[i] = rbm.h1_s;
            }

            //CD-1
            rbm.activate_visible(p_h_a[i], p_h_a[i], rbm.v2_a, rbm.v2_s);
            rbm.activate_hidden(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);

            //CD-k
            for(std::size_t k = 1; k < K; ++k){
                rbm.activate_visible(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);
                rbm.activate_hidden(rbm.h2_a, rbm.h2_s, rbm.v2_a, rbm.v2_s);
            }

            p_h_a[i] = rbm.h2_a;
            p_h_s[i] = rbm.h2_s;

            for(size_t i = 0; i < num_visible; ++i){
                for(size_t j = 0; j < num_hidden; ++j){
                    w_grad(i, j) += rbm.h1_a(j) * rbm.v1(i) - rbm.h2_a(j) * rbm.v2_a(i);
                }
            }

            vbias_grad += rbm.v1 - rbm.v2_a;
            hbias_grad += rbm.h1_a - rbm.h2_a;

            if(rbm_t::Sparsity){
                q_batch += sum(rbm.h2_a);
            }
        }

        //Keep only the mean of the gradients
        w_grad /= n_samples;
        vbias_grad /= n_samples;
        hbias_grad /= n_samples;

        //Compute the mean activation probabilities
        if(rbm_t::Sparsity){
            q_batch /= n_samples * num_hidden;
        }

        nan_check_3(w_grad, vbias_grad, hbias_grad);

        //Update the weights and biases based on the gradients
        this->update_weights(rbm);

        //Compute the reconstruction error

        weight error = 0.0;
        for(size_t i = 0; i < num_visible; ++i){
            error += vbias_grad(i) * vbias_grad(i);
        }
        error = sqrt(error / num_visible);

        return error;
    }
};

} //end of dbn namespace

#endif