/*
 * Copyright (C) 2009 Wim Taymans <wim.taymans@gmail.com>
 * Copyright (C) 2012 Nicolas Bouillot (http://www.nicolasbouillot.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include <string.h>
#include <math.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include "shmdata/base-writer.h"

/*
 * A simple RTP receiver
 *
 *  receives alaw encoded RTP audio on port 5002, RTCP is received on  port 5003.
 *  the receiver RTCP reports are sent to port 5007
 *
 *             .-------.      .----------.     .---------.   .-------.   .--------.
 *  RTP        |udpsrc |      | rtpbin   |     |         |   |       |   |        |
 *  port=5002  |      src->recv_rtp recv_rtp->sink     src->sink   src->sink      |
 *             '-------'      |          |     '---------'   '-------'   '--------'
 *                            |          |
 *                            |          |     .-------.
 *                            |          |     |udpsink|  RTCP
 *                            |    send_rtcp->sink     | port=5007
 *             .-------.      |          |     '-------' sync=false
 *  RTCP       |udpsrc |      |          |               async=false
 *  port=5003  |     src->recv_rtcp      |
 *             '-------'      '----------'
 */

/* the caps of the sender RTP stream. This is usually negotiated out of band with
 * SDP or RTSP. */

/* the destination machine to send RTCP to. This is the address of the sender and
 * is used to send back the RTCP reports of this receiver. If the data is sent
 * from another machine, change this address. */
#define DEST_HOST "127.0.0.1"

typedef struct _App App;
struct _App
{
  GstElement *pipeline;
  shmdata_base_writer_t *writer;
  const char *socketName;
};

App app;

/* print the stats of a source */
static void
print_source_stats (GObject * source)
{
  GstStructure *stats;
  gchar *str;

  g_return_if_fail (source != NULL);

  /* get the source stats */
  g_object_get (source, "stats", &stats, NULL);

  /* simply dump the stats structure */
  str = gst_structure_to_string (stats);
  g_print ("source stats: %s\n", str);

  gst_structure_free (stats);
  g_free (str);
}

/* will be called when gstrtpbin signals on-ssrc-active. It means that an RTCP*/
/* packet was received from another source. */
static void
on_ssrc_active_cb (GstElement * rtpbin, guint sessid, guint ssrc,
		   gpointer user_data /*GstElement * depay */ )
{
  GObject *session, *isrc, *osrc;

  g_print ("got RTCP from session %u, SSRC %u\n", sessid, ssrc);

  /* get the right session */
  g_signal_emit_by_name (rtpbin, "get-internal-session", sessid, &session);

  /* get the internal source (the SSRC allocated to us, the receiver */
  g_object_get (session, "internal-source", &isrc, NULL);
  print_source_stats (isrc);

  /* get the remote source that sent us RTCP */
  g_signal_emit_by_name (session, "get-source-by-ssrc", ssrc, &osrc);
  print_source_stats (osrc);
}

static void
pad_removed_cb (GstElement * rtpbin, GstPad * new_pad,
		gpointer user_data /*GstElement * depay */ )
{
  g_print ("pad removed: %s\n", GST_PAD_NAME (new_pad));

}

/* will be called when rtpbin has validated a payload that we can depayload */
static void
pad_added_cb (GstElement * rtpbin, GstPad * new_pad,
	      gpointer user_data /*GstElement * depay */ )
{
  GstPad *sinkpad;
  GstPadLinkReturn lres;

  App *app = (App *) user_data;

  g_print ("new payload on pad: %s\n", GST_PAD_NAME (new_pad));

  /* the depayloading and decoding */
  GstElement *gstdepay = gst_element_factory_make ("rtpgstdepay", NULL);
  g_assert (gstdepay);

  gst_element_set_state (gstdepay, GST_STATE_PLAYING);

  /* add depayloading and playback to the pipeline and link */
  gst_bin_add (GST_BIN (app->pipeline), gstdepay);

  sinkpad = gst_element_get_static_pad (gstdepay, "sink");
  g_assert (sinkpad);

  lres = gst_pad_link (new_pad, sinkpad);
  g_assert (lres == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);

  app->writer =
    shmdata_base_writer_init (app->socketName, app->pipeline, gstdepay);
}

void
leave (int sig)
{
  gst_element_set_state (app.pipeline, GST_STATE_NULL);
  gst_object_unref (GST_OBJECT (app.pipeline));
  exit (sig);
}

/* build a pipeline equivalent to:
 *
 * gst-launch -v gstrtpbin name=rtpbin                                                \
 *      udpsrc caps=$AUDIO_CAPS port=5002 ! rtpbin.recv_rtp_sink_0              \
 *        rtpbin. ! rtpgstdepay ! appsink \
 *      udpsrc port=5003 ! rtpbin.recv_rtcp_sink_0                              \
 *        rtpbin.send_rtcp_src_0 ! udpsink port=5007 host=$DEST sync=false async=false
 */
