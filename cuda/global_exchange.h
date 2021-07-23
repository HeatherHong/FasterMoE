#include "stream_manager.h"
#ifdef FMOE_USE_NCCL
#include <vector>
#include <torch/extension.h>

void fmoe_cuda_expert_exchange_impl(
        const long* local_expert_count, 
        long* global_expert_count, 
        int n_expert, int world_size,
        CudaStreamManager* smgr) {
    NCCL_SAFE_CALL(ncclGroupStart());
    for (int i = 0; i < world_size; ++i) {
        NCCL_SAFE_CALL(ncclSend(
                local_expert_count + n_expert * i,
                n_expert,
                ncclInt64,
                i,
                smgr->ncclcomm,
                smgr->stream(0)));
        NCCL_SAFE_CALL(ncclRecv(
                global_expert_count + n_expert * i,
                n_expert,
                ncclInt64,
                i,
                smgr->ncclcomm,
                smgr->stream(0)));
    }
    NCCL_SAFE_CALL(ncclGroupEnd());
    smgr->sync(1);
}

template<typename scalar_t>
void fmoe_cuda_global_scatter_impl(
    const scalar_t* local_input_buf,
    const long* local_expert_count,
    const long* global_expert_count,
    scalar_t* input_buf,
    const bool * sent_models,
    const bool * stored_models,
    size_t in_feat, size_t n_expert, size_t world_size,
    CudaStreamManager* smgr) {
    // assert world_size > 1
    int recv_ptr = 0;
    /* TODO: may save for backward */
    long *expert_ptr = new long[n_expert * world_size];
    long *recv_expert_ptr = new long[n_expert * world_size];
    
    expert_ptr[0] = 0;
    recv_expert_ptr[0] = 0;
    for (size_t i = 1; i < n_expert * world_size; ++i) {
        expert_ptr[i] = expert_ptr[i - 1] + local_expert_count[i - 1];
        recv_expert_ptr[i] = recv_expert_ptr[i - 1] + (stored_models[i - 1] ? 0 : global_expert_count[i - 1]);
    }

    std::cout << "Scatter ";
    for (size_t i = 0; i < n_expert * world_size; i++)
        std::cout << recv_expert_ptr[i] << " ";
    std::cout << std::endl;

    for (size_t i = 0; i < n_expert; ++i) {
        NCCL_SAFE_CALL(ncclGroupStart());
        for (size_t j = 0; j < world_size; ++j) {
            int idx = i + j * n_expert;
            if (local_expert_count[idx]) {
                // model fetched from other node
                if (stored_models[idx]) {
                    checkCudaErrors(cudaMemcpyAsync(
                        input_buf + recv_ptr * in_feat,
                        local_input_buf + expert_ptr[idx] * in_feat,
                        local_expert_count[idx] * in_feat * sizeof(scalar_t),
                        cudaMemcpyDeviceToDevice,
                        smgr->stream(1)));
                    printf("Local idx=%d size=%d recv_ptr=%d expert_ptr=%d\n", idx, local_expert_count[idx], recv_ptr, expert_ptr[idx]);
                    recv_ptr += local_expert_count[idx];
                } else {
                    NCCL_SAFE_CALL(ncclSend(
                        local_input_buf + expert_ptr[idx] * in_feat, 
                        local_expert_count[idx] * in_feat * sizeof(scalar_t),
                        ncclChar, 
                        j,
                        smgr->ncclcomm,
                        smgr->stream(0)));
                    printf("Send idx=%d size=%d expert_ptr=%d\n", idx, local_expert_count[idx], expert_ptr[idx]);
                }
            }
            if (global_expert_count[idx] && !sent_models[i]) {
                NCCL_SAFE_CALL(ncclRecv(
                        input_buf + recv_ptr * in_feat,
                        global_expert_count[idx] * in_feat * sizeof(scalar_t),
                        ncclChar,
                        j,
                        smgr->ncclcomm,
                        smgr->stream(0)));
                printf("Recv idx=%d size=%d recv_ptr=%d\n", idx, global_expert_count[idx], recv_ptr);
                recv_ptr += global_expert_count[idx];
            }
        }
        NCCL_SAFE_CALL(ncclGroupEnd());
    }
    delete [] expert_ptr;
    smgr->sync(2);
}

