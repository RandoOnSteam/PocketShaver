#include "sysdeps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svq1_decoder.h"

static uint8 *LoadFile(const char *path, uint32 *length)
{
	FILE *file;
	uint8 *data;
	long size;
	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = (uint8 *)malloc((size_t)size + 1);
	if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
		free(data);
		data = NULL;
	}
	fclose(file);
	*length = (uint32)size;
	return data;
}

static void WritePlane(FILE *output, const uint8 *plane, int pitch, int width, int height)
{
	int row;
	for (row = 0; row < height; row++)
		fwrite(plane + row * pitch, 1, (size_t)width, output);
}

int main(int argc, char **argv)
{
	Svq1Decoder *decoder;
	FILE *output;
	const uint8 *plane;
	char path[1024];
	uint8 *data;
	uint32 length;
	int frames;
	int frame;
	int pitch;
	int width;
	int height;
	int failures;
	if (argc < 6) {
		printf("usage: Svq1Test packetpattern frames width height output.yuv\n");
		return 2;
	}
	frames = atoi(argv[2]);
	decoder = Svq1Create(atoi(argv[3]), atoi(argv[4]));
	output = fopen(argv[5], "wb");
	failures = 0;
	for (frame = 1; frame <= frames && decoder && output; frame++) {
		snprintf(path, sizeof(path), argv[1], frame);
		data = LoadFile(path, &length);
		if (data == NULL || Svq1Decode(decoder, data, length) != 0) {
			printf("FAIL frame %d\n", frame);
			failures++;
			free(data);
			continue;
		}
		free(data);
		width = Svq1Width(decoder);
		height = Svq1Height(decoder);
		plane = Svq1Plane(decoder, 0, &pitch);
		WritePlane(output, plane, pitch, width, height);
		plane = Svq1Plane(decoder, 1, &pitch);
		WritePlane(output, plane, pitch, (width + 3) / 4, (height + 3) / 4);
		plane = Svq1Plane(decoder, 2, &pitch);
		WritePlane(output, plane, pitch, (width + 3) / 4, (height + 3) / 4);
		Svq1Frame(decoder);
	}
	if (output)
		fclose(output);
	if (decoder)
		Svq1Destroy(decoder);
	printf("frames %d failures %d\n", frames, failures);
	return failures != 0;
}
