/*
 * bench_gl: bench.h's workload through my OpenGL ES library (libredwood-gl over libOSMesa's freedreno, the
 * path backboardd draws the UI with), into a texture framebuffer (no read-back).
 * Usage: bench_gl [quads ...]   (default 20 150 500)
 */
#include <dlfcn.h>
#include <stdbool.h>
#include <string.h>
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl3.h>
#include "bench.h"

typedef void *(*create_fn)(int api);
typedef bool (*current_fn)(void *gl);
typedef void *(*proc_fn)(const char *name);
typedef const char *(*library_fn)(void);

static proc_fn proc;
#define GLFN(type, name) static type name
GLFN(PFNGLGENTEXTURESPROC, glGenTextures);
GLFN(PFNGLBINDTEXTUREPROC, glBindTexture);
GLFN(PFNGLTEXIMAGE2DPROC, glTexImage2D);
GLFN(PFNGLTEXPARAMETERIPROC, glTexParameteri);
GLFN(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
GLFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
GLFN(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
GLFN(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
GLFN(PFNGLCREATESHADERPROC, glCreateShader);
GLFN(PFNGLSHADERSOURCEPROC, glShaderSource);
GLFN(PFNGLCOMPILESHADERPROC, glCompileShader);
GLFN(PFNGLGETSHADERIVPROC, glGetShaderiv);
GLFN(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
GLFN(PFNGLCREATEPROGRAMPROC, glCreateProgram);
GLFN(PFNGLATTACHSHADERPROC, glAttachShader);
GLFN(PFNGLLINKPROGRAMPROC, glLinkProgram);
GLFN(PFNGLUSEPROGRAMPROC, glUseProgram);
GLFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
GLFN(PFNGLUNIFORM4FPROC, glUniform4f);
GLFN(PFNGLUNIFORM1IPROC, glUniform1i);
GLFN(PFNGLVIEWPORTPROC, glViewport);
GLFN(PFNGLCLEARCOLORPROC, glClearColor);
GLFN(PFNGLCLEARPROC, glClear);
GLFN(PFNGLENABLEPROC, glEnable);
GLFN(PFNGLBLENDFUNCPROC, glBlendFunc);
GLFN(PFNGLDRAWARRAYSPROC, glDrawArrays);
GLFN(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
GLFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
GLFN(PFNGLFLUSHPROC, glFlush);
GLFN(PFNGLFINISHPROC, glFinish);
GLFN(PFNGLGETSTRINGPROC, glGetString);
GLFN(PFNGLGETERRORPROC, glGetError);
GLFN(PFNGLREADPIXELSPROC, glReadPixels);

static void *
need(const char *name)
{
	void *f = proc(name);

	if (f == NULL) {
		fprintf(stderr, "no %s\n", name);
		exit(1);
	}
	return f;
}

static const char *vs_text =
    "#version 300 es\n"
    "uniform vec4 rect;\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  uv = c;\n"
    "  gl_Position = vec4(rect.xy + c * rect.zw, 0.0, 1.0);\n"
    "}\n";
static const char *fs_text =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "void main() { color = texture(tex, uv); }\n";

static GLuint
shader(GLenum kind, const char *text)
{
	GLuint s = glCreateShader(kind);
	GLint ok = 0;
	char log[1024];

	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

int
main(int argc, char **argv)
{
	void *lib = dlopen("/var/jb/usr/lib/libredwood-gl.dylib", RTLD_NOW);

	if (lib == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		return 1;
	}
	create_fn create = (create_fn)dlsym(lib, "redwood_gl_create");
	current_fn current = (current_fn)dlsym(lib, "redwood_gl_make_current");
	library_fn library = (library_fn)dlsym(lib, "redwood_gl_library");
	proc = (proc_fn)dlsym(lib, "redwood_gl_proc");
	double t0 = bench_ms();
	void *gl = create(3);
	if (gl == NULL || !current(gl)) {
		fprintf(stderr, "no OpenGL ES 3 context from %s\n", library ? library() : "?");
		return 1;
	}
#define GET(name) name = need(#name)
	GET(glGenTextures); GET(glBindTexture); GET(glTexImage2D); GET(glTexParameteri); GET(glGenFramebuffers);
	GET(glBindFramebuffer); GET(glFramebufferTexture2D); GET(glCheckFramebufferStatus); GET(glCreateShader);
	GET(glShaderSource); GET(glCompileShader); GET(glGetShaderiv); GET(glGetShaderInfoLog); GET(glCreateProgram);
	GET(glAttachShader); GET(glLinkProgram); GET(glUseProgram); GET(glGetUniformLocation); GET(glUniform4f);
	GET(glUniform1i); GET(glViewport); GET(glClearColor); GET(glClear); GET(glEnable); GET(glBlendFunc);
	GET(glDrawArrays); GET(glGenVertexArrays); GET(glBindVertexArray); GET(glFlush); GET(glFinish);
	GET(glGetString); GET(glGetError); GET(glReadPixels);
	printf("GL: %s / %s, context in %.1f ms\n", (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION), bench_ms() - t0);

	GLuint target, fb, tex[TEXTURES], vao;
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete\n");
		return 1;
	}
	uint8_t *px = malloc(TEX * TEX * 4);
	glGenTextures(TEXTURES, tex);
	for (int t = 0; t < TEXTURES; t++) {
		bench_texture(t, px);
		glBindTexture(GL_TEXTURE_2D, tex[t]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX, TEX, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	t0 = bench_ms();
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_text));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_text));
	glLinkProgram(prog);
	glUseProgram(prog);
	printf("GL program compiled and linked in %.1f ms\n", bench_ms() - t0);
	GLint rect = glGetUniformLocation(prog, "rect");
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glViewport(0, 0, W, H);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glFinish();

	int sizes[8] = { 20, 150, 500 }, nsizes = 3;
	if (argc > 1) {
		nsizes = 0;
		for (int i = 1; i < argc && nsizes < 8; i++) {
			sizes[nsizes++] = atoi(argv[i]);
		}
	}
	for (int s = 0; s < nsizes; s++) {
		int quads = sizes[s];
		double cpu[FRAMES], frame[FRAMES];

		for (int pass = 0; pass < 2; pass++) {
			double begin = bench_ms();

			for (int f = 0; f < FRAMES; f++) {
				double a = bench_ms();

				glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
				glBindTexture(GL_TEXTURE_2D, tex[0]);
				glUniform4f(rect, -1, -1, 2, 2);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				for (int i = 0; i < quads; i++) {
					float q[4];

					bench_quad(i, f, q);
					glBindTexture(GL_TEXTURE_2D, tex[i % TEXTURES]);
					glUniform4f(rect, q[0], q[1], q[2], q[3]);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				}
				glFlush();
				double b = bench_ms();
				if (pass == 0) {
					glFinish();
					cpu[f] = b - a;
					frame[f] = bench_ms() - a;
				}
			}
			if (pass == 1) {
				glFinish();
				bench_report("GL    ", quads, cpu, frame, FRAMES, (bench_ms() - begin) / FRAMES);
			}
		}
	}
	/* one pixel, to be sure something was drawn */
	uint8_t p[4] = { 0 };
	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
	printf("GL centre pixel %u/%u/%u/%u, error %#x\n", p[0], p[1], p[2], p[3], glGetError());
	return 0;
}
