/* Copyright 2016 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <sys/shm.h>

#include <FL/Fl_RGB_Image.H>
#include <FL/x.H>
#include <FL/Fl_Device.H>

#include <rdr/Exception.h>
#include <rfb/LogWriter.h>

#include "Surface.h"

// see <xorg/fourcc.h>
#ifndef FOURCC_NV12
#define FOURCC_NV12 0x3231564e
#endif

static rfb::LogWriter vlog("Surface::Xv");

void Surface::clear(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
  XRenderColor color;

  color.red = (unsigned)r * 65535 / 255 * a / 255;
  color.green = (unsigned)g * 65535 / 255 * a / 255;
  color.blue = (unsigned)b * 65535 / 255 * a / 255;
  color.alpha = (unsigned)a * 65535 / 255;

  XRenderFillRectangle(fl_display, PictOpSrc, picture, &color,
                       0, 0, width(), height());
}

void Surface::draw(int src_x, int src_y, int x, int y, int w, int h)
{
  XvShmPutImage(fl_display, xv_port, fl_window, fl_gc, xvimage,
    src_x, src_y, xvimage->width, xvimage->height,
    x, y, w, h, False);
}

void Surface::draw(Surface* dst, int src_x, int src_y, int x, int y, int w, int h)
{
  // XvShmPutImage(fl_display, xv_port, fl_window, fl_gc, dst->xvimage,
  //   src_x, src_y, dst->xvimage->width, dst->xvimage->height,
  //   x, y, w, h, False);

  dst->xvimage = xvimage;
  xvimage = NULL;
}

static Picture alpha_mask(int a)
{
  Pixmap pixmap;
  XRenderPictFormat* format;
  XRenderPictureAttributes rep;
  Picture pict;
  XRenderColor color;

  if (a == 255)
    return None;

  pixmap = XCreatePixmap(fl_display, XDefaultRootWindow(fl_display),
                         1, 1, 8);

  format = XRenderFindStandardFormat(fl_display, PictStandardA8);
  rep.repeat = RepeatNormal;
  pict = XRenderCreatePicture(fl_display, pixmap, format, CPRepeat, &rep);
  XFreePixmap(fl_display, pixmap);

  color.alpha = (unsigned)a * 65535 / 255;

  XRenderFillRectangle(fl_display, PictOpSrc, pict, &color,
                       0, 0, 1, 1);

  return pict;
}

void Surface::blend(int src_x, int src_y, int x, int y, int w, int h, int a)
{
  Picture winPict, alpha;

  return;

  winPict = XRenderCreatePicture(fl_display, fl_window, visFormat, 0, NULL);
  alpha = alpha_mask(a);
  XRenderComposite(fl_display, PictOpOver, picture, alpha, winPict,
                   src_x, src_y, 0, 0, x, y, w, h);
  XRenderFreePicture(fl_display, winPict);

  if (alpha != None)
    XRenderFreePicture(fl_display, alpha);
}

void Surface::blend(Surface* dst, int src_x, int src_y, int x, int y, int w, int h, int a)
{
  Picture alpha;

  return;

  alpha = alpha_mask(a);
  XRenderComposite(fl_display, PictOpOver, picture, alpha, dst->picture,
                   src_x, src_y, 0, 0, x, y, w, h);
  if (alpha != None)
    XRenderFreePicture(fl_display, alpha);
}


void Surface::alloc()
{
  // assume Xv and XShm are always available!

  // Might not be open at this point
  fl_open_display();

  // debug only
  XSynchronize(fl_display, True);

  unsigned num_adaptors;
  XvAdaptorInfo *adaptors;
  int r = XvQueryAdaptors(fl_display, XDefaultRootWindow(fl_display), &num_adaptors, &adaptors);
  if (r != Success) {
    throw rdr::Exception("XvQueryAdaptors: %d", r);
  }

  for (int i = 0; i < (int)num_adaptors && !xv_port; i++) {
    XvAdaptorInfo *adaptor = &adaptors[i];

    if (!(adaptor->type & XvImageMask)) {
      vlog.debug("Xv adaptor %s has no support for XvImageMask", adaptor->name);
      continue;
    }

    for (int j = 0; j < (int)adaptor->num_ports; j++) {
      r = XvGrabPort(fl_display, adaptor->base_id + j, 0);
      if (r != Success) {
        vlog.error("XvGrabPort %d for Xv adaptor %s failed: %d", j, adaptor->name, r);
        continue;
      }

      xv_port = adaptor->base_id + j;
      vlog.info("Xv adaptor %s with port %d", adaptor->name, j);
      break;
    }
  }

  XvFreeAdaptorInfo(adaptors);

  if (!xv_port) {
    throw rdr::Exception("No Xv port available");
  }

  // assume NV12
  xvimage = XvShmCreateImage(fl_display, xv_port, FOURCC_NV12, NULL, width(), height(), &shm);
  if (!xvimage) {
    throw rdr::Exception("XvShmCreateImage");
  }

  vlog.info("Xshm image size is %d", xvimage->data_size);

  shm.shmid = shmget(IPC_PRIVATE, xvimage->data_size, IPC_CREAT | 0777);
  shm.shmaddr = (char *)shmat(shm.shmid, NULL, 0);
  if (shm.shmaddr == (char *)-1) {
    throw rdr::Exception("shmat");
  }

  xvimage->data = shm.shmaddr;
  shm.readOnly = False;

  if (!XShmAttach(fl_display, &shm)) {
    throw rdr::Exception("XShmAttach");
  }

  XSync(fl_display, False);

  // it will be deleted as soon as we detach later
  shmctl(shm.shmid, IPC_RMID, NULL);
}

void Surface::dealloc()
{
  if (shm.shmaddr != (char *)-1) {
    XShmDetach(fl_display, &shm);
    XSync(fl_display, False);
    shmdt(shm.shmaddr);
    shm.shmaddr = (char *)-1;
  }

  if (xvimage) {
    XFree(xvimage);
  }

  if (xv_port) {
    XvUngrabPort(fl_display, xv_port, 0);
  }
}

void Surface::update(const Fl_RGB_Image* image)
{
  XImage* img;
  GC gc;

  return;

  int x, y;
  const unsigned char* in;
  unsigned char* out;

  assert(image->w() == width());
  assert(image->h() == height());

  img = XCreateImage(fl_display, CopyFromParent, 32,
                     ZPixmap, 0, NULL, width(), height(),
                     32, 0);
  if (!img)
    throw rdr::Exception("XCreateImage");

  img->data = (char*)malloc(img->bytes_per_line * img->height);
  if (!img->data)
    throw rdr::Exception("malloc");

  // Convert data and pre-multiply alpha
  in = (const unsigned char*)image->data()[0];
  out = (unsigned char*)img->data;
  for (y = 0;y < img->height;y++) {
    for (x = 0;x < img->width;x++) {
      switch (image->d()) {
      case 1:
        *out++ = in[0];
        *out++ = in[0];
        *out++ = in[0];
        *out++ = 0xff;
        break;
      case 2:
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = (unsigned)in[0] * in[1] / 255;
        *out++ = in[1];
        break;
      case 3:
        *out++ = in[2];
        *out++ = in[1];
        *out++ = in[0];
        *out++ = 0xff;
        break;
      case 4:
        *out++ = (unsigned)in[2] * in[3] / 255;
        *out++ = (unsigned)in[1] * in[3] / 255;
        *out++ = (unsigned)in[0] * in[3] / 255;
        *out++ = in[3];
        break;
      }
      in += image->d();
    }
    if (image->ld() != 0)
      in += image->ld() - image->w() * image->d();
  }

  gc = XCreateGC(fl_display, pixmap, 0, NULL);
  XPutImage(fl_display, pixmap, gc, img,
            0, 0, 0, 0, img->width, img->height);
  XFreeGC(fl_display, gc);

  XDestroyImage(img);
}

