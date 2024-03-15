/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * egl_preview.cpp - X/EGL-based preview window.
 */

#include <map>
#include <string>

// Include libcamera stuff before X11, as X11 #defines both Status and None
// which upsets the libcamera headers.

#include "core/options.hpp"

#include "preview.hpp"

#include <libdrm/drm_fourcc.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
// We don't use Status below, so we could consider #undefining it here.
// We do use None, so if we had to #undefine it we could replace it by zero
// in what follows below.

#include <epoxy/egl.h>
#include <epoxy/gl.h>


#include <ft2build.h>
#include FT_FREETYPE_H

class EglPreview : public Preview
{
public:
	EglPreview(Options const *options);
	~EglPreview();
	virtual void SetInfoText(const std::string &text) override;
	// Display the buffer. You get given the fd back in the BufferDoneCallback
	// once its available for re-use.
	virtual void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	virtual void Reset() override;
	// Check if the window manager has closed the preview.
	virtual bool Quit() override;
	// Return the maximum image size allowed.
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = max_image_width_;
		h = max_image_height_;
	}


	virtual void ShowCountdown(int val, float alpha) override;
	virtual void HideCountdown() override;

private:
	struct Buffer
	{
		Buffer() : fd(-1) {}
		int fd;
		size_t size;
		StreamInfo info;
		GLuint texture;
	};

	struct Chara {
		unsigned int texture;
		float verts[8];
	};

	void makeWindow(char const *name);
	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
	
	void gl_setup(int width, int height, int window_width, int window_height);

	::Display *display_;
	EGLDisplay egl_display_;
	Window window_;
	EGLContext egl_context_;
	EGLSurface egl_surface_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	bool first_time_;
	Atom wm_delete_window_;
	// size of preview window
	int x_;
	int y_;
	int width_;
	int height_;
	unsigned int max_image_width_;
	unsigned int max_image_height_;

	bool countdownVisible_;
	int countdownVal_;
	float countdownAlpha_;

	FT_Library ft_;
	FT_Face face_;

	std::map<int, Chara> charas_;

	GLuint progImage_;
	GLuint progCountdown_;
	GLuint alphaUniform_;

	const int FONTSIZE = 400;

	float vertsImage_[8];
	float vertsTexCoords_[8] = { 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0 };

};

static GLint compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);
	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);

	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		GLchar *info;
		GLint size;

		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
		info = (GLchar *)malloc(size);

		glGetShaderInfoLog(s, size, NULL, info);
		throw std::runtime_error("failed to compile shader: " + std::string(info) + "\nsource:\n" +
								 std::string(source));
	}

	return s;
}

static GLint link_program(GLint vs, GLint fs)
{
	GLint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		/* Some drivers return a size of 1 for an empty log.  This is the size
		 * of a log that contains only a terminating NUL character.
		 */
		GLint size;
		GLchar *info = NULL;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
		if (size > 1)
		{
			info = (GLchar *)malloc(size);
			glGetProgramInfoLog(prog, size, NULL, info);
		}

		throw std::runtime_error("failed to link: " + std::string(info ? info : "<empty log>"));
	}

	return prog;
}

