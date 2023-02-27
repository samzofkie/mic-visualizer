#include <vector>
#include <queue>
#include <iostream>
#include <cstring>
//#include <algorithm>
#include <limits>
#include <math.h>

#include <X11/Xlib.h>

#include <cairo.h>
#include <cairo-xlib.h>

#include <pulse/simple.h>

using namespace std;

void err_n_exit(const char* message, int err = 1) {
  cout << message << endl;
  exit(err);
}

class CairoXWindow {
  public:
    double ww, wh;
    Display *display;
    int screen;
    Drawable window;
    cairo_surface_t *surface;
    cairo_t *cr;

    CairoXWindow(double ww_arg, double wh_arg) :
      ww(ww_arg), wh(wh_arg)
    {
      if (!(display = XOpenDisplay(NULL)))
        err_n_exit("XOpenDisplay failed");
      screen = DefaultScreen(display);
      window = XCreateSimpleWindow(display, DefaultRootWindow(display),
          0, 0, ww, wh, 0, 0, 0);
      XSelectInput(display, window, 
          ButtonPressMask | KeyPressMask | ExposureMask); // Nec?
      XMapWindow(display, window);
      surface = cairo_xlib_surface_create(display, window,
          DefaultVisual(display, screen),
          DisplayWidth(display, screen),
          DisplayHeight(display, screen));
      cr = cairo_create(surface);
    }
};

enum PulseStreamType {record, playback};

class PulseStream {
  public:
    pa_simple *s;
    const pa_sample_spec ss;
    int error;

    PulseStream(const pa_sample_spec &ss_arg, PulseStreamType type) :
      ss(ss_arg)
    {
      pa_stream_direction_t dir =
        type == record? PA_STREAM_RECORD : PA_STREAM_PLAYBACK;
      const char *stream_name = 
        type == record? "app_record_stream" : "app_playback_stream";
      if (!(s = pa_simple_new(NULL, "app_name", dir, NULL,
              stream_name, &ss, NULL, NULL, &error))) {
        char *err_mess = (char*)"pa_simple_new failed for ";
        err_n_exit(strcat(err_mess, stream_name));
      }
    } 
};

struct Point {
  double x, y;
};

struct Rectangle {
  double x, y, w, h;
  Rectangle(double _x=0, double _y=0, double _w=300, double _h=100) :
    x(_x), y(_y), w(_w), h(_h) {}
  Rectangle(const Rectangle& _rect) :
    x(_rect.x), y(_rect.y), w(_rect.w), h(_rect.h) {}
  bool Intersects(const Point p) {
    if (p.x >= x && p.x <= x+w)
        if (p.y >= y && p.y <= y+h)
          return true;
      return false;
  }
};

struct ClickableRectangle : public virtual Rectangle {
  using Rectangle::Rectangle;
  virtual void OnClick() =0;
};

struct Glyph : public virtual Rectangle {
  using Rectangle::Rectangle;
  virtual void Draw(cairo_t*) =0;
};

struct RecordButton : public ClickableRectangle, public Glyph {
  bool& recording;
  queue<Glyph*>& redraw_queue;
  RecordButton(const Rectangle& _rect, bool& _recording,
      queue<Glyph*>& _redraw_queue) :
    Rectangle(_rect), recording(_recording), 
    redraw_queue(_redraw_queue) {}
  void OnClick() {
    recording = !recording;
    redraw_queue.push(this);
  }
  void Draw(cairo_t *cr) {
    if (recording)
      cairo_set_source_rgb(cr, 1, 0, 0);
    else
      cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);
  }
};

struct Live {
  const vector<int16_t>& buf;
  Live(const vector<int16_t>& _buf) : buf(_buf) {}
};

struct WaveformViewer : public Glyph, public Live {
  WaveformViewer(const Rectangle& _rect, const vector<int16_t>& _buf) :
    Rectangle(_rect), Live(_buf) {}
	void Draw(cairo_t *cr) {
	  // Black out behind waveform
    cairo_set_source_rgb(cr, 0, 0, 0); 
    cairo_rectangle(cr, x-1, y, w, h);
    cairo_fill(cr);
    // Draw waveform
    cairo_set_source_rgb(cr, 0, 1, 0.5); // Lovely hacker green
    cairo_move_to(cr, x, y + h / 2);
    for (double i=0; i<w; i+=2) {
      int data_index = floor(i * buf.size() / w);
   	  int16_t sample = buf[data_index];
		  double s = sample / pow(2,16) * h * 5/6;	
      cairo_line_to(cr, x+i, y+s+h/2);
    }
    cairo_stroke(cr);
	}	
};

