#include <vector>
#include <iostream>
#include <cstring>
#include <algorithm>
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
    double x, y, &w, &h;
    Glyph(double x, double y, double& w, double& h)
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

class WaveformViewer : public Glyph {
  public:
    int16_t *buf;
		int bufsize;
		using Glyph::Glyph;
	  void Draw(cairo_t *cr) {
			// Black out behind waveform
      cairo_set_source_rgb(cr, 0, 0, 0); 
      cairo_rectangle(cr, x, y, w, h);
      cairo_fill(cr);

      // Draw waveform
      cairo_set_source_rgb(cr, 0, 1, 0.5); // Lovely hacker green
      cairo_move_to(cr, x, y + h / 2);
      for (double i=0; i < w / 2; i++) {
        int data_index = floor(i * bufsize / w) * 2;
   			int16_t sample = buf[data_index];
			  double s = sample / pow(2,15) * h;	
        cairo_line_to(cr, x+i*2, y+s+h/2);
      }
      cairo_stroke(cr);
		}
	  void OnClick() {}
		void SetBuf(int16_t *_buf, int _bufsize) {
			buf = _buf;
			bufsize = _bufsize;
		}	
};

class AudioClip : public Glyph {
  public:
    int bufsize; 
    vector<int16_t> clip;
    int drawn;
    AudioClip(double _x, double _y, double _w, double _h, int _bufsize) :
      Glyph(_x, _y, _w, _h), bufsize(_bufsize), 
      clip(0, bufsize), drawn(0) {}
    void Draw(cairo_t *cr) {
      if (drawn >= w)
        return;
      cairo_set_source_rgb(cr, 0, 0, 0.5);
      cairo_rectangle(cr, x+drawn, y, 1, h);
      cairo_fill(cr);
      vector<int16_t>::iterator start = clip.begin() + drawn * bufsize,
	                        end = clip.begin() + (drawn+1) * bufsize;
      int16_t low = *min_element(start, end);
      int16_t high = *max_element(start, end);
      cairo_set_source_rgb(cr, 1, 1, 1);
      cairo_move_to(cr, x+drawn+0.5, y + h/2 + h/2 * high/pow(2,15));
      cairo_line_to(cr, x+drawn+0.5, y + h/2 + h/2 * low/pow(2,15));
      cairo_stroke(cr);
      drawn++;
    }
    void OnClick(){}
    void Record(PulseStream *record_stream) { 
      vector<int16_t>::size_type prev_size = clip.size();
      clip.resize(clip.size() + bufsize); 
      // We do bufsize * 2 because the PulseAudio API is expecting a uint8_t array,
      // but our vector is of int16_t, which is twice the size of a uint8_t.
      if (pa_simple_read(record_stream->s, 
            &clip[prev_size], bufsize*2, 
            &record_stream->error) < 0)
        err_n_exit("pa_simple_read failed");
  }
};

int main() {
  CairoXWindow X(1250, 750);
  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2 
  };
  PulseStream *record_stream = new PulseStream(ss, record);
  const int bufsize = 2048;
  vector<int16_t> buf;
  buf.resize(bufsize);	
  const int seconds = 50;
  //const int nbufs = floor(seconds * (double)ss.rate / bufsize);
  //AudioClip clip1(0, 100, X.ww, 100, bufsize);
	WaveformViewer viewer(0, 0, X.ww, X.wh);
  for (;;) {
		if (pa_simple_read(record_stream->s, 
            &buf[0], bufsize*2, 
            &record_stream->error) < 0)
        err_n_exit("pa_simple_read failed");

		//clip1.Record(record_stream);
    //clip1.Draw(X.cr);
		viewer.SetBuf(&buf[0], bufsize);
	  viewer.Draw(X.cr);	
    if (XPending(X.display)) {
      XEvent e;
      XNextEvent(X.display, &e);
      switch (e.type) {
        case ButtonPress:
          break;
        case Expose:
          X.ww = e.xexpose.width;
          X.wh = e.xexpose.height;
          // Background color
          cairo_set_source_rgb(X.cr, 0.5, 0.5, 0.5);
          cairo_rectangle(X.cr, 0, 0, X.ww, X.wh);
          cairo_fill(X.cr);
          break;
      }
    }
  }
  
  delete record_stream;

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
