/*
 * s8dfdx: are fragment shader derivatives right on the Mali-G71 (Mesa Panfrost, OSMesa ES 3)?
 *
 * CoreAnimation's OpenGL ES renderer draws rounded shapes (the Search field, the dock, folder
 * platters) with a shader that edges them by fwidth() of a superellipse ("supercircleImage").
 * Drawn by the bridge (tools/dreamlte/ca) on the S8 those come out as their whole rectangle,
 * half covered, as if fwidth() were far too big. This draws a 256 x 256 quad with three
 * shaders, GLSL ES 3.00 as the bridge hands CoreAnimation's to Mesa, reads pixels back and
 * reports them next to what they should be ("S8 MALI: s8dfdx report N: a b"):
 *   700 + i   shader 0, pixel i: (x | y << 16), RGBA word read back (R low byte)
 *             r = dFdx(gl_FragCoord.x) / 4, g = dFdy(gl_FragCoord.y) / 4 (each 64 if right),
 *             b = fwidth(uv.x) * 64 with uv = gl_FragCoord.xy / 128 - 1 (128 if right)
 *   720 + i   shader 1 (CoreAnimation's supercircleImage, uv from a varying -1..1):
 *             r = its coverage l, g = fwidth(d) * 16, b = d / 2
 *   740 + i   expected for shader 1: r, g, b worked out here on the CPU
 *   760 + i   shader 2, the same as 1 but the derivative of the varying itself:
 *             r = fwidth(uv.x) * 64 (2 / 256 * 64 = 0.5: 128), g = fwidth(uv.y) * 64
 *   780 + i   shader 3: a 1024 x 1024 texture, every mipmap level i of it R = 20 * i, sampled
 *             with trilinear filtering over the 256 x 256 quad: the texture unit's own
 *             derivatives should pick level 2 (R = 40); level 0 (R = 0) if quads were broken
 *   799       glGetError, link status bits (1 << shader)
 *
 * Seen (c57-c59): shader 0 r = g = 0, shader 2 = 2|uv| (the other lane reads as zero), shader 3
 * level 2 everywhere (the texture unit's derivatives are right). A Mesa built to ask for the other
 * lane with CLPER_OLD's operands swapped, or its lane number times 4 or 256, gave the same zeros;
 * the full CLPER encoding faulted the job (c59; that experiment is not kept in Mesa).
 *
 *   xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
 *       -I research/mesa-ios/src/mesa-25.0.7/include -o s8dfdx tools/dreamlte/s8dfdx.c \
 *       research/mesa-ios/out/ca/libOSMesa.8.dylib -framework IOKit -framework CoreFoundation
 *   codesign -s - s8dfdx      (run from the drop zone with ca-libOSMesa.8.dylib before it)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/osmesa.h>
#include <GLES3/gl3.h>

#define OSMESA_ES_PROFILE 0x38

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallScalarMethod(io_connect_t connection, uint32_t selector,
    const uint64_t *input, uint32_t inputCnt, uint64_t *output, uint32_t *outputCnt);

static io_connect_t conn;

static void
report(uint64_t code, uint64_t a, uint64_t b)
{
	uint64_t in[3] = { code, a, b };
	uint32_t n = 0;
	if (conn != 0) {
		IOConnectCallScalarMethod(conn, 7, in, 3, NULL, &n);
	}
	printf("report %llu: 0x%llx 0x%llx\n", code, a, b);
}

#define W 256
#define H 256

static const char vertex[] =
    "#version 300 es\n"
    "out highp vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1) * 2. - 1.;\n"
    "  uv = p;\n"
    "  gl_Position = vec4(p, 0., 1.);\n"
    "}\n";

static const char *const fragments[4] = {
	"#version 300 es\n"
	"#define lowp highp\n#define mediump highp\n"
	"precision highp float;\n"
	"in highp vec2 uv;\n"
	"layout(location = 0) out highp vec4 colour;\n"
	"void main() {\n"
	"  vec2 u = gl_FragCoord.xy / 128. - 1.;\n"
	"  colour = vec4(dFdx(gl_FragCoord.x) / 4., dFdy(gl_FragCoord.y) / 4., fwidth(u.x) * 64., 1.);\n"
	"}\n",

	/* CoreAnimation's own (QuartzCore 16.7.16, program 70 of the c55 trace), as the bridge passes it */
	"#version 300 es\n"
	"#define lowp highp\n#define mediump highp\n"
	"precision highp float;\n"
	"in highp vec2 uv;\n"
	"layout(location = 0) out highp vec4 colour;\n"
	"highp float dd, ww;\n"
	"lowp vec4 supercircleImage(highp vec2 uv) {\n"
	"  highp float d, w;\n"
	"  uv = max(abs(uv) * 1.199 - vec2(0.199), 0.0);  d = pow(uv.x, 2.68) + pow(uv.y, 2.68);\n"
	"  w = max(fwidth(d), 1e-4);\n"
	"  dd = d; ww = w;\n"
	"  mediump float l = clamp((w * .5 + (1. - d)) / w, 0., 1.);\n"
	"  return vec4(l);\n"
	"}\n"
	"void main() {\n"
	"  lowp vec4 s1 = supercircleImage(uv);\n"
	"  colour = vec4(s1.a, ww * 16., dd / 2., 1.);\n"
	"}\n",

	"#version 300 es\n"
	"precision highp float;\n"
	"in highp vec2 uv;\n"
	"layout(location = 0) out highp vec4 colour;\n"
	"void main() {\n"
	"  colour = vec4(fwidth(uv.x) * 64., fwidth(uv.y) * 64., 0., 1.);\n"
	"}\n",

	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2D levels;\n"
	"in highp vec2 uv;\n"
	"layout(location = 0) out highp vec4 colour;\n"
	"void main() {\n"
	"  colour = vec4(texture(levels, uv * .5 + .5).r, 0., 0., 1.);\n"
	"}\n",
};

