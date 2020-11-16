#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#define TIME_STRING_LENGTH 13
#define THUMBNAILS_NUMBER  10

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData
{
  GstElement *playbin;     /* Our one and only pipeline */
  GtkWidget *main_window;  /* The uppermost window, containing all other windows */
  GstState state;          /* Current state of the pipeline */
  gint64 duration;         /* Duration of the clip, in nanoseconds */
  gint64 position;         /* Position of the clip, in nanoseconds */
  gint timer_id;           /* The ID of the timer source */
  GstElement *timelinebin; /* Timeline pipline to make thumbnails */
} CustomData;

/* Enumerates widget types */
enum widget_type
{
  WIDGET_TYPE_DURATION, /* Duration label widget */
  WIDGET_TYPE_POSITION, /* Position label widget */
  WIDGET_TYPE_SCALE,    /* Scale widget */
  WIDGET_TYPE_TIMELINE, /* Timeline widget */

  WIDGET_TYPE_COUNT
};

static const gchar *widget_type_strings[] = {
    [WIDGET_TYPE_DURATION]    = "duration",
    [WIDGET_TYPE_POSITION]    = "position",
    [WIDGET_TYPE_SCALE]       = "scale",
    [WIDGET_TYPE_TIMELINE]    = "timeline"
  };

/* This function converts widget type to string */
static const gchar *widget_type_to_string(enum widget_type type)
{
  g_return_val_if_fail(type < WIDGET_TYPE_COUNT, NULL);
  g_return_val_if_fail(widget_type_strings[type] != NULL, NULL);

  return widget_type_strings[type];
}

/* This function converts guint64 time to string with the following format HH:mm:ss.SSS
 *  The returned string should be freed with g_free() when no longer needed.
*/
static gchar *time_to_string(gint64 time)
{
  if (time == GST_CLOCK_TIME_NONE)
    return g_strdup("00:00:00.000");

  gchar *str = g_strdup_printf("%02" GST_TIME_FORMAT, GST_TIME_ARGS(time));
  gchar *res = g_strndup(str, TIME_STRING_LENGTH - 1);
  g_free(str);

  return res;
}

/* This function makes label text from widget type
 * The returned string should be freed with g_free() when no longer needed.
*/
static gchar *make_label_txt(enum widget_type type, gchar *duration)
{
  switch (type)
  {
  case WIDGET_TYPE_DURATION:
    return g_strdup_printf("Duration: %s", duration);

  case WIDGET_TYPE_POSITION:
    return g_strdup_printf("Position: %s", duration);

  default:
    g_error("Cannot make label text: unexpected widget type");
    return g_strdup("");
  }
}

/* This function sets a label text widget */
static void set_label_txt(GtkWidget *label, enum widget_type type, CustomData *data) {
  g_return_if_fail(label != NULL);
  g_return_if_fail(data != NULL);
  g_return_if_fail(type < WIDGET_TYPE_SCALE);

  gint64 time;
  switch (type) {
    case WIDGET_TYPE_DURATION:
      time =  data->duration;
      break;
    case WIDGET_TYPE_POSITION:
      time =  data->position;
      break;
    default:
      g_error("Cannot set label text: unexpected widget type");
      return;
  }

  gchar *time_str = time_to_string(time);
  gchar *label_txt = make_label_txt(type, time_str);
  gtk_label_set_text(GTK_LABEL(label), label_txt);
  g_free(time_str);
  g_free(label_txt);
}

/* This functions adds an image created from file to a widget */
static void widget_add_image(GtkWidget *widget) {
  g_return_if_fail(widget != NULL);

  GtkWidget *image = gtk_image_new_from_file ("snapshot.png");
  gtk_box_pack_start(GTK_BOX(widget), image, FALSE, FALSE, 2);
  gtk_widget_show_all(widget);
}

