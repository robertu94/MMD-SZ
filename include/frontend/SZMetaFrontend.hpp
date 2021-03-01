#ifndef SZ2_FRONT_END
#define SZ2_FRONT_END

#include "SZMetaImpl/meta_quantization.hpp"
#include "SZMetaImpl/meta_prediction.hpp"
#include "SZMetaImpl/meta_optimize_quant_intervals.hpp"
#include "SZMetaImpl/meta_def.hpp"
//#include "SZMetaImpl/meta_utils.hpp"
#include "encoder/HuffmanEncoder.hpp"
#include "utils/MemoryUtil.hpp"
#include <list>

namespace SZ {
    using namespace SZMETA;

    template<class T, uint N, class Quantizer>
    class SZMetaFrontend : public concepts::FrontendInterface<T, N> {
    public:
        SZMetaFrontend(const Config<T, N> &conf, Quantizer quantizer) :
                quantizer(quantizer),
                params(false, conf.block_size, 3, 0, conf.enable_lorenzo, conf.enable_2ndlorenzo,
                       conf.enable_regression, conf.eb),
                precision(conf.eb),
                conf(conf) {
            assert(N == 3 && "SZ2 Front only support 3D compression");
        }

        ~SZMetaFrontend() {
            clear();
        }

        void print() {};


        std::vector<int> compress(T *data) {
            return compress_3d(data);
        };

        T *decompress(std::vector<int> &quant_inds) {
            return decompress_3d(quant_inds);
        };


        void save(uchar *&c) {

            write(params, c);
            write(precision, c);
            write(intv_radius, c);
            write(mean_info, c);
            write(reg_count, c);
            write(unpred_count_buffer, size.block_size * size.block_size, c);
            T *unpred_data_buffer_pos = unpred_data_buffer;
            for (int i = 0; i < size.block_size; i++) {
                for (int j = 0; j < size.block_size; j++) {
                    write(unpred_data_buffer_pos,
                          unpred_count_buffer[i * size.block_size + j], c);
//                    write_array_to_dst(c, unpred_data_buffer_pos,
//                                       unpred_count_buffer[i * size.block_size + j]);
                    unpred_data_buffer_pos += est_unpred_count_per_index;
                }
            }

//            Huffman_encode_tree_and_data(SELECTOR_RADIUS, indicator, size.num_blocks, c);
            HuffmanEncoder<int> selector_encoder = HuffmanEncoder<int>();
            selector_encoder.preprocess_encode(indicator, SELECTOR_RADIUS);
            selector_encoder.save(c);
            selector_encoder.encode(indicator, c);
            selector_encoder.postprocess_encode();

//	convertIntArray2ByteArray_fast_1b_to_result_sz(indicator, size.num_blocks, c);

            if (reg_count) {
                encode_regression_coefficients(reg_params_type, reg_unpredictable_data, RegCoeffNum3d * reg_count,
                                               reg_unpredictable_data_pos - reg_unpredictable_data, c);
            }

        }

        void load(const uchar *&c, size_t &remaining_length) {
            clear();
            const uchar *c_pos = c;

            read(params, c, remaining_length);
            read(precision, c, remaining_length);
            read(intv_radius, c, remaining_length);
            read(mean_info, c, remaining_length);
            read(reg_count, c, remaining_length);

            size_t r1 = conf.dims[0];
            size_t r2 = conf.dims[1];
            size_t r3 = conf.dims[2];
            size = SZMETA::DSize_3d(r1, r2, r3, params.block_size);
            // prepare unpred buffer for vectorization
            est_unpred_count_per_index = size.num_blocks * size.block_size * 1;
            // if(!params.block_independant) est_unpred_count_per_index /= 20;
            unpred_count_buffer = (int *) malloc(size.block_size * size.block_size * sizeof(T));
            read(unpred_count_buffer, size.block_size * size.block_size, c, remaining_length);
            unpred_data_buffer = (T *) malloc(
                    size.block_size * size.block_size * est_unpred_count_per_index * sizeof(T));
            T *unpred_data_buffer_pos = unpred_data_buffer;
            for (int i = 0; i < size.block_size; i++) {
                for (int j = 0; j < size.block_size; j++) {
                    memcpy(unpred_data_buffer_pos, c,
                           unpred_count_buffer[i * size.block_size + j] * sizeof(T));
                    c += unpred_count_buffer[i * size.block_size + j] * sizeof(T);
                    unpred_data_buffer_pos += est_unpred_count_per_index;
                }
            }
            memset(unpred_count_buffer, 0, size.block_size * size.block_size * sizeof(int));
//	unsigned char * indicator = convertByteArray2IntArray_fast_1b_sz(size.num_blocks, c, (size.num_blocks - 1)/8 + 1);
            HuffmanEncoder<int> selector_encoder = HuffmanEncoder<int>();
            selector_encoder.load(c, remaining_length);
            indicator = selector_encoder.decode(c, size.num_blocks);
            selector_encoder.postprocess_decode();


            if (reg_count) {
                reg_params = decode_regression_coefficients(c, reg_count, size.block_size, precision,
                                                            params);
            }
            remaining_length -= c_pos - c;
        }