void EglPreview::gl_setup(int width, int height, int window_width, int window_height)
{
	float w_factor = width / (float)window_width;
	float h_factor = height / (float)window_height;
	float max_dimension = std::max(w_factor, h_factor);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	w_factor /= max_dimension;
	h_factor /= max_dimension;
	char vs[256];
	snprintf(vs, sizeof(vs),
			 "attribute vec4 pos;\n"
			 "varying vec2 texcoord;\n"
			 "\n"
			 "void main() {\n"
			 "  gl_Position = pos;\n"
			 "  texcoord.x = pos.x / %f + 0.5;\n"
			 "  texcoord.y = 0.5 - pos.y / %f;\n"
			 "}\n",
			 2.0 * w_factor, 2.0 * h_factor);
	vs[sizeof(vs) - 1] = 0;
	GLint vs_s = compile_shader(GL_VERTEX_SHADER, vs);
	const char *fs = "#extension GL_OES_EGL_image_external : enable\n"
					 "precision mediump float;\n"
					 "uniform samplerExternalOES s;\n"
					 "varying vec2 texcoord;\n"
					 "void main() {\n"
					 "  gl_FragColor = texture2D(s, texcoord);\n"
					 "}\n";
	GLint fs_s = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLint prog = link_program(vs_s, fs_s);

	progImage_ = prog;


	
	const char *vs_2 = 	"#version 310 es\n"
				"layout (location=0) in vec4 pos;\n"
				"layout (location=1) in vec2 texcoord;\n"
				"uniform float ialphafac;\n"
				"out vec2 tc;\n"
				"out float alphafac;\n"	
				"void main() {\n"
				"	vec4 tmp = pos;\n"
			        "	tmp.x *= ((2.0 - ialphafac));\n"
			        "	tmp.y *= ((2.0 - ialphafac));\n"
				"	gl_Position = tmp;\n"
				"	tc = texcoord;\n"
				"	alphafac = ialphafac;\n"
				"}\n";

	



	const char *fs_2 =	"#version 310 es\n" 
				"precision mediump float;\n"
				"uniform sampler2D sc;\n"
				"in vec2 tc;\n"
				"in float alphafac;\n"
				"out vec4 FragColor;\n"
				"void main() {\n"
				"  vec4 tmp = texture2D(sc, tc);\n"
				"  tmp.a *= alphafac;\n"
				"  FragColor = tmp;\n"
				"}\n";


	
	static const float verts[] = { -w_factor, -h_factor, w_factor, -h_factor, w_factor, h_factor, -w_factor, h_factor };
	static const float vertsn[] = { -1,-1, 1, -1, 1, 1, -1, 1 };
	vs_s = compile_shader(GL_VERTEX_SHADER, vs_2);
	fs_s = compile_shader(GL_FRAGMENT_SHADER, fs_2);
	progCountdown_ = link_program(vs_s, fs_s);

	alphaUniform_ = glGetUniformLocation(progCountdown_, "ialphafac");


	unsigned int tex;
	Chara c;

	int w, h;

	for (int i = 0; i < 9; i++) {
	
		FT_Load_Char(face_, '0' + i, FT_LOAD_RENDER);

		w = face_->glyph->bitmap.width;
		h = face_->glyph->bitmap.rows;


		glGenTextures(1, &tex);

		glBindTexture(GL_TEXTURE_2D, tex);


//		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

		uint32_t * buffer = (uint32_t*) malloc(sizeof(uint32_t) * w * h);

		for (int j = 0; j < (w*h); j++) {
			if ( i == 0) {
				// white plane instead of 0
				buffer[j] = 0xffffffff;
			} else {
				buffer[j] = face_->glyph->bitmap.buffer[j] 
					| (face_->glyph->bitmap.buffer[j] << 8) 
					| (face_->glyph->bitmap.buffer[j] << 16) 
					| (face_->glyph->bitmap.buffer[j] << 24);
					//| 0xff000000;	
			}
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);

		free(buffer);

		for (int j = 0; j < 8; j += 2) {
			if (i == 0) {
				c.verts[j] = verts[j] * 4;
				c.verts[j + 1] = verts[j + 1] * 4;
			} else {
				c.verts[j] = (vertsn[j] * w) / window_width ;
				c.verts[j + 1] = (vertsn[j + 1] * h) / window_height ;
			}
		}

		/*
		for (int j = 0; j < 8; j++) 
			printf("%f ", c.verts[j]);
		printf("\n");
		*/
		
		c.texture = tex;
		charas_.insert(std::pair<int, Chara>(i, c));
	}





	
	for (int i = 0; i < 8; i++) {
		vertsImage_[i] = verts[i];
	}

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertsImage_);
	glEnableVertexAttribArray(0);
	
	glUseProgram(progImage_);
}

EglPreview::EglPreview(Options const *options) : Preview(options), last_fd_(-1), first_time_(true)
{
	
	display_ = XOpenDisplay(NULL);
	if (!display_)
		throw std::runtime_error("Couldn't open X display");

	egl_display_ = eglGetDisplay(display_);
	if (!egl_display_)
		throw std::runtime_error("eglGetDisplay() failed");

	EGLint egl_major, egl_minor;

	if (!eglInitialize(egl_display_, &egl_major, &egl_minor))
		throw std::runtime_error("eglInitialize() failed");

	x_ = options_->preview_x;
	y_ = options_->preview_y;
	width_ = options_->preview_width;
	height_ = options_->preview_height;
	makeWindow("libcamera-app");

	countdownVisible_ = false;

	if (FT_Init_FreeType(&ft_)) {
		LOG(2, "Error initing freetype");
	}

	if (FT_New_Face(ft_, "/usr/share/fonts/truetype/piboto/Piboto-Regular.ttf", 0, &face_)) {
		LOG(2, "Error loading font!");	
	}

	FT_Set_Pixel_Sizes(face_, 0, FONTSIZE);

	// gl_setup() has to happen later, once we're sure we're in the display thread.
	//
	
}