/* Function to update a specific widget */
static void update_widget(CustomData *data, enum widget_type type)
{
  g_return_if_fail(data != NULL);
  g_return_if_fail(type < WIDGET_TYPE_COUNT);

  GList *children = gtk_container_get_children(GTK_CONTAINER(data->main_window));
  GList *first = g_list_first(children);
  GtkWidget *main_box = (GtkWidget *)(first->data);
  g_list_free(children);

  children = gtk_container_get_children(GTK_CONTAINER(main_box));
  for (const GList *iter = children; iter != NULL; iter = g_list_next(iter))
  {
    GtkWidget *box = (GtkWidget *)(iter->data);
    const gchar *box_name = gtk_widget_get_name(box);

    /* Nothing to do for main hbbx widget */
    if (g_strcmp0(box_name, "main_hbox") == 0)
      continue;

    /* Process timeline widget */
    if (g_strcmp0(box_name, "timeline") == 0) {
      if (type != WIDGET_TYPE_TIMELINE)
        continue;
      widget_add_image(box);
      break;
    }

    /* Process controls widgets */
    GList *control_children = gtk_container_get_children(GTK_CONTAINER(box));
    for (const GList *it = control_children; it != NULL; it = g_list_next(it))
    {
      GtkWidget *control = (GtkWidget *)(it->data);
      if (g_strcmp0(gtk_widget_get_name(control), widget_type_to_string(type)) == 0)
      {
        if (type == WIDGET_TYPE_SCALE)
          gtk_range_set_value(GTK_RANGE(control), ((gdouble)data->position/(gdouble)data->duration));
        else
          set_label_txt(control, type, data);
        break;
      }
    }
    g_list_free(control_children);
  }

  g_list_free(children);
}

/*This function extracts thumbnails using timeline pipeline */
static void extract_thumbnails(CustomData *data, gint step) {
  g_return_if_fail(data != NULL);

  gint64 position;
  GstElement *sink = NULL;
  GstSample *sample;
  gint width, height;
  GdkPixbuf *pixbuf;
  GError *error = NULL;
  gboolean res;
  GstMapInfo map;
  GstStateChangeReturn ret;

  /* set to PAUSED to make the first frame arrive in the sink */
  ret = gst_element_set_state (data->timelinebin, GST_STATE_PAUSED);
  switch (ret) {
    case GST_STATE_CHANGE_FAILURE:
      g_print ("failed to play the file\n");
      return;
    case GST_STATE_CHANGE_NO_PREROLL:
      /* for live sources, we need to set the pipeline to PLAYING before we can
       * receive a buffer. We don't do that yet */
      g_print ("live sources not supported yet\n");
      return;
    default:
      break;
  }

  gst_element_query_duration(data->timelinebin, GST_FORMAT_TIME, &data->duration);

  position = (step+1) * data->duration * 10 / 100;

  gst_element_seek_simple (data->timelinebin, GST_FORMAT_TIME,
      GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH, position);

  g_object_get(data->timelinebin, "video-sink", &sink, NULL);

  g_signal_emit_by_name (sink, "pull-preroll", &sample, NULL);
  gst_object_unref (sink);

if (sample) {
    GstBuffer *buffer;
    GstCaps *caps;
    GstStructure *s;

    /* get the snapshot buffer format now. We set the caps on the appsink so
     * that it can only be an rgb buffer. The only thing we have not specified
     * on the caps is the height, which is dependant on the pixel-aspect-ratio
     * of the source material */
    caps = gst_sample_get_caps (sample);
    if (!caps) {
      g_print ("could not get snapshot format\n");
      return;
    }
    s = gst_caps_get_structure (caps, 0);

    /* we need to get the final caps on the buffer to get the size */
    res = gst_structure_get_int (s, "width", &width);
    res |= gst_structure_get_int (s, "height", &height);
    if (!res) {
      g_print ("could not get snapshot dimension\n");
      return;
    }

    /* create pixmap from buffer and save, gstreamer video buffers have a stride
     * that is rounded up to the nearest multiple of 4 */
    buffer = gst_sample_get_buffer (sample);
    gst_buffer_map (buffer, &map, GST_MAP_READ);
    pixbuf = gdk_pixbuf_new_from_data (map.data,
        GDK_COLORSPACE_RGB, FALSE, 8, width, height,
        GST_ROUND_UP_4 (width * 3), NULL, NULL);

    /* save the pixbuf */
    gdk_pixbuf_save (pixbuf, "snapshot.png", "png", &error, NULL);
    gst_buffer_unmap (buffer, &map);
  } else {
    g_print ("could not make snapshot\n");
  }
}