        void clear() {
            if (reg_params_type != nullptr) {
                free(reg_params_type);
                reg_params_type = nullptr;
            }
            if (reg_unpredictable_data != nullptr) {
                free(reg_unpredictable_data);
                reg_unpredictable_data = nullptr;
            }
            if (unpred_data_buffer != nullptr) {
                free(unpred_data_buffer);
                unpred_data_buffer = nullptr;
            }
            if (unpred_count_buffer != nullptr) {
                free(unpred_count_buffer);
                unpred_count_buffer = nullptr;
            }
            if (reg_params != nullptr) {
                free(reg_params);
                reg_params = nullptr;
            }
        }


        int get_radius() const {
            return capacity;
//            return quantizer.get_radius();
        }

        size_t get_num_elements() const { return size.num_elements; };

    private:
        //        unsigned char *
//        compress_3d(const T *data, size_t r1, size_t r2, size_t r3, double precision, size_t &compressed_size,
//                    const SZMETA::meta_params &params, SZMETA::meta_compress_info &compress_info) {
        std::vector<int> compress_3d(const T *data) {
            clear();

            size_t r1 = conf.dims[0];
            size_t r2 = conf.dims[1];
            size_t r3 = conf.dims[2];
            size = SZMETA::DSize_3d(r1, r2, r3, conf.block_size);

            capacity = 0; // num of quant intervals
            mean_info = optimize_quant_invl_3d(data, r1, r2, r3, conf.eb, capacity);
            if (conf.quant_state_num > 0) {
                capacity = conf.quant_state_num;
            }
            intv_radius = (capacity >> 1);
            std::vector<int> type(size.num_elements);
//            int *type = (int *) malloc(size.num_elements * sizeof(int));
//            indicator = (int *) malloc(size.num_blocks * sizeof(int));
            indicator.resize(size.num_blocks);

            reg_params_type = (int *) malloc(RegCoeffNum3d * size.num_blocks * sizeof(int));
            reg_unpredictable_data = (float *) malloc(RegCoeffNum3d * size.num_blocks * sizeof(float));
            reg_unpredictable_data_pos = reg_unpredictable_data;

            // prepare unpred buffer for vectorization
            est_unpred_count_per_index = size.num_blocks * size.block_size * 1;
            // if(!params.block_independant) est_unpred_count_per_index /= 20;
            unpred_data_buffer = (T *) malloc(
                    size.block_size * size.block_size * est_unpred_count_per_index * sizeof(T));
            unpred_count_buffer = (int *) malloc(size.block_size * size.block_size * sizeof(int));
            memset(unpred_count_buffer, 0, size.block_size * size.block_size * sizeof(int));
//        T precision_t = (T) precision;
            reg_count = 0;
            size_t lorenzo_count = 0;
            size_t lorenzo_2layer_count = 0;

            int *type_pos = type.data();
            int *indicator_pos = indicator.data();

            float *reg_params = (float *) malloc(RegCoeffNum3d * (size.num_blocks + 1) * sizeof(float));
            for (int i = 0; i < RegCoeffNum3d; i++) {
                reg_params[i] = 0;
            }
            float *reg_params_pos = reg_params + RegCoeffNum3d;
            int *reg_params_type_pos = reg_params_type;


            T reg_precisions[RegCoeffNum3d];
            T reg_recip_precisions[RegCoeffNum3d];
            for (int i = 0; i < RegCoeffNum3d - 1; i++) {
                reg_precisions[i] = params.regression_param_eb_linear;
                reg_recip_precisions[i] = 1.0 / reg_precisions[i];
            }
            reg_precisions[RegCoeffNum3d - 1] = params.regression_param_eb_independent;
            reg_recip_precisions[RegCoeffNum3d - 1] = 1.0 / reg_precisions[RegCoeffNum3d - 1];

            // maintain a buffer of (block_size+1)*(r2+1)*(r3+1)
            // 2-layer use_lorenzo
            size_t buffer_dim0_offset =
                    (size.d2 + params.lorenzo_padding_layer) * (size.d3 + params.lorenzo_padding_layer);
            size_t buffer_dim1_offset = size.d3 + params.lorenzo_padding_layer;
            T *pred_buffer = (T *) malloc(
                    (size.block_size + params.lorenzo_padding_layer) * (size.d2 + params.lorenzo_padding_layer) *
                    (size.d3 + params.lorenzo_padding_layer) * sizeof(T));
            memset(pred_buffer, 0,
                   (size.block_size + params.lorenzo_padding_layer) * (size.d2 + params.lorenzo_padding_layer) *
                   (size.d3 + params.lorenzo_padding_layer) * sizeof(T));
            int capacity_lorenzo = mean_info.use_mean ? capacity - 2 : capacity;
            auto *lorenzo_pred_and_quant = compress_lorenzo_3d_predict<T>;
            if (params.prediction_dim == 2) lorenzo_pred_and_quant = compress_lorenzo_3d_as2d_predict<T>;
            else if (params.prediction_dim == 1) lorenzo_pred_and_quant = compress_lorenzo_3d_as1d_predict<T>;
            T recip_precision = (T) 1.0 / conf.eb;

            const T *x_data_pos = data;
            for (size_t i = 0; i < size.num_x; i++) {
                const T *y_data_pos = x_data_pos;
                T *pred_buffer_pos = pred_buffer;
                for (size_t j = 0; j < size.num_y; j++) {
                    const T *z_data_pos = y_data_pos;
                    for (size_t k = 0; k < size.num_z; k++) {
                        int size_x = ((i + 1) * size.block_size < size.d1) ? size.block_size : size.d1 -
                                                                                               i * size.block_size;
                        int size_y = ((j + 1) * size.block_size < size.d2) ? size.block_size : size.d2 -
                                                                                               j * size.block_size;
                        int size_z = ((k + 1) * size.block_size < size.d3) ? size.block_size : size.d3 -
                                                                                               k * size.block_size;
                        int min_size = MIN(size_x, size_y);
                        min_size = MIN(min_size, size_z);

                        bool enable_regression = params.use_regression_linear && min_size >= 2;
//                bool enable_regression = params.use_regression_linear && min_size >= 1;

                        if (enable_regression) {
                            compute_regression_coeffcients_3d(z_data_pos, size_x, size_y, size_z, size.dim0_offset,
                                                              size.dim1_offset,
                                                              reg_params_pos);
                        }

                        int selection_result = meta_blockwise_selection_3d(z_data_pos, mean_info, size.dim0_offset,
                                                                           size.dim1_offset,
                                                                           min_size, conf.eb, reg_params_pos,
                                                                           params.prediction_dim,
                                                                           params.use_lorenzo,
                                                                           params.use_lorenzo_2layer,
                                                                           enable_regression);
                        *indicator_pos = selection_result;
                        if (selection_result == SELECTOR_REGRESSION) {
                            // regression
                            compress_regression_coefficient_3d(RegCoeffNum3d, reg_precisions, reg_recip_precisions,
                                                               reg_params_pos,
                                                               reg_params_type_pos,
                                                               reg_unpredictable_data_pos);
                            compress_regression_3d_predict<T>(z_data_pos, reg_params_pos, pred_buffer_pos, precision,
                                                              recip_precision, capacity, intv_radius,
                                                              size_x, size_y, size_z, buffer_dim0_offset,
                                                              buffer_dim1_offset, size.dim0_offset, size.dim1_offset,
                                                              type_pos, unpred_count_buffer, unpred_data_buffer,
                                                              est_unpred_count_per_index,
                                                              params.lorenzo_padding_layer);
                            reg_count++;
                            reg_params_pos += RegCoeffNum3d;
                            reg_params_type_pos += RegCoeffNum3d;
                        } else {
                            // Lorenzo
                            lorenzo_pred_and_quant(mean_info, z_data_pos, pred_buffer_pos, precision, recip_precision,
                                                   capacity_lorenzo,
                                                   intv_radius,
                                                   size_x, size_y, size_z, buffer_dim0_offset, buffer_dim1_offset,
                                                   size.dim0_offset,
                                                   size.dim1_offset, type_pos, unpred_count_buffer, unpred_data_buffer,
                                                   est_unpred_count_per_index,
                                                   params.lorenzo_padding_layer,
                                                   (selection_result == SELECTOR_LORENZO_2LAYER));
                            if (selection_result == SELECTOR_LORENZO_2LAYER) {
                                lorenzo_2layer_count++;
                            } else {
                                lorenzo_count++;
                            }
                        }
                        pred_buffer_pos += size.block_size;
                        indicator_pos++;
                        z_data_pos += size_z;
                    }
                    y_data_pos += size.block_size * size.dim1_offset;
                    pred_buffer_pos += size.block_size * buffer_dim1_offset - size.block_size * size.num_z;
                }
                // copy bottom of buffer to top of buffer
                memcpy(pred_buffer, pred_buffer + size.block_size * buffer_dim0_offset,
                       params.lorenzo_padding_layer * buffer_dim0_offset * sizeof(T));
                x_data_pos += size.block_size * size.dim0_offset;
            }
            free(pred_buffer);
            free(reg_params);

//    printf("block %ld; lorenzo %ld, lorenzo_2layer %ld, regression %ld, poly regression %ld\n", size.num_blocks,
//        lorenzo_count, lorenzo_2layer_count, reg_count);

            return type;
        }

