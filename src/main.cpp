#include "msgpack.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <deque>
#include "libfreenect.h"
#define PNG_DEBUG 3
#include <png.h>

volatile int die = 0;

uint8_t *rgb_back;
int f;

/* structure to store PNG image bytes */
struct mem_encode
{
	char *buffer;
	size_t size;
};

mem_encode state;

struct img
{
	uint8_t type;
	uint32_t state;
	mem_encode data;
};

std::deque<img> imgs;

void abort_(const char * s, ...)
{
	va_list args;
	va_start(args, s);
	vfprintf(stderr, s, args);
	fprintf(stderr, "\n");
	va_end(args);
	abort();
}

void
my_png_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{	
	struct mem_encode* p=(struct mem_encode*)png_ptr->io_ptr;
	size_t nsize = p->size + length;

	/* allocate or grow buffer */
	if(p->buffer)
		p->buffer = (char*)realloc(p->buffer, nsize);
	else
		p->buffer = (char*)malloc(nsize);

	if(!p->buffer)
		png_error(png_ptr, "Write Error");
	/* copy new bytes to end of buffer */
	memcpy(p->buffer + p->size, data, length);
	p->size += length;
}

void encode_thread()
{
	while(1)
	{
		if(imgs.size() > 0)
		{
			img i = imgs.back();
			imgs.pop_back();
			
		}
	}
}

void write_png_file(void* img)
{
	state.size = 0;
	png_structp png_ptr;
	png_infop info_ptr;
	int number_of_passes;
	png_bytep * row_pointers = (png_bytep*)malloc(480 * sizeof(png_bytep));
	for(int h = 0; h < 480; ++h)
	{
		(row_pointers)[h] = (png_bytep)(img+(h*640*2));
	}
	/* initialize stuff */
	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	
	if (!png_ptr)
		abort_("[write_png_file] png_create_write_struct failed");

	/* if my_png_flush() is not needed, change the arg to NULL */
	png_set_write_fn(png_ptr, &state, my_png_write_data, NULL);
		
	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		abort_("[write_png_file] png_create_info_struct failed");

	//png_init_io(png_ptr, f);
	if (setjmp(png_jmpbuf(png_ptr)))
		abort_("[write_png_file] Error during init_io");
	
	/* write header */
	if (setjmp(png_jmpbuf(png_ptr)))
		abort_("[write_png_file] Error during writing header");
	
	png_set_IHDR(png_ptr, info_ptr, 640, 480,
				 16, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
				 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	
	png_write_info(png_ptr, info_ptr);
		
	/* write bytes */
	if (setjmp(png_jmpbuf(png_ptr)))
		abort_("[write_png_file] Error during writing bytes");

	png_write_image(png_ptr, row_pointers);

	// /* end write */
	if (setjmp(png_jmpbuf(png_ptr)))
		abort_("[write_png_file] Error during end of write");

	png_write_end(png_ptr, NULL);

	/* cleanup heap allocation */	
	free(row_pointers);
	free(info_ptr);
	free(png_ptr);
}

void depth_cb(freenect_device *dev, void *v_depth, uint32_t timestamp)
{
	printf("%d\n", timestamp);
	write_png_file(v_depth);
	msgpack_sbuffer buffer;
	msgpack_packer pk;
	msgpack_sbuffer_init(&buffer);
	msgpack_packer_init(&pk, &buffer, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, 1);
	msgpack_pack_int(&pk, timestamp);
	// msgpack_pack_raw(&pk, state.size);
	// msgpack_pack_raw_body(&pk, state.buffer, state.size);
	write(f, buffer.data, buffer.size);
	msgpack_sbuffer_destroy(&buffer);
}

void rgb_cb(freenect_device *dev, void *rgb, uint32_t timestamp)
{
	
}

int main(int argc, char** argv)
{
	f = open("sleepdata.mpack", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	state.buffer = NULL;
	state.size = 0;

	freenect_context *f_ctx;
	freenect_device *f_dev;
	if (freenect_init(&f_ctx, NULL) < 0) {
		printf("freenect_init() failed\n");
		return 1;
	}
	freenect_set_log_level(f_ctx, FREENECT_LOG_DEBUG);
	freenect_select_subdevices(f_ctx, (freenect_device_flags)(FREENECT_DEVICE_CAMERA));

	int nr_devices = freenect_num_devices (f_ctx);
	printf ("Number of devices found: %d\n", nr_devices);

	int user_device_number = 0;
	if (argc > 1)
		user_device_number = atoi(argv[1]);

	if (nr_devices < 1)
		return 1;

	if (freenect_open_device(f_ctx, &f_dev, user_device_number) < 0) {
		printf("Could not open device\n");
		return 1;
	}
	
	int accelCount = 0;

	freenect_set_depth_callback(f_dev, depth_cb);
//	freenect_set_video_callback(f_dev, rgb_cb);
	//	freenect_set_video_mode(f_dev, freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB));
	freenect_set_depth_mode(f_dev, freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_11BIT));
	//freenect_set_video_buffer(f_dev, rgb_back);
	freenect_start_depth(f_dev);
	//freenect_start_video(f_dev);

	while (!die && freenect_process_events(f_ctx) >= 0) {
		sleep(.01);
	}

	printf("\nshutting down streams...\n");

	freenect_stop_depth(f_dev);
	freenect_stop_video(f_dev);

	freenect_close_device(f_dev);
	freenect_shutdown(f_ctx);
	close(f);

	printf("-- done!\n");

	return 0;
}
