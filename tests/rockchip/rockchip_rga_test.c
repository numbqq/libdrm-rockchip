/*
 * Copyright (C) 2017 Fuzhou Rcockhip Electronics Co.Ltd
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Yakir Yang <ykk@rock-chips.com>
 *    Jacob Chen <jacob2.chen@rock-chips.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/stddef.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libkms.h>
#include <drm_fourcc.h>

#include "rockchip_drm.h"
#include "rockchip_drmif.h"
#include "rockchip_rga.h"

#define DRM_MODULE_NAME		"rockchip"

struct rga_context *ctx;
struct timespec start, end;
unsigned long long time_consumed;

struct connector {
	uint32_t id;
	char mode_str[64];
	char format_str[5];
	unsigned int fourcc;
	drmModeModeInfo *mode;
	drmModeEncoder *encoder;
	int crtc;
	int pipe;
	int plane_zpos;
	unsigned int fb_id[2], current_fb_id;
	struct timeval start;

	int swap_count;
};

struct rga_test {
	struct rockchip_device *dev;
	struct rockchip_bo *dst_bo;
	struct rockchip_bo *src_bo;
	struct connector dst_con;

	struct rga_image src_img;
	struct rga_image dst_img;
};

static void connector_find_mode(int fd, struct connector *c,
				drmModeRes * resources)
{
	drmModeConnector *connector;
	int i, j;

	/* First, find the connector & mode */
	c->mode = NULL;

	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);

		if (!connector) {
			fprintf(stderr, "could not get connector %i: %s\n",
				resources->connectors[i], strerror(errno));
			drmModeFreeConnector(connector);
			continue;
		}

		if (!connector->count_modes) {
			drmModeFreeConnector(connector);
			continue;
		}

		if (connector->connector_id != c->id) {
			printf("available connector id %d \n",
			       connector->connector_id);
			drmModeFreeConnector(connector);
			continue;
		}

		for (j = 0; j < connector->count_modes; j++) {
			c->mode = &connector->modes[j];
			if (!strcmp(c->mode->name, c->mode_str))
				break;
		}

		/* Found it, break out */
		if (c->mode)
			break;

		drmModeFreeConnector(connector);
	}

	if (!c->mode) {
		fprintf(stderr, "failed to find mode \"%s\"\n", c->mode_str);
		return;
	}

	/* Now get the encoder */
	for (i = 0; i < resources->count_encoders; i++) {
		c->encoder = drmModeGetEncoder(fd, resources->encoders[i]);

		if (!c->encoder) {
			fprintf(stderr, "could not get encoder %i: %s\n",
				resources->encoders[i], strerror(errno));
			drmModeFreeEncoder(c->encoder);
			continue;
		}

		if (c->encoder->encoder_id == connector->encoder_id)
			break;

		drmModeFreeEncoder(c->encoder);
	}

	if (c->crtc == -1)
		c->crtc = c->encoder->crtc_id;
}

static int drm_set_crtc(struct rockchip_device *dev, struct connector *c,
			unsigned int fb_id)
{
	int ret;

	ret = drmModeSetCrtc(dev->fd, c->crtc, fb_id, 0, 0, &c->id, 1, c->mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		goto err;
	}

	return 0;

err:
	return ret;
}

static struct rockchip_bo *rockchip_create_buffer(struct rockchip_device *dev,
						  unsigned long size,
						  unsigned int flags)
{
	struct rockchip_bo *bo;

	bo = rockchip_bo_create(dev, size, flags);
	if (!bo)
		return bo;

	if (!rockchip_bo_map(bo)) {
		rockchip_bo_destroy(bo);
		return NULL;
	}

	return bo;
}

static void rockchip_destroy_buffer(struct rockchip_bo *bo)
{
	rockchip_bo_destroy(bo);
}