        //T *
//        meta_decompress_3d(const unsigned char *compressed, size_t r1, size_t r2, size_t r3) {
        T *decompress_3d(std::vector<int> &quant_inds) {

            int *type = quant_inds.data();
            T *dec_data = (T *) malloc(size.num_elements * sizeof(T));
//    dec_data_sp_float = (float *) dec_data;
            const float *reg_params_pos = (const float *) (reg_params + RegCoeffNum3d);;

            const int *type_pos = type;
            const int *indicator_pos = indicator.data();
//        const float *reg_params_pos = reg_params;
            // add one more ghost layer
            size_t buffer_dim0_offset =
                    (size.d2 + params.lorenzo_padding_layer) * (size.d3 + params.lorenzo_padding_layer);
            size_t buffer_dim1_offset = size.d3 + params.lorenzo_padding_layer;
            T *pred_buffer = (T *) malloc(
                    (size.block_size + params.lorenzo_padding_layer) * (size.d2 + params.lorenzo_padding_layer) *
                    (size.d3 + params.lorenzo_padding_layer) * sizeof(T));
            memset(pred_buffer, 0,
                   (size.block_size + params.lorenzo_padding_layer) * (size.d2 + params.lorenzo_padding_layer) *
                   (size.d3 + params.lorenzo_padding_layer) * sizeof(T));
            auto *lorenzo_pred_and_decomp = decompress_lorenzo_3d_prediction<T>;
            if (params.prediction_dim == 2) lorenzo_pred_and_decomp = decompress_lorenzo_3d_as2d_prediction<T>;
            else if (params.prediction_dim == 1) lorenzo_pred_and_decomp = decompress_lorenzo_3d_as1d_prediction<T>;
            T *x_data_pos = dec_data;
            for (size_t i = 0; i < size.num_x; i++) {
                T *y_data_pos = x_data_pos;
                T *pred_buffer_pos = pred_buffer;
                for (size_t j = 0; j < size.num_y; j++) {
                    T *z_data_pos = y_data_pos;
                    for (size_t k = 0; k < size.num_z; k++) {
                        int size_x = ((i + 1) * size.block_size < size.d1) ? size.block_size : size.d1 -
                                                                                               i * size.block_size;
                        int size_y = ((j + 1) * size.block_size < size.d2) ? size.block_size : size.d2 -
                                                                                               j * size.block_size;
                        int size_z = ((k + 1) * size.block_size < size.d3) ? size.block_size : size.d3 -
                                                                                               k * size.block_size;
                        if (*indicator_pos == SELECTOR_REGRESSION) {
                            // regression
                            decompress_regression_3d_prediction<T>(reg_params_pos, pred_buffer_pos, precision,
                                                                   intv_radius,
                                                                   size_x, size_y, size_z, buffer_dim0_offset,
                                                                   buffer_dim1_offset,
                                                                   size.dim0_offset, size.dim1_offset, type_pos,
                                                                   unpred_count_buffer,
                                                                   unpred_data_buffer, est_unpred_count_per_index,
                                                                   z_data_pos,
                                                                   params.lorenzo_padding_layer);
                            reg_params_pos += RegCoeffNum3d;
                        } else {
                            // Lorenzo
                            lorenzo_pred_and_decomp(mean_info, pred_buffer_pos, precision, intv_radius, size_x, size_y,
                                                    size_z,
                                                    buffer_dim0_offset, buffer_dim1_offset, size.dim0_offset,
                                                    size.dim1_offset,
                                                    type_pos,
                                                    unpred_count_buffer, unpred_data_buffer, est_unpred_count_per_index,
                                                    z_data_pos,
                                                    params.lorenzo_padding_layer,
                                                    *indicator_pos == SELECTOR_LORENZO_2LAYER);
                        }
                        pred_buffer_pos += size.block_size;
                        indicator_pos++;
                        z_data_pos += size_z;
                    }
                    y_data_pos += size.block_size * size.dim1_offset;
                    pred_buffer_pos += size.block_size * buffer_dim1_offset - size.block_size * size.num_z;
                }
                memcpy(pred_buffer, pred_buffer + size.block_size * buffer_dim0_offset,
                       params.lorenzo_padding_layer * buffer_dim0_offset * sizeof(T));
                x_data_pos += size.block_size * size.dim0_offset;
            }
            free(pred_buffer);

//            free(unpred_count_buffer);
//            free(unpred_data_buffer);
            return dec_data;
        }

