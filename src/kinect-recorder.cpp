#include "msgpack.h"
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <iostream>
#include <boost/shared_ptr.hpp>
#include <boost/shared_array.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/thread.hpp>
#include <boost/array.hpp>
#include <boost/program_options.hpp>
#include "libfreenect.h"
#define PNG_DEBUG 3
#include <png.h>

volatile int die = 0;

void depth_cb(freenect_device *dev, void *v_depth, uint32_t timestamp);
/* convert struct timeval to ms(milliseconds) */
uint64_t tv2ms(struct timeval *a) {
    return ((a->tv_sec * 1000) + (a->tv_usec / 1000));
}

uint64_t gettimeofday_ms() {
    struct timeval curr;
    gettimeofday(&curr, NULL);
    return tv2ms(&curr);
}

/* structure to store PNG image bytes */
struct mem_encode
{
	boost::shared_array<uint8_t> buffer;
	int size;

	mem_encode() :
		size(0)
	{
	}
};

struct CameraImage
{
	uint8_t type;
	uint32_t camera_timestamp;
	uint64_t os_timestamp;
	mem_encode raw;
	mem_encode encoded;
	int mark;
};

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
	{
		boost::shared_array<uint8_t> t(new uint8_t[nsize]);
		memcpy(t.get(), p->buffer.get(), p->size);
		p->buffer = t;
	}
	else
	{
		p->buffer = boost::shared_array<uint8_t>(new uint8_t[nsize]);
	}
	if(!p->buffer)
		png_error(png_ptr, "Write Error");
	/* copy new bytes to end of buffer */
	memcpy(p->buffer.get() + p->size, data, length);
	p->size += length;
}

class Encoder
{
public:
	Encoder() 
	{
		mFile = open("sleepdata.mpack", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}

	~Encoder()
	{
		close(mFile);
	}

	virtual void encodeImage(CameraImage& ci) = 0;
		
	virtual void encode()
	{
		mImgs = boost::shared_ptr<boost::circular_buffer<CameraImage> >(new boost::circular_buffer<CameraImage>(60));
		while(1)
		{
			if(mImgs->size() == 0)
			{
				usleep(3000);
				continue;
			}
			printf("encoding!\n");
			CameraImage i = mImgs->back();
			mImgs->pop_back();
			encodeImage(i);
			msgpack_sbuffer buffer;
			msgpack_packer pk;
			msgpack_sbuffer_init(&buffer);
			msgpack_packer_init(&pk, &buffer, msgpack_sbuffer_write);
			msgpack_pack_array(&pk, 4);
			msgpack_pack_int(&pk, 1);
			msgpack_pack_int(&pk, i.camera_timestamp);
			msgpack_pack_raw(&pk, i.encoded.size);
			msgpack_pack_raw_body(&pk, (void*)i.encoded.buffer.get(), i.encoded.size);
			write(mFile, buffer.data, buffer.size);
			msgpack_sbuffer_destroy(&buffer);
		}
	}
	
	void addImage(CameraImage& ci)
	{
		mImgs->push_back(ci);
	}
private:
	boost::shared_ptr<boost::circular_buffer<CameraImage> > mImgs;
	int mFile;
};

class RawEncoder : public Encoder
{
public:
	virtual void encodeImage(CameraImage& ci)
	{
	}
};

class PngEncoder : public Encoder
{
public:
	virtual void encodeImage(CameraImage& ci)
	{
		write_png_file(ci.raw.buffer, &ci.encoded);
	}

	void write_png_file(boost::shared_array<uint8_t> img, mem_encode* state)
	{
		/*
		static int test = 1;
		char filename[100];
		sprintf(filename, "test%d.png", test);
		++test;
		FILE* f = fopen(filename, "wb+");
		*/
		png_structp png_ptr;
		png_infop info_ptr;
		int number_of_passes;

		png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

		png_bytep * row_pointers = (png_bytep*)png_malloc(png_ptr, 480 * sizeof(png_bytep));
		for(int h = 0; h < 480; ++h)
		{
			(row_pointers)[h] = (png_bytep)((char*)img.get()+(h*640*2));
		}
		/* initialize stuff */

		if (!png_ptr)
			abort_("[write_png_file] png_create_write_struct failed");

		/* if my_png_flush() is not needed, change the arg to NULL */
		png_set_write_fn(png_ptr, state, my_png_write_data, NULL);
		
		info_ptr = png_create_info_struct(png_ptr);
		if (!info_ptr)
			abort_("[write_png_file] png_create_info_struct failed");

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
		png_free (png_ptr, row_pointers);
		png_destroy_write_struct (&png_ptr, &info_ptr);
		//fclose(f);
	}
};

class Camera
{
public:
	Camera()
	{
		mLastUpdate = gettimeofday_ms();
		mFPS = 1000;
	}