class AudioClip : public Glyph, public Live {
  public:
    vector<int16_t> clip;
    uint drawn;
    AudioClip(const Rectangle& _rect, vector<int16_t>& _buf) :
      Rectangle(_rect), Live(_buf), 
      clip(buf.size()), drawn(0) {}
    void Record() {
      memcpy(&clip.back() - (buf.size()-1), &buf.front(), buf.size());
      clip.resize(clip.size() + buf.size());
    }
    void Draw(cairo_t *cr) {
      cairo_set_source_rgb(cr, 0, 0, 0.5);
      cairo_rectangle(cr, x+drawn, y, clip.size() / buf.size() - 1 - drawn, h);
      cairo_fill(cr);
      cairo_set_source_rgb(cr, 1, 1, 1);
      while (drawn < clip.size() / buf.size() - 1) {
        int16_t low = numeric_limits<int16_t>::max(), 
                high = numeric_limits<int16_t>::min();
        for (vector<int16_t>::size_type i=0; i<buf.size(); i++) {
          int index = drawn * buf.size() + i;
          high = clip[index] > high ? clip[index] : high;
          low = clip[index] < low ? clip[index] : low;
        }
        cairo_move_to(cr, x+drawn+0.5, y + h/2 + h/2 * high/pow(2,15));
        cairo_line_to(cr, x+drawn+0.5, y + h/2 + h/2 * low/pow(2,15));
        cairo_stroke(cr);
        drawn++;
      }
    } 
};

int main() {
  CairoXWindow X(1250, 750);

  // Audio stuff
  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2 
  }; 
  PulseStream record_stream(ss, record);
  const int bufsize = 2048;
  vector<int16_t> buf(bufsize);
  
  // Organize components
  vector<Glyph*> live_components; 
  queue<Glyph*> redraw_queue;
  vector<ClickableRectangle*> clickable_components;
  vector<Glyph*> visible_components;

  WaveformViewer viewer({X.ww/3, 0, X.ww/3, 200}, buf);
  live_components.push_back(&viewer);
  
  bool recording = false;
  RecordButton record_button({10,10,20,20}, recording, redraw_queue);
  clickable_components.push_back(&record_button);
  visible_components.push_back(&record_button);

  AudioClip clip1({10, 200, 0, 200}, buf);
  live_components.push_back(&clip1);
  
  for (;;) {
		// Read audio data from PulseAudio into buf
    if (pa_simple_read(record_stream.s, 
            &buf[0], bufsize*2, 
            &record_stream.error) < 0)
        err_n_exit("pa_simple_read failed");

    if (recording)
      clip1.Record();
 
    // Draw live_components
    for (Glyph* comp : live_components)
      comp->Draw(X.cr);

    // Draw any components queued for redraw
    while (!redraw_queue.empty()) {
      redraw_queue.front()->Draw(X.cr);
      redraw_queue.pop();
    }
    
    // Handle X events 
    if (XPending(X.display)) {
      XEvent e;
      XNextEvent(X.display, &e);
      switch (e.type) {
        case ButtonPress:
          for (ClickableRectangle* comp : clickable_components)
            if (comp->Intersects({(double)e.xbutton.x, (double)e.xbutton.y}))
                comp->OnClick();
          break;
        case Expose:
          X.ww = e.xexpose.width;
          X.wh = e.xexpose.height;
          // Background color
          cairo_set_source_rgb(X.cr, 0.5, 0.5, 0.5);
          cairo_rectangle(X.cr, 0, 0, X.ww, X.wh);
          cairo_fill(X.cr);
          // Draw visible components
          clip1.drawn = 0;
          for (Glyph* comp : visible_components)
            comp->Draw(X.cr);
          break;
      }
    }
  }
  
  /*PulseStream playback_stream(ss, playback);
  for (;;) {
    for (int i=0; i<nbufs; i++) {
      if (pa_simple_write(playback_stream.s, 
            &clip1.clip[i*bufsize], bufsize*2,
            &playback_stream.error) < 0)
        err_n_exit("pa_simple_read failed");
    }
    if (pa_simple_drain(playback_stream.s, &playback_stream.error) < 0)
      err_n_exit("pa_simple_drain failed");
  }*/  
}
