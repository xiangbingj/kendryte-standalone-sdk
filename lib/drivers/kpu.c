#include "kpu.h"
#include <platform.h>
#include <sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "printf.h"
#include "dmac.h"
#include <string.h>
#include "bsp.h"

#define LAYER_BURST_SIZE 12

volatile kpu_config_t *const kpu = (volatile kpu_config_t *)AI_BASE_ADDR;
static volatile uint32_t kpu_status;

static int kpu_done(void *ctx)
{
    atomic_swap(&kpu_status, 0);
    kpu_task_t *task = (kpu_task_t *)ctx;
    task->callback();
    return 0;
}

static int kpu_config_input(void *ctx)
{
    kpu_task_t *task = (kpu_task_t *)ctx;
    kpu->interrupt_clear.reg = 7;
    if (task->remain_layers_length <= LAYER_BURST_SIZE)
    {
        for (uint32_t i = 0; i < task->remain_layers_length; i++)
        {
            kpu->layer_argument_fifo = task->remain_layers[i].interrupt_enabe.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_addr.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_channel_num.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_size.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_pool_type_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_load_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_offset.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_calc_type_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].write_back_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].conv_value.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].conv_value2.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].dma_parameter.reg;
        }
        task->remain_layers_length = 0;
        kpu->interrupt_mask.reg = 7;
    }
    else
    {
        for (uint32_t i = 0; i < LAYER_BURST_SIZE; i++)
        {
            kpu->layer_argument_fifo = task->remain_layers[i].interrupt_enabe.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_addr.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_channel_num.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].image_size.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_pool_type_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_load_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_offset.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].kernel_calc_type_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].write_back_cfg.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].conv_value.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].conv_value2.reg;
            kpu->layer_argument_fifo = task->remain_layers[i].dma_parameter.reg;
        }
        task->remain_layers += LAYER_BURST_SIZE;
        task->remain_layers_length -= LAYER_BURST_SIZE;
    }
    return 0;
}

static void kpu_data_output(kpu_task_t *task)
{
    sysctl_dma_select(task->dma_ch, SYSCTL_DMA_SELECT_AI_RX_REQ);
    dmac_set_irq(task->dma_ch, kpu_done, task, 1);
    dmac_set_single_mode(task->dma_ch, (void *)(&kpu->fifo_data_out), (void *)(task->dst), DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
        DMAC_MSIZE_8, DMAC_TRANS_WIDTH_64, task->dst_length);
}

static int kpu_data_ready(void *ctx)
{
    kpu_task_t *task = (kpu_task_t *)ctx;

    dmac->channel[task->dma_ch].intclear = 0xFFFFFFFF;
    kpu_data_output(task);

    kpu->eight_bit_mode.reg = task->eight_bit_mode;
    kpu->interrupt_mask.reg = 7;
    kpu->interrupt_clear.reg = 7;
    kpu->fifo_threshold.data = (kpu_config_fifo_threshold_t)
    {
        .fifo_full_threshold = 12, .fifo_empty_threshold = 1
    };
    plic_irq_enable(IRQN_AI_INTERRUPT);
    plic_set_priority(IRQN_AI_INTERRUPT, 2);
    plic_irq_register(IRQN_AI_INTERRUPT, kpu_config_input, task);
    kpu_config_input(task);
    kpu->interrupt_mask.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
        .layer_cfg_almost_empty_int = 0,
        .layer_cfg_almost_full_int = 1
    };
    return 0;
}

static void kpu_data_input(kpu_task_t *task)
{
    if (task->src == NULL)
    {
        kpu_data_ready(task);
        return;
    }
    dmac_set_irq(task->dma_ch, kpu_data_ready, task, 1);
    dmac_set_single_mode(task->dma_ch, task->src, (void *)(AI_IO_BASE_ADDR), DMAC_ADDR_INCREMENT, DMAC_ADDR_INCREMENT,
        DMAC_MSIZE_16, DMAC_TRANS_WIDTH_64, task->src_length);
}

int kpu_task_init(kpu_task_t *task)
{
    kpu_layer_argument_t *first_layer = &task->layers[0];
    kpu_layer_argument_t *last_layer = &task->layers[task->layers_length - 1];

    last_layer->dma_parameter.data.send_data_out = 1;
    last_layer->interrupt_enabe.data.int_en = 1;
    task->src_length = first_layer->kernel_calc_type_cfg.data.channel_switch_addr * 64 * (first_layer->image_channel_num.data.i_ch_num + 1) / 8;
    task->dst_length = ((last_layer->dma_parameter.data.dma_total_byte + 1) + 7) / 8;
    task->dst = (uint64_t *)malloc(task->dst_length * 8);
    memset(task->dst, 0, task->dst_length * 8);
    if (task->dst == NULL)
        return 1;
    return 0;
}