EglPreview::~EglPreview()
{
}

static void no_border(Display *display, Window window)
{
	static const unsigned MWM_HINTS_DECORATIONS = (1 << 1);
	static const int PROP_MOTIF_WM_HINTS_ELEMENTS = 5;

	typedef struct
	{
		unsigned long flags;
		unsigned long functions;
		unsigned long decorations;
		long inputMode;
		unsigned long status;
	} PropMotifWmHints;

	PropMotifWmHints motif_hints;
	Atom prop, proptype;
	unsigned long flags = 0;

	/* setup the property */
	motif_hints.flags = MWM_HINTS_DECORATIONS;
	motif_hints.decorations = flags;

	/* get the atom for the property */
	prop = XInternAtom(display, "_MOTIF_WM_HINTS", True);
	if (!prop)
	{
		/* something went wrong! */
		return;
	}

	/* not sure this is correct, seems to work, XA_WM_HINTS didn't work */
	proptype = prop;

	XChangeProperty(display, window, /* display, window */
					prop, proptype, /* property, type */
					32, /* format: 32-bit datums */
					PropModeReplace, /* mode */
					(unsigned char *)&motif_hints, /* data */
					PROP_MOTIF_WM_HINTS_ELEMENTS /* nelements */
	);
}

void EglPreview::makeWindow(char const *name)
{
	int screen_num = DefaultScreen(display_);
	XSetWindowAttributes attr;
	unsigned long mask;
	Window root = RootWindow(display_, screen_num);
	int screen_width = DisplayWidth(display_, screen_num);
	int screen_height = DisplayHeight(display_, screen_num);

	// Default behaviour here is to use a 1024x768 window.
	if (width_ == 0 || height_ == 0)
	{
		width_ = 1024;
		height_ = 768;
	}

	if (options_->fullscreen || x_ + width_ > screen_width || y_ + height_ > screen_height)
	{
		x_ = y_ = 0;
		width_ = DisplayWidth(display_, screen_num);
		height_ = DisplayHeight(display_, screen_num);
	}

	static const EGLint attribs[] =
		{
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};
	EGLConfig config;
	EGLint num_configs;
	if (!eglChooseConfig(egl_display_, attribs, &config, 1, &num_configs))
		throw std::runtime_error("couldn't get an EGL visual config");

	EGLint vid;
	if (!eglGetConfigAttrib(egl_display_, config, EGL_NATIVE_VISUAL_ID, &vid))
		throw std::runtime_error("eglGetConfigAttrib() failed\n");

	XVisualInfo visTemplate = {};
	visTemplate.visualid = (VisualID)vid;
	int num_visuals;
	XVisualInfo *visinfo = XGetVisualInfo(display_, VisualIDMask, &visTemplate, &num_visuals);

	/* window attributes */
	attr.background_pixel = 0;
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap(display_, root, visinfo->visual, AllocNone);
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
	/* XXX this is a bad way to get a borderless window! */
	mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

	window_ = XCreateWindow(display_, root, x_, y_, width_, height_, 0, visinfo->depth, InputOutput, visinfo->visual,
							mask, &attr);

	if (options_->fullscreen)
		no_border(display_, window_);

	/* set hints and properties */
	{
		XSizeHints sizehints;
		sizehints.x = x_;
		sizehints.y = y_;
		sizehints.width = width_;
		sizehints.height = height_;
		sizehints.flags = USSize | USPosition;
		XSetNormalHints(display_, window_, &sizehints);
		XSetStandardProperties(display_, window_, name, name, None, (char **)NULL, 0, &sizehints);
	}

	eglBindAPI(EGL_OPENGL_ES_API);

	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, ctx_attribs);
	if (!egl_context_)
		throw std::runtime_error("eglCreateContext failed");

	XFree(visinfo);

	XMapWindow(display_, window_);

	// This stops the window manager from closing the window, so we get an event instead.
	wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(display_, window_, &wm_delete_window_, 1);

	egl_surface_ = eglCreateWindowSurface(egl_display_, config, reinterpret_cast<EGLNativeWindowType>(window_), NULL);
	if (!egl_surface_)
		throw std::runtime_error("eglCreateWindowSurface failed");

	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
	int max_texture_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);




	// FFE 


}