        inline void
        meta_block_error_estimation_3d(const T *data_pos, const float *reg_params_pos,
                                       const meanInfo<T> &mean_info, int x, int y, int z, size_t dim0_offset,
                                       size_t dim1_offset,
                                       T precision, double &err_lorenzo, double &err_lorenzo_2layer, double &err_reg,
                                       const int pred_dim,
                                       const bool use_lorenzo, const bool use_lorenzo_2layer,
                                       const bool use_regression) {
            T noise = 0;
            T noise_2layer = 0;
            const T *cur_data_pos = data_pos + x * dim0_offset + y * dim1_offset + z;
            T cur_data = *cur_data_pos;
            if (use_regression) {
                err_reg += fabs(cur_data - regression_predict_3d<T>(reg_params_pos, x, y, z));
            }
            double lorenzo_predict = 0;
            double lorenzo_2layer_predict = 0;
            if (pred_dim == 3) {
                if (use_lorenzo_2layer) {
                    lorenzo_2layer_predict = lorenzo_predict_3d_2layer(cur_data_pos, dim0_offset, dim1_offset);
                    noise_2layer = Lorenze2LayerNoise3d * precision;
                }
                if (use_lorenzo) {
                    lorenzo_predict = lorenzo_predict_3d(cur_data_pos, dim0_offset, dim1_offset);
                    noise = LorenzeNoise3d * precision;
                }
            } else if (pred_dim == 2) {
                if (use_lorenzo_2layer) {
                    lorenzo_2layer_predict = lorenzo_predict_2d_2layer(cur_data_pos, dim0_offset, dim1_offset);
                    noise_2layer = Lorenze2LayerNoise2d * precision;

                }
                if (use_lorenzo) {
                    lorenzo_predict = lorenzo_predict_2d(cur_data_pos, dim0_offset, dim1_offset);
                    noise = LorenzeNoise2d * precision;
                }
            } else {
                if (use_lorenzo_2layer) {
                    lorenzo_2layer_predict = lorenzo_predict_1d_2layer(cur_data_pos, dim0_offset);
                    noise_2layer = Lorenze2LayerNoise1d * precision;

                }
                if (use_lorenzo) {
                    lorenzo_predict = lorenzo_predict_1d(cur_data_pos, dim0_offset);
                    noise = LorenzeNoise1d * precision;
                }
            }
            err_lorenzo += mean_info.use_mean ? MIN(fabs(cur_data - mean_info.mean),
                                                    fabs(cur_data - lorenzo_predict) + noise) :
                           fabs(cur_data - lorenzo_predict) + noise;
            err_lorenzo_2layer += mean_info.use_mean ? MIN(fabs(cur_data - mean_info.mean),
                                                           fabs(cur_data - lorenzo_2layer_predict) + noise_2layer) :
                                  fabs(cur_data - lorenzo_2layer_predict) + noise_2layer;
        }