int kpu_task_deinit(kpu_task_t *task)
{
    free(task->dst);
    return 0;
}

int kpu_run(kpu_task_t *task)
{
    if (atomic_cas(&kpu_status, 0, 1))
        return -1;

    task->remain_layers_length = task->layers_length;
    task->remain_layers = task->layers;
    kpu_data_input(task);
    return 0;
}

static void kpu_send_layer(const kpu_layer_argument_t *layer)
{
    kpu->layer_argument_fifo = layer->interrupt_enabe.reg;
    kpu->layer_argument_fifo = layer->image_addr.reg;
    kpu->layer_argument_fifo = layer->image_channel_num.reg;
    kpu->layer_argument_fifo = layer->image_size.reg;
    kpu->layer_argument_fifo = layer->kernel_pool_type_cfg.reg;
    kpu->layer_argument_fifo = layer->kernel_load_cfg.reg;
    kpu->layer_argument_fifo = layer->kernel_offset.reg;
    kpu->layer_argument_fifo = layer->kernel_calc_type_cfg.reg;
    kpu->layer_argument_fifo = layer->write_back_cfg.reg;
    kpu->layer_argument_fifo = layer->conv_value.reg;
    kpu->layer_argument_fifo = layer->conv_value2.reg;
    kpu->layer_argument_fifo = layer->dma_parameter.reg;
}

void kpu_init(int eight_bit_mode, plic_irq_callback_t callback, void *userdata)
{
    kpu->interrupt_clear.reg = 7;
    kpu->fifo_threshold.data = (kpu_config_fifo_threshold_t)
    {
        .fifo_full_threshold = 10, .fifo_empty_threshold = 1
    };
    kpu->eight_bit_mode.data = (kpu_config_eight_bit_mode_t)
    {
        .eight_bit_mode = eight_bit_mode
    };
    kpu->interrupt_mask.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 0,
            .layer_cfg_almost_full_int = 1
    };

    plic_irq_enable(IRQN_AI_INTERRUPT);
    plic_set_priority(IRQN_AI_INTERRUPT, 1);
    plic_irq_register(IRQN_AI_INTERRUPT, callback, userdata);
}

void kpu_input_dma(kpu_layer_argument_t *layer, const uint8_t *src, dmac_channel_number_t dma_ch, plic_irq_callback_t callback, void *userdata)
{
    uint64_t input_size = layer->kernel_calc_type_cfg.data.channel_switch_addr * 64 * (layer->image_channel_num.data.i_ch_num + 1);
    dmac_set_irq(dma_ch, callback, userdata, 1);
    dmac_set_single_mode(dma_ch, (void *)src, (void *)(AI_IO_BASE_ADDR + layer->image_addr.data.image_src_addr * 64), DMAC_ADDR_INCREMENT, DMAC_ADDR_INCREMENT,
        DMAC_MSIZE_16, DMAC_TRANS_WIDTH_64, input_size / 8);
}

static void kpu_conv2d_core(kpu_layer_argument_t *layer)
{
    kpu_send_layer(layer);
}

void kpu_conv2d(kpu_layer_argument_t *layer)
{
    kpu->interrupt_clear.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 1,
            .layer_cfg_almost_full_int = 1
    };
    kpu->interrupt_mask.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 0,
            .layer_cfg_almost_full_int = 1
    };
    kpu_conv2d_core(layer);
}

void kpu_conv2d_output(kpu_layer_argument_t *layer, dmac_channel_number_t dma_ch, uint8_t *dest, plic_irq_callback_t callback, void *userdata)
{
    kpu->interrupt_clear.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 1,
            .layer_cfg_almost_full_int = 1
    };
    kpu->interrupt_mask.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 1,
            .layer_cfg_almost_full_int = 1
    };
    layer->dma_parameter.data.send_data_out = 1;
    sysctl_dma_select(dma_ch, SYSCTL_DMA_SELECT_AI_RX_REQ);
    dmac_set_irq(dma_ch, callback, userdata, 1);
    dmac_set_single_mode(dma_ch, (void *)(&kpu->fifo_data_out), dest, DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
        DMAC_MSIZE_8, DMAC_TRANS_WIDTH_64, (layer->dma_parameter.data.dma_total_byte + 8) / 8);
    kpu_conv2d_core(layer);
}

