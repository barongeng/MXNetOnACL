/*!
 * Copyright (c) 2016 by Contributors
 * \file acl_convolution-inl.h
 * \brief
 * \author Joey
*/
#ifndef MXNET_OPERATOR_ACL_CONVOLUTION_INL_H_
#define MXNET_OPERATOR_ACL_CONVOLUTION_INL_H_

#if USE_ACL == 1
#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <mxnet/operator.h>
#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include "../convolution-inl.h"
#include "acl_layer.h"

namespace mxnet {
namespace op {

template<typename xpu, typename DType>
class ACLConvolutionOp : public ConvolutionOp<xpu, DType>,public ACLBaseLayer<arm_compute::CLConvolutionLayer,arm_compute::NEConvolutionLayer> {
 private:
  ConvolutionParam param_;
  Context ctx_;
  bool is_gpu_;

  void SetupACLLayer(const OpContext &ctx, const std::vector<TBlob> &in_data,
                     const std::vector<OpReqType> &req,
                     const std::vector<TBlob> &out_data,
                     const std::vector<TBlob> &aux_args){

      const TShape& ishape=in_data[conv::kData].shape_;
      const TShape& oshape=out_data[conv::kOut].shape_;
      arm_compute::TensorShape input_shape((unsigned int)ishape[3],(unsigned int)ishape[2], (unsigned int)ishape[1],(unsigned int)ishape[0]); //wxhxchxnum
      checkreshape(input_shape,is_gpu_);
      if (!this->init_layer_) return;
      this->init_layer_=false;
    // Initialize ACL.
      if (is_gpu_) {
          new_gpulayer();
      }else{
          new_cpulayer();
      }
      this->force_bypass_acl_path_=false;

      TShape stride;
      TShape dilate;
      TShape pad;
      TShape kernel_shape;
      if (param_.kernel.ndim() == 1) {
        stride[1]=stride[0]=param_.stride[0];
        dilate[1]=dilate[0]=param_.dilate[0];
        pad[1]=pad[0]=param_.pad[0];
        kernel_shape[1]=kernel_shape[0]=param_.kernel[0];
      } else if (param_.kernel.ndim() == 2) {
        stride[1]=param_.stride[0];stride[0]=param_.stride[1];
        dilate[1]=param_.dilate[0];dilate[0]=param_.dilate[1];
        pad[1]=param_.pad[0];pad[0]=param_.pad[1];
        kernel_shape[1]=param_.kernel[0];kernel_shape[0]=param_.kernel[1];
      } 

      int stride_x =stride[1];
      int stride_y =stride[0];
      int pad_x=pad[1];
      int pad_y=pad[0];
      unsigned int channels = ishape[1];
      unsigned int num_output=oshape[1];
      unsigned int kernel_x=kernel_shape[1];
      unsigned int kernel_y=kernel_shape[0];
      arm_compute::PadStrideInfo conv_info(stride_x,stride_y,pad_x,pad_y);
      arm_compute::TensorShape weights_shape(kernel_x,kernel_y,channels, num_output);
      arm_compute::TensorShape biases_shape (num_output);
      arm_compute::TensorShape output_shape((unsigned int)oshape[3],(unsigned int)oshape[2], (unsigned int)oshape[1],(unsigned int)oshape[0]);//wxhxchxnum
      DType * input_data =in_data[conv::kData].dptr<DType>();
      DType * output_data =out_data[conv::kOut].dptr<DType>();
      DType * weithts_data=in_data[conv::kWeight].dptr<DType>();
      DType * bias_data;
      if (!param_.no_bias) 
          bias_data=in_data[conv::kBias].dptr<DType>();

      if (is_gpu_) {
          //[kernel_x, kernel_y, IFM, OFM]
          this->gpu().weights=new_tensor<GPUTensor>(weights_shape,(void*)weithts_data);
          tensor_mem(this->gpu().weights,(void*)weithts_data);
          //[OFM]
          if (!param_.no_bias) {
              this->gpu().biases=new_tensor<GPUTensor>(biases_shape,(void*)bias_data);
              tensor_mem(this->gpu().biases,(void*)bias_data);
          }

          //[width, height, IFM]
          this->gpu().input=new_tensor<GPUTensor>(input_shape,(void*)input_data);
          //[width, height, OFM]
          this->gpu().output=new_tensor<GPUTensor>(output_shape,(void*)output_data);
          this->gpu().layer->configure(this->gpu().input,this->gpu().weights,this->gpu().biases,this->gpu().output,conv_info);
      }else{
          //[kernel_x, kernel_y, IFM, OFM]
          this->cpu().weights=new_tensor<CPUTensor>(weights_shape,(void*)weithts_data);
          tensor_mem(this->cpu().weights,(void*)weithts_data);
          //[OFM]
          if (!param_.no_bias) {
              this->cpu().biases=new_tensor<CPUTensor>(biases_shape,(void*)bias_data);
              tensor_mem(this->cpu().biases,(void*)bias_data);
          }

          //[width, height, IFM]
          this->cpu().input=new_tensor<CPUTensor>(input_shape,(void*)input_data);
          //[width, height, OFM]
          this->cpu().output=new_tensor<CPUTensor>(output_shape,(void*)output_data);
          this->cpu().layer->configure(this->cpu().input,this->cpu().weights,this->cpu().biases,this->cpu().output,conv_info);
      }
  }

 public:
  explicit ACLConvolutionOp(Context & ctx,ConvolutionParam p)
      : ConvolutionOp<xpu, DType>(p) {
    this->param_ = p;
    this->ctx_ = ctx;
    this->is_gpu_ = ctx_.arm_gpu_mode();
  }

 public:
  virtual void Forward(const OpContext &ctx, const std::vector<TBlob> &in_data,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &out_data,
                       const std::vector<TBlob> &aux_args) {
      if (this->force_bypass_acl_path_|| param_.num_group !=1){
         ConvolutionOp<xpu, DType>::Forward(ctx,in_data,req,out_data,aux_args);
         return;
      }
      if (param_.kernel.ndim()>2 || param_.stride.ndim() == 0 || param_.pad.ndim() ==0 || param_.dilate.ndim() == 0) {
          ConvolutionOp<xpu, DType>::Forward(ctx,in_data,req,out_data,aux_args);
          return;
      }
      /* check dilation */
      int dilated=0;

      for(unsigned int i=0;i<param_.dilate.ndim();i++)
      {
          if(param_.dilate[i]!=1) 
             dilated=1;
      }

      if(dilated) {
          ConvolutionOp<xpu, DType>::Forward(ctx,in_data,req,out_data,aux_args);
          return;
      }

      DType * input_data =in_data[conv::kData].dptr<DType>();
      DType * output_data =out_data[conv::kOut].dptr<DType>();
      SetupACLLayer(ctx,in_data,req,out_data,aux_args);
      acl_run((void*)input_data,(void*)output_data,is_gpu_);
  }
};  // class ACLConvolutionOp
}  // namespace op
}  // namespace mxnet
#endif
#endif  // MXNET_OPERATOR_ACL_CONVOLUTION_INL_H_
