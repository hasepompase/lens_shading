/*
Copyright (c) 2017, Raspberry Pi (Trading) Ltd
Copyright (c) 2017, Dave Stevenson
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file lens_shading_analyse
 *
 * Description
 *
 * This application will take a Raw file captured using Raspistill
 * or similar, and analyse it in order to produce a customised lens shading
 * table. In order to get sensible results, the image should be of a
 * plain, uniformly illuminated scene (eg a nice white wall).
 *
 * It'll write out the four colour channels as ch1.bin-ch4.bin, viewable as
 * 16bit/pixel single channel images, although only the bottom 10 bits are used.
 * It also writes a file called ls_table.h, which provides the lens shading grid.
 * Pass that back to the camera component using code similar to:
    {
      MMAL_PARAMETER_LENS_SHADING_T ls = {{MMAL_PARAMETER_LENS_SHADING_OVERRIDE, sizeof(MMAL_PARAMETER_LENS_SHADING_T)}};
      void *grid;

      #include "ls_grid.h"

      ls.enabled = MMAL_TRUE;
      ls.grid_cell_size = 64;
      ls.grid_width = ls.grid_stride = grid_width;
      ls.grid_height = grid_height;
      ls.ref_transform = ref_transform;

      state->lens_shading = vcsm_malloc(ls.grid_stride*ls.grid_height*4, "ls_grid");
      ls.mem_handle_table = vcsm_vc_hdl_from_hdl(state->lens_shading);

      grid = vcsm_lock(state->lens_shading);

      memcpy(grid, ls_grid, vcos_min(sizeof(ls_grid), ls.grid_stride*ls.grid_height*4));

      vcsm_unlock_hdl(state->lens_shading);

      status = mmal_port_parameter_set(camera->control, &ls.hdr);
      if (status != MMAL_SUCCESS)
         vcos_log_error("Failed to set lens shading parameters - %d", status);
   }
 * and
      vcsm_free(state.lens_shading);
 * when finished (the firmware should acquire a reference on the vcsm allocation as it
 * is passed in, and only release it when done, so in theory you can release the handle
 * immediately after passing it in. Needs to be checked.)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define NUM_CHANNELS 4

//This structure is at offset 0xB0 from the 'BRCM' ident.
struct brcm_raw_header {
	uint8_t name[32];
	uint16_t width;
	uint16_t height;
	uint16_t padding_right;
	uint16_t padding_down;
	uint32_t dummy[6];
	uint16_t transform;
	uint16_t format;
	uint8_t bayer_order;
	uint8_t bayer_format;
};
//Values taken from https://github.com/raspberrypi/userland/blob/master/interface/vctypes/vc_image_types.h
#define BRCM_FORMAT_BAYER  33
#define BRCM_BAYER_RAW10   3
#define BRCM_BAYER_RAW12   4

enum bayer_order_t {
	RGGB,
	GBRG,
	BGGR,
	GRBG
};

const int channel_ordering[4][4] = {
	{ 0, 1, 2, 3 },
	{ 2, 3, 0, 1 },
	{ 3, 2, 1, 0 },
	{ 1, 0, 3, 2 }
};

uint8_t* sensor_model_check(int sensor_model, void* buffer, size_t size)
{
		uint8_t* in_buf = 0;

		switch(sensor_model) {
		case 1:
			in_buf = ((uint8_t*)buffer) + size - 6404096;
			break;
		case 2:
			in_buf = ((uint8_t*)buffer) + size - 10270208;
			break;
		case 3:
			in_buf = ((uint8_t*)buffer) + size - 18711040;
			break;
		default:
			return 0;
			break;
		}

		if (memcmp(in_buf, "BRCM", 4) == 0)
		{
			return in_buf;
		}
		else
		{
			return 0;
		}
}

uint16_t black_level_correct(uint16_t raw_pixel, unsigned int black_level, unsigned int max_value)
{
	return ((raw_pixel - black_level) * max_value) / (max_value - black_level);
}

void print_help(void)
{
	printf("\n");
	printf("\n");
	printf("\"lens_shading_analyse\" Lens shading analysis tool\n");
	printf("\n");
	printf("Analyzes the lens shading based on a raw image\n");
	printf("\n");
	printf("usage: lens_shading_analyse -i <filename> [options]\n");
	printf("\n");
	printf("Parameters\n");
	printf("\n");
	printf("-i  : Raw image file (mandatory)\n");
	printf("-b  : Black level\n");
	printf("-s  : Size of the analysis cell. Minimum 2, maximum 32, default 4\n");
	printf("-o  : Output format. Formats can be output together, for example 3 = 1 + 2\n");
	printf("      1  : Header file (default on)\n");
	printf("      2  : Binary file\n");
	printf("      4  : Text file\n");
	printf("      8  : Channel data\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	int in = 0;
	FILE *out, *header, *table, *bin;
	int i, x, y;
	uint16_t *out_buf[NUM_CHANNELS];
	uint16_t max_val;
	void *mmap_buf;
	uint8_t *in_buf;
	struct stat sb;
	int bits_per_sample;
	int bayer_order;
	struct brcm_raw_header *hdr;
	int width, height, stride;
	uint32_t grid_width, grid_height, block_px_max;
	int single_channel_width, single_channel_height;
	unsigned int black_level = 0;
	uint32_t *block_sum;
	uint8_t block_size = 4;
	uint8_t out_frmt = 1;

	if (argc < 2)
	{
		print_help();
		return -1;
	}

	int nArg;
	while ((nArg = getopt(argc, argv, "b:i:o:s:")) != -1)
	{
		switch (nArg) {
		case 'b':
			black_level = strtoul(optarg, NULL, 10);
			break;
		case 'i':
			in = open(optarg, O_RDONLY);
			if (in < 0)
			{
				printf("Failed to open %s\n", argv[1]);
				return -1;
			}
			break;
		case 'o':
			out_frmt = strtoul(optarg, NULL, 10);
			if (!out_frmt & 0x0F)
			{
				printf("Invalid output format\n");
				return -1;
			}
			break;
		case 's':
			block_size = strtoul(optarg, NULL, 10);
			if (block_size<=0 || block_size>32)
			{
				printf("Analysis cell out of range\n");
				return -1;
			}
			else if (block_size%2 == 1)
			{
				block_size++;
			}
			break;
		default:
		case 'h':
			print_help();
			return -1;
			break;
		}
	}

	fstat(in, &sb);
	printf("File size is %ld\n", sb.st_size);

	mmap_buf = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, in, 0);
	if (mmap_buf == MAP_FAILED)
	{
		printf("mmap failed\n");
		goto close_file;
	}

	if (!memcmp(mmap_buf, "\xff\xd8", 2))
	{
		int sensor_model = 1;
		do
		{
			in_buf = sensor_model_check(sensor_model, mmap_buf, sb.st_size);
		}
		while(in_buf == 0 && sensor_model++ <= 3);

		if (in_buf == 0)
		{
			in_buf = (uint8_t*)mmap_buf;
		}
	}
	else
	{
		in_buf = (uint8_t*)mmap_buf;
	}

	if (strncmp(in_buf, "BRCM", 4))
	{
		printf("Raw file missing BRCM header\n");
		goto unmap;
	}

	char model[7];
	memcpy(model, &in_buf[16], 6);
	model[6] = '\0';
	if (strncmp(model, "imx219", 6) == 0)
	{
		printf("Sensor type: %s\n", model);
		if (black_level == 0)
		{
			black_level = 64;
		}
	}
	else if (strncmp(model, "ov5647", 6) == 0)
	{
		printf("Sensor type: %s\n", model);
		if (black_level == 0)
		{
			black_level = 16;
		}
	}
	else if (strncmp(model, "testc", 6) == 0 ||
				strncmp(model, "imx477", 6) == 0)
	{
		printf("Sensor type: %s\n", model);
		if (black_level == 0)
		{
			black_level = 257;
		}
	}
	else if (black_level == 0)
	{
		black_level = 16; // Default value
	}
	printf("Black level: %d\n", black_level);

	hdr = (struct brcm_raw_header*) (in_buf+0xB0);
	printf("Header decoding: mode %s, width %u, height %u, padding %u %u\n",
			hdr->name, hdr->width, hdr->height, hdr->padding_right, hdr->padding_down);
	printf("transform %u, image format %u, bayer order %u, bayer format %u\n",
			hdr->transform, hdr->format, hdr->bayer_order, hdr->bayer_format);
	if (hdr->format != BRCM_FORMAT_BAYER ||
			(hdr->bayer_format != BRCM_BAYER_RAW10 && hdr->bayer_format != BRCM_BAYER_RAW12))
	{
		printf("Raw file is not Bayer raw10 or raw12\n");
		goto unmap;
	}
	bayer_order = hdr->bayer_order;
	bits_per_sample = hdr->bayer_format * 2 + 4;
	max_val = ( 1 << bits_per_sample ) - 1;
	width = hdr->width;
	height = hdr->height;
	single_channel_width = width/2;
	single_channel_height = height/2;
	grid_width = (single_channel_width + 31) / 32;
	grid_height = (single_channel_height + 31) / 32;
	block_px_max = block_size*block_size;
	block_sum = (uint32_t *)malloc(sizeof(uint32_t) * grid_width * grid_height);
	printf("Grid size: %d x %d\n", grid_width, grid_height);

	if (bits_per_sample == 10) {
		//Stride computed via same formula as the firmware uses.
		stride = (((((width + hdr->padding_right)*5)+3)>>2) + 31)&(~31);
	} else {
		stride = (((((width + hdr->padding_right)*6)+3)>>2) + 31)&(~31);
	}

	for (i=0; i<NUM_CHANNELS; i++)
	{
		out_buf[i] = (uint16_t*)malloc(single_channel_width*single_channel_height * sizeof(uint16_t));
		memset(out_buf[i], 0, single_channel_width*single_channel_height * sizeof(uint16_t));
	}

	for (y=0; y<height; y++)
	{
		uint8_t *line = in_buf + (y*stride) + 32768;
		int chan_a, chan_b;
		if (y&1)
		{
			chan_a = 2;
			chan_b = 3;
		}
		else
		{
			chan_a = 0;
			chan_b = 1;
		}

		uint16_t *chan_a_line = out_buf[chan_a] + ((y>>1)*single_channel_width);
		uint16_t *chan_b_line = out_buf[chan_b] + ((y>>1)*single_channel_width);
		if (bits_per_sample == 10) {
			for (x=0; x<width; x+=4)
			{
				uint8_t lsbs = line[4];
				*(chan_a_line) = black_level_correct(((*line)<<2) + (lsbs>>6), black_level, max_val);
				chan_a_line++;
				lsbs<<=2;
				line++;
				*(chan_b_line) = black_level_correct(((*line)<<2) + (lsbs>>6), black_level, max_val);
				chan_b_line++;
				lsbs<<=2;
				line++;
				*(chan_a_line) = black_level_correct(((*line)<<2) + (lsbs>>6), black_level, max_val);
				chan_a_line++;
				lsbs<<=2;
				line++;
				*(chan_b_line) = black_level_correct(((*line)<<2) + (lsbs>>6), black_level, max_val);
				chan_b_line++;
				lsbs<<=2;
				line++;
				line++; //skip the LSBs
			}
		} else {
			for (x=0; x<width; x+=4)
			{
				*(chan_a_line) = black_level_correct(((*line)<<4) + (line[ 2 ]>>4), black_level, max_val);
				chan_a_line++;
				line++;
				*(chan_b_line) = black_level_correct(((*line)<<4) + (line[ 1 ]&0x0F), black_level, max_val);
				chan_b_line++;
				line+= 2;
				*(chan_a_line) = black_level_correct(((*line)<<4) + (line[ 2 ]>>4), black_level, max_val);
				chan_a_line++;
				line++;
				*(chan_b_line) = black_level_correct(((*line)<<4) + (line[ 1 ]&&0x0F), black_level, max_val);
				chan_b_line++;
				line+= 2;
			}
		}
	}

	if (out_frmt&0x01)
	{
		header = fopen("ls_table.h", "wb");
	}
	if (out_frmt&0x02)
	{
		bin =  fopen("ls.bin", "wb");
	}
	if (out_frmt&0x04)
	{
		table = fopen("ls_table.txt", "wb");
	}
	if (out_frmt&0x01)
	{
		fprintf(header, "uint8_t ls_grid[] = {\n");
	}
	if (out_frmt&0x02)
	{
		uint32_t transform = hdr->transform;
		fwrite(&transform, sizeof(uint32_t), 1, bin);
		fwrite(&grid_width, sizeof(uint32_t), 1, bin);
		fwrite(&grid_height, sizeof(uint32_t), 1, bin);
	}
	for (i=0; i<NUM_CHANNELS; i++)
	{
		if (out_frmt&0x08)
		{
			// Write out the raw data for analysis
			const char *filenames[NUM_CHANNELS] = {
				"ch1.bin",
				"ch2.bin",
				"ch3.bin",
				"ch4.bin"
			};
			out = fopen(filenames[i], "wb");
			if (out)
			{
				fwrite(out_buf[i], (single_channel_width*single_channel_height)*sizeof(uint16_t), 1, out);
				fclose(out);
				out = NULL;
			}
		}

		//Write out the lens shading table in the order RGGB
		uint16_t *channel = out_buf[channel_ordering[bayer_order][i]];
		int mid_value_avg = 0;
		int count = 0;
		uint16_t *line;
		const char *channel_comments[4] = {
			"R",
			"Gr",
			"Gb",
			"B"
		};

		// Calculate sum for each block
		uint16_t block_idx = 0;
		uint32_t max_blk_val = 0;
		for (y=0; y<grid_height; y++)
		{
			int y_start = y*32+16-block_size/2;
			if (y_start >= single_channel_height)
				y_start = single_channel_height-1;
			int y_stop  = y_start+block_size;
			if (y_stop > single_channel_height)
				y_stop = single_channel_height;

			for (x=0; x<grid_width; x++)
			{
				int x_start = x*32+16-block_size/2;
				if (x_start >= single_channel_width)
					x_start = single_channel_width-1;
				int x_stop  = x_start+block_size;
				if (x_stop > single_channel_width)
					x_stop = single_channel_width;

				uint32_t block_val = 0;
				uint16_t block_px = 0;

				for (int y_px = y_start; y_px < y_stop; y_px++)
				{
					line = &channel[y_px*(single_channel_width)];
					for (int x_px = x_start; x_px < x_stop; x_px++)
					{
						block_val += line[x_px];
						block_px++;
					}
				}
				if (block_px < block_px_max)
					block_val = block_val * block_px_max / block_px; // Scale sum in case of small edge blocks

				block_sum[block_idx++] =  block_val ? block_val : 1;
				if (block_val > max_blk_val)
					max_blk_val = block_val;
			}
		}

		max_blk_val <<= 5;
		if (out_frmt&0x01)
		{
			fprintf(header, "//%s - Ch %d\n", channel_comments[i], channel_ordering[bayer_order][i]);
		}

		// Calculate gain for each block
		block_idx = 0;
		for (y=0; y<grid_height; y++)
		{
			for (x=0; x<grid_width; x++)
			{
				int gain = max_blk_val / block_sum[block_idx++] + 0.5;
				if (gain > 255)
					gain = 255; //Clip as uint8_t
				else if (gain < 32)
					gain = 32;  //Clip at x1.0, should never happen
				if (out_frmt&0x01)
				{
					fprintf(header, "%d, ", gain);
				}
				if (out_frmt&0x02)
				{
					uint8_t gain_bin = gain;
					fwrite(&gain_bin, sizeof(uint8_t), 1, bin);
				}
				if (out_frmt&0x04)
				{
					fprintf(table, "%d %d %d %d\n", x * 32 + 16, y * 32 + 16, gain, i);
				}
			}
		}

	}
	if (out_frmt&0x01)
	{
		fprintf(header, "};\n");
		fprintf(header, "uint32_t ref_transform = %u;\n", hdr->transform);
		fprintf(header, "uint32_t grid_width = %u;\n", grid_width);
		fprintf(header, "uint32_t grid_height = %u;\n", grid_height);
	}

	for (i=0; i<NUM_CHANNELS; i++)
	{
		 free(out_buf[i]);
	}
unmap:
	munmap(mmap_buf, sb.st_size);
close_file:
	close(in);
	return 0;
}