void kpu_conv2d_output_full_add(kpu_layer_argument_t *layer, dmac_channel_number_t dma_ch, uint64_t *dest, plic_irq_callback_t callback, void *userdata)
{
    uint32_t channels = layer->image_channel_num.data.o_ch_num + 1;
    layer->interrupt_enabe.data.full_add = 1;

    kpu->interrupt_clear.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 1,
            .layer_cfg_almost_full_int = 1
    };
    kpu->interrupt_mask.data = (kpu_config_interrupt_t)
    {
        .calc_done_int = 1,
            .layer_cfg_almost_empty_int = 1,
            .layer_cfg_almost_full_int = 1
    };
    layer->dma_parameter.data.send_data_out = 1;
    sysctl_dma_select(dma_ch, SYSCTL_DMA_SELECT_AI_RX_REQ);
    dmac_set_irq(dma_ch, callback, userdata, 1);
    dmac_set_single_mode(dma_ch, (void *)(&kpu->fifo_data_out), dest, DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
        DMAC_MSIZE_8, DMAC_TRANS_WIDTH_64, channels);
    kpu_conv2d_core(layer);
}

void kpu_add(const uint8_t *src1, const quantize_param_t *src1_param, const uint8_t *src2, const quantize_param_t *src2_param, int width, int height, int channels, uint8_t *dest, const quantize_param_t *dest_param)
{
    quantize_param_t q1 = *src1_param, q2 = *src2_param, q3 = *dest_param;
    size_t oc, y ,x;

    uint32_t row_padding;
    uint32_t row_group;
    uint32_t row_length;

    if (width <= 16)
    {
        row_padding = 16;
        row_group = 4;
        row_length = 1;
    }
    else if (width <= 32)
    {
        row_padding = 32;
        row_group = 2;
        row_length = 1;
    }
    else
    {
        row_padding = 64;
        row_group = 1;
        row_length = (width + 63) / 64;
    }

    for (oc = 0; oc < channels; oc++)
    {
        uint8_t *channel_origin = dest + oc / row_group * row_length * height * 64 + oc % row_group * row_padding;
        for (y = 0; y < height; y++)
        {
            uint8_t *y_origin = channel_origin + y * row_length * 64;
            for (x = 0; x < width; x++)
            {
                int value = ((*src1++ * q1.scale + q1.bias + *src2++ * q2.scale + q2.bias) - q3.bias) / q3.scale;
                if (value < 0) value = 0;
                if (value > 0xFF) value = 0xFF;
                y_origin[x] = value;
            }
        }
    }
}

void kpu_global_average_pool(const uint8_t *src, const quantize_param_t *src_param, int kernel_size, int channels, uint8_t *dest, const quantize_param_t *dest_param)
{
    quantize_param_t q1 = *src_param, q2 = *dest_param;
    size_t oc, y, x;

    if (((uintptr_t)dest) >= AI_IO_BASE_ADDR && ((uintptr_t)dest) < AI_IO_BASE_ADDR + 2 * 1024 * 1024)
    {
        uint32_t row_padding = 16;
        uint32_t row_group = 4;
        uint32_t row_length = 1;
        uint32_t height = 4;
        uint32_t width = 4;

        for (oc = 0; oc < channels; oc++)
        {
            uint8_t *channel_origin = dest + oc / row_group * row_length * height * 64 + oc % row_group * row_padding;
            for (y = 0; y < 1; y++)
            {
                uint8_t *y_origin = channel_origin + y * row_length * 64;
                for (x = 0; x < 1; x++)
                {
                    int64_t sum = 0;
                    size_t i;
                    for (i = 0; i < kernel_size; i++)
                        sum += *src++;

                    int value = ((sum * q1.scale + q1.bias) / kernel_size - q2.bias) / q2.scale;
                    if (value < 0) value = 0;
                    if (value > 0xFF) value = 0xFF;
                    y_origin[x] = value;
                }
            }
        }
    }
    else
    {
        for (oc = 0; oc < channels; oc++)
        {
            int64_t sum = 0;
            size_t i;
            for (i = 0; i < kernel_size; i++)
                sum += *src++;

            int value = ((sum * q1.scale + q1.bias) / kernel_size - q2.bias) / q2.scale;
            if (value < 0) value = 0;
            if (value > 0xFF) value = 0xFF;
            dest[oc] = value;
        }
    }
}

void kpu_matmul_end(const uint8_t *src, int channels, float *dest, const quantize_param_t *dest_param)
{
    quantize_param_t q1 = *dest_param;
    size_t i = 0;
    for (i = 0; i < channels; i++)
        *dest++ = src[i * 16] * q1.scale + q1.bias;
}

void kpu_dequantize(const uint8_t *src, const quantize_param_t *src_param, size_t count, float *dest)
{
    quantize_param_t q1 = *src_param;
    size_t i = 0;
    for (i = 0; i < count; i++)
        *dest++ = src[i] * q1.scale + q1.bias;
}
