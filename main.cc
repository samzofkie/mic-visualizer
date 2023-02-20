#include <vector>
#include <iostream>
#include <cstring>
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

class Glyph {
  public:
    double x, y, w, h;
    Glyph(double x, double y, double w, double h)
      : x(x), y(y), w(w), h(h) {};
    virtual void Draw(cairo_t *cr) =0;
    virtual bool Intersects(const Point& p) {
      if (p.x >= x && p.x <= x+w)
        if (p.y >= y && p.y <= y+h)
          return true;
      return false;
    }
    virtual void OnClick() =0;
};

class Button : public Glyph {
  public:
    using Glyph::Glyph;
    void Draw(cairo_t *cr) {
      cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
      cairo_rectangle(cr, x, y, w, h);
      cairo_fill(cr);
    }
    void OnClick() {
      cout << "Button click" << endl;
    }
};

class Live {
  public:
    uint8_t *buf;
    int bufsize;
    Live(uint8_t *_buf, int _bufsize) :
    buf(_buf), bufsize(_bufsize) {}
};

class LiveGlyph : public Glyph, public Live {
  public:
    LiveGlyph(double _x, double _y, double _w, double _h,
        uint8_t *_buf, int _bufsize) :
      Glyph(_x, _y, _w, _h), Live(_buf, _bufsize) {}
};

class WaveformViewer : public LiveGlyph {
  public:
    using LiveGlyph::LiveGlyph;
    void Draw(cairo_t *cr) {
      // Black out behind waveform
      cairo_set_source_rgb(cr, 0, 0, 0); 
      cairo_rectangle(cr, x-1, y, w, h);
      cairo_fill(cr);

      // Draw waveform
      cairo_set_source_rgb(cr, 0, 1, 0.5); // Lovely hacker green
      cairo_move_to(cr, x, y + h / 2);
      for (double i=0; i < w / 2; i++) {
        int data_index = floor(i * bufsize / w) * 2;
        // Convert two uint8_t's to one signed 16bit (short)
        short sample = buf[data_index] << 8 | buf[data_index+1];
        double s = sample / pow(2,15); // Scale to between -1 and 1
        s *= (h / 3);
        cairo_line_to(cr, x + i * 2, y + s + h / 2);
      }
      cairo_stroke(cr);
    } 
    void OnClick(){}
};

class AudioClip : public LiveGlyph {
  public:
    const int nbufs;
    int curr_buf;
    AudioClip(double _x, double _y, double _w, double _h,
        uint8_t *_buf, int _bufsize, const int _nbufs) :
      LiveGlyph(_x, _y, _w, _h, _buf, _bufsize), 
      nbufs(_nbufs), curr_buf(0) {}
    void Draw(cairo_t *cr) {
      // Background
      cairo_set_source_rgb(cr, 0, 0, 0.5);
      cairo_rectangle(cr, curr_buf*w/nbufs, y, w/nbufs, h);
      cairo_fill(cr);
      
      // Waveform
      double max = pow(2,16);
      int16_t high = 0, low = max;
      for (int i=0; i<bufsize; i+=2) {
        int16_t sample = buf[i] << 8 | buf[i+1];
        high = sample > high? sample : high;
        low = sample < low? sample : low;
      }
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_move_to(cr, x + curr_buf*w/nbufs, y + h/2 + h/2*high/max);
      cairo_line_to(cr, x + curr_buf*w/nbufs, y + h/2 + h/2*low/max);
      cairo_stroke(cr);
      
      curr_buf++; 
    }
    void OnClick(){}
};

int main() {
  CairoXWindow X(1500, 750);
  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2 
  };
  PulseStream *record_stream = new PulseStream(ss, record);

  const int bufsize = 2048;
  vector<int16_t> clip; 
 
  const int seconds = 5;
  const int nbufs = floor(seconds * (double)ss.rate / bufsize);

  // Background color
  cairo_set_source_rgb(X.cr, 0.5, 0.5, 0.5);
  cairo_rectangle(X.cr, 0, 0, X.ww, X.wh);
  cairo_fill(X.cr);

  // Main loop
  for (int i=0; i<nbufs; i++) {
        
    // Read in data from mic
    clip.resize(clip.size() + bufsize);
    // We do bufsize * 2 because the PulseAudio API is expecting a uint8_t array,
    // but our vector is of int16_t, which is twice the size of a uint8_t.
    if (pa_simple_read(record_stream->s, 
          &clip[bufsize*i], bufsize*2, 
          &record_stream->error) < 0)
      err_n_exit("pa_simple_read failed");
    
    if (XPending(X.display)) {
      XEvent e;
      XNextEvent(X.display, &e);
      switch (e.type) {
        case ButtonPress:
          break;
        case Expose:
          X.ww = e.xexpose.width;
          X.wh = e.xexpose.height;
          break;
      }
    }
  }
  
  delete record_stream;

  PulseStream playback_stream(ss, playback);
  for (;;) {
    for (int i=0; i<nbufs; i++) {
      if (pa_simple_write(playback_stream.s, 
            &clip[i*bufsize], bufsize*2,
            &playback_stream.error) < 0)
        err_n_exit("pa_simple_read failed");
    }
    if (pa_simple_drain(playback_stream.s, &playback_stream.error) < 0)
      err_n_exit("pa_simple_drain failed");
  }        
}
