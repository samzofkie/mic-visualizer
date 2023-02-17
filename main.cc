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
    Glyph(double x, double y, double w, double h)
      : x(x), y(y), w(w), h(h) {};
    virtual void Draw(cairo_t *cr) =0;
    virtual bool Intersects(const Point&) =0;
    virtual void OnClick() =0;
    double x, y, w, h;
};

class Button : public Glyph {
  public:
    using Glyph::Glyph;
    void Draw(cairo_t *cr) {
      cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
      cairo_rectangle(cr, x, y, w, h);
      cairo_fill(cr);
    }
    bool Intersects(const Point &p) {
      if (p.x >= x && p.x <= x+w)
        if (p.y >= y && p.y <= y+h)
          return true;
      return false;
    }
    void OnClick() {
      cout << "Button click" << endl;
    }
};

int main() {
  CairoXWindow X(1500, 750);
  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16BE,
    .rate = 44100,
    .channels = 2 
  };
  PulseStream *record_stream = new PulseStream(ss, record);
  Button b(100, 100, 30, 20);

  const int bufsize = 4096;
  const int seconds = 300;
  uint8_t *song = (uint8_t*)malloc(ss.rate * seconds * sizeof(uint8_t));
  const int nbufs = floor(seconds * (double)ss.rate / bufsize);
  for (int i=0; i<nbufs; i++) {
    uint8_t buf[bufsize]; 
    
    // Read in data from mic 
    if (pa_simple_read(record_stream->s, buf, sizeof(buf), 
        &record_stream->error) < 0)
      err_n_exit("pa_simple_read failed");
    if (memcpy(song + i * bufsize, buf, bufsize) != song + i * bufsize)
      err_n_exit("memcpy from buf to song failed");

    // Black out behind waveform
    cairo_set_source_rgb(X.cr, 0, 0, 0); 
    cairo_rectangle(X.cr, 0, X.wh / 6 - 5, X.ww, X.wh * 5 / 6 + 5);
    cairo_fill(X.cr);

    // Draw waveform
    cairo_set_source_rgb(X.cr, 0, 1, 0.5);
    cairo_move_to(X.cr, 0, X.wh / 2);
    for (double j=0; j<X.ww / 2; j++) {
      int data_index = floor(j * bufsize / X.ww) * 2;
      // Convert two uint8_t's to one signed 16bit (short)
      short sample = buf[data_index] << 8 | buf[data_index] ;
      double s = sample / pow(2,15); // Scale to between -1 and 1
      s *= (X.wh / 3);
      cairo_line_to(X.cr, j * 2, s + X.wh / 2);
    }
    cairo_stroke(X.cr);

    // Draw button
    b.Draw(X.cr);
    
    if (XPending(X.display)) {
      XEvent e;
      XNextEvent(X.display, &e);
      switch (e.type) {
        case ButtonPress:
         if (b.Intersects({(double)e.xbutton.x, (double)e.xbutton.y}))
             b.OnClick();
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
            song + i * bufsize, bufsize,
            &playback_stream.error) < 0)
        err_n_exit("pa_simple_read failed");
    }
    if (pa_simple_drain(playback_stream.s, &playback_stream.error) < 0)
      err_n_exit("pa_simple_drain failed");
  }        
  free(song);
}