static gboolean timeline_make_thumbnails(CustomData *data) {
  g_return_val_if_fail(data != NULL, FALSE);

  static int count;
  if (count< THUMBNAILS_NUMBER) {
    extract_thumbnails(data, count);
    update_widget(data, WIDGET_TYPE_TIMELINE);
    count++;
    return TRUE;
  }

  /* Free resources */
  gst_element_set_state(data->timelinebin, GST_STATE_NULL);
  gst_object_unref(data->timelinebin);
  return G_SOURCE_REMOVE;
}

/* This function is called when the GUI toolkit creates the physical window that will hold the video.
 * At this point we can retrieve its handler (which has a different meaning depending on the windowing system)
 * and pass it to GStreamer through the VideoOverlay interface. */
static void realize_cb(GtkWidget *widget, CustomData *data)
{
  GdkWindow *window = gtk_widget_get_window(widget);
  guintptr window_handle;

  if (!gdk_window_ensure_native(window))
    g_error("Couldn't create native window needed for GstVideoOverlay!");

  window_handle = GDK_WINDOW_XID(window);
  /* Pass it to playbin, which implements VideoOverlay and will forward it to the video sink */
  gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(data->playbin), window_handle);
}

/* This function is called when the PLAY button is clicked */
static void play_cb(GtkButton *button, CustomData *data)
{
  gst_element_set_state(data->playbin, GST_STATE_PLAYING);
}

/* This function is called when the PAUSE button is clicked */
static void pause_cb(GtkButton *button, CustomData *data)
{
  gst_element_set_state(data->playbin, GST_STATE_PAUSED);
}

/* This function is called when the STOP button is clicked */
static void stop_cb(GtkButton *button, CustomData *data)
{
  gst_element_set_state(data->playbin, GST_STATE_READY);
}

/* This function is called when the OPEN button is clicked */
static void open_cb(GtkButton *button, CustomData *data)
{
  gint res;
  GtkWidget *dialog;

  dialog = gtk_file_chooser_dialog_new("Open File",
                                       GTK_WINDOW(data->main_window),
                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                       "_Cancel",
                                       GTK_RESPONSE_CANCEL,
                                       "_Open",
                                       GTK_RESPONSE_ACCEPT,
                                       NULL);
  res = gtk_dialog_run(GTK_DIALOG(dialog));
  if (res == GTK_RESPONSE_ACCEPT)
  {
    char *filename;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    filename = gtk_file_chooser_get_uri(chooser);
    /* Set the URI to timelinebin */
    g_object_set(data->timelinebin, "uri", filename, NULL);
    g_timeout_add(1000, (GSourceFunc) timeline_make_thumbnails, data);
    /* Set the URI to playbin */
    g_object_set(data->playbin, "uri", filename, NULL);
    gst_element_set_state(data->playbin, GST_STATE_PLAYING);
    g_free(filename);
  }
  gtk_widget_destroy(dialog);
}

/* This function is called when the main window is closed */
static void delete_event_cb(GtkWidget *widget, GdkEvent *event, CustomData *data)
{
  stop_cb(NULL, data);
  gtk_main_quit();
}

