/*
 *  Copyright (C) 2009 Michal Hruby <michal.mhr@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 *  Author : Michal Hruby <michal.mhr@gmail.com>
 *
 */

#include "config.h"

#include <gdk/gdk.h>
#include <libawn/awn-cairo-utils.h>
#include <libawn/awn-config-client.h>
#include <math.h>

#include "awn-background-edgy.h"

G_DEFINE_TYPE (AwnBackgroundEdgy, awn_background_edgy, AWN_TYPE_BACKGROUND)

static void awn_background_edgy_draw (AwnBackground  *bg,
                                      cairo_t        *cr,
                                      AwnOrientation  orient,
                                      GdkRectangle   *area);

static void awn_background_edgy_get_shape_mask (AwnBackground  *bg,
                                                cairo_t        *cr,
                                                AwnOrientation  orient,
                                                GdkRectangle   *area);

static void awn_background_edgy_padding_request (AwnBackground *bg,
                                                 AwnOrientation orient,
                                                 guint *padding_top,
                                                 guint *padding_bottom,
                                                 guint *padding_left,
                                                 guint *padding_right);

static void
awn_background_edgy_align_changed (AwnBackground *bg) // has more params...
{
  awn_background_emit_padding_changed(bg);
}

static void
awn_background_edgy_constructed (GObject *object)
{
  G_OBJECT_CLASS (awn_background_edgy_parent_class)->constructed (object);

  gpointer monitor = NULL;
  g_return_if_fail (AWN_BACKGROUND (object)->panel);
  g_object_get (AWN_BACKGROUND (object)->panel, "monitor", &monitor, NULL);

  g_return_if_fail (monitor);

  g_signal_connect_swapped (monitor, "notify::monitor-align",
                            G_CALLBACK (awn_background_edgy_align_changed),
                            object);
}

static void
awn_background_edgy_dispose (GObject *object)
{
  gpointer monitor = NULL;
  if (AWN_BACKGROUND (object)->panel)
    g_object_get (AWN_BACKGROUND (object)->panel, "monitor", &monitor, NULL);

  if (monitor)
    g_signal_handlers_disconnect_by_func (monitor, 
        G_CALLBACK (awn_background_edgy_align_changed), object);

  G_OBJECT_CLASS (awn_background_edgy_parent_class)->dispose (object);
}

static void
awn_background_edgy_class_init (AwnBackgroundEdgyClass *klass)
{
  AwnBackgroundClass *bg_class = AWN_BACKGROUND_CLASS (klass);

  GObjectClass *obj_class = G_OBJECT_CLASS (klass);
  obj_class->constructed  = awn_background_edgy_constructed;
  obj_class->dispose = awn_background_edgy_dispose;

  bg_class->draw = awn_background_edgy_draw;
  bg_class->padding_request = awn_background_edgy_padding_request;
  bg_class->get_shape_mask = awn_background_edgy_get_shape_mask;
}


static void
awn_background_edgy_init (AwnBackgroundEdgy *bg)
{
  ;
}

AwnBackground * 
awn_background_edgy_new (AwnConfigClient *client, AwnPanel *panel)
{
  AwnBackground *bg;

  bg = g_object_new (AWN_TYPE_BACKGROUND_EDGY,
                     "client", client,
                     "panel", panel,
                     NULL);
  return bg;
}


/*
 * Drawing functions
 */
static void
draw_path (cairo_t *cr, gdouble radius, gint width, gint height,
           gboolean bottom_left)
{
  if (bottom_left)
    cairo_arc (cr, 0, height, radius, -M_PI / 2.0, 0.0);
  else
    cairo_arc_negative (cr, width, height, radius, -M_PI / 2.0, -M_PI);
}

static void
draw_top_bottom_background (AwnBackground  *bg,
                            cairo_t        *cr,
                            gint            width,
                            gint            height)
{
  cairo_pattern_t *pat;

  gfloat align = awn_background_get_panel_alignment (bg);
  if (align != 0.0 && align != 1.0) return;

  gboolean bottom_left = align == 0.0;

  /* Basic set-up */
  cairo_set_line_width (cr, 1.0);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

  if (gtk_widget_is_composited (GTK_WIDGET (bg->panel)) == FALSE)
  {
    goto paint_lines;
  }

  /*
   * [0,0]
   *  __
   * |  '-.
   * |     '.
   * |       \
   * |        ;
   * '--------'  [width, height]
   * [0,height]
   */

  /* Draw the background */
  pat = cairo_pattern_create_radial (bottom_left ? 0 : width, height, 1,
                                     bottom_left ? 0 : width, height, height);
  awn_cairo_pattern_add_color_stop_color (pat, 0.0, bg->g_step_2);
  awn_cairo_pattern_add_color_stop_color (pat, 1.0, bg->g_step_1);
  draw_path(cr, height - 1.0, width, height, bottom_left);
  cairo_line_to (cr, bottom_left ? 0.0 : width, height);

  cairo_set_source (cr, pat);
  cairo_fill (cr);
  cairo_pattern_destroy (pat);

  /* Draw the hi-light
  pat = cairo_pattern_create_linear (0, 0, 0, (height/3.0));
  awn_cairo_pattern_add_color_stop_color (pat, 0.0, bg->g_histep_1);
  awn_cairo_pattern_add_color_stop_color (pat, 1.0, bg->g_histep_2);
  draw_rect (bg, cr, 1, 1, width-2, height/3.0);

  cairo_set_source (cr, pat);
  cairo_fill (cr);
  cairo_pattern_destroy (pat);
  */
  paint_lines:

  /* Internal border */
  awn_cairo_set_source_color (cr, bg->hilight_color);
  draw_path(cr, height - 2.0, width, height, bottom_left);
  cairo_stroke (cr);

  /* External border */
  awn_cairo_set_source_color (cr, bg->border_color);
  draw_path(cr, height - 1.0, width, height, bottom_left);
  cairo_stroke (cr);
}