static void get_colour_space_info(std::optional<libcamera::ColorSpace> const &cs, EGLint &encoding, EGLint &range)
{
	encoding = EGL_ITU_REC601_EXT;
	range = EGL_YUV_NARROW_RANGE_EXT;

	if (cs == libcamera::ColorSpace::Sycc)
		range = EGL_YUV_FULL_RANGE_EXT;
	else if (cs == libcamera::ColorSpace::Smpte170m)
		/* all good */;
	else if (cs == libcamera::ColorSpace::Rec709)
		encoding = EGL_ITU_REC709_EXT;
	else
		LOG(1, "EglPreview: unexpected colour space " << libcamera::ColorSpace::toString(cs));
}

void EglPreview::makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer)
{
	if (first_time_)
	{
		// This stuff has to be delayed until we know we're in the thread doing the display.
		if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
			throw std::runtime_error("eglMakeCurrent failed");
		gl_setup(info.width, info.height, width_, height_);
		first_time_ = false;
	}

	buffer.fd = fd;
	buffer.size = size;
	buffer.info = info;

	EGLint encoding, range;
	get_colour_space_info(info.colour_space, encoding, range);

	EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(info.width),
		EGL_HEIGHT, static_cast<EGLint>(info.height),
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.stride),
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height),
		EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_DMA_BUF_PLANE2_FD_EXT, fd,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height + (info.stride / 2) * (info.height / 2)),
		EGL_DMA_BUF_PLANE2_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_YUV_COLOR_SPACE_HINT_EXT, encoding,
		EGL_SAMPLE_RANGE_HINT_EXT, range,
		EGL_NONE
	};

	EGLImage image = eglCreateImageKHR(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!image)
		throw std::runtime_error("failed to import fd " + std::to_string(fd));

	glGenTextures(1, &buffer.texture);
	

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl_display_, image);
}

void EglPreview::SetInfoText(const std::string &text)
{
	if (!text.empty())
		XStoreName(display_, window_, text.c_str());
}

void EglPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{
	Buffer &buffer = buffers_[fd];
	if (buffer.fd == -1)
		makeBuffer(fd, span.size(), info, buffer);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(progImage_);
	glDisableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertsImage_);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);


	// VLO
	if (countdownVisible_) {
		if (countdownAlpha_ > 0.01) {
			glEnableVertexAttribArray(1);
			glUseProgram(progCountdown_);
			glUniform1f(alphaUniform_, countdownAlpha_);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, charas_[countdownVal_].verts);
			glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, vertsTexCoords_);
			glBindTexture(GL_TEXTURE_2D, charas_[countdownVal_].texture);
			glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
			countdownAlpha_ -= 0.10f;
		} else {
			countdownVisible_ = false;
		}
	}

	EGLBoolean success [[maybe_unused]] = eglSwapBuffers(egl_display_, egl_surface_);
	if (last_fd_ >= 0)
		done_callback_(last_fd_);
	last_fd_ = fd;
}

void EglPreview::Reset()
{
	for (auto &it : buffers_)
		glDeleteTextures(1, &it.second.texture);

	for (auto &it : charas_)
		glDeleteTextures(1, &it.second.texture);

	buffers_.clear();
	charas_.clear();
	last_fd_ = -1;
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	first_time_ = true;
}

bool EglPreview::Quit()
{
	XEvent event;
	while (XCheckTypedWindowEvent(display_, window_, ClientMessage, &event))
	{
		if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_)
			return true;
	}
	return false;
}


void EglPreview::ShowCountdown(int val, float alpha) 
{
	//countdownVisible_ = false;


	//glBindTexture(GL_TEXTURE_2D, countdownTexture_);

	//glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   
	//
	//
	
	//glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, face_->glyph->bitmap.width, face_->glyph->bitmap.rows, 0, GL_RED, GL_UNSIGNED_BYTE, face_->glyph->bitmap.buffer);

	countdownVisible_ = true;
	countdownVal_ = val;
	countdownAlpha_ = alpha;


}

void EglPreview::HideCountdown()
{
	countdownVisible_ = false;
}


Preview *make_egl_preview(Options const *options)
{
	return new EglPreview(options);
}