static int rga_color_fill_test(struct rga_test *test)
{
	int ret, count;
	struct rga_image *dst = &test->dst_img;

	printf("color fill test.\n");

	/*
	 * RGA API Related:
	 *
	 * Initialize the source framebuffer and dest framebuffer with BLACK color.
	 *
	 * The "->fill_color" variable is corresponding to RGA target color, and it's
	 * ARGB8888 format, like if you want the source framebuffer filled with
	 * RED COLOR, then you should fill the "->fill_color" with 0x00ff0000.
	 */
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (count = 0; count < 10; count++) {
		dst->fill_color = 0xff000000 + (random() & 0xffffff);
		rga_solid_fill(ctx, dst, 0, 0, dst->width, dst->height);
		ret = rga_exec(ctx);
		if (ret)
			return ret;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	time_consumed = (end.tv_sec - start.tv_sec) * 1000000000ULL;
	time_consumed += (end.tv_nsec - start.tv_nsec);
	time_consumed /= 10000;

	printf("*[RGA DEBUG]* : soid fill a %d*%d NV12 buffer use %llu usecs\n",
	       dst->width, dst->height, time_consumed);

	return 0;
}

static int rga_copy_test(struct rga_test *test)
{
	struct rga_image *src = &test->src_img;
	struct rga_image *dst = &test->dst_img;
	int ret, count;

	printf("copy test.\n");

	src->fill_color = 0xff0000ff;
	ret = rga_solid_fill(ctx, src, 0, 0, src->width, src->height);
	if (ret)
		return ret;

	/* clean screen */
	dst->fill_color = 0xff;
	ret = rga_solid_fill(ctx, dst, 0, 0, dst->width, dst->height);
	ret = rga_exec(ctx);
	if (ret)
		return ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (count = 0; count < 10; count++) {
		rga_copy(ctx, src, dst, 0, 0, 0, 0, dst->width, dst->height);
		ret = rga_exec(ctx);
		if (ret)
			return ret;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	time_consumed = (end.tv_sec - start.tv_sec) * 1000000000ULL;
	time_consumed += (end.tv_nsec - start.tv_nsec);
	time_consumed /= 10000;

	printf
	    ("*[RGA DEBUG]* : copy a %d*%d ARGB8888 buffer to NV12 buffer use %llu usecs\n",
	     dst->width, dst->height, time_consumed);

	return 0;
}

static int rga_scale_test(struct rga_test *test)
{
	struct rga_image *src = &test->src_img;
	struct rga_image *dst = &test->dst_img;
	unsigned int src_w, src_h;
	int ret, count;

	printf("scale test.\n");

	/* source size */
	src_w = dst->width / 2;
	src_h = dst->height / 2;

	/* create color bar */
	src->fill_color = 0xff0000ff;
	ret = rga_solid_fill(ctx, src, 0, 0, src_w, src_h / 3);
	src->fill_color = 0xff00ff00;
	ret = rga_solid_fill(ctx, src, 0, src_h / 3, src_w, src_h / 3);
	src->fill_color = 0xffff0000;
	ret = rga_solid_fill(ctx, src, 0, src_h * 2 / 3, src_w, src_h / 3);
	ret = rga_exec(ctx);
	if (ret)
		return ret;

	/* clean screen */
	dst->fill_color = 0x0;
	ret = rga_solid_fill(ctx, dst, 0, 0, dst->width, dst->height);
	ret = rga_exec(ctx);
	if (ret)
		return ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (count = 0; count < 10; count++) {
		rga_copy_with_scale(ctx, src, dst, 0, 0, src_w, src_h, 0, 0,
				    dst->width, dst->height);
		ret = rga_exec(ctx);
		if (ret)
			return ret;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	time_consumed = (end.tv_sec - start.tv_sec) * 1000000000ULL;
	time_consumed += (end.tv_nsec - start.tv_nsec);
	time_consumed /= 10000;

	printf
	    ("*[RGA DEBUG]* : scale a %d*%d ARGB8888 buffer to %d*%d NV12 use %llu usecs\n",
	     src_w, src_h, dst->width, dst->height, time_consumed);

	return 0;
}

static int rga_rotate_test(struct rga_test *test)
{
	struct rga_image *src = &test->src_img;
	struct rga_image *dst = &test->dst_img;
	unsigned int src_w, src_h;
	int ret, count;

	printf("rotate test.\n");

	src_w = dst->height;
	src_h = dst->width;

	/* change src buffer to nv12 to ensure enough size */
	src->stride = src->width;
	src->color_mode = DRM_FORMAT_NV12;

	/* create color bar */
	src->fill_color = 0xff0000ff;
	ret = rga_solid_fill(ctx, src, 0, 0, src_w, src_h / 3);
	src->fill_color = 0xff00ff00;
	ret = rga_solid_fill(ctx, src, 0, src_h / 3, src_w, src_h / 3);
	src->fill_color = 0xffff0000;
	ret = rga_solid_fill(ctx, src, 0, src_h * 2 / 3, src_w, src_h / 3);
	ret = rga_exec(ctx);
	if (ret)
		goto out;

	/* clean screen */
	dst->fill_color = 0x0;
	ret = rga_solid_fill(ctx, dst, 0, 0, dst->width, dst->height);
	ret = rga_exec(ctx);
	if (ret)
		goto out;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (count = 0; count < 10; count++) {
		rga_copy_with_rotate(ctx, src, dst, 0, 0, src_w, src_h, 0, 0,
				     dst->width, dst->height, 90);
		ret = rga_exec(ctx);
		if (ret)
			goto out;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	time_consumed = (end.tv_sec - start.tv_sec) * 1000000000ULL;
	time_consumed += (end.tv_nsec - start.tv_nsec);
	time_consumed /= 10000;

	printf("*[RGA DEBUG]* : rotate a %d*%d NV12 use %llu usecs\n",
	       src_w, src_h, time_consumed);

out:
	src->stride = src->width * 4;
	src->color_mode = DRM_FORMAT_ARGB8888;

	return ret;
}

static int rga_blend_test(struct rga_test *test)
{
	struct rockchip_device *dev = test->dev;
	struct rga_image test1 = { 0 }, test2 = { 0 };
	struct rockchip_bo *test1_bo, *test2_bo;
	int test1_fd, test2_fd, ret;
	unsigned int src_w, src_h, dst_w, dst_h;

	printf("blend test.\n");

	src_w = 1920;
	src_h = 1080;

	dst_w = 3840;
	dst_h = 2160;

	test1_bo = rockchip_create_buffer(dev, src_w * src_h * 4, 0);
	test2_bo = rockchip_create_buffer(dev, dst_w * dst_h * 4, 0);

	drmPrimeHandleToFD(dev->fd, test1_bo->handle, 0, &test1_fd);
	drmPrimeHandleToFD(dev->fd, test2_bo->handle, 0, &test2_fd);

	test1.bo[0] = test1_fd;
	test2.bo[0] = test2_fd;

	test1.width = src_w;
	test1.height = src_h;
	test1.stride = test1.width * 4;
	test1.buf_type = RGA_BUF_TYPE_GEMFD | RGA_BUF_TYPE_FLUSH;
	test1.color_mode = DRM_FORMAT_ARGB8888;

	test2.width = dst_w;
	test2.height = dst_h;
	test2.stride = test2.width * 4;
	test2.buf_type = RGA_BUF_TYPE_GEMFD | RGA_BUF_TYPE_FLUSH;
	test2.color_mode = DRM_FORMAT_ARGB8888;

	test1.fill_color = 0x000000ff;
	ret = rga_solid_fill(ctx, &test1, 0, 0, test1.width, test1.height);
	ret = rga_exec(ctx);
	if (ret)
		goto out;

	test2.fill_color = 0xff00ff00;
	ret =
	    rga_solid_fill(ctx, &test2, test2.width / 4, test2.height / 4,
			   test2.width / 2, test2.height / 2);
	ret = rga_exec(ctx);
	if (ret)
		goto out;

	clock_gettime(CLOCK_MONOTONIC, &start);
	rga_blend(ctx, &test1, &test2, 0, 0, test1.width, test1.height, 0, 0,
		  test2.width, test2.height, 0, RGA_OP_CONSTANT, 0xff, 0x80);
	ret = rga_exec(ctx);
	if (ret)
		goto out;
	clock_gettime(CLOCK_MONOTONIC, &end);

	time_consumed = (end.tv_sec - start.tv_sec) * 1000000000ULL;
	time_consumed += (end.tv_nsec - start.tv_nsec);
	time_consumed /= 1000;

	printf
	    ("*[RGA DEBUG]* : blend %d*%d ARGB8888 and %d*%d ARGB8888 buffers use %llu usecs\n",
	     test1.width, test1.height, test2.width, test2.height,
	     time_consumed);

	/* display the outcome */
	rga_copy_with_scale(ctx, &test2, &test->dst_img, 0, 0, test2.width,
			    test2.height, 0, 0, test->dst_img.width,
			    test->dst_img.height);
	ret = rga_exec(ctx);
	if (ret)
		return ret;

out:
	close(test1_fd);
	close(test2_fd);

	rockchip_destroy_buffer(test1_bo);
	rockchip_destroy_buffer(test2_bo);

	return ret;
}

static struct rockchip_bo *init_crtc(struct connector *con,
				     struct rockchip_device *dev)
{
	struct rockchip_bo *bo;
	unsigned int screen_width, screen_height;
	drmModeRes *resources;

	resources = drmModeGetResources(dev->fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
		return NULL;
	}

	connector_find_mode(dev->fd, con, resources);
	drmModeFreeResources(resources);
	if (!con->mode) {
		fprintf(stderr, "failed to find usable connector\n");
		return NULL;
	}

	screen_width = con->mode->hdisplay;
	screen_height = con->mode->vdisplay;

	if (screen_width == 0 || screen_height == 0) {
		fprintf(stderr,
			"failed to find sane resolution on connector\n");
		return NULL;
	}

	printf("screen width = %d, screen height = %d\n", screen_width,
	       screen_height);

	bo = rockchip_create_buffer(dev, screen_width * screen_height * 4, 0);
	if (!bo) {
		return NULL;
	}

	con->plane_zpos = -1;

	return bo;
}

static void wait_for_user_input(int last)
{
	printf("press <ENTER> to %s\n", last ? "exit test application" :
	       "skip to next test");

	getchar();
}

static int rga_run_test(struct rga_test *test)
{
	struct rockchip_device *dev = test->dev;
	struct rockchip_bo *dst_bo = test->dst_bo;
	struct rockchip_bo *src_bo = test->src_bo;
	struct connector *dst_con = &test->dst_con;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	struct rga_image src_img = { 0 }, dst_img = { 0 };
	unsigned int dst_fb_id, img_w, img_h;
	int ret, modes, dst_fd, src_fd;

	/*
	 * Dest FB Displayed Related:
	 *
	 * Add the dest framebuffer to DRM connector, note that for NV12
	 * display, the virtual stride is (width), that's why pitches[0]
	 * is hdisplay.
	 */
	modes = DRM_FORMAT_NV12;
	pitches[0] = dst_con->mode->hdisplay;
	handles[1] = dst_bo->handle;
	pitches[1] = dst_con->mode->hdisplay;
	offsets[1] = dst_con->mode->hdisplay * dst_con->mode->vdisplay;

	handles[0] = dst_bo->handle;
	offsets[0] = 0;

	ret =
	    drmModeAddFB2(dev->fd, dst_con->mode->hdisplay,
			  dst_con->mode->vdisplay, modes, handles, pitches,
			  offsets, &dst_fb_id, 0);
	if (ret < 0)
		return -EFAULT;

	ret = drm_set_crtc(dev, dst_con, dst_fb_id);
	if (ret < 0)
		return -EFAULT;

	/*
	 * RGA API Related:
	 *
	 * Due to RGA API only accept the fd of dma_buf, so we need
	 * to conver the dma_buf Handle to dma_buf FD.
	 *
	 * And then just assigned the src/dst framebuffer FD to the
	 * "struct rga_img".
	 *
	 * And for now, RGA driver only support GEM buffer type, so
	 * we also need to assign the src/dst buffer type to RGA_BUF_TYPE_GEMFD.
	 *
	 * For futher, I would try to add user point support.
	 */
	drmPrimeHandleToFD(dev->fd, dst_bo->handle, 0, &dst_fd);
	drmPrimeHandleToFD(dev->fd, src_bo->handle, 0, &src_fd);

	dst_img.bo[0] = dst_fd;
	src_img.bo[0] = src_fd;

	/*
	 * RGA API Related:
	 *
	 * Configure the source FB width / height / stride / color_mode.
	 * 
	 * The width / height is correspond to the framebuffer width /height
	 *
	 * The stride is equal to (width * pixel_width).
	 *
	 * The color_mode should configure to the standard DRM color format
	 * which defined in "/user/include/drm/drm_fourcc.h"
	 *
	 */

	img_w = dst_con->mode->hdisplay;
	img_h = dst_con->mode->vdisplay;

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.buf_type = RGA_BUF_TYPE_GEMFD;

	src_img.stride = img_w * 4;
	src_img.color_mode = DRM_FORMAT_ARGB8888;

	img_w = dst_con->mode->hdisplay;
	img_h = dst_con->mode->vdisplay;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.buf_type = RGA_BUF_TYPE_GEMFD;
	dst_img.stride = img_w;
	dst_img.color_mode = DRM_FORMAT_NV12;
	test->dst_img = dst_img;
	test->src_img = src_img;

	srand(time(NULL));

	ret = rga_color_fill_test(test);
	if (ret) {
		printf("*[RGA ERROR]*: Failed at color fill test\n");
		return ret;
	}

	wait_for_user_input(0);

	ret = rga_copy_test(test);
	if (ret) {
		printf("*[RGA ERROR]*: Failed at copy test\n");
		return ret;
	}

	wait_for_user_input(0);

	ret = rga_scale_test(test);
	if (ret) {
		printf("*[RGA ERROR]*: Failed at scale test\n");
		return ret;
	}

	wait_for_user_input(0);

	ret = rga_rotate_test(test);
	if (ret) {
		printf("*[RGA ERROR]*: Failed at roate test\n");
		return ret;
	}

	wait_for_user_input(0);

	ret = rga_blend_test(test);
	if (ret) {
		printf("*[RGA ERROR]*: Failed at blend test\n");
		return ret;
	}

	wait_for_user_input(1);

	/*
	 * Display Related:
	 *
	 * Released the display framebufffer refer which hold
	 * by DRM display framework
	 */
	drmModeRmFB(dev->fd, dst_fb_id);

	close(src_fd);
	close(dst_fd);

	return 0;
}

static void usage(char *name)
{
	fprintf(stderr, "usage: %s [-s]\n", name);
	fprintf(stderr, "-s <connector_id>@<crtc_id>:<mode>\n");
	exit(0);
}

extern char *optarg;
static const char optstr[] = "s:";

int main(int argc, char **argv)
{
	struct rockchip_device *dev;
	struct connector dst_con;
	struct rga_test test = { 0 };
	int fd, c;

	memset(&dst_con, 0, sizeof(struct connector));

	if (argc != 3) {
		usage(argv[0]);
		return -EINVAL;
	}

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 's':
			dst_con.crtc = -1;
			if (sscanf(optarg, "%d:0x%64s",
				   &dst_con.id,
				   dst_con.mode_str) != 2 &&
			    sscanf(optarg, "%d@%d:%64s",
				   &dst_con.id,
				   &dst_con.crtc, dst_con.mode_str) != 3)
				usage(argv[0]);
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	fd = drmOpen(DRM_MODULE_NAME, NULL);
	if (fd < 0) {
		fprintf(stderr, "failed to open.\n");
		return fd;
	}

	dev = rockchip_device_create(fd);
	if (!dev) {
		drmClose(dev->fd);
		return -EFAULT;
	}

	/*
	 * RGA API Related:
	 *
	 * Open the RGA device
	 */
	ctx = rga_init(dev->fd);
	if (!ctx) {
		fprintf(stderr, "failed to open rga.\n");
		return -EFAULT;
	}

	test.dst_bo = init_crtc(&dst_con, dev);
	test.dst_con = dst_con;
	test.dev = dev;

	test.src_bo =
	    rockchip_create_buffer(dev,
				   dst_con.mode->hdisplay *
				   dst_con.mode->vdisplay * 4, 0);
	if (!test.src_bo) {
		fprintf(stderr, "Failed to create source fb!\n");
		return -EFAULT;
	}

	rga_run_test(&test);

	/*
	 * RGA API Related:
	 *
	 * Close the RGA device
	 */
	rga_fini(ctx);

	rockchip_destroy_buffer(test.src_bo);
	rockchip_destroy_buffer(test.dst_bo);

	drmClose(dev->fd);
	rockchip_device_destroy(dev);

	return 0;
}