int
main (int argc, char *argv[])
{
  GstElement *rtpbin, *rtcpsrc, *rtcpsink;
  GstElement *rtpsrc;
  GMainLoop *loop;
  GstCaps *caps;
  GstPadLinkReturn lres;
  GstPad *srcpad, *sinkpad;

  /* Check input arguments */
  if (argc != 2)
    {
      g_printerr ("Usage: %s <socket-path>\n", argv[0]);
      return -1;
    }
  app.socketName = argv[1];

  (void) signal (SIGINT, leave);

  /* always init first */
  gst_init (&argc, &argv);

  /* the pipeline to hold everything */
  app.pipeline = gst_pipeline_new (NULL);
  g_assert (app.pipeline);

  /* the udp src and source we will use for RTP and RTCP */
  rtpsrc = gst_element_factory_make ("udpsrc", "rtpsrc");
  g_assert (rtpsrc);
  g_object_set (rtpsrc, "port", 5002, NULL);

  /* we need to set caps on the udpsrc for the RTP data */
  // caps inside caps is obtained with "echo application/x-pcd | base64", removing the "0=" at the end
  caps =
    gst_caps_from_string
    ("application/x-rtp,media=(string)application,payload=(int)96,clock-rate=(int)90000,encoding-name=(string)X-GST,caps=(string)YXBwbGljYXRpb24veC1wY2QK");
  g_object_set (rtpsrc, "caps", caps, NULL);
  gst_caps_unref (caps);

  rtcpsrc = gst_element_factory_make ("udpsrc", "rtcpsrc");
  g_assert (rtcpsrc);
  g_object_set (rtcpsrc, "port", 5003, NULL);

  rtcpsink = gst_element_factory_make ("udpsink", "rtcpsink");
  g_assert (rtcpsink);
  g_object_set (rtcpsink, "port", 5007, "host", DEST_HOST, NULL);
  /* no need for synchronisation or preroll on the RTCP sink */
  g_object_set (rtcpsink, "async", FALSE, "sync", FALSE, NULL);

  gst_bin_add_many (GST_BIN (app.pipeline), rtpsrc, rtcpsrc, rtcpsink, NULL);

  /* the rtpbin element */
  rtpbin = gst_element_factory_make ("gstrtpbin", "rtpbin");
  g_assert (rtpbin);

  gst_bin_add (GST_BIN (app.pipeline), rtpbin);

  /* now link all to the rtpbin, start by getting an RTP sinkpad for session 0 */
  srcpad = gst_element_get_static_pad (rtpsrc, "src");
  g_assert (srcpad);
  sinkpad = gst_element_get_request_pad (rtpbin, "recv_rtp_sink_0");
  g_assert (sinkpad);
  lres = gst_pad_link (srcpad, sinkpad);
  g_print ("%d \n", lres);
  g_assert (lres == GST_PAD_LINK_OK);
  gst_object_unref (srcpad);

  /* get an RTCP sinkpad in session 0 */
  srcpad = gst_element_get_static_pad (rtcpsrc, "src");
  sinkpad = gst_element_get_request_pad (rtpbin, "recv_rtcp_sink_0");
  lres = gst_pad_link (srcpad, sinkpad);
  g_assert (lres == GST_PAD_LINK_OK);
  gst_object_unref (srcpad);
  gst_object_unref (sinkpad);

  /* get an RTCP srcpad for sending RTCP back to the sender */
  srcpad = gst_element_get_request_pad (rtpbin, "send_rtcp_src_0");
  sinkpad = gst_element_get_static_pad (rtcpsink, "sink");
  lres = gst_pad_link (srcpad, sinkpad);
  g_assert (lres == GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);

  /* the RTP pad that we have to connect to the depayloader will be created
   * dynamically so we connect to the pad-added signal, pass the depayloader as
   * user_data so that we can link to it. */
  g_signal_connect (rtpbin, "pad-added", G_CALLBACK (pad_added_cb), &app);
  g_signal_connect (rtpbin, "pad-removed", G_CALLBACK (pad_removed_cb), &app);

  /* give some stats when we receive RTCP */
  g_signal_connect (rtpbin, "on-ssrc-active", G_CALLBACK (on_ssrc_active_cb),
		    NULL);

  /* set the pipeline to playing */
  g_print ("starting receiver pipeline\n");
  gst_element_set_state (app.pipeline, GST_STATE_PLAYING);

  /* we need to run a GLib main loop to get the messages */
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_print ("stopping receiver pipeline\n");
  gst_element_set_state (app.pipeline, GST_STATE_NULL);

  gst_object_unref (app.pipeline);

  return 0;
}

