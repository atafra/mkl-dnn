/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef CPU_JIT_AVX512_CORE_BF16_CONVOLUTION_HPP
#define CPU_JIT_AVX512_CORE_BF16_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "memory_tracking.hpp"
#include "mkldnn_thread.hpp"
#include "utils.hpp"

#include "cpu_barrier.hpp"
#include "cpu_convolution_pd.hpp"
#include "cpu_reducer.hpp"

#include "jit_avx512_core_bf16cvt.hpp"
#include "jit_transpose_src_utils.hpp"
#include "jit_avx512_core_bf16_conv_kernel.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

struct jit_avx512_core_bf16_convolution_fwd_t : public cpu_primitive_t {
    struct pd_t : public cpu_convolution_fwd_pd_t {
        pd_t(engine_t *engine, const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const typename pd_t::base_class *hint_fwd_pd)
            : cpu_convolution_fwd_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_() {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit_bf16:", avx512_core, ""),
                jit_avx512_core_bf16_convolution_fwd_t);

        status_t init()
        {
            bool ok = true
                    && mayiuse(avx512_core)
                    && is_fwd()
                    && set_default_alg_kind(alg_kind::convolution_direct)
                    && (expect_data_types(data_type::bf16, data_type::bf16,
                               data_type::undef, data_type::bf16, data_type::undef)
                         || expect_data_types(data_type::bf16, data_type::bf16,
                               data_type::undef, data_type::f32, data_type::undef))
                    && IMPLICATION(with_bias(),
                               utils::one_of(weights_md(1)->data_type,
                                           data_type::f32, data_type::bf16))
                    && !has_zero_dim_memory()
                    && set_default_formats();
            if (!ok)
                return status::unimplemented;

            status_t status = jit_avx512_core_bf16_fwd_kernel::init_conf(
                    jcp_, *desc(), *src_md(), *weights_md(0),
                    *dst_md(), *weights_md(1), *attr(),
                    mkldnn_get_max_threads());
            if (status != status::success) return status::unimplemented;

           auto scratchpad = scratchpad_registry().registrar();
           jit_avx512_core_bf16_fwd_kernel::init_scratchpad(
                   scratchpad, jcp_);

            return status::success;
        }

        jit_conv_conf_t jcp_;

        protected:
            bool set_default_formats() {
                using namespace format_tag;

                auto dat_tag = utils::pick(ndims() - 3, nCw16c, nChw16c, nCdhw16c);
                auto wei_tag = utils::pick(2 * ndims() - 6 + with_groups(),
                        OIw8i16o2i, gOIw8i16o2i, OIhw8i16o2i, gOIhw8i16o2i,
                        OIdhw8i16o2i, gOIdhw8i16o2i);

                return set_default_formats_common(dat_tag, wei_tag, dat_tag);
            }
    };

    jit_avx512_core_bf16_convolution_fwd_t(const pd_t *apd)
        : cpu_primitive_t(apd)
    {
        kernel_ = new jit_avx512_core_bf16_fwd_kernel(pd()->jcp_,
                    *pd()->attr());
    }
    ~jit_avx512_core_bf16_convolution_fwd_t() { delete kernel_;}

    typedef typename prec_traits<data_type::bf16>::type src_data_t;
    typedef typename prec_traits<data_type::bf16>::type wei_data_t;

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        execute_forward(ctx);
        if (pd()->wants_zero_pad_dst())
            ctx.memory(MKLDNN_ARG_DST)->zero_pad();

        return status::success;
    }

private:
    void prepare_padded_bias(const char *&bias,
            const memory_tracking::grantor_t &scratchpad) const;
    void execute_forward(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd(); }

    jit_avx512_core_bf16_fwd_kernel *kernel_;
};

struct jit_avx512_core_bf16_convolution_bwd_data_t: public cpu_primitive_t {
    struct pd_t: public cpu_convolution_bwd_data_pd_t {
        pd_t(engine_t *engine,
                const convolution_desc_t *adesc,
                const primitive_attr_t *attr,
                const convolution_fwd_pd_t *hint_fwd_pd)
            : cpu_convolution_bwd_data_pd_t(engine, adesc, attr, hint_fwd_pd)
            , jcp_()
        {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit_bf16:", avx512_core, ""),
                jit_avx512_core_bf16_convolution_bwd_data_t);

        status_t init() {
            using namespace prop_kind;
            bool ok = true
                && mayiuse(avx512_core)
                && is_bwd_d()
                && set_default_alg_kind(alg_kind::convolution_direct)
                && (expect_data_types(data_type::f32, data_type::bf16,
                        data_type::undef, data_type::bf16, data_type::undef)
                || expect_data_types(data_type::bf16, data_type::bf16,
                        data_type::undef, data_type::bf16, data_type::undef))
                && !has_zero_dim_memory()
                && set_default_formats();
            if (!ok) return status::unimplemented;

            status_t status = jit_avx512_core_bf16_bwd_data_kernel::init_conf(
                    jcp_, *desc(), *diff_src_md(), *weights_md(), *diff_dst_md());
            return status;
        }

        jit_conv_conf_t jcp_;

    protected:
        bool set_default_formats() {
            using namespace format_tag;

            auto dat_tag = utils::pick(ndims() - 3, nCw16c, nChw16c, nCdhw16c);
            auto wei_tag = utils::pick(2 * ndims() - 6 + with_groups(),
                    OIw8o16i2o, gOIw8o16i2o, OIhw8o16i2o, gOIhw8o16i2o,
                    OIdhw8o16i2o, gOIdhw8o16i2o);

            return set_default_formats_common(dat_tag, wei_tag, dat_tag);
        }
    };

    jit_avx512_core_bf16_convolution_bwd_data_t(const pd_t *apd)
        : cpu_primitive_t(apd)
    {
        kernel_ = new jit_avx512_core_bf16_bwd_data_kernel(pd()->jcp_);
    }
    ~jit_avx512_core_bf16_convolution_bwd_data_t() { delete kernel_; };

    typedef typename prec_traits<data_type::bf16>::type diff_dst_data_t;
    typedef typename prec_traits<data_type::bf16>::type wei_data_t;

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        execute_backward_data(ctx);
        return status::success;
    }

private:
    void execute_backward_data(const exec_ctx_t &ctx) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd(); }
    jit_avx512_core_bf16_bwd_data_kernel *kernel_;
};

}
}
}
#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s