	~Camera()
	{
	}

	bool initCamera()
	{
		
		if (freenect_init(&f_ctx, NULL) < 0) {
			printf("freenect_init() failed\n");
			return false;
		}
		freenect_set_log_level(f_ctx, FREENECT_LOG_DEBUG);
		freenect_select_subdevices(f_ctx, (freenect_device_flags)(FREENECT_DEVICE_CAMERA));
		
		int nr_devices = freenect_num_devices (f_ctx);
		printf ("Number of devices found: %d\n", nr_devices);
		
		int user_device_number = 0;
		
		if (nr_devices < 1)
			return false;
		
		if (freenect_open_device(f_ctx, &f_dev, user_device_number) < 0) {
			printf("Could not open device\n");
			return false;
		}
		
		int accelCount = 0;
		
		freenect_set_depth_callback(f_dev, depth_cb);
		freenect_set_depth_mode(f_dev, freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_11BIT));
		freenect_set_user(f_dev, (void*)this);
	}

	void start()
	{
		boost::thread encoderThread = boost::thread(&Encoder::encode, p);
		freenect_start_depth(f_dev);
		while (!die && freenect_process_events(f_ctx) >= 0) {
			usleep(1000);
		}
	}

	void stop()
	{
		freenect_stop_depth(f_dev);
		freenect_stop_video(f_dev);
	}

	void destroyCamera()
	{
		freenect_close_device(f_dev);
		freenect_shutdown(f_ctx);		
	}

	void addImage(uint8_t* buf, uint32_t size, uint32_t timestamp)
	{

		uint64_t t = gettimeofday_ms();
		if(((t - mLastUpdate) * .001) > (float)(1.0/mFPS))
		{
			mLastUpdate = t;
		}
		else
		{
			return;
		}
		printf("Adding!\n");
		CameraImage ci;
		ci.type = 1;
		ci.camera_timestamp = timestamp;
		ci.raw.size = size;
		ci.raw.buffer = boost::shared_array<uint8_t>(new uint8_t[size]);
		memcpy((void*)ci.raw.buffer.get(), buf, size);
		p->addImage(ci);
	}
	
	template<typename T>
	void setEncoder()
	{
		p.reset(new T());
	}

	void setFPS(int f)
	{
		mFPS = f;
	}
private:
	freenect_context *f_ctx;
	freenect_device *f_dev;
	boost::shared_ptr<Encoder> p;
	int mFPS;
	uint64_t mLastUpdate;
};

void depth_cb(freenect_device *dev, void *v_depth, uint32_t timestamp)
{
	Camera* c = (Camera*)freenect_get_user(dev);
	c->addImage((uint8_t*)v_depth, 640*480*2, timestamp);
}

int main(int argc, char** argv)
{
	namespace po = boost::program_options;
	po::options_description desc("options");
	desc.add_options()
		("help", "help message")
		("raw", "use raw")
		("png", "use png")
		("fps", po::value<int>(), "fps for recording");
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);    

	if (vm.count("help")) {
		std::cout << desc << std::endl;
		return 1;
	}
	
	Camera c;
	if(vm.count("raw"))
	{
		c.setEncoder<RawEncoder>();
	}
	else if (vm.count("png"))
	{
		c.setEncoder<PngEncoder>();
	}
	else
	{
		std::cout << "Encoder type required!" << std::endl;
		return 1;
	}

	if(vm.count("fps"))
	{
		c.setFPS(vm["fps"].as<int>());
	}
	c.initCamera();
	c.start();
	c.destroyCamera();
	return 0;
}