        inline int
        meta_blockwise_selection_3d(const T *data_pos, const meanInfo<T> &mean_info, size_t dim0_offset,
                                    size_t dim1_offset,
                                    int min_size,
                                    T precision, const float *reg_params_pos, const int pred_dim,
                                    const bool use_lorenzo, const bool use_lorenzo_2layer, const bool use_regression) {
            double err_lorenzo = 0;
            double err_lorenzo_2layer = 0;
            double err_reg = 0;
            for (int i = 2; i < min_size - 1; i++) {
                int bmi = min_size - i;
                meta_block_error_estimation_3d(data_pos, reg_params_pos, mean_info, i, i, i, dim0_offset, dim1_offset,
                                               precision, err_lorenzo, err_lorenzo_2layer, err_reg, pred_dim,
                                               use_lorenzo,
                                               use_lorenzo_2layer, use_regression);
                meta_block_error_estimation_3d(data_pos, reg_params_pos, mean_info, i, i, bmi, dim0_offset,
                                               dim1_offset, precision, err_lorenzo, err_lorenzo_2layer, err_reg,
                                               pred_dim,
                                               use_lorenzo, use_lorenzo_2layer, use_regression);
                meta_block_error_estimation_3d(data_pos, reg_params_pos, mean_info, i, bmi, i, dim0_offset,
                                               dim1_offset, precision, err_lorenzo, err_lorenzo_2layer, err_reg,
                                               pred_dim,
                                               use_lorenzo, use_lorenzo_2layer, use_regression);
                meta_block_error_estimation_3d(data_pos, reg_params_pos, mean_info, i, bmi, bmi, dim0_offset,
                                               dim1_offset, precision, err_lorenzo, err_lorenzo_2layer, err_reg,
                                               pred_dim,
                                               use_lorenzo, use_lorenzo_2layer, use_regression);
            }
            if (min_size > 3) {
                meta_block_error_estimation_3d(data_pos, reg_params_pos, mean_info, min_size - 1, min_size - 1,
                                               min_size - 1,
                                               dim0_offset, dim1_offset,
                                               precision, err_lorenzo, err_lorenzo_2layer, err_reg, pred_dim,
                                               use_lorenzo,
                                               use_lorenzo_2layer, use_regression);
            }

            if (use_regression && (!use_lorenzo || err_reg <= err_lorenzo)
                && (!use_lorenzo_2layer || err_reg < err_lorenzo_2layer)) {
                return SELECTOR_REGRESSION;
            } else if (use_lorenzo_2layer && (!use_lorenzo || err_lorenzo_2layer <= err_lorenzo)
                       && (!use_regression || err_lorenzo_2layer <= err_reg)) {
                return SELECTOR_LORENZO_2LAYER;
            } else {
                return SELECTOR_LORENZO;
            }
        }

        meta_params params;
        SZMETA::DSize_3d size;
        SZMETA::meanInfo<T> mean_info;
        double precision;
        int intv_radius;
        size_t reg_count = 0;
        int est_unpred_count_per_index = 0;
        int capacity = 0; // num of quant intervals

        int *unpred_count_buffer = nullptr;
        T *unpred_data_buffer = nullptr;
        std::vector<int> indicator;
        int *reg_params_type = nullptr;
        float *reg_unpredictable_data = nullptr;
        float *reg_params = nullptr;
        float *reg_unpredictable_data_pos;
        Quantizer quantizer;
        Config<T, N> conf;

    };

    template<class T, uint N, class Predictor>
    SZMetaFrontend<T, N, Predictor>
    make_sz_meta_frontend(const Config<T, N> &conf, Predictor predictor) {
        return SZMetaFrontend<T, N, Predictor>(conf, predictor);
    }
}


#endif