/* This function is called when scale value changed */
static void scale_cb(GtkRange *scale, GtkScrollType scroll, gdouble value, CustomData *data)
{
  if (data == NULL) {
    g_printerr("%s: user data is empty \n", __func__);
    return;
  }

  gint64 position = value * data->duration;
  if (!gst_element_seek_simple (data->playbin, GST_FORMAT_TIME,
      GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH, position))
    g_printerr("Seek failed ! \n");
}

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui(CustomData *data)
{
  GtkWidget *video_window;                                           /* The drawing area where the video will be shown */
  GtkWidget *main_box;                                               /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;                                              /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;                                               /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button, *open_button; /* Buttons */
  GtkWidget *duration;                                               /* Duration label */
  GtkWidget *position;                                               /* Position label */
  GtkWidget *scale;                                                  /* Scale widget */
  GtkWidget *timeline;                                               /* Timeline */

  data->main_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g_signal_connect(G_OBJECT(data->main_window), "delete-event", G_CALLBACK(delete_event_cb), data);

  video_window = gtk_drawing_area_new();
  g_signal_connect(video_window, "realize", G_CALLBACK(realize_cb), data);

  play_button = gtk_button_new_from_icon_name("media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect(G_OBJECT(play_button), "clicked", G_CALLBACK(play_cb), data);

  pause_button = gtk_button_new_from_icon_name("media-playback-pause", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect(G_OBJECT(pause_button), "clicked", G_CALLBACK(pause_cb), data);

  stop_button = gtk_button_new_from_icon_name("media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect(G_OBJECT(stop_button), "clicked", G_CALLBACK(stop_cb), data);

  open_button = gtk_button_new_from_icon_name("gtk-open", GTK_ICON_SIZE_SMALL_TOOLBAR);
  g_signal_connect(G_OBJECT(open_button), "clicked", G_CALLBACK(open_cb), data);

  position = gtk_label_new(NULL);
  gtk_widget_set_name(position, "position");
  set_label_txt(position, WIDGET_TYPE_POSITION, data);

  scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
  g_object_set(scale, "width-request", 1350, NULL);
  gtk_widget_set_name(scale, "scale");
  g_signal_connect(G_OBJECT(scale), "change-value", G_CALLBACK(scale_cb), data);

  duration = gtk_label_new(NULL);
  gtk_widget_set_name(duration, "duration");
  set_label_txt(duration, WIDGET_TYPE_DURATION, data);

  controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(controls, "controls");
  gtk_box_pack_start(GTK_BOX(controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), open_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), position, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), scale, FALSE, FALSE, 10);
  gtk_box_pack_start(GTK_BOX(controls), duration, FALSE, FALSE, 2);

  main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(main_hbox, "main_hbox");
  gtk_box_pack_start(GTK_BOX(main_hbox), video_window, TRUE, TRUE, 0);

  timeline =  gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(timeline, "timeline");

  main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(main_box), controls, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(main_box), timeline, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(data->main_window), main_box);
  gtk_window_set_default_size(GTK_WINDOW(data->main_window), 1600, 680);

  gtk_widget_show_all(data->main_window);
}

