#include <string.h>

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#define TIME_STRING_LENGTH 13

/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData
{
  GstElement *playbin;    /* Our one and only pipeline */
  GtkWidget *main_window; /* The uppermost window, containing all other windows */
  GstState state;         /* Current state of the pipeline */
  gint64 duration;        /* Duration of the clip, in nanoseconds */
} CustomData;

/* Enumerates label types */
enum label_type
{
  LABEL_TYPE_DURATION, /* Duration label */
  LABEL_TYPE_POSITION, /* Position label */

  LABEL_TYPE_COUNT
};

static const gchar *label_type_strings[] = {
    [LABEL_TYPE_DURATION] = "duration",
    [LABEL_TYPE_POSITION] = "position"};

/* This function converts label type to string */
static const gchar *label_type_to_string(enum label_type type)
{
  g_return_val_if_fail(type < LABEL_TYPE_COUNT, NULL);
  g_return_val_if_fail(label_type_strings[type] != NULL, NULL);

  return label_type_strings[type];
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

/* This function makes label text for a specific label type
 * The returned string should be freed with g_free() when no longer needed.
*/
gchar *make_label_txt(enum label_type type, gchar *duration)
{
  switch (type)
  {
  case LABEL_TYPE_DURATION:
    return g_strdup_printf("Duration: %s", duration);

  case LABEL_TYPE_POSITION:
    return g_strdup_printf("Position: %s", duration);

  default:
    g_error("Cannot make label text: unknow label type");
    return g_strdup("");
  }
}

/* Function to update a specific label */
static void update_label(CustomData *data, enum label_type type)
{
  g_return_if_fail(data != NULL);

  GList *children = gtk_container_get_children(GTK_CONTAINER(data->main_window));
  GList *first = g_list_first(children);
  GtkWidget *main_box = (GtkWidget *)(first->data);
  g_list_free(children);

  children = gtk_container_get_children(GTK_CONTAINER(main_box));
  for (const GList *iter = children; iter != NULL; iter = g_list_next(iter))
  {
    GtkWidget *box = (GtkWidget *)(iter->data);
    if (g_strcmp0(gtk_widget_get_name(box), "controls") != 0)
      continue;

    GList *control_children = gtk_container_get_children(GTK_CONTAINER(box));
    for (const GList *it = control_children; it != NULL; it = g_list_next(it))
    {
      GtkWidget *control = (GtkWidget *)(it->data);
      if (g_strcmp0(gtk_widget_get_name(control), label_type_to_string(type)) == 0)
      {
        gchar *time_str = time_to_string(data->duration);
        gchar *label_txt = make_label_txt(type, time_str);
        gtk_label_set_text(GTK_LABEL(control), label_txt);
        g_free(time_str);
        g_free(label_txt);
        break;
      }
    }
    g_list_free(control_children);
    break;
  }

  g_list_free(children);
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
    /* Set the URI to play */
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

/* This creates all the GTK+ widgets that compose our application, and registers the callbacks */
static void create_ui(CustomData *data)
{
  GtkWidget *video_window;                                           /* The drawing area where the video will be shown */
  GtkWidget *main_box;                                               /* VBox to hold main_hbox and the controls */
  GtkWidget *main_hbox;                                              /* HBox to hold the video_window and the stream info text widget */
  GtkWidget *controls;                                               /* HBox to hold the buttons and the slider */
  GtkWidget *play_button, *pause_button, *stop_button, *open_button; /* Buttons */
  GtkWidget *duration;                                               /* Duration label */
  gchar *time_str = NULL;
  gchar *label_txt = NULL;

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

  time_str = time_to_string(data->duration);
  label_txt = make_label_txt(LABEL_TYPE_DURATION, time_str);
  duration = gtk_label_new(label_txt);
  gtk_widget_set_name(duration, "duration");
  g_free(time_str);
  g_free(label_txt);

  controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(controls, "controls");
  gtk_box_pack_start(GTK_BOX(controls), play_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), pause_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), stop_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), open_button, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(controls), duration, FALSE, FALSE, 2);

  main_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(main_hbox, "main_hbox");
  gtk_box_pack_start(GTK_BOX(main_hbox), video_window, TRUE, TRUE, 0);

  main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(main_box), main_hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(main_box), controls, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(data->main_window), main_box);
  gtk_window_set_default_size(GTK_WINDOW(data->main_window), 640, 480);

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
      gst_element_query_duration(data->playbin, GST_FORMAT_TIME, &data->duration);
      update_label(data, LABEL_TYPE_DURATION);
    }
  }
}

int main(int argc, char *argv[])
{
  CustomData data;
  GstStateChangeReturn ret;
  GstBus *bus;
  GstElement *video_sink;

  /* Initialize GTK */
  gtk_init(&argc, &argv);

  /* Initialize GStreamer */
  gst_init(&argc, &argv);

  /* Initialize our data structure */
  memset(&data, 0, sizeof(data));
  data.duration = GST_CLOCK_TIME_NONE;

  /* Create the elements */
  data.playbin = gst_element_factory_make("playbin", "playbin");
  video_sink = gst_element_factory_make("ximagesink", "videosink");
  g_object_set(data.playbin, "video-sink", video_sink, NULL);

  if (!data.playbin)
  {
    g_printerr("Not all elements could be created.\n");
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