static
void awn_background_edgy_padding_request (AwnBackground *bg,
                                          AwnOrientation orient,
                                          guint *padding_top,
                                          guint *padding_bottom,
                                          guint *padding_left,
                                          guint *padding_right)
{
  gint size, offset;
  g_object_get (bg->panel, "size", &size, "offset", &offset, NULL);

  gint req = sqrt ((size + offset) * (size + offset) * 2) - (size + offset) + 1;
  gint side_req = req + offset;
  gboolean bottom_left = awn_background_get_panel_alignment (bg) == 0.0;

  switch (orient)
  {
    case AWN_ORIENTATION_TOP:
      *padding_top  = 0; *padding_bottom = req;
      *padding_left = bottom_left ? 0 : side_req;
      *padding_right = bottom_left ? side_req : 0;
      break;
    case AWN_ORIENTATION_BOTTOM:
      *padding_top  = req; *padding_bottom = 0;
      *padding_left = bottom_left ? 0 : side_req;
      *padding_right = bottom_left ? side_req : 0;
      break;
    case AWN_ORIENTATION_LEFT:
      *padding_top  = bottom_left ? 0 : side_req;
      *padding_bottom = bottom_left ? side_req : 0;
      *padding_left = 0; *padding_right = req;
      break;
    case AWN_ORIENTATION_RIGHT:
      *padding_top  = bottom_left ? 0 : side_req;
      *padding_bottom = bottom_left ? side_req : 0;
      *padding_left = req; *padding_right = 0;
      break;
    default:
      break;
  }
}

static void 
awn_background_edgy_draw (AwnBackground  *bg,
                          cairo_t        *cr, 
                          AwnOrientation  orient,
                          GdkRectangle   *area)
{
  gint temp;
  gint x = area->x, y = area->y;
  gint width = area->width, height = area->height;
  cairo_save (cr);

  switch (orient)
  {
    case AWN_ORIENTATION_RIGHT:
      cairo_translate (cr, x, y);
      cairo_rotate (cr, M_PI * 0.5);
      cairo_scale (cr, 1.0, -1.0);
      temp = width;
      width = height; height = temp;
      break;
    case AWN_ORIENTATION_LEFT:
      cairo_translate (cr, x+width, y);
      cairo_rotate (cr, M_PI * 0.5);
      temp = width;
      width = height; height = temp;
      break;
    case AWN_ORIENTATION_TOP:
      cairo_translate (cr, x, height - y);
      cairo_scale (cr, 1.0, -1.0);
      break;
    default:
      cairo_translate (cr, x, y);
      break;
  }

  draw_top_bottom_background (bg, cr, width, height);

  cairo_restore (cr);
}

static void 
awn_background_edgy_get_shape_mask (AwnBackground  *bg,
                                    cairo_t        *cr, 
                                    AwnOrientation  orient,
                                    GdkRectangle   *area)
{
  gint temp;
  gint x = area->x, y = area->y;
  gint width = area->width, height = area->height;
  cairo_save (cr);

  // FIXME: doesn't work yet

  switch (orient)
  {
    case AWN_ORIENTATION_RIGHT:
      cairo_translate (cr, x, y+height);
      cairo_rotate (cr, M_PI * 1.5);
      temp = width;
      width = height; height = temp;
      break;
    case AWN_ORIENTATION_LEFT:
      cairo_translate (cr, x+width, y);
      cairo_rotate (cr, M_PI * 0.5);
      temp = width;
      width = height; height = temp;
      break;
    case AWN_ORIENTATION_TOP:
      cairo_translate (cr, x+width, y+height);
      cairo_rotate (cr, M_PI);
      break;
    default:
      cairo_translate (cr, x, y);
      break;
  }

  //draw_rect (bg, cr, 0, 0, width, height+3);
  //cairo_fill (cr);

  cairo_restore (cr);
}

/* vim: set et ts=2 sts=2 sw=2 : */