static const int probes[][2] = {
	{ 128, 128 }, { 64, 64 }, { 10, 10 }, { 2, 128 }, { 128, 2 }, { 250, 128 }, { 200, 200 }, { 230, 230 },
	{ 240, 30 }, { 245, 245 },
};
#define PROBES (sizeof(probes) / sizeof(probes[0]))

static GLuint
program(const char *fragment, int *linked)
{
	GLuint p = glCreateProgram(), shaders[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
	const char *texts[2] = { vertex, fragment };
	GLint good = 0;
	char log[512];

	for (int i = 0; i < 2; i++) {
		glShaderSource(shaders[i], 1, &texts[i], NULL);
		glCompileShader(shaders[i]);
		glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &good);
		if (!good) {
			glGetShaderInfoLog(shaders[i], sizeof(log), NULL, log);
			printf("shader %d: %s\n", i, log);
		}
		glAttachShader(p, shaders[i]);
	}
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &good);
	*linked = good;
	return p;
}

/* What shader 1 gives at the middle of pixel (x, y), worked out here; w from neighbouring pixels. */
static double
super_d(double x, double y)
{
	double u = fabs((x + .5) / 128 - 1), v = fabs((y + .5) / 128 - 1);

	u = fmax(u * 1.199 - .199, 0);
	v = fmax(v * 1.199 - .199, 0);
	return pow(u, 2.68) + pow(v, 2.68);
}

static uint8_t
unorm(double value)
{
	return (uint8_t)lround(fmin(fmax(value, 0), 1) * 255);
}

int
main(void)
{
	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("S8Mali"));
	static uint32_t pixels[W * H];
	const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE, OSMESA_CONTEXT_MAJOR_VERSION, 3,
		OSMESA_CONTEXT_MINOR_VERSION, 0, 0
	};
	OSMesaContext context;
	GLuint vao, texture, framebuffer;
	int linked_bits = 0;

	if (svc != 0) {
		IOServiceOpen(svc, mach_task_self(), 0, &conn);
		IOObjectRelease(svc);
	}
	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, pixels, GL_UNSIGNED_BYTE, W, H)) {
		report(799, 0xdead, 0);
		return 1;
	}
	printf("%s\n", (const char *)glGetString(GL_RENDERER));
	/* into a texture of our own, as the bridge draws (not OSMesa's buffer) */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glViewport(0, 0, W, H);
	{
		GLuint levels;
		static uint32_t level[1024 * 1024];

		glActiveTexture(GL_TEXTURE1);
		glGenTextures(1, &levels);
		glBindTexture(GL_TEXTURE_2D, levels);
		for (int i = 0, size = 1024; size >= 1; i++, size /= 2) {
			for (int k = 0; k < size * size; k++) {
				level[k] = (uint32_t)(20 * i) | 0xff000000u;
			}
			glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, level);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glActiveTexture(GL_TEXTURE0);
	}
	for (int s = 0; s < 4; s++) {
		int linked = 0;
		GLuint p = program(fragments[s], &linked);

		linked_bits |= linked << s;
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(p);
		glUniform1i(glGetUniformLocation(p, "levels"), 1);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		for (unsigned i = 0; i < PROBES; i++) {
			int x = probes[i][0], y = probes[i][1];

			report((s == 0 ? 700 : s == 1 ? 720 : s == 2 ? 760 : 780) + i, (uint64_t)(x | y << 16), pixels[y * W + x]);
			if (s == 1) {
				double d = super_d(x, y);
				double w = fmax(fabs(super_d(x + 1, y) - d) + fabs(super_d(x, y + 1) - d), 1e-4);
				double l = fmin(fmax((w * .5 + (1 - d)) / w, 0), 1);
				uint32_t want = unorm(l) | unorm(w * 16) << 8 | unorm(d / 2) << 16 | 0xffu << 24;

				report(740 + i, (uint64_t)(x | y << 16), want);
			}
		}
	}
	report(799, glGetError(), (uint64_t)linked_bits);

	return 0;
}
