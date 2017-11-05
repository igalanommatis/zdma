#include "common.h"
#include "ap_int.h"

int32_t blur(axi_stream_t& src, axi_stream_t& dst, int32_t line_width, int32_t kernel_dim, ap_uint<4> *debug)
{
#pragma HLS INTERFACE axis port=src bundle=INPUT_STREAM
#pragma HLS INTERFACE axis port=dst bundle=OUTPUT_STREAM
#pragma HLS INTERFACE s_axilite port=line_width bundle=CONTROL_BUS offset=0x14
#pragma HLS INTERFACE s_axilite port=kernel_dim bundle=CONTROL_BUS offset=0x1C
#pragma HLS INTERFACE s_axilite port=return bundle=CONTROL_BUS offset=0x24
#pragma HLS INTERFACE ap_stable port=line_width
#pragma HLS INTERFACE ap_stable port=kernel_dim
#pragma HLS INTERFACE ap_none port=debug
	axi_elem_t data_in, data_out;
	int32_t ret = 0;
	uint8_t lnbuf[MAX_KERNEL_DIM][MAX_LINE_WIDTH];
	int32_t kern_sum;
	union {
		axi_data_t all;
		uint8_t at[AXI_TDATA_NBYTES];
	} pixel;
	int32_t acc[AXI_TDATA_NBYTES];

	*debug = 2;
	if (kernel_dim < 3 || kernel_dim > MAX_KERNEL_DIM || (kernel_dim & 1) != 1) {
		kernel_dim = 5;
		ret = -1;
		*debug = 2+8;
	}
	if (line_width < 0 || line_width > MAX_LINE_WIDTH) {
		line_width = MAX_LINE_WIDTH;
		ret = -1;
		*debug = 2+8;
	}

	kern_sum = kernel_dim*kernel_dim;

	int col = 0;
	do {
#pragma HLS loop_tripcount min=76800 max=288000
		src >> data_in;
		data_out = data_in;
		pixel.all = data_out.data;
		data_out.data = 0;
		for (int px = 0; px < AXI_TDATA_NBYTES; px++) {
#pragma HLS unroll
			for (int i = 0; i < kernel_dim - 1; i++) {
#pragma HLS loop_tripcount min=1 max=11
				lnbuf[i][col+px] = lnbuf[i+1][col+px];
			}

			lnbuf[kernel_dim-1][col+px] = pixel.at[px];

			acc[px] = 0;
			for (int i = 0; i < kernel_dim; i++) {
#pragma HLS loop_tripcount min=3 max=11
				for (int j = 0; j < kernel_dim; j++) {
#pragma HLS loop_tripcount min=3 max=11
					acc[px] += lnbuf[i][j+col+px];
				}
			}

			int64_t val = acc[px] / kern_sum;
			if (val > 255) val = 255;
			data_out.data |= val << (px << AXI_TDATA_SHIFT);

		}
		col += AXI_TDATA_NBYTES;

		if (col >= line_width)
			col = 0;

		dst << data_out;
	} while (!data_out.last);
	*debug = 0;
	return ret;
}
