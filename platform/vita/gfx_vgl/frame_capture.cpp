// SDK domain: PNG output of one finished frame (both backends).
#include "frame_capture.hpp"
#include "runtime/telemetry.hpp"
#include <png.h>
#include <csetjmp>
#include <cstdio>

namespace twv::gpu {
bool write_png_rgba(const char* path,const unsigned char* pixels,uint32_t width,uint32_t height,
                    size_t stride,bool bottom_up) {
    FILE* file=std::fopen(path,"wb");
    if(!file){twv::vita::telemetryText("capture_error",path);return false;}
    png_structp png=png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
    png_infop info=png?png_create_info_struct(png):nullptr;
    if(!png || !info || setjmp(png_jmpbuf(png))) {
        if(png)png_destroy_write_struct(&png,info?&info:nullptr);
        std::fclose(file);twv::vita::telemetryText("capture_error",path);return false;
    }
    png_init_io(png,file);
    // Fast settings: these are evidence files, read back by tools/vita_gpu_compare.py.
    png_set_compression_level(png,1);
    png_set_IHDR(png,info,width,height,8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png,info);
    for(uint32_t row=0;row<height;++row) {
        const uint32_t y=bottom_up?height-1-row:row;
        png_write_row(png,const_cast<png_bytep>(pixels+size_t(y)*stride));
    }
    png_write_end(png,info);png_destroy_write_struct(&png,&info);
    const bool closed=std::fclose(file)==0;
    if(closed)twv::vita::telemetryText("frame_capture",path);
    else twv::vita::telemetryText("capture_error",path);
    return closed;
}
}
