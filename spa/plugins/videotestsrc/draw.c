/* Spa Video Test Source
 * Copyright (C) 2016 Axis Communications AB
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

typedef enum
{
  GRAY = 0,
  YELLOW,
  CYAN,
  GREEN,
  MAGENTA,
  RED,
  BLUE,
  BLACK,
  NEG_I,
  WHITE,
  POS_Q,
  DARK_BLACK,
  LIGHT_BLACK,
  N_COLORS
} Color;

typedef struct _Pixel Pixel;

struct _Pixel
{
  unsigned char R;
  unsigned char G;
  unsigned char B;
  unsigned char Y;
  unsigned char U;
  unsigned char V;
};

static Pixel colors[N_COLORS] =
{
  {191, 191, 191, 0, 0, 0}, /* GRAY */
  {191, 191, 0,   0, 0, 0}, /* YELLOW */
  {0, 191, 191,   0, 0, 0}, /* CYAN */
  {0, 191, 0,     0, 0, 0}, /* GREEN */
  {191, 0, 191,   0, 0, 0}, /* MAGENTA */
  {191, 0, 0,     0, 0, 0}, /* RED */
  {0, 0, 191,     0, 0, 0}, /* BLUE */
  {19, 19, 19,    0, 0, 0}, /* BLACK */
  {0, 33, 76,     0, 0, 0}, /* NEGATIVE I */
  {255, 255, 255, 0, 0, 0}, /* WHITE */
  {49, 0, 107,    0, 0, 0}, /* POSITIVE Q */
  {9, 9, 9,       0, 0, 0}, /* DARK BLACK */
  {29, 29, 29,    0, 0, 0}, /* LIGHT BLACK */
};
/* YUV values are computed in init_colors() */

typedef struct _DrawingData DrawingData;

typedef void (*DrawPixelFunc) (DrawingData *dd,
                               int x,
                               Pixel *pixel);

struct _DrawingData {
  char* line;
  int width;
  int height;
  int stride;
  DrawPixelFunc draw_pixel;
};

static inline void
update_yuv (Pixel *pixel)
{
  uint16_t y, u, v;

  /* see https://en.wikipedia.org/wiki/YUV#Studio_swing_for_BT.601 */

  y =  76 * pixel->R + 150 * pixel->G  + 29 * pixel->B;
  u = -43 * pixel->R  - 84 * pixel->G + 127 * pixel->B;
  v = 127 * pixel->R - 106 * pixel->G  - 21 * pixel->B;

  y = (y + 128) >> 8;
  u = (u + 128) >> 8;
  v = (v + 128) >> 8;

  pixel->Y = y;
  pixel->U = u + 128;
  pixel->V = v + 128;
}

static void
init_colors (void)
{
  int i;

  if (colors[WHITE].Y != 0) {
    /* already computed */
    return;
  }

  for (i = 0; i < N_COLORS; i++) {
    update_yuv (&colors[i]);
  }
}

static void
draw_pixel_rgb (DrawingData *dd, int x, Pixel *color)
{
  dd->line[3 * x + 0] = color->R;
  dd->line[3 * x + 1] = color->G;
  dd->line[3 * x + 2] = color->B;
}

static void
draw_pixel_uyvy (DrawingData *dd, int x, Pixel *color)
{
  if (x & 1) {
    /* odd pixel */
    dd->line[2 * (x - 1) + 3] = color->Y;
  } else {
    /* even pixel */
    dd->line[2 * x + 0] = color->U;
    dd->line[2 * x + 1] = color->Y;
    dd->line[2 * x + 2] = color->V;
  }
}

static int
drawing_data_init (DrawingData *dd,
                   struct impl *this,
                   char* data)
{
  struct spa_video_info *format = &this->current_format;
  struct spa_rectangle *size = &format->info.raw.size;

  if ((format->media_type != this->type.media_type.video) ||
      (format->media_subtype != this->type.media_subtype.raw))
    return SPA_RESULT_NOT_IMPLEMENTED;

  if (format->info.raw.format == this->type.video_format.RGB) {
    dd->draw_pixel = draw_pixel_rgb;
  }
  else if (format->info.raw.format == this->type.video_format.UYVY) {
    dd->draw_pixel = draw_pixel_uyvy;
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  dd->line = data;
  dd->width = size->width;
  dd->height = size->height;
  dd->stride = this->stride;

  return SPA_RESULT_OK;
}

static inline void
draw_pixels (DrawingData *dd,
             int offset,
             Color color,
             int length)
{
  int x;

  for (x = offset; x < offset + length; x++) {
    dd->draw_pixel (dd, x, &colors[color]);
  }
}

static inline void
next_line (DrawingData *dd)
{
  dd->line += dd->stride;
}

static void
draw_smpte_snow (DrawingData *dd)
{
  int h, w;
  int y1, y2;
  int i, j;

  w = dd->width;
  h = dd->height;
  y1 = 2 * h / 3;
  y2 = 3 * h / 4;

  for (i = 0; i < y1; i++) {
    for (j = 0; j < 7; j++) {
      int x1 = j * w / 7;
      int x2 = (j + 1) * w / 7;
      draw_pixels (dd, x1, j, x2 - x1);
    }
    next_line (dd);
  }

  for (i = y1; i < y2; i++) {
    for (j = 0; j < 7; j++) {
      int x1 = j * w / 7;
      int x2 = (j + 1) * w / 7;
      Color c = (j & 1) ? BLACK : BLUE - j;

      draw_pixels (dd, x1, c, x2 - x1);
    }
    next_line (dd);
  }

  for (i = y2; i < h; i++) {
    int x = 0;

    /* negative I */
    draw_pixels (dd, x, NEG_I, w / 6);
    x += w / 6;

    /* white */
    draw_pixels (dd, x, WHITE, w / 6);
    x += w / 6;

    /* positive Q */
    draw_pixels (dd, x, POS_Q, w / 6);
    x += w / 6;

    /* pluge */
    draw_pixels (dd, x, DARK_BLACK, w / 12);
    x += w / 12;
    draw_pixels (dd, x, BLACK, w / 12);
    x += w / 12;
    draw_pixels (dd, x, LIGHT_BLACK, w / 12);
    x += w / 12;

    /* war of the ants (a.k.a. snow) */
    for (j = x; j < w; j++) {
      Pixel p;
      unsigned char r = rand ();

      p.R = r;
      p.G = r;
      p.B = r;
      update_yuv (&p);
      dd->draw_pixel (dd, j, &p);
    }

    next_line (dd);
  }
}

static void
draw_snow (DrawingData *dd)
{
  int x, y;

  for (y = 0; y < dd->height; y++) {
    for (x = 0; x < dd->width; x++) {
      Pixel p;
      unsigned char r = rand ();

      p.R = r;
      p.G = r;
      p.B = r;
      update_yuv (&p);
      dd->draw_pixel (dd, x, &p);
    }

    next_line (dd);
  }
}

static int
draw (struct impl *this, char *data)
{
  DrawingData dd;
  int res;
  uint32_t pattern;

  init_colors ();

  res = drawing_data_init (&dd, this, data);
  if (res != SPA_RESULT_OK)
    return res;

  pattern = this->props.pattern;
  if (pattern == this->type.pattern_smpte_snow)
    draw_smpte_snow (&dd);
  else if (pattern == this->type.pattern_snow)
    draw_snow (&dd);
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}