/* This function is called when an error message is posted on the bus */
static void error_cb(GstBus *bus, GstMessage *msg, CustomData *data)
{
  GError *err;
  gchar *debug_info;

  /* Print error details on the screen */
  gst_message_parse_error(msg, &err, &debug_info);
  g_printerr("Error received from element %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
  g_printerr("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error(&err);
  g_free(debug_info);

  /* Set the pipeline to READY (which stops playback) */
  gst_element_set_state(data->playbin, GST_STATE_READY);
}

/* This function is called when an End-Of-Stream message is posted on the bus.
 * We just set the pipeline to READY (which stops playback) */
static void eos_cb(GstBus *bus, GstMessage *msg, CustomData *data)
{
  g_print("End-Of-Stream reached.\n");
  gst_element_set_state(data->playbin, GST_STATE_READY);

  data->position = data->duration;
  update_widget(data, WIDGET_TYPE_POSITION);
}

static gboolean timer_src_func(CustomData *data) {
  g_return_val_if_fail(data != NULL, G_SOURCE_REMOVE);

  gst_element_query_position(data->playbin, GST_FORMAT_TIME, &data->position);

  if (data->position != data->duration) {
    update_widget(data, WIDGET_TYPE_POSITION);
    update_widget(data, WIDGET_TYPE_SCALE);
    return TRUE;
  }

  data->timer_id = -1;
  return G_SOURCE_REMOVE;
}

/* This function is called when the pipeline changes states. We use it to
 * keep track of the current state. */
static void state_changed_cb(GstBus *bus, GstMessage *msg, CustomData *data)
{
  GstState old_state, new_state, pending_state;

  gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
  if (GST_MESSAGE_SRC(msg) == GST_OBJECT(data->playbin))
  {
    data->state = new_state;
    g_print("State set to %s\n", gst_element_state_get_name(new_state));
    if (new_state == GST_STATE_PLAYING)
    {
      /* Add timer to update current position and slider every 20 ms */
      data->timer_id = g_timeout_add(20, (GSourceFunc) timer_src_func, data);

      gst_element_query_duration(data->playbin, GST_FORMAT_TIME, &data->duration);
      update_widget(data, WIDGET_TYPE_DURATION);
    }
    else if (new_state == GST_STATE_PAUSED)
    {
      /* Remove timer to avoid updating current position */
      if (data->timer_id > 0) {
        g_source_remove(data->timer_id);
        data->timer_id = -1;
      }
    }
  }
}

int main(int argc, char *argv[])
{
  CustomData data;
  GstStateChangeReturn ret;
  GstBus *bus;
  GstElement *video_sink;
  GstElement *app_sink;

  /* Initialize GTK */
  gtk_init(&argc, &argv);

  /* Initialize GStreamer */
  gst_init(&argc, &argv);

  /* Initialize our data structure */
  memset(&data, 0, sizeof(data));
  data.duration = GST_CLOCK_TIME_NONE;
  data.position = GST_CLOCK_TIME_NONE;
  data.timer_id = -1;

  /* Create the elements */
  data.playbin = gst_element_factory_make("playbin", "playbin");
  video_sink = gst_element_factory_make("ximagesink", "videosink");
  g_object_set(data.playbin, "video-sink", video_sink, NULL);

  if (!data.playbin)
  {
    g_printerr("Not all playbin elements could be created.\n");
    return -1;
  }

  data.timelinebin = gst_element_factory_make("playbin", "timelinebin");
  app_sink = gst_element_factory_make("appsink", "videosink");
  GstCaps *caps  = gst_caps_from_string ("video/x-raw,format=RGB,width=160,pixel-aspect-ratio=1/1");
       
  g_object_set(app_sink, "caps", caps, NULL);
  gst_caps_unref(caps);
  g_object_set(data.timelinebin, "video-sink", app_sink, NULL);

  if (!data.timelinebin)
  {
    g_printerr("Not all timelinebin elements could be created.\n");
    return -1;
  }

  /* Create the GUI */
  create_ui(&data);

  /* Instruct the bus to emit signals for each received message, and connect to the interesting signals */
  bus = gst_element_get_bus(data.playbin);
  gst_bus_add_signal_watch(bus);
  g_signal_connect(G_OBJECT(bus), "message::error", (GCallback)error_cb, &data);
  g_signal_connect(G_OBJECT(bus), "message::eos", (GCallback)eos_cb, &data);
  g_signal_connect(G_OBJECT(bus), "message::state-changed", (GCallback)state_changed_cb, &data);
  gst_object_unref(bus);

  /* Start the GTK main loop. We will not regain control until gtk_main_quit is called. */
  gtk_main();

  /* Free resources */
  gst_element_set_state(data.playbin, GST_STATE_NULL);
  gst_object_unref(data.playbin);
  return 0;
}