template<typename scalar_t>
void fmoe_cuda_global_gather_impl(
    const scalar_t* output_buf,
    const long* local_expert_count,
    const long* global_expert_count,
    scalar_t* local_output_buf,
    const bool * sent_models,
    const bool * stored_models,
    size_t out_feat, size_t n_expert, size_t world_size,
    CudaStreamManager* smgr) {
    long send_ptr = 0;
    /* TODO: may save for backward */
    long *expert_ptr = new long[n_expert * world_size];
    long *recv_expert_ptr = new long[n_expert * world_size];
    
    expert_ptr[0] = 0;
    recv_expert_ptr[0] = 0;
    for (size_t i = 1; i < n_expert * world_size; ++i) {
        expert_ptr[i] = expert_ptr[i - 1] + local_expert_count[i - 1];
        recv_expert_ptr[i] = recv_expert_ptr[i - 1] + (stored_models[i - 1] ? 0 : global_expert_count[i - 1]);
    }

    std::cout << "Gather ";
    for (size_t i = 0; i < n_expert * world_size; i++)
        std::cout << recv_expert_ptr[i] << " ";
    std::cout << std::endl;

    for (size_t i = 0; i < n_expert; ++i) {
        NCCL_SAFE_CALL(ncclGroupStart());
        for (size_t j = 0; j < world_size; ++j) {
            int idx = i + j * n_expert;
            if (local_expert_count[idx]) {
                if (stored_models[idx]) {
                    checkCudaErrors(cudaMemcpyAsync(
                        local_output_buf + expert_ptr[idx] * out_feat,
                        output_buf + send_ptr * out_feat,
                        local_expert_count[idx] * out_feat * sizeof(scalar_t),
                        cudaMemcpyDeviceToDevice,
                        smgr->stream(1)));
                    printf("Local idx=%d size=%d send_ptr=%d expert_ptr=%d\n", idx, local_expert_count[idx], send_ptr, expert_ptr[idx]);
                    send_ptr += local_expert_count[idx];
                } else{
                    NCCL_SAFE_CALL(ncclRecv(
                        local_output_buf + expert_ptr[idx] * out_feat, 
                        local_expert_count[idx] * out_feat * sizeof(scalar_t),
                        ncclChar, 
                        j,
                        smgr->ncclcomm,
                        smgr->stream(0)));
                    printf("Recv idx=%d size=%d expert_ptr=%d\n", idx, local_expert_count[idx], expert_ptr[idx]);   
                }
            }

            if (global_expert_count[idx] && !sent_models[i]) {
                NCCL_SAFE_CALL(ncclSend(
                    output_buf + send_ptr * out_feat,
                    global_expert_count[idx] * out_feat * sizeof(scalar_t),
                    ncclChar,
                    j,
                    smgr->ncclcomm,
                    smgr->stream(0)));
                printf("Send idx=%d size=%d send_ptr=%d\n", idx, global_expert_count[idx], send_ptr);
                send_ptr += global_expert_count[idx];
            }
        }
        NCCL_SAFE_CALL(ncclGroupEnd());
    }
    delete [] expert_ptr;
    smgr->sync(2);
}

void fmoe_cuda_exchange_cache_info_impl(
    bool * sent_models,
    bool * stored_models,
    long num_expert,
    long world_size,
    CudaStreamManager * smgr) {

    // exchange broadcast information
    NCCL_SAFE_CALL(ncclAllGather(
        sent_models,
        stored_models,
        num_expert,
        ncclChar,
        smgr->ncclcomm,
        smgr->stream(0)));

    smgr->sync(1);
}

template<typename scalar_t>
void fmoe_cuda_model_exchange_impl(
    bool * sent_models,
    bool * stored_models,
    std::vector<torch::Tensor> local_params,
    std::vector<std::vector<torch::Tensor>> params,
    long num_expert,
    long world_size,
    CudaStreamManager * smgr) {
    
    int rank;
    NCCL_SAFE_CALL(ncclCommUserRank(smgr->ncclcomm, &rank));

    // send broadcasted data
    for (size_t i = 0; i < num_expert; i++) {
        for (size_t j = 0; j < world_size; j++) {
            auto idx = i + j * num_expert;
            if (stored_models[idx]) {
                scalar_t * param = j == rank ? local_params[i].data_ptr<scalar_t>() : params[j][i].data_ptr<scalar_t>();
                // std::cout << "broadcast idx " << idx << std::endl;
                NCCL_SAFE_CALL(ncclBroadcast(
                    param,
                    param,
                    local_params[i].numel() * sizeof(scalar_t),
                    ncclChar,
                    j,
                    smgr->ncclcomm,
                    smgr->stream(0)));
            }
        }
    }

    smgr->sync(1);
    // printf("Model exchange: Sent %ld Received %ld\n", amount_sent, amount_received);
}


template<typename scalar_t>
void fmoe_cuda_gradient_exchange_impl(
    bool * sent_models, 
    bool * stored_models,
    long * expert_counts,
    std::vector<torch::Tensor> local_grads, 
    std::vector<std::vector<torch::Tensor>> grads, 
    long num_expert, 
    long world_size,
    CudaStreamManager * smgr) {

    int rank;
    NCCL_SAFE_CALL(ncclCommUserRank(smgr->ncclcomm, &rank));
    size_t amount_sent = 0, amount_received = 0;

    // send back broadcasted data
    for (size_t i = 0; i < num_expert; i++) {
        for (size_t j = 0; j < world_size; j++) {
            auto idx = i + j * num_expert;
            if (stored_models[idx]) {
                scalar_t * param = j == rank ? local_grads[i].data_ptr<scalar_t>() : grads[j][i].data_ptr<scalar_t>();

                auto size = local_grads[i].numel() * sizeof(scalar_t);
                
                NCCL_SAFE_CALL(ncclReduce(
                    param,
                    param,
                    size,
                    ncclChar,
                    ncclSum,
                    j,
                    smgr->ncclcomm,
                    smgr->stream(0)));
            }
        }
    }

    smgr->sync(1);
    checkCudaErrors(cudaGetLastError());
}

#endif  // FMOE_USE_NCCL
