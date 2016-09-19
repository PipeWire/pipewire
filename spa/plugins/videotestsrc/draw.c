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

#define PIXEL_SIZE      3

#define GET_IMAGE_WIDTH(this)   this->current_format.info.raw.size.width
#define GET_IMAGE_HEIGHT(this)  this->current_format.info.raw.size.height

enum
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
};

struct pixel
{
  char R;
  char G;
  char B;
};

static struct pixel colors[N_COLORS] =
{
  {191, 191, 191}, /* GRAY */
  {191, 191, 0},   /* YELLOW */
  {0, 191, 191},   /* CYAN */
  {0, 191, 0},     /* GREEN */
  {191, 0, 191},   /* MAGENTA */
  {191, 0, 0},     /* RED */
  {0, 0, 191},     /* BLUE */
  {19, 19, 19},    /* BLACK */
  {0, 33, 76},     /* NEGATIVE I */
  {255, 255, 255}, /* WHITE */
  {49, 0, 107},    /* POSITIVE Q */
  {9, 9, 9},       /* DARK BLACK */
  {29, 29, 29},    /* LIGHT BLACK */
};

static void
draw_line (char *data, struct pixel *c, int w)
{
  int i;

  for (i = 0; i < w; i++) {
    data[i * PIXEL_SIZE + 0] = c->R;
    data[i * PIXEL_SIZE + 1] = c->G;
    data[i * PIXEL_SIZE + 2] = c->B;
  }
}

#define DRAW_LINE(data,line,offset,color,width) \
  draw_line (data + (line * w + offset) * PIXEL_SIZE, colors + color, width);

static void
draw_smpte_snow (SpaVideoTestSrc *this, char *data)
{
  int h, w;
  int y1, y2;
  int i, j;

  w = GET_IMAGE_WIDTH (this);
  h = GET_IMAGE_HEIGHT (this);
  y1 = 2 * h / 3;
  y2 = 3 * h / 4;

  for (i = 0; i < y1; i++) {
    for (j = 0; j < 7; j++) {
      int x1 = j * w / 7;
      int x2 = (j + 1) * w / 7;
      DRAW_LINE (data, i, x1, j, x2 - x1);
    }
  }

  for (i = y1; i < y2; i++) {
    for (j = 0; j < 7; j++) {
      int x1 = j * w / 7;
      int x2 = (j + 1) * w / 7;
      int c = (j & 1) ? BLACK : BLUE - j;

      DRAW_LINE (data, i, x1, c, x2 - x1);
    }
  }

  for (i = y2; i < h; i++) {
    int x = 0;

    /* negative I */
    DRAW_LINE (data, i, x, NEG_I, w / 6);
    x += w / 6;

    /* white */
    DRAW_LINE (data, i, x, WHITE, w / 6);
    x += w / 6;

    /* positive Q */
    DRAW_LINE (data, i, x, POS_Q, w / 6);
    x += w / 6;

    /* pluge */
    DRAW_LINE (data, i, x, DARK_BLACK, w / 12);
    x += w / 12;
    DRAW_LINE (data, i, x, BLACK, w / 12);
    x += w / 12;
    DRAW_LINE (data, i, x, LIGHT_BLACK, w / 12);
    x += w / 12;

    /* war of the ants (a.k.a. snow) */
    for (j = x; j < w; j++) {
      unsigned char r = rand ();
      data[(i * w + j) * PIXEL_SIZE + 0] = r;
      data[(i * w + j) * PIXEL_SIZE + 1] = r;
      data[(i * w + j) * PIXEL_SIZE + 2] = r;
    }
  }
}
